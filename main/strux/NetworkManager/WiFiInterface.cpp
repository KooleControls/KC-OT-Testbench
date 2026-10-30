#include "WiFiInterface.h"

#include <assert.h>
#include <cstdio>
#include <cstring>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"

static WiFiInterface* s_instance = nullptr;

void WiFiInterface::Init()
{
    s_instance = this;

    staNetif_ = esp_netif_create_default_wifi_sta();
    assert(staNetif_ != nullptr);

    apNetif_ = esp_netif_create_default_wifi_ap();
    assert(apNetif_ != nullptr);

    // Default to STA netif for base class operations
    InitBase(staNetif_);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, this, nullptr));

    // No modem sleep. esp_wifi defaults to WIFI_PS_MIN_MODEM, where the station only
    // wakes for DTIM beacons — so every TCP round trip waits out a beacon interval,
    // and serving a 477-byte index.html measured 13.5 s. A Strux device exists to
    // answer a browser off a mains supply; latency is the resource worth spending
    // here, not milliamps. A battery-powered fork is the one that should change this.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void WiFiInterface::SetHostname(const char* hostname)
{
    esp_netif_set_hostname(staNetif_, hostname);
    esp_netif_set_hostname(apNetif_, hostname);
}

void WiFiInterface::ConnectSta(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "Connecting to '%s'", ssid);

    isAP_ = false;
    netif_ = staNetif_;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ApplyStaConfig(ssid, password);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
}

void WiFiInterface::ReconnectSta()
{
    // No set_config: the credentials and scan policy are already in the driver, and
    // re-connecting is the pattern esp_wifi is built around.
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "reconnect refused: %s", esp_err_to_name(err));
}

void WiFiInterface::ApplyStaConfig(const char* ssid, const char* password)
{
    wifi_config_t config = {};
    // strncpy, NOT snprintf, and deliberately so: these are esp_wifi's fixed-width
    // fields, not C strings. A 32-character SSID legitimately fills ssid[32] with no
    // terminator, so snprintf would silently drop its last character. Do not
    // "modernize" these four calls — see the note in NetworkManager.
    strncpy((char*)config.sta.ssid, ssid, sizeof(config.sta.ssid) - 1);
    strncpy((char*)config.sta.password, password, sizeof(config.sta.password) - 1);

    // Scan every channel and take the strongest, rather than the default fast scan's
    // "first beacon that answers". One SSID is routinely more than one radio — a
    // repeater or extender rebroadcasting the network is the ordinary case, not the
    // exotic one — and the first of those heard is not necessarily one that will
    // complete an association. The cost is a second or so of scan per attempt, which
    // is inside the attempt timeout and cheap against a round that fails.
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    // Advertise PMF support. `config = {}` zeroes pmf_cfg, and a station that claims
    // no Protected Management Frames is refused outright by a WPA3 access point and
    // by the WPA2/WPA3 transition mode most modern routers and extenders ship with.
    // The refusal arrives as an auth timeout (reason 2) at full signal strength,
    // which reads as a range or antenna problem and is neither.
    //
    // capable, not required: an AP that does not offer PMF must still be joinable,
    // and plenty of WPA2-only hardware does not.
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
}

void WiFiInterface::StartAP(const char* ssid, const char* password, uint8_t channel, uint8_t maxConnections)
{
    ESP_LOGI(TAG, "Starting AP '%s'", ssid);

    isAP_ = true;
    netif_ = apNetif_;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t config = {};
    // Fixed-width esp_wifi fields — see the note in Connect().
    strncpy((char*)config.ap.ssid, ssid, sizeof(config.ap.ssid) - 1);
    config.ap.ssid_len = strlen(ssid);
    strncpy((char*)config.ap.password, password, sizeof(config.ap.password) - 1);
    config.ap.channel = channel;
    config.ap.max_connection = maxConnections;
    config.ap.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void WiFiInterface::Stop()
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}

bool WiFiInterface::GetRssi(int8_t& out) const
{
    if (isAP_) return false;

    // Fails when the station is not associated, which is exactly when there is no
    // signal strength to report — so the error is the answer, not a problem.
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (ap.rssi <= RssiUnknown) return false;

    out = ap.rssi;
    return true;
}

const char* WiFiInterface::AuthModeName(wifi_auth_mode_t mode)
{
    switch (mode)
    {
        case WIFI_AUTH_OPEN:                  return "open";
        case WIFI_AUTH_WEP:                   return "WEP";
        case WIFI_AUTH_WPA_PSK:               return "WPA";
        case WIFI_AUTH_WPA2_PSK:              return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:          return "WPA/WPA2";
        case WIFI_AUTH_ENTERPRISE:            return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK:              return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:         return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:              return "WAPI";
        case WIFI_AUTH_OWE:                   return "OWE";
        case WIFI_AUTH_WPA3_ENT_192:          return "WPA3-ENT-192";
        case WIFI_AUTH_DPP:                   return "DPP";
        case WIFI_AUTH_WPA3_ENTERPRISE:       return "WPA3-ENT";
        case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:  return "WPA2/WPA3-ENT";
        case WIFI_AUTH_WPA_ENTERPRISE:        return "WPA-ENT";
        default:                              return "unknown";
    }
}

int WiFiInterface::Scan(ScanResult* out, int maxResults)
{
    // Switch to APSTA if needed so STA scan works while AP is running
    wifi_mode_t prevMode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&prevMode);
    if (prevMode == WIFI_MODE_AP)
    {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scanConfig = {};
    scanConfig.show_hidden = true;
    scanConfig.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scanConfig.scan_time.active.min = 100;
    scanConfig.scan_time.active.max = 300;

    ESP_LOGI(TAG, "Starting WiFi scan...");
    esp_err_t err = esp_wifi_scan_start(&scanConfig, true);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        if (prevMode == WIFI_MODE_AP) esp_wifi_set_mode(prevMode);
        return 0;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    ESP_LOGI(TAG, "Scan found %d networks", found);

    // One record at a time, so nothing here is sized by how many networks happen to
    // be in the air: esp_wifi_scan_get_ap_record hands over the next record and frees
    // it, where esp_wifi_scan_get_ap_records wants an array as long as the result set
    // — which is why this used to allocate one per scan.
    int count = 0;
    wifi_ap_record_t record;
    while (count < maxResults && esp_wifi_scan_get_ap_record(&record) == ESP_OK)
    {
        snprintf(out[count].ssid, sizeof(out[count].ssid), "%s",
                 reinterpret_cast<const char*>(record.ssid));
        out[count].rssi = record.rssi;
        out[count].channel = record.primary;
        out[count].secure = record.authmode != WIFI_AUTH_OPEN;
        out[count].authmode = record.authmode;

        // One line per network, with the auth mode spelled out: a join that fails at
        // full signal is a mode mismatch, and this is the line that says so. The
        // signal reads "--" rather than a filler value when there is nothing to
        // measure, because a wrong number here is worse than no number.
        char signal[12];
        if (record.rssi > RssiUnknown)
            snprintf(signal, sizeof(signal), "%d dBm", record.rssi);
        else
            snprintf(signal, sizeof(signal), "-- dBm");
        ESP_LOGI(TAG, "SCAN: '%s' ch%u %s auth=%s", out[count].ssid,
                 static_cast<unsigned>(record.primary), signal,
                 AuthModeName(record.authmode));

        count++;
    }

    // Stopping at maxResults leaves the rest of the driver's list allocated; this is
    // what releases it. Harmless when the loop already drained the list.
    esp_wifi_clear_ap_list();

    if (prevMode == WIFI_MODE_AP)
    {
        esp_wifi_set_mode(prevMode);
    }

    return count;
}

void WiFiInterface::SetEventHandler(NetworkEventHandler handler)
{
    eventHandler_ = std::move(handler);
}

void WiFiInterface::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto* self = static_cast<WiFiInterface*>(arg);
    self->OnWifiEvent(event_base, event_id, event_data);
}

void WiFiInterface::OnWifiEvent(esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected");
            RaiseEvent(NetworkEventType::LinkUp);
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            // The reason code travels up because the manager acts on it, not just
            // logs it: a LinkDown carrying no reason is the AP netif stopping or this
            // interface's own disconnect echoing back, and retrying on either would
            // burn an attempt that was never made. See NetworkManager's LinkDown arm.
            auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            ESP_LOGW(TAG, "STA disconnected (reason %d)", event->reason);
            RaiseEvent(NetworkEventType::LinkDown, event->reason);
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started");
            RaiseEvent(NetworkEventType::LinkUp);
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGW(TAG, "AP stopped");
            RaiseEvent(NetworkEventType::LinkDown);
            break;

        case WIFI_EVENT_AP_STACONNECTED:
        {
            auto* event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
            ESP_LOGI(TAG, "Station " MACSTR " joined (AID=%d)",
                     MAC2STR(event->mac), event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            auto* event = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
            ESP_LOGI(TAG, "Station " MACSTR " left (AID=%d)",
                     MAC2STR(event->mac), event->aid);
            break;
        }

        default:
            break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        switch (event_id)
        {
        case IP_EVENT_STA_GOT_IP:
        {
            auto* event = static_cast<ip_event_got_ip_t*>(event_data);
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            RaiseEvent(NetworkEventType::Ipv4Acquired);
            break;
        }

        case IP_EVENT_STA_LOST_IP:
            ESP_LOGW(TAG, "Lost IP");
            RaiseEvent(NetworkEventType::Ipv4Lost);
            break;

        case IP_EVENT_AP_STAIPASSIGNED:
        {
            auto* event = static_cast<ip_event_ap_staipassigned_t*>(event_data);
            ESP_LOGI(TAG, "Assigned IP to station: " IPSTR, IP2STR(&event->ip));
            break;
        }

        default:
            break;
        }
    }
}

void WiFiInterface::RaiseEvent(NetworkEventType type, uint8_t reason)
{
    if (!eventHandler_)
        return;

    NetworkEvent event{};
    event.type = type;
    event.status = getStatus();
    event.reason = reason;
    eventHandler_(event);
}
