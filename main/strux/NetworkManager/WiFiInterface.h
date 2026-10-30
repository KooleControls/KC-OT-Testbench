#pragma once

#include "NetworkInterface.h"
#include "esp_wifi.h"

class WiFiInterface final : public NetworkInterface {
    static constexpr const char* TAG = "WiFiInterface";

public:
    void Init();
    void SetHostname(const char* hostname);

    /// Bring the station up on this network from whatever state the radio is in.
    void ConnectSta(const char* ssid, const char* password);

    /// Try the network already loaded in the driver again, unchanged. Valid only while
    /// the station is up, which is what makes it cheaper than ConnectSta: stopping the
    /// station raises a disconnect of its own that a caller then has to tell apart from
    /// a real failure, and starting it again re-runs PHY init for nothing.
    void ReconnectSta();

    void StartAP(const char* ssid, const char* password, uint8_t channel = 1, uint8_t maxConnections = 4);
    void Stop();

    bool IsAP() const { return isAP_; }

    /// At or below this, esp_wifi is not reporting a measurement: -128 is its filler
    /// for "nothing to measure", and a reader who takes it for a reading concludes the
    /// opposite of the truth (a dead radio looks like a distant AP). No station
    /// associates near -100 anyway, so the threshold costs no real reading.
    static constexpr int8_t RssiUnknown = -100;

    /// Signal strength of the AP this station is associated with, in dBm (negative;
    /// around -50 is strong, -80 is marginal). False in AP mode, while not associated,
    /// or when the driver has no measurement - there is no number then, and reporting
    /// one would read as a real reading rather than as "unknown".
    bool GetRssi(int8_t& out) const;

    /// The name of an auth mode, for a log line or a reply. ASCII, static storage.
    static const char* AuthModeName(wifi_auth_mode_t mode);

    struct ScanResult {
        char ssid[33];
        int8_t rssi;
        uint8_t channel;
        /// True for anything but an open network - the padlock the settings UI draws.
        bool secure;
        /// The mode itself, because one bit cannot tell a refusal from a distance: an
        /// AP in WPA2/WPA3 transition mode refuses a station without PMF during auth,
        /// at full signal, as reason 2 (auth timeout), which in the log is exactly what
        /// being out of range looks like. Carrying the mode settles that by
        /// measurement instead of by reasoning.
        wifi_auth_mode_t authmode;
    };

    /// Scan for WiFi networks. Returns the number of results written.
    int Scan(ScanResult* out, int maxResults);

    const char* getName() const override { return "wifi"; }
    void SetEventHandler(NetworkEventHandler handler) override;

private:
    NetworkEventHandler eventHandler_;
    esp_netif_t* staNetif_ = nullptr;
    esp_netif_t* apNetif_ = nullptr;
    bool isAP_ = false;

    /// Loads a network into the driver. Shared by ConnectSta and anything else that
    /// needs the credentials in place without touching the radio's mode.
    void ApplyStaConfig(const char* ssid, const char* password);

    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    void OnWifiEvent(esp_event_base_t event_base, int32_t event_id, void* event_data);
    void RaiseEvent(NetworkEventType type, uint8_t reason = 0);
};
