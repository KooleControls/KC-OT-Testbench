#include "BleThermostatManager.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "RoutingManager.h"
#include "ReplyWriter.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// Bond storage is NimBLE's "store config" module, which has no public header; the IDF
// examples declare it by hand like this.
extern "C" void ble_store_config_init(void);

// NimBLE's sync and reset callbacks carry no argument, so the one manager has to be
// reachable statically. Set in Init(), before the stack can start, never cleared.
static BleThermostatManager* s_instance = nullptr;

namespace {

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

constexpr CommandArg<const char*> codeArg{
    "code", "The gateway's install code, its 6 public digits: the BLE passkey. Left out: ble.code, "
            "which keeps the last code a pairing proved.",
    6, Presence::Optional };

constexpr CommandArg<uint32_t> gatewayArg{
    "gateway", "Pair with the gateway that advertises this gateway id, scanning for it if the last "
               "scan did not hear it.",
    Presence::Optional };

constexpr CommandArg<const char*> addrArg{
    "addr", "Pair with the gateway at this address, aa:bb:cc:dd:ee:ff as `ble status` prints it, "
            "without scanning.",
    17, Presence::Optional };

constexpr CommandArg<uint32_t> addrTypeArg{
    "addrType", "With addr: its type, 0 public (the default) or 1 random.", Presence::Optional };

constexpr CommandArg<bool> onArg{
    "on", "Turn the BLE thermostat on (true) or off (false). Saved (ble.enable)." };

constexpr CommandArg<const char*> modeArg{
    "mode", "The tile pressed on the panel: off, heat, cool or auto.", 4 };

constexpr CommandArg<uint32_t> secondsArg{
    "seconds", "How long the thermostat stays out of reach, in seconds. 0 drops the link once and "
               "lets it reconnect as it would after any dropout, 5 s later." };

constexpr CommandArg<uint32_t> msArg{
    "ms", "How late to answer, in ms; 0 answers at once again." };

constexpr CommandArg<const char*> delayTypeArg{
    "command", "Only requests of this type, as the gateway names it (e.g. 'climate status'). Left "
               "out or empty: every request.",
    31, Presence::Optional };

constexpr CommandArg<const char*> rejectTypeArg{
    "command", "The request type to refuse, as the gateway names it (e.g. 'climate set'). Left out "
               "or empty: refuse nothing again.",
    31, Presence::Optional };

} // namespace

// ── Commands ─────────────────────────────────────────────────

CommandEntry BleThermostatManager::statusCommand_{
    "ble status", &InvokeCommand<&BleThermostatManager::Cmd_Status>,
    "Report the simulated KC thermostat's BLE side: the link and its state, the gateway on "
    "record, the panel's mode as picked and as the gateway reads it, what the gateway has asked "
    "(per request type), a firmware push, the faults set, internal RAM, and the last scan."
};

CommandEntry BleThermostatManager::scanCommand_{
    "ble scan", &InvokeCommand<&BleThermostatManager::Cmd_Scan>,
    "Scan 8 s for KC gateways advertising. Answers at once; poll `ble status` until 'scanning' "
    "is false, its 'scan' lists what was heard."
};

CommandEntry BleThermostatManager::pairCommand_{
    "ble pair", &InvokeCommand<&BleThermostatManager::Cmd_Pair>,
    "Pair with a gateway, by its gateway id or its address, or with the only one a scan hears "
    "when neither is given. Answers at once; poll `ble status` for 'state' ready, or "
    "'lastFailure' ('wrong code' forgets the gateway). The gateway is saved and reconnected "
    "after every dropout and reboot. An existing bond is used as it is: `ble forget` first to "
    "make the gateway ask for the code again.",
    { &codeArg, &gatewayArg, &addrArg, &addrTypeArg }
};

CommandEntry BleThermostatManager::forgetCommand_{
    "ble forget", &InvokeCommand<&BleThermostatManager::Cmd_Forget>,
    "Drop the link, forget the gateway and delete its bond. The mode choice goes with it, as on "
    "the thermostat."
};

CommandEntry BleThermostatManager::enableCommand_{
    "ble enable", &InvokeCommand<&BleThermostatManager::Cmd_Enable>,
    "Turn the BLE thermostat on or off. Saved. BLE off at boot gives its RAM back, so turning it "
    "on then takes a reboot ('rebootNeeded'). On, it reconnects the gateway on record and the "
    "panel's mode gates what the thermostat simulator asks for; off drops the link and the gate.",
    { &onArg }
};

CommandEntry BleThermostatManager::modeCommand_{
    "ble mode", &InvokeCommand<&BleThermostatManager::Cmd_Mode>,
    "Press a mode tile on the panel, as a guest would: the pick, saved (ble.mode). Whether it is "
    "in force is the mode choice, which only a gateway on the link gives.",
    { &modeArg }
};

CommandEntry BleThermostatManager::dropCommand_{
    "ble drop", &InvokeCommand<&BleThermostatManager::Cmd_Drop>,
    "Fault: drop the link and stay out of the gateway's reach for a while, then reconnect at "
    "once. The two-minute mode grace runs as it would.",
    { &secondsArg }
};

CommandEntry BleThermostatManager::delayCommand_{
    "ble delay", &InvokeCommand<&BleThermostatManager::Cmd_Delay>,
    "Fault: answer the gateway late (it gives most requests 3 s). Not saved.",
    { &msArg, &delayTypeArg }
};

CommandEntry BleThermostatManager::rejectCommand_{
    "ble reject", &InvokeCommand<&BleThermostatManager::Cmd_Reject>,
    "Fault: refuse one request type with \"unknown command\", as a thermostat whose firmware "
    "lacks it would. Not saved.",
    { &rejectTypeArg }
};

BleThermostatManager::BleThermostatManager(AppProvider& app)
    : app_(app)
{
}

void BleThermostatManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    s_instance = this;
    StruxProvider& strux = app_.getStrux();
    strux.getSettingsManager().Register({ &enable_, &code_, &peer_, &peerType_, &peerName_, &peerId_ });
    panel_.Init(app_.getRoutingManager().ThermostatSim(), strux.getSettingsManager());

    // Decided now, before anything could start the controller: its RAM can only be given
    // back before. Off, all of it goes -- the controller's 56 KB reserve and every byte of
    // NimBLE's data -- and the testbench has the RAM it had before BLE existed. On, only
    // the Classic half, which a BLE-only controller never uses.
    const bool enabled = enable_.Get();
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const esp_err_t err = enabled ? esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)
                                  : esp_bt_mem_release(ESP_BT_MODE_BTDM);
    released_ = !enabled;
    ESP_LOGI(TAG, "%s: %u bytes of internal RAM back (%s)",
             enabled ? "Classic BT released" : "BLE off, all BT memory released",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - before), esp_err_to_name(err));

    strux.getCommandManager().Register(this, { &statusCommand_, &scanCommand_, &pairCommand_, &forgetCommand_,
                                               &enableCommand_, &modeCommand_, &dropCommand_, &delayCommand_,
                                               &rejectCommand_ });
    if (enabled)
        StartWorker();

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized (%s)", enabled ? "enabled" : "off until ble.enable and a reboot");
}

// ── Lifecycle ────────────────────────────────────────────────

void BleThermostatManager::StartWorker()
{
    // The transport, before the stack: the queue has to be there before the first
    // notification can be. Queue and frames in PSRAM; internal RAM is what the controller
    // and the stacks need, and none of these is touched from an ISR.
    inQueue_ = xQueueCreateWithCaps(IN_QUEUE_DEPTH, sizeof(Chunk), MALLOC_CAP_SPIRAM);
    writeDone_ = xSemaphoreCreateBinary();
    outFrame_ = static_cast<uint8_t*>(heap_caps_malloc(kcble::HEADER_LEN + kcble::CHUNK_MAX, MALLOC_CAP_SPIRAM));
    void* body = heap_caps_malloc(sizeof(Chunk), MALLOC_CAP_SPIRAM);
    bodyChunk_ = body != nullptr ? new (body) Chunk() : nullptr;
    if (inQueue_ == nullptr || writeDone_ == nullptr || outFrame_ == nullptr || bodyChunk_ == nullptr)
    {
        ESP_LOGE(TAG, "Could not allocate the session transport; BLE stays off until a reboot");
        LOCK(mutex_);
        enabled_ = true;   // wanted, and failed: `ble status` says so rather than "off"
        stackFailed_ = true;
        FailLocked("out of memory for the transport");
        return;
    }

    // Below the router: a frame the thermostat simulator sends is late the moment it waits,
    // a BLE reply has seconds.
    task_.Init("ble", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
    {
        ESP_LOGE(TAG, "Failed to start the BLE task; BLE stays off until a reboot");
        LOCK(mutex_);
        enabled_ = true;
        stackFailed_ = true;
        FailLocked("BLE task did not start");
    }
}

void BleThermostatManager::TaskLoop()
{
    uint32_t nextRefreshMs = NowMs();   // due at once: ble.enable is read before anything else
    for (;;)
    {
        Chunk chunk;
        if (xQueueReceive(inQueue_, &chunk, pdMS_TO_TICKS(TICK_MS)) == pdTRUE)
            Serve(chunk);

        const uint32_t now = NowMs();
        // Again every second, so `settings set ble.enable` applies like any other setting.
        if (Due(now, nextRefreshMs))
        {
            nextRefreshMs = now + REFRESH_MS;
            Refresh(now);
        }
        Tick(now);
    }
}

void BleThermostatManager::StartStack()
{
    // The controller allocates internal RAM in nimble_port_init and NimBLE's host task
    // needs a 4 KB internal stack, whose creation esp_nimble_enable() does not even check.
    // Refused up front with one clear line rather than an assert or a silent dead host.
    const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    {
        LOCK(mutex_);
        heapAtStart_ = freeInternal;
        largestAtStart_ = largest;
        stackFailed_ = false;
    }
    ESP_LOGI(TAG, "Internal RAM before the stack: %u free, %u largest",
             static_cast<unsigned>(freeInternal), static_cast<unsigned>(largest));
    if (freeInternal < MIN_INTERNAL_HEAP)
    {
        ESP_LOGE(TAG, "BLE not started: %u bytes of internal RAM free, %u needed",
                 static_cast<unsigned>(freeInternal), static_cast<unsigned>(MIN_INTERNAL_HEAP));
        LOCK(mutex_);
        stackFailed_ = true;
        FailLocked("not started: internal RAM low");
        return;
    }

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        LOCK(mutex_);
        stackFailed_ = true;
        FailLocked("nimble_port_init failed");
        return;
    }

    ble_hs_cfg.sync_cb = &BleThermostatManager::OnSyncTrampoline;
    ble_hs_cfg.reset_cb = &BleThermostatManager::OnResetTrampoline;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Pairing as the thermostat pairs: the gateway holds the install code and we type it
    // in, so we are the keyboard. MITM makes the pairing authenticated rather than
    // just-works, LE Secure Connections, and bonded so a reconnect needs no code.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();

    // The name a peer would read; the thermostat's own. Nothing is advertised.
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("KCThermostat");

    nimble_port_freertos_init(&BleThermostatManager::HostTask);

    const size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    {
        LOCK(mutex_);
        stackUp_ = true;
        heapAfterStart_ = after;
    }
    ESP_LOGI(TAG, "Stack up; internal RAM left: %u free, %u largest", static_cast<unsigned>(after),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void BleThermostatManager::Refresh(uint32_t nowMs)
{
    const bool want = enable_.Get();
    bool was, up;
    {
        LOCK(mutex_);
        was = enabled_;
        enabled_ = want;
        up = stackUp_;
    }
    if (want == was)
        return;

    if (!want)
    {
        bool wasScanning;
        {
            LOCK(mutex_);
            wasScanning = scanning_;
            scanning_ = false;
            pair_.pending = false;
            reconnectPending_ = false;
            graceRunning_ = false;
            away_ = false;
            rebooting_ = false;
        }
        if (wasScanning)
            ble_gap_disc_cancel();
        Terminate();
        ESP_LOGI(TAG, "Off: link dropped, nothing tried until ble.enable");
        return;
    }

    // On, as at a thermostat's boot.
    if (!up)
        StartStack();
    LoadTarget();

    // The mode choice is there at boot. A gateway on record gets two minutes to come and
    // read the mode; with none, nobody ever will.
    panel_.SetModeChoice(true);
    bool target;
    {
        LOCK(mutex_);
        target = haveTarget_;
        graceRunning_ = false;
        if (target)
        {
            StartModeGraceLocked(nowMs);
            ScheduleReconnectLocked(0);
        }
    }
    if (!target)
        panel_.SetModeChoice(false);
    ESP_LOGI(TAG, "On%s", target ? ", reconnecting the gateway on record" : ", no gateway on record");
}

void BleThermostatManager::LoadTarget()
{
    char stored[24] = {};
    peer_.Get(stored, sizeof(stored));
    char name[MAX_NAME + 1] = {};
    peerName_.Get(name, sizeof(name));
    const uint32_t id = peerId_.Get();

    ble_addr_t addr{};
    const bool valid = stored[0] != '\0' && ParseAddr(stored, static_cast<uint8_t>(peerType_.Get()), addr);
    if (stored[0] != '\0' && !valid)
        ESP_LOGW(TAG, "Stored gateway '%s' is no address, ignored", stored);

    LOCK(mutex_);
    haveTarget_ = valid;
    if (!valid)
        return;
    target_ = addr;
    snprintf(targetName_, sizeof(targetName_), "%s", name);
    targetId_ = id;
    ESP_LOGI(TAG, "Gateway on record: %s '%s' id %lu", stored, name, static_cast<unsigned long>(id));
}

void BleThermostatManager::Tick(uint32_t nowMs)
{
    bool enabled, up;
    {
        LOCK(mutex_);
        enabled = enabled_;
        up = stackUp_;
    }

    // The panel is there whenever BLE is on, linked or not: its mode gates the simulator.
    // Not every tick: it reads half a dozen settings, and the simulator asks once a second.
    if (Due(nowMs, nextApplyMs_))
    {
        nextApplyMs_ = nowMs + APPLY_MS;
        panel_.Apply(enabled);
    }
    if (!enabled || !up)
        return;

    bool graceOut = false, boot = false, connect = false;
    {
        LOCK(mutex_);
        if (graceRunning_ && Due(nowMs, graceEndsMs_))
        {
            graceRunning_ = false;
            graceOut = link_ != Link::Ready;   // back just in time otherwise
        }
        if (away_ && Due(nowMs, awayUntilMs_))
        {
            away_ = false;
            if (haveTarget_)
            {
                reconnectPending_ = true;
                reconnectAtMs_ = nowMs;
            }
        }
        if (rebooting_ && Due(nowMs, rebootUntilMs_))
        {
            rebooting_ = false;
            boot = true;
        }
        connect = reconnectPending_ && Due(nowMs, reconnectAtMs_) && synced_ && haveTarget_
               && link_ == Link::Down && !scanning_ && !away_ && !rebooting_ && !pair_.pending;
        if (connect)
            reconnectPending_ = false;
    }

    // Only the KC gateway learns the guest's mode, and only over this link. Out of reach
    // this long, the thermostat runs the way the one it replaces does.
    if (graceOut)
    {
        ESP_LOGW(TAG, "Gateway out of reach for %lu s: it picks the mode until it is back",
                 static_cast<unsigned long>(MODE_GRACE_MS / 1000));
        panel_.SetModeChoice(false);
    }

    // Back from the fake reboot: what a boot leaves, and straight to the gateway on record.
    if (boot)
    {
        panel_.OnBoot();
        bool target;
        {
            LOCK(mutex_);
            target = haveTarget_;
            graceRunning_ = false;
            if (target)
            {
                StartModeGraceLocked(nowMs);
                ScheduleReconnectLocked(0);
            }
        }
        if (!target)
            panel_.SetModeChoice(false);
        ESP_LOGI(TAG, "Back from the reboot");
    }

    ServePairRequest(nowMs);
    if (connect)
        StartConnect();
}

// ── Scanning and pairing ─────────────────────────────────────

bool BleThermostatManager::StartScan()
{
    uint8_t ownType;
    bool connecting;
    {
        LOCK(mutex_);
        if (!stackUp_ || !synced_)
            return false;
        if (scanning_)
            return true;
        // A fresh scan starts from an empty list: a gateway switched off disappears.
        for (Peer& p : peers_)
            p = Peer{};
        ownType = ownAddrType_;
        connecting = link_ == Link::Connecting;
        if (connecting)
            cancelingConnect_ = true;
        scanning_ = true;
        scanStartMs_ = NowMs();
    }

    // Scanning and connecting cannot run at once: an attempt in progress gives way, and
    // the reconnect waits for the scan to end.
    if (connecting)
        ble_gap_conn_cancel();

    // Active, for the name in the scan response, and duplicates not filtered: the
    // advertisement and the scan response arrive as two reports and both are wanted.
    ble_gap_disc_params params = {};
    params.passive = 0;
    params.filter_duplicates = 0;
    params.limited = 0;

    int rc = BLE_HS_EBUSY;
    for (int attempt = 0; attempt < 10 && (rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY); ++attempt)
    {
        rc = ble_gap_disc(ownType, SCAN_MS, &params, &BleThermostatManager::GapEventTrampoline, this);
        if (rc != 0)
            vTaskDelay(pdMS_TO_TICKS(20));   // a cancelled connect takes a moment to let go
    }
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        LOCK(mutex_);
        scanning_ = false;
        return false;
    }
    ESP_LOGI(TAG, "Scanning for %d ms", static_cast<int>(SCAN_MS));
    return true;
}

// One advertising report into the table. The gateway id comes from the advertisement's
// manufacturer data, the name from the scan response, so a gateway takes two reports.
void BleThermostatManager::NotePeer(const struct ble_gap_disc_desc& disc)
{
    ble_hs_adv_fields fields = {};
    if (ble_hs_adv_parse_fields(&fields, disc.data, disc.length_data) != 0)
        return;

    bool isGateway = false;
    uint32_t id = 0;
    if (fields.mfg_data != nullptr && fields.mfg_data_len >= kcble::MFG_DATA_LEN)
    {
        const uint16_t company = static_cast<uint16_t>(fields.mfg_data[0] | (fields.mfg_data[1] << 8));
        if (company == kcble::COMPANY_ID && fields.mfg_data[kcble::MFG_OFF_TYPE] == kcble::MFG_TYPE_GATEWAY_V1)
        {
            isGateway = true;
            const uint8_t* d = fields.mfg_data + kcble::MFG_OFF_ID;
            id = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8)
               | (static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);
        }
    }

    LOCK(mutex_);
    Peer* slot = nullptr;
    for (Peer& p : peers_)
        if (p.used && ble_addr_cmp(&p.addr, &disc.addr) == 0) { slot = &p; break; }

    if (slot == nullptr)
    {
        // A scan response from an address never heard advertising as a gateway is not ours.
        if (!isGateway)
            return;
        for (Peer& p : peers_)
            if (!p.used) { slot = &p; break; }
        if (slot == nullptr)
            return;   // a full table: eight gateways in range is already more than a bench has
        slot->used = true;
        slot->addr = disc.addr;
    }

    slot->rssi = disc.rssi;
    if (isGateway)
    {
        slot->gatewayId = id;
        slot->hasId = true;
    }
    if (fields.name != nullptr && fields.name_len > 0)
    {
        const size_t n = fields.name_len < MAX_NAME ? fields.name_len : MAX_NAME;
        memcpy(slot->name, fields.name, n);
        slot->name[n] = '\0';
    }
}

void BleThermostatManager::ServePairRequest(uint32_t nowMs)
{
    (void)nowMs;
    Peer chosen{};
    bool found = false, scan = false;
    char code[7] = {};
    {
        LOCK(mutex_);
        if (!pair_.pending || !synced_)
            return;

        if (pair_.byAddr)
        {
            // An address needs no scan; the name and id come along if a scan heard it.
            found = true;
            chosen.addr = pair_.addr;
            for (const Peer& p : peers_)
                if (p.used && ble_addr_cmp(&p.addr, &pair_.addr) == 0) { chosen = p; break; }
        }
        else if (pair_.byId)
        {
            for (const Peer& p : peers_)
                if (p.used && p.hasId && p.gatewayId == pair_.gatewayId) { chosen = p; found = true; break; }
            if (!found)
            {
                if (scanning_)
                    return;   // still listening
                if (pair_.scanned)
                {
                    pair_.pending = false;
                    FailLocked("gateway not heard in the scan");
                    return;
                }
                pair_.scanned = true;
                scan = true;
            }
        }
        else
        {
            // Neither given: the only gateway a whole scan hears.
            if (scanning_)
                return;
            int heard = 0;
            for (const Peer& p : peers_)
                if (p.used && p.hasId) { chosen = p; heard++; }
            if (heard == 1)
                found = true;
            else if (heard == 0 && !pair_.scanned)
            {
                pair_.scanned = true;
                scan = true;
            }
            else
            {
                pair_.pending = false;
                FailLocked(heard == 0 ? "no gateway heard"
                                      : "more than one gateway heard: name one with gateway= or addr=");
                return;
            }
        }

        if (found)
        {
            pair_.pending = false;
            memcpy(code, pair_.code, sizeof(code));
        }
    }

    if (scan)
    {
        if (!StartScan())
        {
            LOCK(mutex_);
            pair_.pending = false;
            FailLocked("scan could not start");
        }
        return;
    }
    if (found)
        Connect(chosen, code);
}

void BleThermostatManager::Connect(const Peer& peer, const char* code)
{
    char text[18];
    FormatAddr(peer.addr, text, sizeof(text));
    const uint32_t id = peer.hasId ? peer.gatewayId : 0;

    bool wasScanning, connecting;
    uint16_t handle;
    {
        LOCK(mutex_);
        target_ = peer.addr;
        haveTarget_ = true;
        snprintf(targetName_, sizeof(targetName_), "%s", peer.name);
        targetId_ = id;
        // Text up to here: "000000" is a valid passkey that a numeric 0 could not tell
        // from "none given".
        havePasskey_ = true;
        passkey_ = static_cast<uint32_t>(strtoul(code, nullptr, 10));
        passkeyUsed_ = false;
        wasScanning = scanning_;
        scanning_ = false;
        handle = connHandle_;
        connecting = link_ == Link::Connecting;
        if (connecting)
            cancelingConnect_ = true;
        reconnectPending_ = true;   // as soon as the radio is free
        reconnectAtMs_ = NowMs();
    }

    // Remembered before the attempt, as the thermostat does: a pairing that fails halfway
    // still leaves it retrying the gateway that was picked.
    peer_.Set(text);
    peerType_.Set(peer.addr.type);
    peerName_.Set(peer.name);
    peerId_.Set(id);
    app_.getStrux().getSettingsManager().Save();

    if (wasScanning)
        ble_gap_disc_cancel();
    if (connecting)
        ble_gap_conn_cancel();
    // Linked already, to this gateway or another: that link goes first.
    if (handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);

    ESP_LOGI(TAG, "Pairing with %s '%s' id %lu", text, peer.name, static_cast<unsigned long>(id));
}

void BleThermostatManager::StartConnect()
{
    uint8_t ownType;
    ble_addr_t addr;
    {
        LOCK(mutex_);
        if (!haveTarget_ || !synced_ || link_ != Link::Down)
            return;
        ownType = ownAddrType_;
        addr = target_;
        link_ = Link::Connecting;
    }

    char text[18];
    FormatAddr(addr, text, sizeof(text));
    ESP_LOGI(TAG, "Connecting to %s (type %u)", text, addr.type);

    // The thermostat's connection parameters: 15-20 ms, no latency, 4 s supervision.
    ble_gap_conn_params params = {};
    params.scan_itvl = 16;              // 10 ms, in 0.625 ms
    params.scan_window = 16;
    params.itvl_min = 12;               // 15 ms, in 1.25 ms
    params.itvl_max = 16;               // 20 ms
    params.latency = 0;
    params.supervision_timeout = 400;   // 4 s, in 10 ms
    params.min_ce_len = 0;
    params.max_ce_len = 0;

    const int rc = ble_gap_connect(ownType, &addr, CONNECT_TIMEOUT_MS, &params,
                                   &BleThermostatManager::GapEventTrampoline, this);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        LOCK(mutex_);
        link_ = Link::Down;
        FailLocked("connect could not start");
        ScheduleReconnectLocked(RECONNECT_MS);
    }
}

void BleThermostatManager::ScheduleReconnectLocked(uint32_t delayMs)
{
    // Never given up on: a gateway switched off for a week is picked up when it returns.
    // One already due sooner stands.
    const uint32_t at = NowMs() + delayMs;
    if (reconnectPending_ && static_cast<int32_t>(reconnectAtMs_ - at) <= 0)
        return;
    reconnectPending_ = true;
    reconnectAtMs_ = at;
}

// Not restarted while it is counting, or once it has run out: a failed reconnect every
// few seconds must not keep pushing the moment back.
void BleThermostatManager::StartModeGraceLocked(uint32_t nowMs)
{
    if (graceRunning_ || !panel_.ModeChoice())
        return;
    graceRunning_ = true;
    graceEndsMs_ = nowMs + MODE_GRACE_MS;
}

void BleThermostatManager::FailLocked(const char* why)
{
    snprintf(lastFailure_, sizeof(lastFailure_), "%s", why);
    lastFailureMs_ = NowMs();
}

void BleThermostatManager::DropLink(const char* why)
{
    uint16_t handle;
    {
        LOCK(mutex_);
        handle = connHandle_;
        connHandle_ = BLE_HS_CONN_HANDLE_NONE;
        link_ = Link::Down;
        mtu_ = 23;
        svcStart_ = svcEnd_ = 0;
        inboundHandle_ = outboundHandle_ = inboundCccd_ = 0;
        FailLocked(why);
        ScheduleReconnectLocked(RECONNECT_MS);
    }
    if (handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGW(TAG, "Link down: %s", why);
}

void BleThermostatManager::Terminate()
{
    uint16_t handle;
    bool connecting;
    {
        LOCK(mutex_);
        handle = connHandle_;
        connecting = link_ == Link::Connecting;
        if (connecting)
            cancelingConnect_ = true;
    }
    if (connecting)
        ble_gap_conn_cancel();
    if (handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
}

void BleThermostatManager::Forget()
{
    ble_addr_t addr;
    bool had, up;
    {
        LOCK(mutex_);
        addr = target_;
        had = haveTarget_;
        up = stackUp_;
        haveTarget_ = false;
        targetName_[0] = '\0';
        targetId_ = 0;
        havePasskey_ = false;
        passkey_ = 0;
        passkeyUsed_ = false;
        reconnectPending_ = false;
        graceRunning_ = false;
        pair_.pending = false;
    }

    peer_.Set("");
    peerType_.Set(0);
    peerName_.Set("");
    peerId_.Set(0);
    app_.getStrux().getSettingsManager().Save();

    // No gateway left to hear the guest's mode, and none coming back.
    panel_.SetModeChoice(false);
    Terminate();

    // The bond too, or pairing this gateway again would resume the old keys instead of
    // asking for the code.
    if (had && up)
        ble_store_util_delete_peer(&addr);
    ESP_LOGI(TAG, "Gateway forgotten");
}

// The install code was wrong. Nothing got paired, so the gateway on record goes: kept,
// the same wrong code would go out every few seconds, and again after every reboot.
void BleThermostatManager::RejectInstallCode()
{
    ESP_LOGW(TAG, "Wrong install code, not retrying");
    Forget();
    LOCK(mutex_);
    FailLocked("wrong code");
}

// ── GATT discovery: service, characteristics, the inbound CCCD, subscribe ──

void BleThermostatManager::StartDiscovery()
{
    uint16_t handle;
    {
        LOCK(mutex_);
        handle = connHandle_;
        link_ = Link::Discovering;
        svcStart_ = svcEnd_ = 0;
        inboundHandle_ = outboundHandle_ = inboundCccd_ = 0;
    }
    if (ble_gattc_disc_svc_by_uuid(handle, &kcble::SERVICE_UUID.u, &BleThermostatManager::SvcDiscTrampoline, this) != 0)
        DropLink("service discovery failed to start");
}

int BleThermostatManager::OnSvcDisc(const struct ble_gatt_error* error, const struct ble_gatt_svc* svc)
{
    if (error->status == 0 && svc != nullptr)
    {
        LOCK(mutex_);
        svcStart_ = svc->start_handle;
        svcEnd_ = svc->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE)
    {
        ESP_LOGE(TAG, "Service discovery error: %d", error->status);
        DropLink("service discovery error");
        return 0;
    }

    uint16_t handle, start, end;
    {
        LOCK(mutex_);
        handle = connHandle_;
        start = svcStart_;
        end = svcEnd_;
    }
    if (start == 0)
    {
        DropLink("service not found");
        return 0;
    }
    // One pass over every characteristic, matching the UUIDs here.
    if (ble_gattc_disc_all_chrs(handle, start, end, &BleThermostatManager::ChrDiscTrampoline, this) != 0)
        DropLink("characteristic discovery failed to start");
    return 0;
}

int BleThermostatManager::OnChrDisc(const struct ble_gatt_error* error, const struct ble_gatt_chr* chr)
{
    if (error->status == 0 && chr != nullptr)
    {
        LOCK(mutex_);
        if (ble_uuid_cmp(&chr->uuid.u, &kcble::CHR_INBOUND_UUID.u) == 0)
            inboundHandle_ = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &kcble::CHR_OUTBOUND_UUID.u) == 0)
            outboundHandle_ = chr->val_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE)
    {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
        DropLink("characteristic discovery error");
        return 0;
    }

    uint16_t handle, inbound, outbound, end;
    {
        LOCK(mutex_);
        handle = connHandle_;
        inbound = inboundHandle_;
        outbound = outboundHandle_;
        end = svcEnd_;
    }
    if (inbound == 0 || outbound == 0)
    {
        DropLink("incomplete service");
        return 0;
    }
    // The CCCD is a descriptor of the inbound characteristic: the first after it.
    if (ble_gattc_disc_all_dscs(handle, inbound, end, &BleThermostatManager::DscDiscTrampoline, this) != 0)
        DropLink("descriptor discovery failed to start");
    return 0;
}

int BleThermostatManager::OnDscDisc(const struct ble_gatt_error* error, const struct ble_gatt_dsc* dsc)
{
    if (error->status == 0 && dsc != nullptr)
    {
        const ble_uuid16_t cccd = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);
        LOCK(mutex_);
        if (inboundCccd_ == 0 && ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0)
            inboundCccd_ = dsc->handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE)
    {
        ESP_LOGE(TAG, "Descriptor discovery error: %d", error->status);
        DropLink("descriptor discovery error");
        return 0;
    }

    uint16_t handle, cccd;
    {
        LOCK(mutex_);
        handle = connHandle_;
        cccd = inboundCccd_;
    }
    if (cccd == 0)
    {
        DropLink("no CCCD");
        return 0;
    }
    static const uint8_t NOTIFY_ON[2] = { 0x01, 0x00 };
    if (ble_gattc_write_flat(handle, cccd, NOTIFY_ON, sizeof(NOTIFY_ON), &BleThermostatManager::SubscribeTrampoline, this) != 0)
        DropLink("subscribe write failed to start");
    return 0;
}

int BleThermostatManager::OnSubscribed(const struct ble_gatt_error* error)
{
    if (error->status != 0)
    {
        ESP_LOGE(TAG, "Subscribe failed: %d", error->status);
        DropLink("subscribe failed");
        return 0;
    }

    uint16_t mtu;
    {
        LOCK(mutex_);
        link_ = Link::Ready;
        readySinceMs_ = NowMs();
        graceRunning_ = false;
        mtu = mtu_;
    }
    // The gateway can read the guest's mode again.
    panel_.SetModeChoice(true);
    ESP_LOGI(TAG, "Link READY (mtu %u)", mtu);
    return 0;
}

// ── GAP events, on NimBLE's host task ────────────────────────

void BleThermostatManager::OnSync()
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }
    uint8_t type = 0;
    rc = ble_hs_id_infer_auto(0, &type);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    {
        LOCK(mutex_);
        ownAddrType_ = type;
        synced_ = true;
        if (haveTarget_)
            ScheduleReconnectLocked(0);
    }
    ESP_LOGI(TAG, "Controller synced (own address type %u)", type);
}

void BleThermostatManager::EnqueueChunk(const struct os_mbuf* om)
{
    // Host task: copy out and return. A full queue drops the chunk and says so, so the
    // stream it belongs to ends on the hole instead of going on without it.
    Chunk chunk;
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(om, chunk.data, sizeof(chunk.data), &len) != 0)
    {
        ESP_LOGW(TAG, "Inbound chunk longer than %u bytes, dropped", static_cast<unsigned>(sizeof(chunk.data)));
        return;
    }
    chunk.len = len;
    if (xQueueSend(inQueue_, &chunk, 0) != pdTRUE)
    {
        inboundDropped_.store(true, std::memory_order_relaxed);
        ESP_LOGW(TAG, "Inbound queue full, chunk dropped");
    }
}

int BleThermostatManager::OnGapEvent(struct ble_gap_event* event)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
        NotePeer(event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE: {
        int heard = 0;
        {
            LOCK(mutex_);
            scanning_ = false;
            for (const Peer& p : peers_)
                if (p.used) heard++;
        }
        ESP_LOGI(TAG, "Scan complete (reason %d), %d gateway(s) heard", event->disc_complete.reason, heard);
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0)
        {
            LOCK(mutex_);
            link_ = Link::Down;
            if (cancelingConnect_)
                cancelingConnect_ = false;   // our own doing, not a failure
            else
            {
                char why[40];
                snprintf(why, sizeof(why), "connect failed (status %d)", event->connect.status);
                FailLocked(why);
                ESP_LOGW(TAG, "%s", why);
            }
            ScheduleReconnectLocked(RECONNECT_MS);
            return 0;
        }

        const uint16_t handle = event->connect.conn_handle;
        {
            LOCK(mutex_);
            cancelingConnect_ = false;
            connHandle_ = handle;
            link_ = Link::Securing;
            passkeyUsed_ = false;
        }
        ESP_LOGI(TAG, "Connected (handle %u), securing", handle);

        // The MTU is the chunk size. Then encryption before anything is discovered: a
        // first pairing asks for the passkey here, a bond is resumed silently.
        ble_gattc_exchange_mtu(handle, nullptr, nullptr);
        if (ble_gap_security_initiate(handle) != 0)
            DropLink("security could not be started");
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        const uint32_t now = NowMs();
        {
            LOCK(mutex_);
            connHandle_ = BLE_HS_CONN_HANDLE_NONE;
            link_ = Link::Down;
            mtu_ = 23;
            inboundHandle_ = outboundHandle_ = inboundCccd_ = 0;
            passkeyUsed_ = false;
            readySinceMs_ = 0;
            ScheduleReconnectLocked(RECONNECT_MS);
            StartModeGraceLocked(now);
        }
        // A reply waiting on its write ends now rather than after its timeout.
        writeStatus_.store(BLE_HS_ENOTCONN, std::memory_order_relaxed);
        xSemaphoreGive(writeDone_);
        ESP_LOGW(TAG, "Disconnected, reason 0x%x", event->disconnect.reason);
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        ble_gap_conn_desc desc{};
        if (event->enc_change.status != 0 || ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0)
        {
            const int status = event->enc_change.status;
            ESP_LOGW(TAG, "Encryption failed: status 0x%x", status);
            // Confirm values that disagree are the passkey disagreeing, whichever side
            // checked first: a wrong install code, which no retry can fix.
            if (status == BLE_HS_SM_US_ERR(BLE_SM_ERR_CONFIRM_MISMATCH)
                || status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_CONFIRM_MISMATCH))
            {
                RejectInstallCode();
                return 0;
            }
            DropLink("encryption failed");
            return 0;
        }

        // The whole authentication decision: encrypted is not enough, the passkey must
        // really have been used. A just-works link gets nothing.
        if (!desc.sec_state.encrypted || !desc.sec_state.authenticated)
        {
            ESP_LOGE(TAG, "Refusing link: encrypted=%d authenticated=%d",
                     desc.sec_state.encrypted, desc.sec_state.authenticated);
            DropLink("link not authenticated");
            return 0;
        }

        // A pairing that used the passkey proved the install code: kept. A bonded
        // reconnect proves nothing new.
        char proven[7] = {};
        {
            LOCK(mutex_);
            if (passkeyUsed_)
                snprintf(proven, sizeof(proven), "%06lu", static_cast<unsigned long>(passkey_));
            havePasskey_ = false;
            passkey_ = 0;
            passkeyUsed_ = false;
        }
        if (proven[0] != '\0')
        {
            code_.Set(proven);
            app_.getStrux().getSettingsManager().Save();
        }
        ESP_LOGI(TAG, "Link encrypted and authenticated (bonded=%d)", desc.sec_state.bonded);
        StartDiscovery();
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.params.action != BLE_SM_IOACT_INPUT)
        {
            ESP_LOGE(TAG, "Unexpected passkey action %d: the gateway holds the passkey and we enter it",
                     event->passkey.params.action);
            return 0;
        }
        uint32_t key;
        bool have;
        {
            LOCK(mutex_);
            key = passkey_;
            have = havePasskey_;
        }
        if (!have)
        {
            // As the thermostat: the bond is gone on the gateway's side, and only a new
            // `ble pair` with the code can fix it.
            DropLink("gateway asked for the code, none given: bond gone? ble pair again");
            return 0;
        }
        ble_sm_io io = {};
        io.action = BLE_SM_IOACT_INPUT;
        io.passkey = key;
        const int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
        ESP_LOGI(TAG, "Passkey submitted (rc %d)", rc);
        if (rc == 0)
        {
            LOCK(mutex_);
            passkeyUsed_ = true;
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU: {
        LOCK(mutex_);
        mtu_ = event->mtu.value;
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        EnqueueChunk(event->notify_rx.om);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The gateway pairs again while we hold a bond for it: ours goes, or the link
        // would stay stuck on stale keys.
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

// ── Trampolines ──────────────────────────────────────────────

void BleThermostatManager::HostTask(void* param)
{
    (void)param;
    nimble_port_run();   // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void BleThermostatManager::OnSyncTrampoline()
{
    if (s_instance)
        s_instance->OnSync();
}

void BleThermostatManager::OnResetTrampoline(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
    if (s_instance == nullptr)
        return;
    LOCK(s_instance->mutex_);
    s_instance->synced_ = false;
    s_instance->scanning_ = false;
    s_instance->connHandle_ = BLE_HS_CONN_HANDLE_NONE;
    s_instance->link_ = Link::Down;
    s_instance->ScheduleReconnectLocked(RECONNECT_MS);
}

int BleThermostatManager::GapEventTrampoline(struct ble_gap_event* event, void* arg)
{
    auto* self = static_cast<BleThermostatManager*>(arg);
    return self ? self->OnGapEvent(event) : 0;
}

int BleThermostatManager::SvcDiscTrampoline(uint16_t, const struct ble_gatt_error* error,
                                            const struct ble_gatt_svc* svc, void* arg)
{
    auto* self = static_cast<BleThermostatManager*>(arg);
    return self ? self->OnSvcDisc(error, svc) : 0;
}

int BleThermostatManager::ChrDiscTrampoline(uint16_t, const struct ble_gatt_error* error,
                                            const struct ble_gatt_chr* chr, void* arg)
{
    auto* self = static_cast<BleThermostatManager*>(arg);
    return self ? self->OnChrDisc(error, chr) : 0;
}

int BleThermostatManager::DscDiscTrampoline(uint16_t, const struct ble_gatt_error* error, uint16_t,
                                            const struct ble_gatt_dsc* dsc, void* arg)
{
    auto* self = static_cast<BleThermostatManager*>(arg);
    return self ? self->OnDscDisc(error, dsc) : 0;
}

int BleThermostatManager::SubscribeTrampoline(uint16_t, const struct ble_gatt_error* error,
                                              struct ble_gatt_attr*, void* arg)
{
    auto* self = static_cast<BleThermostatManager*>(arg);
    return self ? self->OnSubscribed(error) : 0;
}

int BleThermostatManager::WriteDoneTrampoline(uint16_t, const struct ble_gatt_error* error,
                                              struct ble_gatt_attr*, void* arg)
{
    auto* self = static_cast<BleThermostatManager*>(arg);
    if (self == nullptr)
        return 0;
    self->writeStatus_.store(error != nullptr ? error->status : 0, std::memory_order_relaxed);
    xSemaphoreGive(self->writeDone_);
    return 0;
}

// ── Sessions ─────────────────────────────────────────────────

void BleThermostatManager::Serve(const Chunk& first)
{
    if (first.len < kcble::HEADER_LEN)
        return;

    // A request can beat our own bring-up: a bonded gateway has its CCCD restored the
    // moment the link is encrypted, and asks while we are still discovering -- measured
    // at ~1.5 s on the real pair. Held, as the thermostat holds it, not dropped.
    size_t cap = 0;
    bool ready = false;
    for (uint32_t waited = 0; ; waited += 50)
    {
        {
            LOCK(mutex_);
            ready = link_ == Link::Ready;
            // One chunk is one write: the MTU less the ATT header and ours.
            cap = mtu_ > 6 ? mtu_ - 6 : 0;
            if (cap > kcble::CHUNK_MAX - kcble::HEADER_LEN)
                cap = kcble::CHUNK_MAX - kcble::HEADER_LEN;
        }
        if (ready || waited >= READY_WAIT_MS)
            break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!ready || cap == 0)
    {
        ESP_LOGW(TAG, "Request dropped: the link never became ready");
        return;
    }

    const uint16_t sid = kcble::ReadU16(first.data);
    const uint8_t flags = first.data[2];
    inboundDropped_.store(false, std::memory_order_relaxed);   // a drop belongs to its own stream

    // The JSON line: the first chunk's payload up to its newline. What follows it, if
    // anything, is the start of a body.
    const uint8_t* payload = first.data + kcble::HEADER_LEN;
    const size_t len = first.len - kcble::HEADER_LEN;
    const uint8_t* nl = static_cast<const uint8_t*>(memchr(payload, '\n', len));
    const size_t lineLen = nl != nullptr ? static_cast<size_t>(nl - payload) : len;
    char line[161];
    const size_t take = lineLen < sizeof(line) - 1 ? lineLen : sizeof(line) - 1;
    memcpy(line, payload, take);
    line[take] = '\0';
    const uint8_t* rest = nl != nullptr ? nl + 1 : payload + len;

    SessionBody body(*this, sid, rest, static_cast<size_t>(payload + len - rest), (flags & kcble::FLAG_FINAL) != 0);
    SessionReply reply(*this, sid, cap);
    char reason[96] = {};
    switch (panel_.Serve(line, body, reply, reason, sizeof(reason)))
    {
    case ThermostatPanel::Outcome::Finish:
        reply.Finish();
        break;
    case ThermostatPanel::Outcome::Reject:
        reply.Reject(reason);
        break;
    case ThermostatPanel::Outcome::Reboot:
        FakeReboot();
        return;
    }

    // What is still queued is residue, not the next request: a session that ended early
    // leaves its body behind, and read as a header it would invent a command.
    int stale = 0;
    while (xQueueReceive(inQueue_, bodyChunk_, 0) == pdTRUE)
        stale++;
    if (stale > 0)
        ESP_LOGW(TAG, "Discarded %d stale chunk(s) left over from the last request", stale);

    // This task's stack is internal RAM: say once per new low what it has left.
    static UBaseType_t worst = ~static_cast<UBaseType_t>(0);
    const UBaseType_t headroom = uxTaskGetStackHighWaterMark(nullptr);
    if (headroom < worst)
    {
        worst = headroom;
        ESP_LOGI(TAG, "Task stack headroom low-water: %u bytes of %d", static_cast<unsigned>(headroom), TASK_STACK);
    }
}

size_t BleThermostatManager::SessionBody::Next(const uint8_t*& data)
{
    if (firstLen > 0)
    {
        data = first;
        const size_t n = firstLen;
        firstLen = 0;
        return n;
    }
    while (!ended && !failed)
    {
        // A dropped chunk is a hole nothing downstream could see: the stream ends here.
        if (m.inboundDropped_.load(std::memory_order_relaxed))
        {
            ESP_LOGE(TAG, "A chunk of this stream was dropped, ending it");
            failed = true;
            break;
        }
        if (xQueueReceive(m.inQueue_, m.bodyChunk_, pdMS_TO_TICKS(RECV_TIMEOUT_MS)) != pdTRUE)
        {
            ESP_LOGW(TAG, "No further chunk within %lu ms, ending the stream", static_cast<unsigned long>(RECV_TIMEOUT_MS));
            failed = true;
            break;
        }
        const Chunk& c = *m.bodyChunk_;
        if (c.len < kcble::HEADER_LEN || kcble::ReadU16(c.data) != sid)
        {
            ESP_LOGE(TAG, "Chunk of another session in the middle of a stream, ending it");
            failed = true;
            break;
        }
        ended = (c.data[2] & kcble::FLAG_FINAL) != 0;
        const size_t n = c.len - kcble::HEADER_LEN;
        if (n == 0)
            continue;   // the empty FINAL that ends a body
        data = c.data + kcble::HEADER_LEN;
        return n;
    }
    return 0;
}

void BleThermostatManager::SessionReply::Write(const char* text, size_t n)
{
    while (n > 0)
    {
        const size_t take = n < cap - len ? n : cap - len;
        memcpy(m.outFrame_ + kcble::HEADER_LEN + len, text, take);
        len += take;
        text += take;
        n -= take;
        if (len == cap)
            Send(0);   // full: a chunk that is not the last
    }
}

void BleThermostatManager::SessionReply::Flush()
{
    if (len > 0)
        Send(0);
}

void BleThermostatManager::SessionReply::Send(uint8_t flags)
{
    kcble::WriteHeader(m.outFrame_, sid, flags);
    if (!m.SendRaw(m.outFrame_, kcble::HEADER_LEN + len))
        failed = true;
    len = 0;
}

void BleThermostatManager::SessionReply::Reject(const char* reason)
{
    const size_t n = strlen(reason) < cap ? strlen(reason) : cap;
    memcpy(m.outFrame_ + kcble::HEADER_LEN, reason, n);
    len = n;
    Send(kcble::FLAG_REJECT);
}

bool BleThermostatManager::SendRaw(const uint8_t* frame, size_t len)
{
    uint16_t conn, handle;
    {
        LOCK(mutex_);
        conn = connHandle_;
        handle = outboundHandle_;
    }
    if (conn == BLE_HS_CONN_HANDLE_NONE || handle == 0)
        return false;

    xSemaphoreTake(writeDone_, 0);   // a stale completion

    // One write in flight, each waiting for its acknowledgement: that is the flow
    // control. BUSY and ENOMEM pass as earlier procedures and buffers complete.
    int rc = BLE_HS_EBUSY;
    for (int attempt = 0; attempt < 60 && (rc == BLE_HS_EBUSY || rc == BLE_HS_ENOMEM); ++attempt)
    {
        rc = ble_gattc_write_flat(conn, handle, frame, len, &BleThermostatManager::WriteDoneTrampoline, this);
        if (rc == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Write failed: %d", rc);
        return false;
    }
    if (xSemaphoreTake(writeDone_, pdMS_TO_TICKS(WRITE_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Write never completed within %lu ms", static_cast<unsigned long>(WRITE_TIMEOUT_MS));
        return false;
    }
    return writeStatus_.load(std::memory_order_relaxed) == 0;
}

// The thermostat's `system reboot` buffers {"ok":true}, waits half a second and
// restarts, so its answer never leaves -- the gateway expects none. Nothing is sent here
// either. Then the link goes, and the thermostat stays away as long as a boot takes.
void BleThermostatManager::FakeReboot()
{
    ESP_LOGI(TAG, "system reboot: away for %lu ms", static_cast<unsigned long>(REBOOT_REPLY_MS + REBOOT_AWAY_MS));
    vTaskDelay(pdMS_TO_TICKS(REBOOT_REPLY_MS));
    {
        LOCK(mutex_);
        rebooting_ = true;
        rebootUntilMs_ = NowMs() + REBOOT_AWAY_MS;
    }
    Terminate();
    xQueueReset(inQueue_);
}

// ── Helpers ──────────────────────────────────────────────────

void BleThermostatManager::FormatAddr(const ble_addr_t& addr, char* out, size_t cap)
{
    // Little-endian on air; people read it most significant byte first.
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr.val[5], addr.val[4], addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
}

bool BleThermostatManager::ParseAddr(const char* text, uint8_t type, ble_addr_t& out)
{
    unsigned b[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    for (int i = 0; i < 6; ++i)
    {
        if (b[i] > 0xFF)
            return false;
        out.val[5 - i] = static_cast<uint8_t>(b[i]);
    }
    out.type = type;
    return true;
}

bool BleThermostatManager::IsInstallCode(const char* text)
{
    if (strlen(text) != 6)
        return false;
    for (const char* p = text; *p != '\0'; ++p)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

// ── Command handlers ─────────────────────────────────────────

void BleThermostatManager::WriteState(CommandContext& ctx)
{
    const uint32_t now = NowMs();

    // Copied out under the lock; NimBLE is asked nothing while it is held.
    bool enabled, up, failed, synced, scanning, haveTarget, graceRunning, away, rebooting;
    Link link;
    ble_addr_t target;
    char targetName[MAX_NAME + 1];
    uint32_t targetId, readySince, graceEnds, awayUntil, rebootUntil, lastFailureMs;
    uint16_t conn, mtu;
    char lastFailure[sizeof(lastFailure_)];
    size_t heapAtStart, largestAtStart, heapAfterStart;
    Peer peers[MAX_PEERS];
    {
        LOCK(mutex_);
        enabled = enabled_;
        up = stackUp_;
        failed = stackFailed_;
        synced = synced_;
        scanning = scanning_;
        haveTarget = haveTarget_;
        graceRunning = graceRunning_;
        away = away_;
        rebooting = rebooting_;
        link = link_;
        target = target_;
        memcpy(targetName, targetName_, sizeof(targetName));
        targetId = targetId_;
        readySince = readySinceMs_;
        graceEnds = graceEndsMs_;
        awayUntil = awayUntilMs_;
        rebootUntil = rebootUntilMs_;
        conn = connHandle_;
        mtu = mtu_;
        memcpy(lastFailure, lastFailure_, sizeof(lastFailure));
        lastFailureMs = lastFailureMs_;
        heapAtStart = heapAtStart_;
        largestAtStart = largestAtStart_;
        heapAfterStart = heapAfterStart_;
        memcpy(peers, peers_, sizeof(peers));
    }

    bool bonded = false;
    if (up && haveTarget)
    {
        ble_store_key_sec key = {};
        key.peer_addr = target;
        ble_store_value_sec value = {};
        bonded = ble_store_read_peer_sec(&key, &value) == 0;
    }
    int8_t rssi = 0;
    const bool haveRssi = conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_rssi(conn, &rssi) == 0;

    const char* state = "idle";
    if (!enabled)                state = "off";
    else if (failed)             state = "failed";
    else if (!up || !synced)     state = "starting";
    else if (rebooting)          state = "rebooting";
    else
    {
        switch (link)
        {
        case Link::Ready:       state = "ready"; break;
        case Link::Discovering: state = "discovering"; break;
        case Link::Securing:    state = "pairing"; break;
        case Link::Connecting:  state = "connecting"; break;
        case Link::Down:        state = scanning ? "scanning" : (away || haveTarget) ? "away" : "idle"; break;
        }
    }

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    const bool wanted = enable_.Get();
    resp.field("enabled", wanted);
    // Set since a boot that had it off: BLE's RAM went back to the heap then.
    resp.field("rebootNeeded", wanted && released_);
    resp.field("state", state);
    resp.field("stackUp", up);

    char addr[18] = {};
    if (haveTarget)
        FormatAddr(target, addr, sizeof(addr));
    resp.field("peer", addr);
    if (haveTarget)
    {
        resp.field("peerType", static_cast<uint32_t>(target.type));
        resp.field("peerName", targetName);
        resp.field("gatewayId", targetId);
    }
    resp.field("bonded", bonded);
    if (link == Link::Ready && readySince != 0)
    {
        resp.field("connectedFor", (now - readySince) / 1000);
        resp.field("mtu", static_cast<uint32_t>(mtu));
    }
    if (haveRssi)
        resp.field("rssi", static_cast<int32_t>(rssi));
    if (graceRunning)
        resp.field("graceLeftS", static_cast<uint32_t>(static_cast<int32_t>(graceEnds - now) > 0 ? (graceEnds - now) / 1000 : 0));
    if (away)
        resp.field("awayLeftS", static_cast<uint32_t>(static_cast<int32_t>(awayUntil - now) > 0 ? (awayUntil - now) / 1000 : 0));
    if (rebooting)
        resp.field("rebootLeftMs", static_cast<uint32_t>(static_cast<int32_t>(rebootUntil - now) > 0 ? rebootUntil - now : 0));
    resp.field("lastFailure", lastFailure);
    if (lastFailureMs != 0)
        resp.field("lastFailureAgoMs", now - lastFailureMs);

    panel_.Report(resp, now);

    {
        auto heap = resp.object("heap");
        heap.field("internalFree", static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        heap.field("internalLargest", static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        if (heapAtStart != 0)
        {
            heap.field("beforeStack", static_cast<uint32_t>(heapAtStart));
            heap.field("beforeStackLargest", static_cast<uint32_t>(largestAtStart));
            heap.field("afterStack", static_cast<uint32_t>(heapAfterStart));
        }
    }

    resp.field("scanning", scanning);
    auto list = resp.array("scan");
    for (const Peer& p : peers)
    {
        if (!p.used)
            continue;
        char a[18];
        FormatAddr(p.addr, a, sizeof(a));
        auto o = list.object();
        o.field("addr", a);
        o.field("addrType", static_cast<uint32_t>(p.addr.type));
        o.field("name", p.name);
        if (p.hasId)
            o.field("gatewayId", p.gatewayId);
        o.field("rssi", static_cast<int32_t>(p.rssi));
    }
}

CommandResult BleThermostatManager::Cmd_Status(CommandContext& ctx)
{
    WriteState(ctx);
    return CommandResult::Ok;
}

const char* BleThermostatManager::Unavailable() const
{
    if (!enable_.Get())
        return released_ ? "BLE is off: ble enable on=true, then reboot" : "BLE is off: ble enable on=true";
    if (released_)
        return "BLE was off at boot: reboot to start it";
    return nullptr;
}

CommandResult BleThermostatManager::Cmd_Scan(CommandContext& ctx)
{
    if (const char* error = Unavailable())
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", error);
        return CommandResult::Ok;
    }
    {
        LOCK(mutex_);
        lastFailure_[0] = '\0';
        lastFailureMs_ = 0;
    }
    if (!StartScan())
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "scan could not start: the stack is not up yet, or the radio is busy");
        return CommandResult::Ok;
    }
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult BleThermostatManager::Cmd_Pair(CommandContext& ctx)
{
    // Which code, which gateway and whether BLE is on are meaning, not form: replies.
    char code[8] = {};
    if (ctx.has(codeArg))
        snprintf(code, sizeof(code), "%s", ctx.arg(codeArg));
    else
        code_.Get(code, sizeof(code));

    const char* error = Unavailable();
    ble_addr_t addr{};
    if (error == nullptr && !IsInstallCode(code))
        error = "the code is six digits";
    if (error == nullptr && ctx.has(addrArg)
        && !ParseAddr(ctx.arg(addrArg), static_cast<uint8_t>(ctx.has(addrTypeArg) ? ctx.arg(addrTypeArg) : 0), addr))
        error = "addr is aa:bb:cc:dd:ee:ff";
    if (error != nullptr)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", error);
        return CommandResult::Ok;
    }

    {
        LOCK(mutex_);
        pair_ = PairRequest{};
        pair_.pending = true;
        pair_.byAddr = ctx.has(addrArg);
        pair_.addr = addr;
        pair_.byId = !pair_.byAddr && ctx.has(gatewayArg);
        pair_.gatewayId = pair_.byId ? ctx.arg(gatewayArg) : 0;
        memcpy(pair_.code, code, sizeof(pair_.code) - 1);
        lastFailure_[0] = '\0';
        lastFailureMs_ = 0;
    }
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult BleThermostatManager::Cmd_Forget(CommandContext& ctx)
{
    Forget();
    {
        LOCK(mutex_);
        lastFailure_[0] = '\0';
        lastFailureMs_ = 0;
    }
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult BleThermostatManager::Cmd_Enable(CommandContext& ctx)
{
    // The task picks it up within a second, as it would `settings set`; after a boot that
    // had it off, only the next boot does ('rebootNeeded').
    enable_.Set(ctx.arg(onArg));
    app_.getStrux().getSettingsManager().Save();
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult BleThermostatManager::Cmd_Mode(CommandContext& ctx)
{
    ThermostatPanel::Mode mode;
    if (!ThermostatPanel::ParseMode(ctx.arg(modeArg), mode))
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "mode is off, heat, cool or auto");
        return CommandResult::Ok;
    }
    panel_.SetPick(mode);
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult BleThermostatManager::Cmd_Drop(CommandContext& ctx)
{
    const uint32_t seconds = ctx.arg(secondsArg);
    {
        LOCK(mutex_);
        away_ = seconds > 0;
        awayUntilMs_ = NowMs() + seconds * 1000;
    }
    Terminate();
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult BleThermostatManager::Cmd_Delay(CommandContext& ctx)
{
    panel_.SetDelay(ctx.arg(msArg), ctx.has(delayTypeArg) ? ctx.arg(delayTypeArg) : "");
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult BleThermostatManager::Cmd_Reject(CommandContext& ctx)
{
    panel_.SetReject(ctx.has(rejectTypeArg) ? ctx.arg(rejectTypeArg) : "");
    WriteState(ctx);
    return CommandResult::Ok;
}
