#include "Max14830.h"
#include "ContextLock.h"
#include "esp_log.h"
#include <cstring>

static constexpr uint8_t MAX14830_REV_ID = 0xB0;
static constexpr uint8_t WRITE_BIT = 0x80;

Max14830::Max14830(const Max14830Config& config)
    : config_(config)
{
    for (uint8_t port = 0; port < PORTS; port++)
        uarts_[port].Bind(*this, port);
}

bool Max14830::Init()
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = config_.mosi;
    bus.miso_io_num = config_.miso;
    bus.sclk_io_num = config_.sclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = MAX310X_FIFOSIZE + 4;
    esp_err_t err = spi_bus_initialize(config_.host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    // The first byte of every transfer is the register address, with bit 7 set for a
    // write and the UART port in bits 6:5 — the SPI driver's command phase.
    spi_device_interface_config_t dev = {};
    dev.command_bits = 8;
    dev.mode = 0;
    dev.clock_speed_hz = static_cast<int>(config_.spiClockHz);
    dev.spics_io_num = config_.cs;
    dev.queue_size = 1;
    err = spi_bus_add_device(config_.host, &dev, &spi_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return false;
    }

    {
        LOCK(mutex_);

        if (!Detect())
            return false;

        if (!SetRefClock())
            return false;

        for (uint8_t port = 0; port < PORTS; port++)
        {
            // RX interrupt on "FIFO not empty" rather than on a fill level.
            PortWrite(port, MAX310X_MODE2_REG, MAX310X_MODE2_RXEMPTINV_BIT);
            // Route this port's interrupts to the IRQ pin.
            PortUpdate(port, MAX310X_MODE1_REG, MAX310X_MODE1_IRQSEL_BIT, MAX310X_MODE1_IRQSEL_BIT);

            PortRead(port, MAX310X_IRQSTS_REG);
            PortRead(port, MAX310X_LSR_IRQSTS_REG);
            PortRead(port, MAX310X_SPCHR_IRQSTS_REG);
            PortRead(port, MAX310X_STS_IRQSTS_REG);

            // Until the UART is configured only its GPIOs can interrupt.
            PortWrite(port, MAX310X_IRQEN_REG, MAX310X_IRQ_STS_BIT);
            PortWrite(port, MAX310X_LSR_IRQEN_REG, 0);
            PortWrite(port, MAX310X_SPCHR_IRQEN_REG, 0);
            PortWrite(port, MAX310X_STS_IRQEN_REG, 0);
            PortWrite(port, MAX310X_GPIOCFG_REG, 0);
        }
        ReadReg(MAX310X_GLOBALIRQ_REG);
    }

    task_.Init("max14830", 10, 4096);
    task_.SetHandler([this] { ServiceTask(); });
    task_.Run();

    // The line is level-triggered and the handler masks it until the task has serviced
    // the chip, so an interrupt that is still pending afterwards fires again at once.
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << config_.irq;
    io.mode = GPIO_MODE_INPUT;
    io.intr_type = GPIO_INTR_LOW_LEVEL;
    gpio_config(&io);
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)   // already installed is fine
        ESP_LOGE(TAG, "GPIO ISR service: %s", esp_err_to_name(err));
    gpio_isr_handler_add(config_.irq, &Max14830::IrqHandler, this);

    ready_ = true;
    ESP_LOGI(TAG, "Initialized, reference clock %lu Hz", static_cast<unsigned long>(refClock_));
    return true;
}

// ──────────────────────────────────────────────────────────────
// Service task
// ──────────────────────────────────────────────────────────────

void IRAM_ATTR Max14830::IrqHandler(void* arg)
{
    auto* self = static_cast<Max14830*>(arg);
    gpio_intr_disable(self->config_.irq);
    BaseType_t woken = pdFALSE;
    self->task_.NotifyFromISR(1, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

void Max14830::ServiceTask()
{
    for (;;)
    {
        uint32_t bits = 0;
        task_.NotifyWait(&bits, pdMS_TO_TICKS(SERVICE_POLL_MS));

        {
            LOCK(mutex_);
            ServiceLocked();
        }

        gpio_intr_enable(config_.irq);
    }
}

void Max14830::ServiceLocked()
{
    for (uint8_t port = 0; port < PORTS; port++)
    {
        // Reading the status clears it.
        const uint8_t isr = PortRead(port, MAX310X_IRQSTS_REG);
        if (isr & MAX310X_IRQ_STS_BIT)
        {
            const uint8_t sts = PortRead(port, MAX310X_STS_IRQSTS_REG);
            if (sts & 0x0F)
                RefreshInputs(port);
        }

        if (uarts_[port].IsConfigured())
        {
            DrainRx(port);
            FillTx(port);
        }
    }
}

void Max14830::DrainRx(uint8_t port)
{
    // Until empty: the RX interrupt is raised when the FIFO stops being empty, so bytes
    // left behind here would not raise another.
    for (;;)
    {
        const uint8_t level = PortRead(port, MAX310X_RXFIFOLVL_REG);
        if (level == 0)
            return;
        const size_t n = level > MAX310X_FIFOSIZE ? MAX310X_FIFOSIZE : level;
        if (!Read((port << 5) | MAX310X_RHR_REG, rxBuf_, n))
            return;
        uarts_[port].PushRx(rxBuf_, n);
    }
}

void Max14830::FillTx(uint8_t port)
{
    const uint8_t level = PortRead(port, MAX310X_TXFIFOLVL_REG);
    if (level >= MAX310X_FIFOSIZE)
        return;
    const size_t n = uarts_[port].PopTx(txBuf_, MAX310X_FIFOSIZE - level);
    if (n > 0)
        Write((port << 5) | MAX310X_THR_REG, txBuf_, n);
}

void Max14830::RefreshInputs(uint8_t port)
{
    const uint16_t levels = (PortRead(port, MAX310X_GPIODATA_REG) >> 4) & 0x0F;
    const uint16_t mask = 0x0F << (port * 4);
    inputs_ = (inputs_ & ~mask) | (levels << (port * 4));
}

// ──────────────────────────────────────────────────────────────
// UART ports
// ──────────────────────────────────────────────────────────────

bool Max14830::ConfigurePort(uint8_t port, uint32_t baud, bool cts, bool rs485)
{
    if (!ready_)
        return false;

    LOCK(mutex_);

    PortWrite(port, MAX310X_IRQEN_REG, 0);

    // The KC1245 firmware also set TXDIS here when CTS was used; the Linux max310x
    // driver does not, and with TXDIS the transmitter does not send at all. Auto-CTS
    // alone is what pauses it.
    PortUpdate(port, MAX310X_MODE1_REG, MAX310X_MODE1_TXDIS_BIT, 0);
    PortWrite(port, MAX310X_FLOWCTRL_REG, cts ? MAX310X_FLOWCTRL_AUTOCTS_BIT : 0);

    SetBaudLocked(port, baud);
    PortWrite(port, MAX310X_LCR_REG, MAX310X_LCR_LENGTH0_BIT | MAX310X_LCR_LENGTH1_BIT);   // 8N1

    // RS485: the chip drives RTS as the transceiver's driver enable, with the setup and
    // hold delays the KC1245 used.
    PortUpdate(port, MAX310X_MODE1_REG, MAX310X_MODE1_TRNSCVCTRL_BIT,
               rs485 ? MAX310X_MODE1_TRNSCVCTRL_BIT : 0);
    PortWrite(port, MAX310X_HDPIXDELAY_REG, rs485 ? 0x11 : 0);

    // Reset both FIFOs, keeping RX-not-empty as the RX interrupt. RS485 is half duplex:
    // the receiver also ignores the line while this side sends, so a transceiver that
    // hears itself cannot hand this side's own frames back as someone else's.
    PortWrite(port, MAX310X_MODE2_REG, MAX310X_MODE2_RXEMPTINV_BIT | MAX310X_MODE2_FIFORST_BIT);
    PortWrite(port, MAX310X_MODE2_REG,
              MAX310X_MODE2_RXEMPTINV_BIT | (rs485 ? MAX310X_MODE2_ECHOSUPR_BIT : 0));

    PortRead(port, MAX310X_IRQSTS_REG);
    PortRead(port, MAX310X_LSR_IRQSTS_REG);

    PortWrite(port, MAX310X_IRQEN_REG,
              MAX310X_IRQ_RXEMPTY_BIT | MAX310X_IRQ_TXEMPTY_BIT | MAX310X_IRQ_STS_BIT);
    return true;
}

void Max14830::SetBaud(uint8_t port, uint32_t baud)
{
    if (!ready_)
        return;
    LOCK(mutex_);
    SetBaudLocked(port, baud);
}

void Max14830::SetBaudLocked(uint8_t port, uint32_t baud)
{
    // Divider = clock / baud, in 1/16ths. When it does not divide evenly, try the
    // generator's 2x and 4x modes for a finer step.
    uint8_t mode = 0;
    uint32_t clk = refClock_;
    uint32_t div = clk / baud;
    if (div < 16)
        div = 16;

    if ((clk % baud) && ((div / 16) < 0x8000))
    {
        mode = MAX310X_BRGCFG_2XMODE_BIT;
        clk = refClock_ * 2;
        div = clk / baud;
        if ((clk % baud) && ((div / 16) < 0x8000))
        {
            mode = MAX310X_BRGCFG_4XMODE_BIT;
            clk = refClock_ * 4;
            div = clk / baud;
        }
    }

    PortWrite(port, MAX310X_BRGDIVMSB_REG, (div / 16) >> 8);
    PortWrite(port, MAX310X_BRGDIVLSB_REG, div / 16);
    PortWrite(port, MAX310X_BRGCFG_REG, (div % 16) | mode);
}

// ──────────────────────────────────────────────────────────────
// GPIO
// ──────────────────────────────────────────────────────────────

void Max14830::PinModeOutput(uint8_t pin, bool level, bool openDrain)
{
    if (!ready_ || pin >= PINS)
        return;

    const uint8_t port = pin / 4;
    const uint8_t bit = 1 << (pin % 4);

    WritePin(pin, level);   // the level first, so the pin never shows the old one

    LOCK(mutex_);
    gpioCfg_[port] |= bit;
    if (openDrain)
        gpioCfg_[port] |= bit << 4;
    else
        gpioCfg_[port] &= ~(bit << 4);
    PortWrite(port, MAX310X_GPIOCFG_REG, gpioCfg_[port]);

    stsIrqEn_[port] &= ~bit;
    PortWrite(port, MAX310X_STS_IRQEN_REG, stsIrqEn_[port]);
}

void Max14830::PinModeInput(uint8_t pin)
{
    if (!ready_ || pin >= PINS)
        return;

    const uint8_t port = pin / 4;
    const uint8_t bit = 1 << (pin % 4);

    LOCK(mutex_);
    gpioCfg_[port] &= ~(bit | (bit << 4));
    PortWrite(port, MAX310X_GPIOCFG_REG, gpioCfg_[port]);

    // An input interrupts on every change, which is what keeps ReadPin() current.
    stsIrqEn_[port] |= bit;
    PortWrite(port, MAX310X_STS_IRQEN_REG, stsIrqEn_[port]);

    RefreshInputs(port);
}

void Max14830::WritePin(uint8_t pin, bool level)
{
    if (!ready_ || pin >= PINS)
        return;

    const uint8_t port = pin / 4;

    LOCK(mutex_);
    if (level)
        outputs_ |= 1 << pin;
    else
        outputs_ &= ~(1 << pin);
    PortWrite(port, MAX310X_GPIODATA_REG, (outputs_ >> (port * 4)) & 0x0F);
}

// ──────────────────────────────────────────────────────────────
// Chip setup
// ──────────────────────────────────────────────────────────────

bool Max14830::Detect()
{
    WriteReg(MAX310X_GLOBALCMD_REG, MAX310X_EXTREG_ENBL);
    const uint8_t rev = PortRead(0, MAX310X_REVID_EXTREG);
    WriteReg(MAX310X_GLOBALCMD_REG, MAX310X_EXTREG_DSBL);

    if ((rev & MAX310x_REV_MASK) != MAX14830_REV_ID)
    {
        ESP_LOGE(TAG, "Chip not found (revision 0x%02x)", rev);
        return false;
    }
    return true;
}

namespace {

/// The remainder a reference clock leaves at 115200 baud x16 — how badly it divides
/// down to the common rates. Updates `best` and returns true when `f` is better.
bool BetterClock(uint64_t f, int64_t& best)
{
    const int64_t err = f % (115200 * 16);
    if (best < 0 || err < best)
    {
        best = err;
        return true;
    }
    return false;
}

} // namespace

bool Max14830::SetRefClock()
{
    // Try the clock as is, then every PLL divider/multiplier the datasheet allows,
    // keeping whichever divides best into the standard baud rates.
    const uint64_t fin = config_.refClockHz;
    int64_t best = -1;
    uint64_t bestFreq = fin;
    uint8_t pllcfg = 0;

    BetterClock(fin, best);

    for (uint32_t div = 1; div <= 63 && best != 0; div++)
    {
        const uint64_t fdiv = fin / div;

        struct { uint64_t lo, hi; uint32_t mul; uint8_t sel; } const options[] = {
            { 500000, 800000,   6, 0 },
            { 850000, 1200000, 48, 1 },
            { 425000, 1000000, 96, 2 },
            { 390000, 667000, 144, 3 },
        };
        for (const auto& o : options)
        {
            if (fdiv < o.lo || fdiv > o.hi)
                continue;
            const uint64_t f = fdiv * o.mul;
            if (BetterClock(f, best))
            {
                pllcfg = static_cast<uint8_t>((o.sel << 6) | div);
                bestFreq = f;
            }
        }
    }

    uint8_t clksrc = config_.crystal ? MAX310X_CLKSRC_CRYST_BIT : MAX310X_CLKSRC_EXTCLK_BIT;
    if (pllcfg)
    {
        clksrc |= MAX310X_CLKSRC_PLL_BIT;
        WriteReg(MAX310X_PLLCFG_REG, pllcfg);
    }
    else
    {
        clksrc |= MAX310X_CLKSRC_PLLBYP_BIT;
    }
    WriteReg(MAX310X_CLKSRC_REG, clksrc);

    if (config_.crystal)
    {
        // Up to two seconds for the oscillator to start.
        bool stable = false;
        for (int i = 0; i < 100 && !stable; i++)
        {
            stable = ReadReg(MAX310X_STS_IRQSTS_REG) & MAX310X_STS_CLKREADY_BIT;
            if (!stable)
                vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!stable)
        {
            ESP_LOGE(TAG, "Crystal did not start");
            return false;
        }
    }

    refClock_ = static_cast<uint32_t>(bestFreq);
    return true;
}

// ──────────────────────────────────────────────────────────────
// Register access
// ──────────────────────────────────────────────────────────────

bool Max14830::Write(uint8_t reg, const uint8_t* data, size_t len)
{
    spi_transaction_t t = {};
    t.cmd = WRITE_BIT | reg;
    t.length = len * 8;
    if (len <= 4)
    {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    }
    else
    {
        t.tx_buffer = data;
    }
    return spi_device_polling_transmit(spi_, &t) == ESP_OK;
}

bool Max14830::Read(uint8_t reg, uint8_t* data, size_t len)
{
    spi_transaction_t t = {};
    t.cmd = reg;
    t.length = len * 8;
    t.rxlength = len * 8;
    if (len <= 4)
    {
        t.flags = SPI_TRANS_USE_RXDATA;
        if (spi_device_polling_transmit(spi_, &t) != ESP_OK)
            return false;
        memcpy(data, t.rx_data, len);
        return true;
    }
    t.rx_buffer = data;
    return spi_device_polling_transmit(spi_, &t) == ESP_OK;
}

uint8_t Max14830::ReadReg(uint8_t reg)
{
    uint8_t value = 0;
    Read(reg, &value, 1);
    return value;
}

void Max14830::PortUpdate(uint8_t port, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t v = PortRead(port, reg);
    v = (v & ~mask) | (value & mask);
    PortWrite(port, reg, v);
}
