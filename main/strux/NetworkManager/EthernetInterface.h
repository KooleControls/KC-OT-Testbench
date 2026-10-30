#pragma once

#include <atomic>

#include "NetworkInterface.h"
#include "esp_eth_driver.h"
#include "esp_event.h"

// ──────────────────────────────────────────────────────────────
// The network side of an Ethernet port: netif, DHCP, link and address events.
//
// The hardware side is the board's. It installs a driver for whatever MAC and PHY it
// has and hands over the esp_eth_handle_t — through the application, since Strux
// does not see the board — and this attaches to it and starts it. Nothing here knows
// which MAC, which PHY or which pins.
// ──────────────────────────────────────────────────────────────
class EthernetInterface final : public NetworkInterface
{
    static constexpr const char* TAG = "EthernetInterface";

public:
    EthernetInterface() = default;

    /// Attach to an installed driver and start it. False if the netif could not be
    /// created; the interface then stays down.
    bool Init(esp_eth_handle_t handle, const char* hostname);

    bool IsAttached() const { return handle_ != nullptr; }

    NetworkStatus getStatus() const override;

    const char* getName() const override { return "ethernet"; }
    void SetEventHandler(NetworkEventHandler handler) override { eventHandler_ = std::move(handler); }

private:
    static void StaticEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    void HandleConnected();
    void HandleDisconnected();
    void HandleGotIp(const ip_event_got_ip_t& event);
    void HandleLostIp();
    void RaiseEvent(NetworkEventType type);

    esp_eth_handle_t handle_ = nullptr;
    std::atomic<bool> linkUp_{false};
    std::atomic<bool> hasIp_{false};
    NetworkEventHandler eventHandler_;
};
