#include "DnsConfiguration.h"

#include "esp_err.h"

void DnsConfiguration::Init(esp_netif_t* netif)
{
    assert(netif != nullptr);
    netif_ = netif;
}

DnsServers DnsConfiguration::getStatus() const
{
    assert(netif_ != nullptr);

    DnsServers result{};

    if (esp_netif_get_dns_info(netif_, ESP_NETIF_DNS_MAIN, &result.main) == ESP_OK)
    {
        result.has_main = (result.main.ip.type != ESP_IPADDR_TYPE_ANY);
    }

    if (esp_netif_get_dns_info(netif_, ESP_NETIF_DNS_BACKUP, &result.backup) == ESP_OK)
    {
        result.has_backup = (result.backup.ip.type != ESP_IPADDR_TYPE_ANY);
    }

    if (esp_netif_get_dns_info(netif_, ESP_NETIF_DNS_FALLBACK, &result.fallback) == ESP_OK)
    {
        result.has_fallback = (result.fallback.ip.type != ESP_IPADDR_TYPE_ANY);
    }

    return result;
}

void DnsConfiguration::setMain(const esp_netif_dns_info_t& dns)
{
    assert(netif_ != nullptr);
    esp_netif_dns_info_t dnsCopy = dns;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif_, ESP_NETIF_DNS_MAIN, &dnsCopy));
}

void DnsConfiguration::setBackup(const esp_netif_dns_info_t& dns)
{
    assert(netif_ != nullptr);
    esp_netif_dns_info_t dnsCopy = dns;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif_, ESP_NETIF_DNS_BACKUP, &dnsCopy));
}

void DnsConfiguration::setFallback(const esp_netif_dns_info_t& dns)
{
    assert(netif_ != nullptr);
    esp_netif_dns_info_t dnsCopy = dns;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif_, ESP_NETIF_DNS_FALLBACK, &dnsCopy));
}
