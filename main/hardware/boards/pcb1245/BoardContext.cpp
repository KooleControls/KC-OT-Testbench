#include "BoardContext.h"
#include "esp_log.h"

void BoardContext::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    if (max_.Init())
    {
        led_.Init();
        relay1_.Init();
        relay2_.Init();
        input1_.Init();
        input2_.Init();
        resetButton_.Init();
        sdDetect_.Init();
        xbeeReset_.Init();
        pcb1246Reset_.Init();
        pcb1246Isp_.Init();

        max_.Uart(BoardConfig::UART_XBEE).Configure(BoardConfig::XBEE_BAUD, true, false);
        max_.Uart(BoardConfig::UART_SERVICE).Configure(BoardConfig::SERVICE_BAUD);
        max_.Uart(BoardConfig::UART_PCB1246).Configure(BoardConfig::PCB1246_BAUD);
        max_.Uart(BoardConfig::UART_RS485).Configure(BoardConfig::RS485_BAUD, false, true);
    }
    else
    {
        // Without the expander there is no LED, relay, input or UART; the board
        // still comes up so the device stays reachable over the network.
        ESP_LOGE(TAG, "MAX14830 did not come up");
    }

    ethernet_.Init();

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

Relay &BoardContext::GetRelay(RelayId id)
{
    switch (id)
    {
    case RelayId::Relay1: return relay1_;
    case RelayId::Relay2: return relay2_;
    }
    return relay1_;
}

DigitalInput &BoardContext::GetInput(InputId id)
{
    switch (id)
    {
    case InputId::Input1:       return input1_;
    case InputId::Input2:       return input2_;
    case InputId::ResetButton:  return resetButton_;
    case InputId::SdCardDetect: return sdDetect_;
    }
    return input1_;
}

SerialPort &BoardContext::GetSerialPort(SerialPortId id)
{
    switch (id)
    {
    case SerialPortId::XBee:    return max_.Uart(BoardConfig::UART_XBEE);
    case SerialPortId::Service: return max_.Uart(BoardConfig::UART_SERVICE);
    case SerialPortId::Rs485:   return max_.Uart(BoardConfig::UART_RS485);
    }
    return max_.Uart(BoardConfig::UART_SERVICE);
}
