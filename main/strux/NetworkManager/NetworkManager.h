#pragma once

#include <atomic>
#include <stdint.h>

#include "WiFiInterface.h"
#include "EthernetInterface.h"
#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Timer.h"

class Stream;

class NetworkManager {
    static constexpr const char* TAG = "NetworkManager";

    /// How long one connect attempt is given before it is retried. This is the
    /// ceiling on an attempt that says nothing at all; an attempt that fails out
    /// loud is retried after StaRetryDelayMs instead, because waiting out a timeout
    /// for news that has already arrived is nine idle seconds per attempt.
    static constexpr int StaConnectTimeoutMs = 10000;

    /// How long to leave the radio alone after a failed association before trying
    /// again. Not zero: the failures that clear on their own are the ones where the
    /// AP was momentarily unable to answer, and a retry inside the same instant is
    /// the same instant. Short, because the round is bounded by attempts, not time.
    static constexpr int StaRetryDelayMs = 2000;

    /// How long DHCP gets once the radio has associated. A separate wait from
    /// StaConnectTimeoutMs, and it has to be: association and address are two steps,
    /// and a timer that treats the gap between them as a failed attempt tears down a
    /// station that has already joined. DHCP on a busy AP routinely takes longer than
    /// one connect timeout. See staAssociated_ below.
    static constexpr int StaDhcpTimeoutMs = 15000;

    /// The device alternates forever: StaAttemptsPerRound attempts at the configured
    /// network, then an AP window, then the same again, for as long as it is powered.
    /// Neither half is terminal, and that is the entire design.
    ///
    /// Both terminal versions were wrong in the same way — each assumed it could
    /// tell, from a failed association, which kind of failure it was looking at.
    /// Ending in the AP stranded a device whose network was merely absent, since
    /// nothing re-entered station mode without a reboot: an access point rebooting
    /// cost a power-cycle. Ending in STA leaves an unattended device with no way in
    /// when the credentials are the thing that is wrong. Cycling needs to distinguish
    /// nothing: whichever failure it is, the half that addresses it comes round again
    /// within fifteen minutes.
    ///
    /// The halves are deliberately lopsided (30 s of STA to a 15 min AP window). The
    /// station round is a machine retrying a local association and needs no longer;
    /// the AP window is the half a human has to notice, walk over to and use. It is
    /// also the half that costs something — DefaultApPassword is empty, so every
    /// window puts an open network back on the air, and on a device whose
    /// web.password is unset that is an unauthenticated console. Where the old
    /// fallback did that once, this does it every fifteen minutes: set web.password
    /// on anything running outside a lab.
    static constexpr int StaAttemptsPerRound = 3;
    static constexpr int ApWindowMs = 15 * 60 * 1000;

    /// How often a device with no credentials at all re-reads its settings. Not a
    /// window length — nothing is torn down and nothing is attempted, so it is only
    /// how long after pressing Save the AP notices, and fifteen minutes of standing
    /// there is not that.
    static constexpr int ProvisioningPollMs = 30000;

    /// Shown both to a device that has never been told which network to join
    /// (provisioning) and between station rounds (recovery). Credentials are re-read
    /// at the start of every round, so a network provisioned through this AP is
    /// picked up by the next round without a reboot.
    ///
    /// The SSID is composed at Init — `<device.name>-AP-<MAC suffix>` — not a
    /// constant. See ComposeApSsid.
    static constexpr const char* ApSsidSuffix = "-AP-";
    static constexpr const char* DefaultApPassword = ""; // Open network

public:
    explicit NetworkManager(StruxProvider& strux);

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;
    NetworkManager(NetworkManager&&) = delete;
    NetworkManager& operator=(NetworkManager&&) = delete;

    void Init();

    /// Bring up Ethernet on a driver the board installed. Called by the application,
    /// which is the layer that sees both the board and this manager; a board without
    /// Ethernet simply never calls it. After Init().
    void AttachEthernet(esp_eth_handle_t handle);

    WiFiInterface& wifi();
    const WiFiInterface& wifi() const;

    bool IsAccessPoint() const { return wifi_interface_.IsAP(); }

    /// Is there a route off this device yet? Asked by anything that dials OUT (the
    /// relay), because attempting it before there is one produces nothing but a
    /// failed connection and the log noise of one.
    ///
    /// Deliberately not "does an interface have an address": the AP netif is always
    /// 192.168.4.1, so that question answers yes for the whole of a recovery AP
    /// window — the one stretch of time when there is certainly nowhere to dial. It
    /// is the station's address that means the world is reachable, which is what
    /// staConnected_ tracks.
    /// Ethernet with an address counts as well, whatever the radio is doing.
    bool HasUpstream() const
    {
        return ethernetConnected_ || (staConnected_ && !wifi_interface_.IsAP());
    }

    /// Associated AP's signal strength in dBm. False when there is none to report
    /// (AP mode, or not associated).
    bool GetRssi(int8_t& out) const { return wifi_interface_.GetRssi(out); }

    /// The active interface's IPv4 address as a dotted quad — Ethernet's when it has
    /// one, else the station's when associated, else the AP's own when serving one. False when there is no address
    /// yet, which is not the same as an address of 0.0.0.0.
    bool GetIpv4(char* out, size_t len) const;

private:
    StruxProvider& strux_;

    InitState initState;
    WiFiInterface wifi_interface_;
    EthernetInterface ethernet_interface_;

    /// Ethernet has an address. Set and cleared by HandleEthernetEvent only.
    std::atomic<bool> ethernetConnected_{false};

    // ── STA connection state ──
    /// The configured network, re-read at the start of every round — which is what
    /// lets provisioning take effect without a reboot. An empty SSID is not a network
    /// to fail against; it is a device that has not been told one yet.
    char staSsid_[33] = {};
    char staPassword_[65] = {};

    /// Has an address, so there is a route off the device. Set only by Ipv4Acquired.
    std::atomic<bool> staConnected_{false};

    /// Associated, but without an address yet — the step between LinkUp and
    /// Ipv4Acquired. staConnected_ cannot answer this: it means "has an address",
    /// which is what its one caller (HasUpstream) needs. Without a flag for the
    /// middle state the cycle timer reads an associated station as an attempt still
    /// failing and tears down the link that was about to work.
    std::atomic<bool> staAssociated_{false};

    /// Attempts in the current round, and across the whole outage. Only the first
    /// decides anything — when it reaches StaAttemptsPerRound the AP window opens;
    /// the second exists to be reported, by the connect that finally succeeds.
    std::atomic<int> staRoundAttempts_{0};
    std::atomic<int> staOutageAttempts_{0};

    /// When the current outage began. Attempts × timeout used to stand in for how
    /// long the network had been gone, and stopped being that number the moment AP
    /// windows started sitting between the attempts.
    TickType_t staOutageStartTick_ = 0;

    /// Whether the AP is up as the second half of the cycle (or for provisioning)
    /// rather than because a station attempt is in flight. It is also what keeps the
    /// STA-disconnect path still while the station is being torn down to open the
    /// window, that teardown raising a disconnect of its own which would otherwise be
    /// counted as another failed attempt.
    std::atomic<bool> apWindowOpen_{false};

    /// Why the last association failed, kept because the line that explains an outage
    /// is written by the cycle timer, one tick after the event that knows the answer.
    /// Reason 0 means no radio event arrived at all — the attempt ran out of time in
    /// silence, which is a different report and a different suspicion.
    std::atomic<uint8_t> staLastReason_{0};

    /// Whether the current outage has already been explained. A retry loop that runs
    /// for the lifetime of the device turns "one line per failure" into one line every
    /// ten seconds, forever — so the reason is logged once and the repeats are
    /// counted, reported by the connect that finally succeeds.
    std::atomic<bool> staOutageLogged_{false};

    /// One timer, three periods: the patience for a single connect attempt, the wait
    /// for DHCP once associated, and the length of the AP window. Which one is running
    /// is apWindowOpen_ plus staAssociated_.
    Timer connectTimer_;

    /// This device's own AP name, `<device.name>-AP-<MAC suffix>`, composed once at
    /// Init. Every device gets a distinct one, and it stays the same across reboots
    /// because the MAC does — so a client that saved it keeps working, and a label on
    /// the case can carry it.
    ///
    /// The suffix is unconditional rather than only present on an unnamed device. Two
    /// Strux devices falling back to an identically-named AP is precisely the
    /// situation the recovery window exists for — a bench with several of them, none
    /// of which joined the network — and there would be no way to tell which one you
    /// had joined. Making it appear only when the name is unset also means the SSID
    /// changes identity the moment somebody names the device, invalidating the
    /// profile every client had saved.
    char apSsid_[33] = {};

    /// Builds apSsid_ from the device name and the SoftAP MAC's low three bytes. The
    /// name is what gets truncated when the whole thing will not fit in 32 characters:
    /// the MAC suffix is the part that distinguishes one device from another, so it is
    /// the part that must survive.
    void ComposeApSsid();

    void HandleNetworkEvent(const NetworkEvent& event);
    void HandleEthernetEvent(const NetworkEvent& event);
    void OnCycleTimer();
    void BeginStaRound();

    /// One attempt at the configured network. `freshRadio` restarts the station from
    /// scratch, which is what a round beginning needs (the radio may be in AP mode)
    /// and what an attempt inside a round must not do — see
    /// WiFiInterface::ReconnectSta.
    void AttemptStaConnect(bool freshRadio);

    void OpenApWindow();
    void StartProvisioningAp();

    // ── WebSocket commands (registered with CommandManager in Init) ──
    CommandResult Cmd_WifiScan(CommandContext& ctx);

    // Defined in NetworkManager.cpp, beside the handler.
    static CommandEntry wifiScanCommand_;

    // ── Settings (registered with SettingsManager in Init) ──
    inline static StringSetting wifiSsid_    { "wifi.ssid",     "WiFi SSID",     "" };
    inline static StringSetting wifiPassword_{ "wifi.password", "WiFi Password", "" };

    // On, because <name>.local is how you find a device whose address you were never
    // told, and answering the occasional multicast query costs a radio that never
    // sleeps nothing at all. The fork that wants this off is the one that enabled
    // modem or light sleep, where every multicast query on the subnet becomes a wake
    // this device pays for and did not ask for. Read once in Init, so a change takes
    // effect on the next boot.
    inline static BoolSetting mdnsEnabled_{ "net.mdns", "mDNS Enabled", true };
};
