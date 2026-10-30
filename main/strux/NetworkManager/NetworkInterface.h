#pragma once

#include <assert.h>
#include <functional>

#include "esp_netif.h"
#include "NetworkTypes.h"
#include "DnsConfiguration.h"

using NetworkEventHandler = std::function<void(const NetworkEvent&)>;

class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;

    virtual NetworkStatus getStatus() const;
    void setStaticIpv4(const esp_netif_ip_info_t& ipv4);
    void enableDhcpIpv4();

    DnsConfiguration& dns();
    const DnsConfiguration& dns() const;

    esp_netif_t* getNetif() const { return netif_; }

    virtual const char* getName() const = 0;

    virtual void SetEventHandler(NetworkEventHandler handler) = 0;

protected:
    void InitBase(esp_netif_t* netif);

    esp_netif_t* netif_ = nullptr;
    DnsConfiguration dnsConfig_;
};
