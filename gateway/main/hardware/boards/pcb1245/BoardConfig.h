#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// BoardContext configuration — PCB1245, the KC1245 gateway (ESP32, 16 MB flash,
// PSRAM). Pin assignments and constants for this board only, taken from the KC1245
// firmware.
// ──────────────────────────────────────────────────────────────

namespace BoardConfig
{
    // ── MAX14830: 4 UARTs + 16 GPIOs on SPI ──────────────────
    static constexpr spi_host_device_t MAX_SPI_HOST = SPI2_HOST;
    static constexpr gpio_num_t MAX_MISO = GPIO_NUM_35;
    static constexpr gpio_num_t MAX_MOSI = GPIO_NUM_4;
    static constexpr gpio_num_t MAX_SCLK = GPIO_NUM_33;
    static constexpr gpio_num_t MAX_CS   = GPIO_NUM_32;
    static constexpr gpio_num_t MAX_IRQ  = GPIO_NUM_39;
    static constexpr uint32_t MAX_SPI_HZ = 20 * 1000 * 1000;
    static constexpr uint32_t MAX_XTAL_HZ = 4 * 1000 * 1000;   // crystal on XIN/XOUT

    // MAX14830 UART ports
    static constexpr uint8_t UART_XBEE    = 0;   // 57600, CTS flow control
    static constexpr uint8_t UART_SERVICE = 1;   // FTDI, 115200
    static constexpr uint8_t UART_PCB1246 = 2;   // the OpenTherm plug-in board, 57600
    static constexpr uint8_t UART_RS485   = 3;   // RS485 transceiver control; Modbus RTU

    static constexpr uint32_t XBEE_BAUD    = 57600;
    static constexpr uint32_t SERVICE_BAUD = 115200;
    static constexpr uint32_t PCB1246_BAUD = 57600;
    static constexpr uint32_t RS485_BAUD   = 9600;     // a CoolPlug's default; the rs485.baud setting after boot

    // MAX14830 GPIOs (0-15). Polarity as the KC1245 drives them.
    static constexpr uint8_t PIN_XBEE_RESET  = 0;    // out, active low
    static constexpr uint8_t PIN_SD_DETECT   = 1;    // in,  active low
    static constexpr uint8_t PIN_RESET_BTN   = 2;    // in,  active low
    static constexpr uint8_t PIN_LED         = 3;    // out, active low (heartbeat)
    static constexpr uint8_t PIN_RELAY1      = 4;    // out, active high
    static constexpr uint8_t PIN_RELAY2      = 5;    // out, active high
    static constexpr uint8_t PIN_INPUT1      = 6;    // in,  active low
    static constexpr uint8_t PIN_INPUT2      = 7;    // in,  active low
    static constexpr uint8_t PIN_PCB1246_RST = 10;   // out, active high
    static constexpr uint8_t PIN_PCB1246_ISP = 11;   // out, active high
    // 8, 9, 12-15: expansion header, unused.

    // ── Ethernet: ESP32 EMAC + RMII PHY ──────────────────────
    static constexpr gpio_num_t ETH_MDC  = GPIO_NUM_23;
    static constexpr gpio_num_t ETH_MDIO = GPIO_NUM_18;
    static constexpr gpio_num_t ETH_REF_CLOCK = GPIO_NUM_0;   // 50 MHz from the PHY
    static constexpr gpio_num_t ETH_PHY_RESET = GPIO_NUM_NC;  // not wired
    static constexpr int ETH_PHY_ADDRESS = 0;
}
