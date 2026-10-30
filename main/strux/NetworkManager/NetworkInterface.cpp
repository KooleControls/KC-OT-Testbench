#include "NetworkInterface.h"

#include <string.h>


void NetworkInterface::InitBase(esp_netif_t *netif)
{
    netif_ = netif;
    dnsConfig_.Init(netif);
}

NetworkStatus NetworkInterface::getStatus() const
{
    assert(netif_ != nullptr);

    NetworkStatus status{};

    esp_netif_ip_info_t ipInfo{};
    if (esp_netif_get_ip_info(netif_, &ipInfo) == ESP_OK)
    {
        status.ipv4 = ipInfo;
        status.has_ipv4 = (ipInfo.ip.addr != 0);
    }

    uint8_t mac[NetworkStatus::MacLength] = {};
    if (esp_netif_get_mac(netif_, mac) == ESP_OK)
    {
        memcpy(status.mac, mac, sizeof(status.mac));
    }

    status.link_up = status.has_ipv4;

    return status;
}

void NetworkInterface::setStaticIpv4(const esp_netif_ip_info_t& ipv4)
{
    assert(netif_ != nullptr);

    esp_err_t err = esp_netif_dhcpc_stop(netif_);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif_, &ipv4));
}

void NetworkInterface::enableDhcpIpv4()
{
    assert(netif_ != nullptr);

    esp_err_t err = esp_netif_dhcpc_start(netif_);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    {
        ESP_ERROR_CHECK(err);
    }
}

DnsConfiguration& NetworkInterface::dns()
{
    return dnsConfig_;
}

const DnsConfiguration& NetworkInterface::dns() const
{
    return dnsConfig_;
}
