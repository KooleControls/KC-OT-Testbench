#pragma once

#include <assert.h>

#include "esp_netif.h"
#include "NetworkTypes.h"

class DnsConfiguration {
public:
    DnsConfiguration() = default;

    void Init(esp_netif_t* netif);

    DnsServers getStatus() const;

    void setMain(const esp_netif_dns_info_t& dns);
    void setBackup(const esp_netif_dns_info_t& dns);
    void setFallback(const esp_netif_dns_info_t& dns);

private:
    esp_netif_t* netif_ = nullptr;
};
