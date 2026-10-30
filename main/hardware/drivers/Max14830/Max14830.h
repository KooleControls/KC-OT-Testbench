#pragma once

#include "Max14830Uart.h"
#include "Task.h"
#include "Mutex.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// MAX14830: four UARTs and sixteen GPIOs behind one SPI device and one IRQ line.
//
// The chip is owned by one object, and everything that touches its registers goes
// through mutex_ — the service task, GPIO writes from any task, a UART being
// configured. That is the whole concurrency story.
//
// One service task does all the moving: the IRQ line wakes it, and on every wake it
// drains each UART's RX FIFO into that UART's buffer, refills each TX FIFO from the
// UART's buffer, and re-reads the GPIO inputs if one changed. It also wakes every
// SERVICE_POLL_MS without an IRQ, so a missed edge delays data instead of stranding it.
//
// GPIO numbering is the chip's: pin 0-15, four per UART port (pin / 4 is the port).
// Clock setup comes from the ESP-MAX14830 component; the register map is max310x.h.
// ──────────────────────────────────────────────────────────────

struct Max14830Config
{
    spi_host_device_t host;
    gpio_num_t miso;
    gpio_num_t mosi;
    gpio_num_t sclk;
    gpio_num_t cs;
    gpio_num_t irq;              // active low, needs a pull-up on the board
    uint32_t spiClockHz;
    uint32_t refClockHz;         // the crystal or external clock on XIN
    bool crystal;                // true: crystal on XIN/XOUT; false: external clock on XIN
};

class Max14830
{
    static constexpr const char* TAG = "Max14830";

public:
    static constexpr uint8_t PORTS = 4;
    static constexpr uint8_t PINS = 16;
    static constexpr uint32_t SERVICE_POLL_MS = 100;

    explicit Max14830(const Max14830Config& config);

    Max14830(const Max14830&) = delete;
    Max14830& operator=(const Max14830&) = delete;

    /// SPI, chip detect, clock, the four ports' interrupt setup, the IRQ and the service
    /// task. False when the chip does not answer; everything else then stays inert.
    bool Init();

    bool IsReady() const { return ready_; }

    Max14830Uart& Uart(uint8_t port) { return uarts_[port]; }

    // ── GPIO ─────────────────────────────────────────────────
    void PinModeOutput(uint8_t pin, bool level, bool openDrain = false);
    void PinModeInput(uint8_t pin);
    void WritePin(uint8_t pin, bool level);
    /// The pin's level as of the last input-change interrupt (or PinModeInput).
    bool ReadPin(uint8_t pin) const { return (inputs_ >> pin) & 1; }

    // ── Used by Max14830Uart ─────────────────────────────────
    bool ConfigurePort(uint8_t port, uint32_t baud, bool cts, bool rs485);
    void SetBaud(uint8_t port, uint32_t baud);
    /// Wake the service task: a UART has bytes to send.
    void KickTx() { task_.Notify(1); }

private:
    Max14830Config config_;
    Max14830Uart uarts_[PORTS];

    Mutex mutex_;
    Task task_;
    spi_device_handle_t spi_ = nullptr;
    bool ready_ = false;
    uint32_t refClock_ = 0;        // after the PLL: what the baud generators divide

    // GPIO shadows. Writes go through these so one pin never disturbs its neighbours.
    uint16_t outputs_ = 0;         // GPIODATA low nibble per port
    uint8_t gpioCfg_[PORTS] = {};  // GPIOCFG per port: [7:4] open-drain, [3:0] output
    uint8_t stsIrqEn_[PORTS] = {}; // which inputs raise an interrupt on change
    volatile uint16_t inputs_ = 0;

    // FIFO transfer buffers. Members rather than locals: the SPI driver uses DMA for a
    // transfer this long, and these are word-aligned in internal RAM.
    alignas(4) uint8_t rxBuf_[MAX310X_FIFOSIZE];
    alignas(4) uint8_t txBuf_[MAX310X_FIFOSIZE];

    void SetBaudLocked(uint8_t port, uint32_t baud);
    bool Detect();
    bool SetRefClock();
    void ServiceTask();
    void ServiceLocked();
    void DrainRx(uint8_t port);
    void FillTx(uint8_t port);
    void RefreshInputs(uint8_t port);

    static void IRAM_ATTR IrqHandler(void* arg);

    // ── Register access. Caller holds mutex_. ────────────────
    bool Write(uint8_t reg, const uint8_t* data, size_t len);
    bool Read(uint8_t reg, uint8_t* data, size_t len);
    void WriteReg(uint8_t reg, uint8_t value) { Write(reg, &value, 1); }
    uint8_t ReadReg(uint8_t reg);
    void PortWrite(uint8_t port, uint8_t reg, uint8_t value) { WriteReg((port << 5) | reg, value); }
    uint8_t PortRead(uint8_t port, uint8_t reg) { return ReadReg((port << 5) | reg); }
    void PortUpdate(uint8_t port, uint8_t reg, uint8_t mask, uint8_t value);
};
