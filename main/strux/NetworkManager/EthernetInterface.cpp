#include "EthernetInterface.h"
#include "esp_eth_netif_glue.h"
#include "esp_log.h"
#include "esp_netif.h"

bool EthernetInterface::Init(esp_eth_handle_t handle, const char* hostname)
{
    esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&netifConfig);
    if (netif == nullptr)
    {
        ESP_LOGE(TAG, "netif create failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(handle)));
    InitBase(netif);
    handle_ = handle;

    if (hostname != nullptr && hostname[0] != '\0')
        esp_netif_set_hostname(netif, hostname);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &EthernetInterface::StaticEventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &EthernetInterface::StaticEventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_LOST_IP, &EthernetInterface::StaticEventHandler, this, nullptr));

    ESP_ERROR_CHECK(esp_eth_start(handle));
    ESP_LOGI(TAG, "Started");
    return true;
}

NetworkStatus EthernetInterface::getStatus() const
{
    if (netif_ == nullptr)
        return {};

    NetworkStatus status = NetworkInterface::getStatus();
    status.link_up = linkUp_;
    status.has_ipv4 = hasIp_;
    return status;
}

void EthernetInterface::StaticEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    auto* self = static_cast<EthernetInterface*>(arg);

    if (base == ETH_EVENT)
    {
        // Every Ethernet driver posts here; only ours is this interface's business.
        if (*static_cast<esp_eth_handle_t*>(data) != self->handle_)
            return;

        if (id == ETHERNET_EVENT_CONNECTED)
            self->HandleConnected();
        else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP)
            self->HandleDisconnected();
    }
    else if (base == IP_EVENT)
    {
        if (id == IP_EVENT_ETH_GOT_IP)
        {
            auto* event = static_cast<ip_event_got_ip_t*>(data);
            if (event->esp_netif == self->netif_)
                self->HandleGotIp(*event);
        }
        else if (id == IP_EVENT_ETH_LOST_IP)
        {
            self->HandleLostIp();
        }
    }
}

void EthernetInterface::HandleConnected()
{
    linkUp_ = true;
    ESP_LOGI(TAG, "Link up");
    RaiseEvent(NetworkEventType::LinkUp);

    // With a static address IP_EVENT_ETH_GOT_IP never fires, so the link coming up
    // is the moment the address becomes usable.
    esp_netif_dhcp_status_t dhcp;
    esp_netif_ip_info_t ip{};
    if (esp_netif_dhcpc_get_status(netif_, &dhcp) == ESP_OK && dhcp == ESP_NETIF_DHCP_STOPPED &&
        esp_netif_get_ip_info(netif_, &ip) == ESP_OK && ip.ip.addr != 0)
    {
        ip_event_got_ip_t event{};
        event.esp_netif = netif_;
        event.ip_info = ip;
        HandleGotIp(event);
    }
}

void EthernetInterface::HandleDisconnected()
{
    const bool wasUp = linkUp_.exchange(false);
    hasIp_ = false;
    if (wasUp)
    {
        ESP_LOGW(TAG, "Link down");
        RaiseEvent(NetworkEventType::LinkDown);
    }
}

void EthernetInterface::HandleGotIp(const ip_event_got_ip_t& event)
{
    hasIp_ = true;
    ESP_LOGI(TAG, "Got IP " IPSTR " gw " IPSTR, IP2STR(&event.ip_info.ip), IP2STR(&event.ip_info.gw));
    RaiseEvent(NetworkEventType::Ipv4Acquired);
}

void EthernetInterface::HandleLostIp()
{
    hasIp_ = false;
    ESP_LOGW(TAG, "Lost IP");
    RaiseEvent(NetworkEventType::Ipv4Lost);
}

void EthernetInterface::RaiseEvent(NetworkEventType type)
{
    if (!eventHandler_)
        return;

    NetworkEvent event{};
    event.type = type;
    event.status = getStatus();
    eventHandler_(event);
}
