#pragma once

#include "BoardConfig.h"
#include "interfaces/BoardProvider.h"
#include "drivers/LpcUart0.h"
#include "drivers/LpcGpioOutput.h"
#include "drivers/ManchesterLine.h"
#include "drivers/LpcWatchdog.h"

// ──────────────────────────────────────────────────────────────
// The board layer's context for the PCB1246: owns every hardware driver
// instance and answers BoardProvider.
//
// The bottom layer, and it depends on nothing above it — not the
// framework, not the application. Drivers take their pins as constructor
// arguments, so nothing here needs the provider to find a peer;
// BoardProvider exists for the layer above.
//
// Every board folder provides a class named BoardContext; #include
// "BoardContext.h" resolves to the board selected with -DBOARD=<name>.
//
// No RTOS: every driver here is interrupts plus state the main loop polls.
// ──────────────────────────────────────────────────────────────

class BoardContext : public BoardProvider
{
public:
    BoardContext() = default;

    BoardContext(const BoardContext &) = delete;
    BoardContext &operator=(const BoardContext &) = delete;
    BoardContext(BoardContext &&) = delete;
    BoardContext &operator=(BoardContext &&) = delete;

    void Init();

    // ── Roles ────────────────────────────────────────────────
    SerialPort &GetGatewaySerial() override { return gatewaySerial_; }
    OpenThermLine &GetOpenThermLine(OpenThermSide side) override;
    DigitalOutput &GetThermostatPower() override { return thermostatPower_; }

    // ── Concrete accessors ───────────────────────────────────
    /// Feed it from the main loop; it resets the board when that stops.
    LpcWatchdog &GetWatchdog() { return watchdog_; }

private:
    LpcUart0 gatewaySerial_{
        LPC_IOCON->PIO0_18, BoardConfig::GATEWAY_RX_IOCON,
        LPC_IOCON->PIO0_19, BoardConfig::GATEWAY_TX_IOCON };

    ManchesterLine thermostat_{ {
        BoardConfig::THERMOSTAT_SCT,
        &LPC_IOCON->PIO1_30, BoardConfig::THERMOSTAT_RX_IOCON, BoardConfig::THERMOSTAT_RX_INVERTED,
        &LPC_IOCON->PIO0_16, BoardConfig::THERMOSTAT_TX_IOCON,
        BoardConfig::THERMOSTAT_TX_PORT, BoardConfig::THERMOSTAT_TX_PIN,
        BoardConfig::THERMOSTAT_TX_INVERTED } };

    ManchesterLine boiler_{ {
        BoardConfig::BOILER_SCT,
        &LPC_IOCON->PIO2_15, BoardConfig::BOILER_RX_IOCON, BoardConfig::BOILER_RX_INVERTED,
        &LPC_IOCON->PIO0_14, BoardConfig::BOILER_TX_IOCON,
        BoardConfig::BOILER_TX_PORT, BoardConfig::BOILER_TX_PIN,
        BoardConfig::BOILER_TX_INVERTED } };

    // On from boot: an OpenTherm master draws its line power from the slave side, so
    // with this off the thermostat cannot send a single frame.
    LpcGpioOutput thermostatPower_{
        LPC_IOCON->PIO1_21, BoardConfig::THERMOSTAT_POWER_IOCON,
        BoardConfig::THERMOSTAT_POWER_PORT, BoardConfig::THERMOSTAT_POWER_PIN, true, true };

    LpcWatchdog watchdog_;
};
