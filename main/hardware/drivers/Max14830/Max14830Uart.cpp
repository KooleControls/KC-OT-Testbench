#include "Max14830Uart.h"
#include "Max14830.h"
#include "ContextLock.h"
#include "esp_log.h"

bool Max14830Uart::Configure(uint32_t baud, bool cts, bool rs485)
{
    if (chip_ == nullptr || !chip_->IsReady())
        return false;

    if (rx_ == nullptr)
    {
        rx_ = xStreamBufferCreate(RX_BUFFER, 1);
        tx_ = xStreamBufferCreate(TX_BUFFER, 1);
        if (rx_ == nullptr || tx_ == nullptr)
        {
            ESP_LOGE(TAG, "Port %u: no memory for buffers", port_);
            return false;
        }
    }

    return chip_->ConfigurePort(port_, baud, cts, rs485);
}

size_t Max14830Uart::write(const void* data, size_t size, TickType_t timeout)
{
    if (!IsConfigured())
        return 0;

    LOCK(txMutex_);
    auto* p = static_cast<const uint8_t*>(data);

    // What fits now, then a kick so the service task starts emptying the buffer, then
    // the rest — which waits for exactly that emptying.
    size_t sent = xStreamBufferSend(tx_, p, size, 0);
    chip_->KickTx();
    if (sent < size && timeout > 0)
    {
        sent += xStreamBufferSend(tx_, p + sent, size - sent, timeout);
        chip_->KickTx();
    }
    return sent;
}

size_t Max14830Uart::read(void* buffer, size_t size, TickType_t timeout)
{
    if (!IsConfigured())
        return 0;
    return xStreamBufferReceive(rx_, buffer, size, timeout);
}

size_t Max14830Uart::available() const
{
    return IsConfigured() ? xStreamBufferBytesAvailable(rx_) : 0;
}

void Max14830Uart::SetBaudRate(uint32_t baud)
{
    if (chip_ != nullptr)
        chip_->SetBaud(port_, baud);
}

void Max14830Uart::PushRx(const uint8_t* data, size_t len)
{
    const size_t n = xStreamBufferSend(rx_, data, len, 0);
    if (n < len)
        ESP_LOGW(TAG, "Port %u: RX buffer full, dropped %u bytes", port_, (unsigned)(len - n));
}

size_t Max14830Uart::PopTx(uint8_t* data, size_t maxLen)
{
    return xStreamBufferReceive(tx_, data, maxLen, 0);
}
