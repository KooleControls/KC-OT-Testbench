#pragma once

#include "InitState.h"
#include "BoardConfig.h"
#include "interfaces/BoardProvider.h"
#include "interfaces/DigitalOutput.h"
#include "drivers/Max14830/Max14830.h"
#include "drivers/Max14830/Max14830Pin.h"
#include "drivers/Esp32Ethernet.h"

// ──────────────────────────────────────────────────────────────
// The board layer's context for the PCB1245: owns every hardware driver
// instance (and bus host) and answers BoardProvider.
//
// The bottom layer, and it depends on nothing above it — not the
// framework, not the application. Drivers take their pins and buses as
// constructor arguments, so nothing here needs the provider to find a
// peer; BoardProvider exists for the layer above.
//
// Every board folder provides a class named BoardContext; #include
// "BoardContext.h" resolves to the board selected with -DBOARD=<name>.
//
// Surface rules:
//   • role interfaces (Led&, ...) for devices the application
//     addresses by meaning — declared on BoardProvider, so every board
//     owes every role and binds a Mock* driver when not fitted;
//   • concrete driver accessors are the escape hatch for when the
//     application needs a driver's full API. Those stay OFF
//     BoardProvider and are checked at compile time, which is what
//     stops the role list becoming the union of every board's
//     peripherals.
//
// Almost everything hangs off the MAX14830: the LED, relays, inputs and every
// UART. Ethernet is the ESP32's own MAC.
// ──────────────────────────────────────────────────────────────

class BoardContext : public BoardProvider
{
    static constexpr const char *TAG = "Board";

public:
    BoardContext() = default;

    BoardContext(const BoardContext &) = delete;
    BoardContext &operator=(const BoardContext &) = delete;
    BoardContext(BoardContext &&) = delete;
    BoardContext &operator=(BoardContext &&) = delete;

    void Init();

    // ── Roles ────────────────────────────────────────────────
    Led &GetLed() override { return led_; }
    Relay &GetRelay(RelayId id) override;
    DigitalInput &GetInput(InputId id) override;
    SerialPort &GetSerialPort(SerialPortId id) override;
    OpenThermModule &GetOpenThermModule() override { return pcb1246_; }

    // ── Concrete accessors ───────────────────────────────────
    Max14830 &GetMax14830() { return max_; }

    /// The Ethernet driver, installed but not started — the network stack starts it.
    /// Null when the MAC or PHY failed to come up.
    esp_eth_handle_t GetEthernet() const { return ethernet_.Handle(); }

    /// The XBee's reset line, held released after Init().
    DigitalOutput &GetXBeeReset() { return xbeeReset_; }

private:
    // The PCB1246 as one role: its UART and its two control lines.
    class Pcb1246 final : public OpenThermModule
    {
    public:
        Pcb1246(SerialPort &serial, DigitalOutput &reset, DigitalOutput &isp)
            : serial_(serial), reset_(reset), isp_(isp) {}

        SerialPort &Serial() override { return serial_; }
        void SetReset(bool asserted) override { reset_.Set(asserted); }
        void SetIsp(bool asserted) override { isp_.Set(asserted); }

    private:
        SerialPort &serial_;
        DigitalOutput &reset_;
        DigitalOutput &isp_;
    };

    InitState initState_;

    // Hardware instances — buses first, then the drivers that use them.
    Max14830 max_{ {
        BoardConfig::MAX_SPI_HOST,
        BoardConfig::MAX_MISO, BoardConfig::MAX_MOSI, BoardConfig::MAX_SCLK,
        BoardConfig::MAX_CS, BoardConfig::MAX_IRQ,
        BoardConfig::MAX_SPI_HZ, BoardConfig::MAX_XTAL_HZ, true } };

    Max14830Output<Led> led_{ max_, BoardConfig::PIN_LED, false };
    Max14830Output<Relay> relay1_{ max_, BoardConfig::PIN_RELAY1, true };
    Max14830Output<Relay> relay2_{ max_, BoardConfig::PIN_RELAY2, true };
    Max14830Input input1_{ max_, BoardConfig::PIN_INPUT1, false };
    Max14830Input input2_{ max_, BoardConfig::PIN_INPUT2, false };
    Max14830Input resetButton_{ max_, BoardConfig::PIN_RESET_BTN, false };
    Max14830Input sdDetect_{ max_, BoardConfig::PIN_SD_DETECT, false };
    Max14830Output<DigitalOutput> xbeeReset_{ max_, BoardConfig::PIN_XBEE_RESET, false };
    Max14830Output<DigitalOutput> pcb1246Reset_{ max_, BoardConfig::PIN_PCB1246_RST, true };
    Max14830Output<DigitalOutput> pcb1246Isp_{ max_, BoardConfig::PIN_PCB1246_ISP, true };

    Pcb1246 pcb1246_{ max_.Uart(BoardConfig::UART_PCB1246), pcb1246Reset_, pcb1246Isp_ };

    Esp32Ethernet ethernet_{ {
        BoardConfig::ETH_MDC, BoardConfig::ETH_MDIO, BoardConfig::ETH_PHY_ADDRESS,
        BoardConfig::ETH_PHY_RESET, BoardConfig::ETH_REF_CLOCK } };
};
