#pragma once

#include "interfaces/SerialPort.h"
#include "max310x.h"
#include "Mutex.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include <cstdint>

class Max14830;

// ──────────────────────────────────────────────────────────────
// One MAX14830 UART: a byte pipe with an RX and a TX buffer in front of the chip's
// 128-byte FIFOs. The chip's service task is the only writer of RX and the only
// reader of TX; read() has one consumer, write() may be called from several tasks
// and is serialised by txMutex_.
// ──────────────────────────────────────────────────────────────
class Max14830Uart final : public SerialPort
{
    static constexpr const char* TAG = "Max14830Uart";

public:
    static constexpr size_t RX_BUFFER = 512;
    static constexpr size_t TX_BUFFER = 512;

    Max14830Uart() = default;

    /// Bring the port up: 8N1 at `baud`, with automatic CTS flow control and/or RS485
    /// transceiver control (the chip drives RTS as the driver enable).
    bool Configure(uint32_t baud, bool cts = false, bool rs485 = false);

    // ── SerialPort ───────────────────────────────────────────
    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override;
    size_t read(void* buffer, size_t size, TickType_t timeout = portMAX_DELAY) override;
    size_t available() const override;
    void SetBaudRate(uint32_t baud) override;

private:
    friend class Max14830;

    Max14830* chip_ = nullptr;
    uint8_t port_ = 0;
    StreamBufferHandle_t rx_ = nullptr;
    StreamBufferHandle_t tx_ = nullptr;
    Mutex txMutex_;

    void Bind(Max14830& chip, uint8_t port) { chip_ = &chip; port_ = port; }
    bool IsConfigured() const { return rx_ != nullptr; }

    // ── Service-task side ────────────────────────────────────
    void PushRx(const uint8_t* data, size_t len);
    size_t PopTx(uint8_t* data, size_t maxLen);
};
