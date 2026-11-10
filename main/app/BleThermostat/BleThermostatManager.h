#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Task.h"
#include "Mutex.h"
#include "KcBleProtocol.h"
#include "ThermostatPanel.h"
#include "host/ble_hs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// A KC thermostat's BLE link to a KC1245 gateway, played by the testbench, so a bench
// suite can drive the gateway's THR 3 support without the thermostat itself.
//
// To the gateway it must be the thermostat. So this is the thermostat's BleManager in
// every way the gateway can see: a NimBLE central that scans for the gateway's
// manufacturer data, connects, pairs with the 6-digit install code as the passkey
// (keyboard side, MITM, LE Secure Connections, bonded), refuses a link that is not
// encrypted AND authenticated, discovers the KC service, subscribes to its inbound
// characteristic and then answers what the gateway asks, one session at a time, with
// the thermostat's own answers (ThermostatPanel). A link that fails at any step is
// dropped and tried again 5 s later, for ever, straight to the gateway on record. A
// wrong install code is the one failure that is not retried: the gateway is forgotten.
//
// The thermostat's OpenTherm side is the ThermostatSimulator. The panel reads its room
// and setpoint, and hands it the mode in force, so with BLE on, the simulator asks for
// heat and cooling the way the KC thermostat would in that mode.
//
// ble.enable is decided at boot, because of the RAM. On the ESP32, a build with BT
// reserves 56 KB of internal RAM for the controller, and NimBLE's own data comes on top.
// Off at boot, all of it goes back to the heap -- the testbench then has the RAM it had
// before BLE existed -- and BLE cannot start until a reboot; `ble enable on=true` says
// so. On at boot, only the controller's Classic half goes back, and one task of its own
// serves requests and runs the clock: reconnects, the two-minute mode grace, the fake
// reboot and the faults. NimBLE's host task delivers the radio's events, which only
// update state here and queue what arrives; nothing waits on that task. Turned off
// while running, the link is dropped and nothing is tried; on again, it reconnects.
//
// A few things a test can break on purpose, none saved: `ble drop` (out of reach for N
// seconds), `ble delay` (answer late), `ble reject` (refuse a command).
// ──────────────────────────────────────────────────────────────
class BleThermostatManager
{
    static constexpr const char* TAG = "BleThermostat";

public:
    explicit BleThermostatManager(AppProvider& app);

    BleThermostatManager(const BleThermostatManager&) = delete;
    BleThermostatManager& operator=(const BleThermostatManager&) = delete;
    BleThermostatManager(BleThermostatManager&&) = delete;
    BleThermostatManager& operator=(BleThermostatManager&&) = delete;

    void Init();

private:
    // ── The thermostat's numbers, so the gateway sees its timing ──
    static constexpr int MAX_PEERS = 8;
    static constexpr size_t MAX_NAME = 30;                 // a scan response holds ~29 characters
    static constexpr int32_t SCAN_MS = 8000;
    static constexpr int32_t CONNECT_TIMEOUT_MS = 10000;
    static constexpr uint32_t RECONNECT_MS = 5000;
    static constexpr uint32_t MODE_GRACE_MS = 2 * 60 * 1000;
    static constexpr uint32_t REBOOT_REPLY_MS = 500;      // its `system reboot` waits this long, then restarts
    static constexpr uint32_t READY_WAIT_MS = 5000;       // a request that beats discovery is held this long
    static constexpr uint32_t RECV_TIMEOUT_MS = 10000;    // a stream quiet this long has ended
    static constexpr uint32_t WRITE_TIMEOUT_MS = 3000;    // one reply chunk, written and acknowledged

    // ── Ours ──
    // How long the fake reboot keeps the thermostat away. A real one is back on the link
    // a few seconds after a restart.
    static constexpr uint32_t REBOOT_AWAY_MS = 4000;
    // Below this much free internal RAM the stack is not started, rather than letting the
    // controller assert in its own allocation and boot-loop the testbench with ble.enable
    // saved. The gateway lost ~12 KB to NimBLE at runtime on this chip; the host task and
    // ours take 4 KB and 5 KB more. The KC thermostat's floor is 48 KB, on an S3.
    static constexpr size_t MIN_INTERNAL_HEAP = 32 * 1024;
    static constexpr int TASK_STACK = 5120;
    static constexpr uint32_t TICK_MS = 50;
    static constexpr uint32_t APPLY_MS = 200;
    static constexpr uint32_t REFRESH_MS = 1000;
    static constexpr int IN_QUEUE_DEPTH = 16;              // the thermostat's: a stream never outruns it

    /// One gateway heard in a scan: the advertisement gives its id, the scan response
    /// its name, merged by address.
    struct Peer
    {
        ble_addr_t addr{};
        char name[MAX_NAME + 1] = {};
        uint32_t gatewayId = 0;
        bool hasId = false;
        int8_t rssi = 0;
        bool used = false;
    };

    /// One session chunk off the radio.
    struct Chunk
    {
        uint16_t len = 0;
        uint8_t data[kcble::CHUNK_MAX] = {};
    };

    // Down -> Connecting -> Securing -> Discovering -> Ready; only Ready answers.
    enum class Link : uint8_t { Down, Connecting, Securing, Discovering, Ready };

    /// What `ble pair` asked for, done by the task: pick the gateway, scanning for it
    /// first when the last scan did not hear it.
    struct PairRequest
    {
        bool pending = false;
        bool byId = false, byAddr = false;
        uint32_t gatewayId = 0;
        ble_addr_t addr{};
        char code[7] = {};
        bool scanned = false;
    };

    /// A request's body, read off the queue as the panel asks for it.
    struct SessionBody final : ThermostatPanel::Body
    {
        BleThermostatManager& m;
        uint16_t sid;
        const uint8_t* first;       // what followed the line in the first chunk
        size_t firstLen;
        bool ended;                 // a FINAL chunk was read
        bool failed = false;
        SessionBody(BleThermostatManager& m, uint16_t sid, const uint8_t* first, size_t firstLen, bool ended)
            : m(m), sid(sid), first(first), firstLen(firstLen), ended(ended) {}
        size_t Next(const uint8_t*& data) override;
        bool Failed() const override { return failed; }
    };

    /// A reply, cut into chunks of what the MTU carries and written one at a time.
    struct SessionReply final : ThermostatPanel::Reply
    {
        BleThermostatManager& m;
        uint16_t sid;
        size_t cap;                 // payload per chunk
        size_t len = 0;             // buffered in m.outFrame_
        bool failed = false;
        SessionReply(BleThermostatManager& m, uint16_t sid, size_t cap) : m(m), sid(sid), cap(cap) {}
        void Write(const char* text, size_t n) override;
        void Flush() override;
        void Send(uint8_t flags);
        void Finish() { Send(kcble::FLAG_FINAL); }
        void Reject(const char* reason);
    };

    AppProvider& app_;
    InitState initState_;
    Task task_;
    ThermostatPanel panel_;

    // BLE was off at boot and its RAM went back to the heap: the stack cannot start, and
    // nothing of NimBLE may be touched, until a reboot. Set in Init(), before any command.
    bool released_ = false;

    // ── The session transport. Created with the task; frames in PSRAM ──
    QueueHandle_t inQueue_ = nullptr;
    SemaphoreHandle_t writeDone_ = nullptr;
    std::atomic<int> writeStatus_{ 0 };
    std::atomic<bool> inboundDropped_{ false };
    uint8_t* outFrame_ = nullptr;   // [ header | payload ]
    Chunk* bodyChunk_ = nullptr;

    // ── Under mutex_: the host task's events and the commands meet here ──
    mutable Mutex mutex_;
    bool enabled_ = false;          // ble.enable, as the task last read it
    bool stackUp_ = false;
    bool stackFailed_ = false;
    bool synced_ = false;
    uint8_t ownAddrType_ = 0;

    bool scanning_ = false;
    uint32_t scanStartMs_ = 0;
    Peer peers_[MAX_PEERS];
    PairRequest pair_;

    Link link_ = Link::Down;
    bool haveTarget_ = false;
    ble_addr_t target_{};
    char targetName_[MAX_NAME + 1] = {};
    uint32_t targetId_ = 0;
    uint16_t connHandle_ = BLE_HS_CONN_HANDLE_NONE;
    uint16_t mtu_ = 23;
    uint16_t svcStart_ = 0, svcEnd_ = 0;
    uint16_t inboundHandle_ = 0, outboundHandle_ = 0, inboundCccd_ = 0;
    uint32_t readySinceMs_ = 0;
    bool cancelingConnect_ = false;

    uint32_t passkey_ = 0;          // only while havePasskey_
    bool havePasskey_ = false;      // between `ble pair` and the pairing
    bool passkeyUsed_ = false;      // the gateway asked for it on this connection

    bool reconnectPending_ = false;
    uint32_t reconnectAtMs_ = 0;
    bool graceRunning_ = false;
    uint32_t graceEndsMs_ = 0;
    bool away_ = false;             // `ble drop`
    uint32_t awayUntilMs_ = 0;
    bool rebooting_ = false;        // the fake reboot after `system reboot`
    uint32_t rebootUntilMs_ = 0;

    char lastFailure_[64] = {};
    uint32_t lastFailureMs_ = 0;

    size_t heapAtStart_ = 0, largestAtStart_ = 0, heapAfterStart_ = 0;

    uint32_t nextApplyMs_ = 0;      // the task's own

    // ── Lifecycle ──
    void StartWorker();
    void TaskLoop();
    void StartStack();
    void Refresh(uint32_t nowMs);
    void Tick(uint32_t nowMs);
    void LoadTarget();

    // ── Link ──
    bool StartScan();
    void NotePeer(const struct ble_gap_disc_desc& disc);
    void ServePairRequest(uint32_t nowMs);
    void Connect(const Peer& peer, const char* code);
    void StartConnect();
    void ScheduleReconnectLocked(uint32_t delayMs);
    void StartModeGraceLocked(uint32_t nowMs);
    void DropLink(const char* why);
    void Terminate();
    void Forget();
    void RejectInstallCode();
    void StartDiscovery();
    void FailLocked(const char* why);

    // ── NimBLE callbacks ──
    void OnSync();
    int OnGapEvent(struct ble_gap_event* event);
    int OnSvcDisc(const struct ble_gatt_error* error, const struct ble_gatt_svc* svc);
    int OnChrDisc(const struct ble_gatt_error* error, const struct ble_gatt_chr* chr);
    int OnDscDisc(const struct ble_gatt_error* error, const struct ble_gatt_dsc* dsc);
    int OnSubscribed(const struct ble_gatt_error* error);
    void EnqueueChunk(const struct os_mbuf* om);

    static void HostTask(void* param);
    static void OnSyncTrampoline();
    static void OnResetTrampoline(int reason);
    static int GapEventTrampoline(struct ble_gap_event* event, void* arg);
    static int SvcDiscTrampoline(uint16_t conn, const struct ble_gatt_error* error,
                                 const struct ble_gatt_svc* svc, void* arg);
    static int ChrDiscTrampoline(uint16_t conn, const struct ble_gatt_error* error,
                                 const struct ble_gatt_chr* chr, void* arg);
    static int DscDiscTrampoline(uint16_t conn, const struct ble_gatt_error* error,
                                 uint16_t chrValHandle, const struct ble_gatt_dsc* dsc, void* arg);
    static int SubscribeTrampoline(uint16_t conn, const struct ble_gatt_error* error,
                                   struct ble_gatt_attr* attr, void* arg);
    static int WriteDoneTrampoline(uint16_t conn, const struct ble_gatt_error* error,
                                   struct ble_gatt_attr* attr, void* arg);

    // ── Sessions ──
    void Serve(const Chunk& first);
    bool SendRaw(const uint8_t* frame, size_t len);
    void FakeReboot();

    static void FormatAddr(const ble_addr_t& addr, char* out, size_t cap);
    static bool ParseAddr(const char* text, uint8_t type, ble_addr_t& out);
    static bool IsInstallCode(const char* text);

    // ── Settings. Keys are at most 15 characters (NVS). ──
    // Read at boot for the RAM (see above), and every second after, for the link.
    inline static BoolSetting enable_{ "ble.enable", "BLE Thermostat Enable", false };
    // The install code the gateway was last paired with, and the one `ble pair` uses
    // when it is given none.
    inline static StringSetting code_{ "ble.code", "BLE Gateway Install Code", "130000" };
    inline static StringSetting peer_{ "ble.peer", "BLE Gateway Address", "" };
    inline static UInt32Setting peerType_{ "ble.peerType", "BLE Gateway Address Type", 0 };
    inline static StringSetting peerName_{ "ble.peerName", "BLE Gateway Name", "" };
    inline static UInt32Setting peerId_{ "ble.peerId", "BLE Gateway Id", 0 };

    // ── Commands ──
    CommandResult Cmd_Status(CommandContext& ctx);
    CommandResult Cmd_Scan(CommandContext& ctx);
    CommandResult Cmd_Pair(CommandContext& ctx);
    CommandResult Cmd_Forget(CommandContext& ctx);
    CommandResult Cmd_Enable(CommandContext& ctx);
    CommandResult Cmd_Mode(CommandContext& ctx);
    CommandResult Cmd_Drop(CommandContext& ctx);
    CommandResult Cmd_Delay(CommandContext& ctx);
    CommandResult Cmd_Reject(CommandContext& ctx);
    const char* Unavailable() const;
    void WriteState(CommandContext& ctx);
    static CommandEntry statusCommand_;
    static CommandEntry scanCommand_;
    static CommandEntry pairCommand_;
    static CommandEntry forgetCommand_;
    static CommandEntry enableCommand_;
    static CommandEntry modeCommand_;
    static CommandEntry dropCommand_;
    static CommandEntry delayCommand_;
    static CommandEntry rejectCommand_;
};
