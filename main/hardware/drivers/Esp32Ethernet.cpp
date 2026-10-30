#include "Esp32Ethernet.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_log.h"

bool Esp32Ethernet::Init()
{
    eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
    phyConfig.phy_addr = config_.phyAddress;
    phyConfig.reset_gpio_num = config_.phyReset;

    eth_esp32_emac_config_t emacConfig = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emacConfig.smi_gpio.mdc_num = config_.mdc;
    emacConfig.smi_gpio.mdio_num = config_.mdio;
    emacConfig.interface = EMAC_DATA_INTERFACE_RMII;
    emacConfig.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emacConfig.clock_config.rmii.clock_gpio = config_.refClock;

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emacConfig, &macConfig);
    if (mac == nullptr)
    {
        ESP_LOGE(TAG, "MAC create failed");
        return false;
    }

    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phyConfig);
    if (phy == nullptr)
    {
        ESP_LOGE(TAG, "PHY create failed");
        mac->del(mac);
        return false;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, &handle_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Driver install failed: %s", esp_err_to_name(err));
        phy->del(phy);
        mac->del(mac);
        handle_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Initialized (PHY address %d)", config_.phyAddress);
    return true;
}
