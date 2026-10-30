#include "NetworkManager.h"
#include "SettingsManager.h"
#include "SystemManager.h"
#include "CommandManager.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "mdns.h"
#include <cstdio>
#include <cstring>

NetworkManager::NetworkManager(StruxProvider& strux)
    : strux_(strux)
{
}

CommandEntry NetworkManager::wifiScanCommand_{
    "wifi scan", &InvokeCommand<&NetworkManager::Cmd_WifiScan>,
    "Scan for WiFi networks in range and report each one's SSID and signal "
    "strength. Takes a few seconds and briefly disturbs the connection."
};

void NetworkManager::Init()
{
    auto initAttempt = initState.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }

    // Reduce noisy WiFi/LWIP init logs
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    // Before the rest of this Init, not at the end of it with the command table:
    // Setting::Get() aborts on a setting that was never registered, and from here on
    // this function reads its own settings (mDNS below, the credentials in the first
    // station round). Registering is what publishes them, so it has to come first.
    strux_.getSettingsManager().Register({ &wifiSsid_, &wifiPassword_, &mdnsEnabled_ });

    wifi_interface_.SetEventHandler([this](const NetworkEvent& e) { HandleNetworkEvent(e); });
    wifi_interface_.Init();

    // Set hostname from the device name so it shows in the router
    char deviceName[33] = {};
    strux_.getSystemManager().GetDeviceName(deviceName, sizeof(deviceName));
    wifi_interface_.SetHostname(deviceName);

    ComposeApSsid();

    // mDNS — <deviceName>.local
    if (mdnsEnabled_.Get())
    {
        ESP_ERROR_CHECK(mdns_init());
        mdns_hostname_set(deviceName);
        mdns_instance_name_set(deviceName);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    else
    {
        ESP_LOGI(TAG, "mDNS disabled - this device is reachable by address only");
    }

    // One timer drives the whole cycle — see OnCycleTimer.
    connectTimer_.Init("net_cycle", pdMS_TO_TICKS(StaConnectTimeoutMs), false);
    connectTimer_.SetHandler([this]() { OnCycleTimer(); });

    strux_.getCommandManager().Register(this, { &wifiScanCommand_ });

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");

    // Credentials are read by the station round rather than here, because the round
    // re-reads them every time and there is no first-time case left to special-case.
    BeginStaRound();
}

WiFiInterface& NetworkManager::wifi()
{
    WAIT_FOR_READY(initState);
    return wifi_interface_;
}

const WiFiInterface& NetworkManager::wifi() const
{
    WAIT_FOR_READY(initState);
    return wifi_interface_;
}

void NetworkManager::AttachEthernet(esp_eth_handle_t handle)
{
    WAIT_FOR_READY(initState);

    if (handle == nullptr)
    {
        ESP_LOGW(TAG, "No Ethernet driver to attach");
        return;
    }

    char deviceName[33] = {};
    strux_.getSystemManager().GetDeviceName(deviceName, sizeof(deviceName));

    ethernet_interface_.SetEventHandler([this](const NetworkEvent& e) { HandleEthernetEvent(e); });
    ethernet_interface_.Init(handle, deviceName);
}

void NetworkManager::HandleEthernetEvent(const NetworkEvent& event)
{
    switch (event.type)
    {
    case NetworkEventType::Ipv4Acquired:
        ethernetConnected_ = true;
        break;
    case NetworkEventType::Ipv4Lost:
    case NetworkEventType::LinkDown:
        ethernetConnected_ = false;
        break;
    case NetworkEventType::LinkUp:
        break;
    }
}

bool NetworkManager::GetIpv4(char* out, size_t len) const
{
    if (out == nullptr || len == 0)
        return false;

    out[0] = '\0';

    if (ethernet_interface_.IsAttached())
    {
        const NetworkStatus eth = ethernet_interface_.getStatus();
        if (eth.has_ipv4)
        {
            snprintf(out, len, IPSTR, IP2STR(&eth.ipv4.ip));
            return true;
        }
    }

    const NetworkStatus status = wifi_interface_.getStatus();
    if (!status.has_ipv4)
        return false;

    snprintf(out, len, IPSTR, IP2STR(&status.ipv4.ip));
    return true;
}

void NetworkManager::ComposeApSsid()
{
    // The SoftAP MAC, not the station's: it is the address this AP actually beacons
    // from, so it is the one a client sees beside the name it is being put in. The two
    // differ by one on an ESP32 — close enough to confuse, far enough to be the wrong
    // number in a bug report.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    char code[7];
    snprintf(code, sizeof(code), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    char deviceName[33] = {};
    strux_.getSystemManager().GetDeviceName(deviceName, sizeof(deviceName));

    // 32 is esp_wifi's ssid[32], and a full 32 characters is legal there — no
    // terminator to leave room for (see the strncpy note in WiFiInterface). What has
    // to fit around the name is "-AP-" plus six hex digits, so the name gets what is
    // left: a "%.*s" precision rather than a truncating write, because that bound is
    // one the compiler can see (it rejects the manual version under
    // -Werror=format-truncation, and it is right to — the reasoning was mine, not the
    // code's).
    const int nameRoom = 32 - static_cast<int>(strlen(ApSsidSuffix) + strlen(code));
    snprintf(apSsid_, sizeof(apSsid_), "%.*s%s%s",
             nameRoom, deviceName, ApSsidSuffix, code);
    ESP_LOGI(TAG, "Own AP name: '%s'", apSsid_);
}

void NetworkManager::OnCycleTimer()
{
    // An expiring AP window is the one tick that is not a failed attempt. On an
    // unprovisioned device it is not even that — see StartProvisioningAp, where the
    // same tick is how credentials that arrive through the AP get noticed.
    if (apWindowOpen_)
    {
        BeginStaRound();
        return;
    }

    if (staConnected_)
        return;

    // Associated, but no address yet. A slow DHCP is not a network refusing us, and
    // this tick must not treat it as one: esp_wifi refuses a reconnect on a connected
    // station ("sta is connected, disconnect before connecting to new ap"), so the
    // old timer's retry left this manager believing it had moved on while the link it
    // meant to abandon was the one that worked.
    //
    // Tear the station down and try again: the network that let us in is still the
    // best candidate. It counts as an attempt, so an AP that associates and never
    // hands out an address still reaches the AP window instead of looping here.
    if (staAssociated_.exchange(false))
    {
        staRoundAttempts_++;
        staOutageAttempts_++;
        ESP_LOGW(TAG, "'%s' associated but gave no address within %d ms; "
                      "restarting the station", staSsid_, StaDhcpTimeoutMs);

        if (staRoundAttempts_ >= StaAttemptsPerRound)
        {
            OpenApWindow();
            return;
        }

        AttemptStaConnect(true);
        return;
    }

    staRoundAttempts_++;
    staOutageAttempts_++;

    // Say why once per outage, then go quiet and count: this loop runs for the life
    // of the device, so one line per failure is one line forever. Ipv4Acquired
    // reports the total.
    //
    // "Why" is the radio's reason code, not this manager's guess at one. The
    // alternative was a line that read the same whether the password was wrong, the
    // network absent, or the AP merely too far to reach — three failures with three
    // different answers, and the log is all a remote device has.
    if (!staOutageLogged_.exchange(true))
    {
        const uint8_t reason = staLastReason_.load();
        if (reason != 0)
            ESP_LOGW(TAG, "'%s' not joined (reason %d); alternating %d attempts with "
                          "a %d min AP window for as long as this device is powered",
                     staSsid_, reason, StaAttemptsPerRound, ApWindowMs / 60000);
        else
            ESP_LOGW(TAG, "'%s' said nothing at all within %d ms, not even a refusal; "
                          "alternating %d attempts with a %d min AP window for as long "
                          "as this device is powered",
                     staSsid_, StaConnectTimeoutMs, StaAttemptsPerRound,
                     ApWindowMs / 60000);
    }

    if (staRoundAttempts_ >= StaAttemptsPerRound)
    {
        OpenApWindow();
        return;
    }

    // A flat cadence rather than a backoff: this is an association with a local
    // access point, not a dial-out to a server a retry loop could overwhelm. The
    // radio stays up across the attempt — see AttemptStaConnect.
    AttemptStaConnect(false);
}

void NetworkManager::BeginStaRound()
{
    // Re-read the credentials, so a network provisioned through the AP window is the
    // one this round tries. Nothing else re-reads them, and before the cycle existed
    // there was no second round to re-read them for.
    char previousSsid[sizeof(staSsid_)];
    char previousPassword[sizeof(staPassword_)];
    memcpy(previousSsid, staSsid_, sizeof(previousSsid));
    memcpy(previousPassword, staPassword_, sizeof(previousPassword));

    // Zeroed before every read, so the comparison below is of credentials rather than
    // of whatever a longer SSID left behind the terminator of a shorter one.
    memset(staSsid_, 0, sizeof(staSsid_));
    memset(staPassword_, 0, sizeof(staPassword_));
    wifiSsid_.Get(staSsid_, sizeof(staSsid_));
    wifiPassword_.Get(staPassword_, sizeof(staPassword_));

    if (memcmp(previousSsid, staSsid_, sizeof(previousSsid)) != 0 ||
        memcmp(previousPassword, staPassword_, sizeof(previousPassword)) != 0)
    {
        // New credentials are a new problem. Counting their failures into the outage
        // the old ones caused would hide the one line worth reading — that what was
        // just entered does not work either.
        staOutageAttempts_ = 0;
        staOutageLogged_ = false;
        staLastReason_ = 0;
        staOutageStartTick_ = xTaskGetTickCount();
    }

    if (staSsid_[0] == '\0')
    {
        StartProvisioningAp();
        return;
    }

    if (apWindowOpen_)
        ESP_LOGI(TAG, "AP window over, trying '%s' again", staSsid_);

    staRoundAttempts_ = 0;
    AttemptStaConnect(true);
}

void NetworkManager::AttemptStaConnect(bool freshRadio)
{
    if (staSsid_[0] == '\0')
    {
        StartProvisioningAp();
        return;
    }

    // Only the first attempt of an outage announces itself; the rest are the cycle,
    // which OnCycleTimer has already accounted for.
    if (staOutageAttempts_ == 0)
        ESP_LOGI(TAG, "Attempting STA connection to '%s'", staSsid_);

    apWindowOpen_ = false;
    staConnected_ = false;
    staAssociated_ = false;

    if (freshRadio)
    {
        wifi_interface_.Stop();
        wifi_interface_.ConnectSta(staSsid_, staPassword_);
    }
    else
    {
        wifi_interface_.ReconnectSta();
    }

    connectTimer_.SetPeriod(pdMS_TO_TICKS(StaConnectTimeoutMs));
    connectTimer_.Start();
}

void NetworkManager::OpenApWindow()
{
    // Set before the station goes down, not after: Stop() raises a disconnect that
    // arrives on the event task whenever it arrives, and this flag is the only thing
    // that stops it being read as one more failed attempt.
    apWindowOpen_ = true;

    connectTimer_.Stop();
    wifi_interface_.Stop();

    ESP_LOGW(TAG, "'%s' unreachable after %d attempts, opening '%s' for %d min",
             staSsid_, staRoundAttempts_.load(), apSsid_, ApWindowMs / 60000);
    wifi_interface_.StartAP(apSsid_, DefaultApPassword);

    connectTimer_.SetPeriod(pdMS_TO_TICKS(ApWindowMs));
    connectTimer_.Start();
}

void NetworkManager::StartProvisioningAp()
{
    // There is nothing for a station round to try, so this window ends only when
    // BeginStaRound re-reads the settings and finds a network in them. That is what
    // makes provisioning through this AP take effect on its own; the poll is short
    // because the person who just pressed Save is standing there.
    //
    // The radio is left alone when the AP is already up: that same person may be
    // part-way through typing, and bouncing it under them loses the page.
    apWindowOpen_ = true;

    if (!wifi_interface_.IsAP())
    {
        connectTimer_.Stop();
        wifi_interface_.Stop();
        ESP_LOGI(TAG, "No WiFi network configured, starting AP '%s'", apSsid_);
        wifi_interface_.StartAP(apSsid_, DefaultApPassword);
    }

    connectTimer_.SetPeriod(pdMS_TO_TICKS(ProvisioningPollMs));
    connectTimer_.Start();
}

void NetworkManager::HandleNetworkEvent(const NetworkEvent& event)
{
    switch (event.type)
    {
    case NetworkEventType::LinkUp:
        if (!wifi_interface_.IsAP())
        {
            ESP_LOGI(TAG, "STA connected to AP");

            // Associated. The attempt has succeeded at the only thing an attempt can
            // do; what is left is DHCP, which takes as long as the AP takes. Hand the
            // timer over to the address wait, or the next tick tears down a station
            // that just let us in.
            staAssociated_ = true;
            connectTimer_.SetPeriod(pdMS_TO_TICKS(StaDhcpTimeoutMs));
            connectTimer_.Start();
        }
        else
        {
            ESP_LOGI(TAG, "AP started");
        }
        break;

    case NetworkEventType::LinkDown:
    {
        // apWindowOpen_ covers the disconnect this manager caused itself by taking the
        // station down to open the window: it is not news, and acting on it would
        // count the teardown as a failed attempt.
        if (wifi_interface_.IsAP() || apWindowOpen_)
            break;

        // Kept for the summary line, which is written a tick later by the cycle timer
        // and has nowhere else to learn this from.
        staLastReason_ = event.reason;

        if (staConnected_)
        {
            // Was connected, lost the link — a fresh outage, so it gets to explain
            // itself once more and starts its own round of attempts.
            staConnected_ = false;
            staAssociated_ = false;
            staRoundAttempts_ = 0;
            staOutageAttempts_ = 0;
            staOutageLogged_ = false;
            staOutageStartTick_ = xTaskGetTickCount();
            ESP_LOGI(TAG, "Lost connection, attempting reconnect");
            wifi_interface_.ReconnectSta();
            connectTimer_.SetPeriod(pdMS_TO_TICKS(StaConnectTimeoutMs));
            connectTimer_.Start();
            break;
        }

        // Not connected yet, so this is the attempt in progress failing — and once it
        // has failed there is nothing left for its timeout to wait for. Standing on
        // the full StaConnectTimeoutMs spent nine of every ten seconds of a station
        // round with the radio doing nothing, in the one situation where the fix is
        // often simply to ask again.
        //
        // Only a reason the radio actually reported counts. Reason 0 is a LinkDown
        // from somewhere with nothing to say (the AP stopping as a round begins), and
        // ASSOC_LEAVE is this manager's own esp_wifi_disconnect echoing back; acting
        // on either would retry an attempt that has not been made yet, and three of
        // those inside a second is a round spent before it started.
        if (event.reason == 0 || event.reason == WIFI_REASON_ASSOC_LEAVE)
            break;

        staAssociated_ = false;
        connectTimer_.SetPeriod(pdMS_TO_TICKS(StaRetryDelayMs));
        connectTimer_.Start();
        break;
    }

    case NetworkEventType::Ipv4Acquired:
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event.status.ipv4.ip));

        // The one place the attempt count is worth saying out loud: how long the
        // network was gone, in a single line, to somebody arriving late. Measured
        // rather than derived, because AP windows sit between the attempts.
        if (staOutageAttempts_ > 0)
            ESP_LOGI(TAG, "Reconnected to '%s' after %d attempts (~%u s down)",
                     staSsid_, staOutageAttempts_.load() + 1,
                     static_cast<unsigned>(
                         pdTICKS_TO_MS(xTaskGetTickCount() - staOutageStartTick_) / 1000));

        connectTimer_.Stop();
        apWindowOpen_ = false;
        staConnected_ = true;
        staAssociated_ = false;
        staRoundAttempts_ = 0;
        staOutageAttempts_ = 0;
        staOutageLogged_ = false;
        staLastReason_ = 0;
        break;

    case NetworkEventType::Ipv4Lost:
        // The address is gone; the association is not. esp_netif raises this when the
        // lease goes away, and a station that actually left raises LinkDown as well —
        // so this handler never has to guess which happened.
        //
        // Clearing staAssociated_ here is what made the next cycle tick read a joined
        // station as an attempt that never associated. That branch answers with
        // ReconnectSta, which esp_wifi refuses outright on a connected station
        // ("sta is connected, disconnect before connecting to new ap"), and the
        // refusals still counted as attempts — so a device whose radio was fine walked
        // its round up towards the AP window. Exactly the failure the DHCP branch was
        // written to prevent, re-opened from the one event that clears the flag it
        // depends on.
        //
        // Associated without an address is precisely what staAssociated_ means, so say
        // that and hand the timer back to the address wait. If DHCP really is gone the
        // tick that follows restarts the station the proper way, radio and all.
        ESP_LOGW(TAG, "Lost IP");
        staConnected_ = false;
        staAssociated_ = true;
        connectTimer_.SetPeriod(pdMS_TO_TICKS(StaDhcpTimeoutMs));
        connectTimer_.Start();
        break;
    }
}

// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

CommandResult NetworkManager::Cmd_WifiScan(CommandContext& ctx)
{
    WiFiInterface::ScanResult results[20] = {};
    int count = wifi().Scan(results, 20);

    auto root = ctx.reply.object();
    root.field("ok", true);
    auto networks = root.array("networks");

    for (int i = 0; i < count; i++)
    {
        auto n = networks.object();
        n.field("ssid", results[i].ssid);
        n.field("rssi", static_cast<int32_t>(results[i].rssi));
        n.field("channel", static_cast<int32_t>(results[i].channel));
        n.field("secure", results[i].secure);
        // "secure" is the padlock; "auth" is the answer to why a join was refused at
        // full signal, which is the one question the bit cannot settle. A caller
        // reaching this device over the relay cannot read the firmware log, so the
        // name goes on the reply too.
        n.field("auth", WiFiInterface::AuthModeName(results[i].authmode));
    }
    return CommandResult::Ok;
}
