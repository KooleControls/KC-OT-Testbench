#pragma once

#include <stdint.h>

#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"

struct DnsServers {
    bool has_main = false;
    bool has_backup = false;
    bool has_fallback = false;

    esp_netif_dns_info_t main = {};
    esp_netif_dns_info_t backup = {};
    esp_netif_dns_info_t fallback = {};
};

struct NetworkStatus {
    static constexpr uint8_t MacLength = 6;

    bool link_up = false;
    bool has_ipv4 = false;

    esp_netif_ip_info_t ipv4 = {};
    uint8_t mac[MacLength] = {};
};

enum class NetworkEventType
{
    LinkUp,
    LinkDown,
    Ipv4Acquired,
    Ipv4Lost
};

struct NetworkEvent
{
    NetworkEventType type;
    NetworkStatus status;

    /// Why the link went down, as a wifi_err_reason_t. Only a LinkDown carries one,
    /// and only one the radio reported: reason 0 means the event came from somewhere
    /// with nothing to say (the AP stopping), which is what tells a manager that this
    /// LinkDown is its own teardown rather than an association that failed.
    uint8_t reason = 0;
};
