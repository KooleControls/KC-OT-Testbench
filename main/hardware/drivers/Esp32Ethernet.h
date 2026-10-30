#pragma once

#include "esp_eth_driver.h"
#include "driver/gpio.h"
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The ESP32's internal Ethernet MAC with an RMII PHY. The hardware half only: MAC,
// PHY and driver are installed here, and the result is an esp_eth_handle_t — ESP-IDF's
// own driver seam. Starting it, the netif, DHCP and the events are the network stack's
// (Strux's NetworkManager), which is handed the handle by the application.
//
// The RMII data pins are fixed by the chip (TXD0 19, TXD1 22, TX_EN 21, RXD0 25,
// RXD1 26, CRS_DV 27); the board chooses the management pins, the PHY address, the
// PHY reset and where the 50 MHz reference clock comes from.
// ──────────────────────────────────────────────────────────────

struct Esp32EthernetConfig
{
    gpio_num_t mdc;
    gpio_num_t mdio;
    int phyAddress;
    gpio_num_t phyReset;     // GPIO_NUM_NC when the PHY's reset is not wired
    gpio_num_t refClock;     // the 50 MHz RMII clock input (GPIO0 on the ESP32)
};

class Esp32Ethernet
{
    static constexpr const char* TAG = "Esp32Ethernet";

public:
    explicit Esp32Ethernet(const Esp32EthernetConfig& config) : config_(config) {}

    Esp32Ethernet(const Esp32Ethernet&) = delete;
    Esp32Ethernet& operator=(const Esp32Ethernet&) = delete;

    /// Install the driver. False when the MAC or PHY cannot be created; Handle() then
    /// stays null.
    bool Init();

    /// The installed driver, not yet started. Null until Init() succeeded.
    esp_eth_handle_t Handle() const { return handle_; }

private:
    Esp32EthernetConfig config_;
    esp_eth_handle_t handle_ = nullptr;
};
