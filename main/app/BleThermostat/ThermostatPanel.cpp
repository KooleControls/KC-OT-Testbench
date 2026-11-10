#include "ThermostatPanel.h"
#include "SettingsManager.h"
#include "ReplyWriter.h"
#include "JsonHelpers.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// The types the KC thermostat answers, in the order `ble status` counts them. The
// counters have one slot more, for everything else.
constexpr const char* TYPES[] = {
    "system info", "climate status", "climate set", "bletest ping",
    "partition status", "partition clear", "partition write", "partition activate", "system reboot",
};
static_assert(sizeof(TYPES) / sizeof(TYPES[0]) == 9, "TYPE_COUNT in the header");

// The KC thermostat's own setpoint range, and the frost setpoint Off regulates against.
constexpr float SETPOINT_MIN = 5.0f;
constexpr float SETPOINT_MAX = 30.0f;
constexpr float FROST = ThermostatSimulator::FROST_SETPOINT;

// The KC thermostat's app slots. Its partitions are 4 MB on a 16 MB part; a write past
// that is refused as the thermostat would refuse it.
constexpr const char* SLOTS[] = { "ota_0", "ota_1" };
constexpr uint32_t SLOT_SIZE = 0x400000;

// A write flushes {"p":<bytes>} this often, as the thermostat's does. A gateway writes
// 4 KB a request, so it never sees one; a longer write would.
constexpr uint32_t REPORT_EVERY = 32 * 1024;

// An ESP-IDF app image: the header's first byte, and the app description that starts
// the first segment (24-byte image header, 8-byte segment header) with its version 16
// bytes in.
constexpr uint8_t IMAGE_MAGIC = 0xE9;
constexpr uint32_t APP_DESC_MAGIC = 0xABCD5432;
constexpr size_t APP_DESC_OFFSET = 32;
constexpr size_t APP_VERSION_OFFSET = APP_DESC_OFFSET + 16;
constexpr size_t APP_VERSION_LEN = 32;

// The thermostat pads a ping's answer to the size of a real status reply, so a ping
// costs the radio what a command costs it.
constexpr const char* PING_PAD =
    "0123456789012345678901234567890123456789"
    "0123456789012345678901234567890123456789"
    "0123456789012345678901234567890123456789"
    "0123456789012345678901234567890123456789";

const char* Bool(bool v) { return v ? "true" : "false"; }

/// A JSON string value, quoted; quotes and backslashes escaped, control characters
/// dropped. Settings are typed by people.
void AppendString(FixBuf& b, const char* s)
{
    b.append("\"");
    for (const char* p = s; *p != '\0'; ++p)
    {
        if (*p == '"' || *p == '\\')
        {
            const char esc[2] = { '\\', *p };
            b.append(esc, 2);
        }
        else if (static_cast<unsigned char>(*p) >= 0x20)
        {
            b.append(p, 1);
        }
    }
    b.append("\"");
}

/// The route, as the thermostat reads it: "type" from a JSON line, or the first two
/// words of a console line. False when it is not two words.
bool Route(const char* line, char* type, size_t cap)
{
    char raw[64] = {};
    if (line[0] == '{')
    {
        if (!ExtractJsonString(line, "type", raw, sizeof(raw)))
            return false;
    }
    else
    {
        snprintf(raw, sizeof(raw), "%s", line);
    }

    // Category and command, split on the first run of whitespace, joined by one space.
    auto isSep = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    const char* p = raw;
    const char* catEnd = p;
    while (*catEnd != '\0' && !isSep(*catEnd)) ++catEnd;
    const char* cmd = catEnd;
    while (*cmd != '\0' && isSep(*cmd)) ++cmd;
    const char* cmdEnd = cmd;
    while (*cmdEnd != '\0' && !isSep(*cmdEnd)) ++cmdEnd;
    if (catEnd == p || cmdEnd == cmd)
        return false;

    snprintf(type, cap, "%.*s %.*s", static_cast<int>(catEnd - p), p, static_cast<int>(cmdEnd - cmd), cmd);
    return true;
}

/// A number from the line; false when the key is absent or not a number.
bool ReadNumber(const char* line, const char* key, double& out)
{
    const char* v = FindJsonField(line, key);
    if (v == nullptr)
        return false;
    char* end = nullptr;
    out = strtod(v, &end);
    return end != v;
}

} // namespace

void ThermostatPanel::Init(ThermostatSimulator& sim, SettingsManager& settings)
{
    sim_ = &sim;
    settings_ = &settings;
    settings.Register({ &mode_, &firmware_ });

    // In PSRAM: internal RAM is what the BLE controller and the task stacks need.
    reply_ = static_cast<char*>(heap_caps_malloc(REPLY_LEN, MALLOC_CAP_SPIRAM));
    if (reply_ == nullptr)
        reply_ = static_cast<char*>(malloc(REPLY_LEN));

    const uint32_t stored = mode_.Get();
    LOCK(mutex_);
    pick_ = stored <= static_cast<uint32_t>(Mode::Auto) ? static_cast<Mode>(stored) : Mode::Auto;
}

// ── The mode ─────────────────────────────────────────────────

bool ThermostatPanel::ParseMode(const char* name, Mode& out)
{
    // The thermostat's ParseClimateMode: the names, or their numbers.
    if (!strcmp(name, "off")  || !strcmp(name, "0")) { out = Mode::Off;  return true; }
    if (!strcmp(name, "heat") || !strcmp(name, "1")) { out = Mode::Heat; return true; }
    if (!strcmp(name, "cool") || !strcmp(name, "2")) { out = Mode::Cool; return true; }
    if (!strcmp(name, "auto") || !strcmp(name, "3")) { out = Mode::Auto; return true; }
    return false;
}

const char* ThermostatPanel::ModeName(Mode mode)
{
    switch (mode)
    {
    case Mode::Off:  return "off";
    case Mode::Heat: return "heat";
    case Mode::Cool: return "cool";
    case Mode::Auto: return "auto";
    }
    return "off";
}

// Off is Off either way. Without the choice, any other pick is the plant's one
// direction. With it, the pick -- except on a plant that cannot cool, where heating is
// the only direction there is.
ThermostatPanel::Mode ThermostatPanel::InForceLocked() const
{
    if (pick_ == Mode::Off) return pick_;
    if (!modeChoice_ || !coolingSupported_) return DirectionLocked();
    return pick_;
}

bool ThermostatPanel::ModeChoice()
{
    LOCK(mutex_);
    return modeChoice_;
}

void ThermostatPanel::SetModeChoice(bool choice)
{
    LOCK(mutex_);
    if (choice == modeChoice_)
        return;
    modeChoice_ = choice;
    if (choice)
    {
        ESP_LOGI(TAG, "Mode choice back: the %s pick is in force", ModeName(pick_));
        return;
    }
    ESP_LOGI(TAG, "Mode choice withdrawn: running in %s, %s pick kept",
             ModeName(InForceLocked()), ModeName(pick_));
    if (pick_ == Mode::Off)
        HoldFrostLocked();
}

void ThermostatPanel::SetPick(Mode mode)
{
    LOCK(mutex_);
    SetPickLocked(mode);
}

void ThermostatPanel::SetPickLocked(Mode mode)
{
    if (mode == pick_)
        return;
    pick_ = mode;
    StorePickLocked();
    if (!modeChoice_ && pick_ == Mode::Off)
        HoldFrostLocked();
}

// Clamped to the thermostat's range. Without the choice, Off is the setpoint held at
// 5 °C, so raising it ends Off the way turning a Honeywell up from 5 does.
void ThermostatPanel::SetUserSetpointLocked(float celsius, float current)
{
    if (celsius < SETPOINT_MIN) celsius = SETPOINT_MIN;
    if (celsius > SETPOINT_MAX) celsius = SETPOINT_MAX;
    if (fabsf(celsius - current) < 0.001f)
        return;
    sim_->SetSetpoint(celsius);
    if (!modeChoice_ && pick_ == Mode::Off && celsius > FROST)
    {
        pick_ = DirectionLocked();
        StorePickLocked();
        ESP_LOGI(TAG, "Setpoint raised to %.1f: Off ends, running in %s", celsius, ModeName(pick_));
    }
}

// Off without the choice: over OpenTherm the only way to say it is the setpoint, so it
// goes to 5 °C, on the panel and on the wire alike.
void ThermostatPanel::HoldFrostLocked()
{
    if (fabsf(sim_->ReadPanel().setpoint - FROST) < 0.001f)
        return;
    sim_->SetSetpoint(FROST);
    ESP_LOGI(TAG, "Off without the mode choice: setpoint held at %.0f", FROST);
}

void ThermostatPanel::StorePickLocked()
{
    mode_.Set(static_cast<uint32_t>(pick_));
    settings_->Save();
}

void ThermostatPanel::Apply(bool active)
{
    if (!active)
    {
        sim_->SetModeGate(ThermostatSimulator::ModeGate::None);
        return;
    }

    Mode inForce;
    {
        // Read under the lock: SetModeChoice() holds it while it holds the setpoint at 5,
        // and a setpoint read before that must not look raised after it.
        LOCK(mutex_);
        const ThermostatSimulator::Panel p = sim_->ReadPanel();
        coolingSupported_ = p.coolingSupported;   // decides the direction without the choice

        // Off without the choice waits for exactly this: a setpoint above 5 °C, from an
        // override the simulator followed or a hand at tsim.setpoint. One at 5 °C agrees
        // with Off and keeps it.
        if (!modeChoice_ && pick_ == Mode::Off && p.setpoint > FROST + 0.001f)
        {
            pick_ = DirectionLocked();
            StorePickLocked();
            ESP_LOGI(TAG, "Setpoint at %.1f: Off ends, running in %s", p.setpoint, ModeName(pick_));
        }
        inForce = InForceLocked();
    }

    using Gate = ThermostatSimulator::ModeGate;
    switch (inForce)
    {
    case Mode::Off:  sim_->SetModeGate(Gate::Off); break;
    case Mode::Heat: sim_->SetModeGate(Gate::Heat); break;
    case Mode::Cool: sim_->SetModeGate(Gate::Cool); break;
    case Mode::Auto: sim_->SetModeGate(Gate::Auto); break;
    }
}

void ThermostatPanel::OnBoot()
{
    LOCK(mutex_);
    modeChoice_ = true;
    if (push_.state != Push::State::Activated)
        return;

    // The activated slot is what boots, and its version is what the thermostat says it
    // runs from now on -- kept, as flashed firmware is.
    snprintf(running_, sizeof(running_), "%s", push_.slot);
    firmware_.Set(push_.image);
    settings_->Save();
    push_.state = Push::State::Booted;
    ESP_LOGI(TAG, "Booted the pushed image from %s: %s", running_, push_.image);
}

ThermostatPanel::Climate ThermostatPanel::ReadClimate()
{
    const ThermostatSimulator::Panel p = sim_->ReadPanel();
    Climate c;
    {
        LOCK(mutex_);
        coolingSupported_ = p.coolingSupported;
        c.inForce = InForceLocked();
        c.choice = modeChoice_;
    }
    c.coolingSupported = p.coolingSupported;
    c.userSetpoint = p.setpoint;
    c.activeSetpoint = c.inForce == Mode::Off ? FROST : p.setpoint;
    c.room = p.room;
    c.ch = p.ch;
    c.cool = p.cool;
    // The simulator asks for a flow temperature rather than running a PID: it asks for
    // tsim.flow whenever it asks for heat.
    c.pidOutput = p.ch ? 1.0f : 0.0f;
    c.tSet = p.ch ? p.flow : 0.0f;
    c.overrideActive = c.inForce != Mode::Off && p.overrideKnown && p.remoteOverride > 0.0f;
    return c;
}

// ── Requests ─────────────────────────────────────────────────

ThermostatPanel::Outcome ThermostatPanel::Serve(const char* line, Body& body, Reply& out,
                                                char* reason, size_t reasonCap)
{
    const uint32_t now = NowMs();
    char type[TYPE_LEN] = {};
    const bool routed = Route(line, type, sizeof(type));

    uint32_t delayMs = 0;
    bool refuse = false;
    {
        LOCK(mutex_);
        snprintf(lastRequest_, sizeof(lastRequest_), "%s", line);
        lastRequestMs_ = now;
        size_t i = 0;
        while (i < TYPE_COUNT && (!routed || strcmp(type, TYPES[i]) != 0)) ++i;
        counts_[i]++;
        refuse = routed && rejectType_[0] != '\0' && strcmp(type, rejectType_) == 0;
        if (delayMs_ > 0 && (delayType_[0] == '\0' || (routed && strcmp(type, delayType_) == 0)))
            delayMs = delayMs_;
    }

    if (reply_ == nullptr)
    {
        snprintf(reason, reasonCap, "out of memory");
        return Outcome::Reject;
    }
    FixBuf b(reply_, REPLY_LEN);

    Outcome outcome = Outcome::Finish;
    if (!routed)
    {
        snprintf(reason, reasonCap, "expected: <category> <command>");
        outcome = Outcome::Reject;
    }
    else if (refuse)
    {
        // As a thermostat whose firmware has no such command: the fault `ble reject`.
        snprintf(reason, reasonCap, "unknown command");
        outcome = Outcome::Reject;
    }
    else if (!strcmp(type, "system info"))        WriteSystemInfo(b);
    else if (!strcmp(type, "climate status"))     WriteClimate(b);
    else if (!strcmp(type, "climate set"))        outcome = ClimateSet(line, b);
    else if (!strcmp(type, "bletest ping"))       outcome = Ping(line, b, reason, reasonCap);
    else if (!strcmp(type, "partition status"))   WritePartitionStatus(b);
    else if (!strcmp(type, "partition clear"))    outcome = PartitionClear(line, b, reason, reasonCap);
    else if (!strcmp(type, "partition write"))    outcome = PartitionWrite(line, body, out, b, reason, reasonCap);
    else if (!strcmp(type, "partition activate")) outcome = PartitionActivate(line, b, reason, reasonCap);
    else if (!strcmp(type, "system reboot"))      return Outcome::Reboot;
    else
    {
        snprintf(reason, reasonCap, "unknown command");
        outcome = Outcome::Reject;
    }

    if (outcome == Outcome::Reject)
    {
        LOCK(mutex_);
        rejected_++;
    }

    // Late, as a thermostat busy with something else would be: the fault `ble delay`.
    // After the body, so a streamed write is slow to answer rather than slow to take.
    if (delayMs > 0)
        vTaskDelay(pdMS_TO_TICKS(delayMs));

    if (outcome == Outcome::Finish && b.len > 0)
        out.Write(b.data, static_cast<size_t>(b.len));
    return outcome;
}

void ThermostatPanel::Firmware(char* out, size_t cap) const
{
    firmware_.Get(out, cap);
}

void ThermostatPanel::WriteSystemInfo(FixBuf& b)
{
    // The thermostat's own fields in its order. The build facts are this testbench's,
    // as nothing else is running; "chip" is what the KC thermostat is.
    const esp_app_desc_t* app = esp_app_get_description();
    char firmware[40];
    Firmware(firmware, sizeof(firmware));

    char deviceTime[32] = "Not synced";
    const time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    if (local.tm_year + 1900 >= 2020)
        strftime(deviceTime, sizeof(deviceTime), "%F %T", &local);

    b.append("{\"name\":\"Thermostat\",\"project\":\"Thermostat\",\"firmware\":");
    AppendString(b, firmware);
    b.appendf(",\"idf\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"chip\":\"esp32s3\","
              "\"heapFree\":%lu,\"heapMin\":%lu,\"deviceTime\":\"%s\"}",
              app->idf_ver, app->date, app->time,
              static_cast<unsigned long>(esp_get_free_heap_size()),
              static_cast<unsigned long>(esp_get_minimum_free_heap_size()), deviceTime);
}

void ThermostatPanel::WriteClimate(FixBuf& b)
{
    const Climate c = ReadClimate();
    b.appendf("{\"mode\":\"%s\",\"modeChoice\":%s,\"userSetpoint\":%.2f,\"activeSetpoint\":%.2f,"
              "\"roomTemp\":%.2f,\"roomValid\":true,\"pidOutput\":%.2f,\"tSet\":%.2f,"
              "\"chEnable\":%s,\"coolEnable\":%s,\"overrideActive\":%s}",
              ModeName(c.inForce), Bool(c.choice), static_cast<double>(c.userSetpoint),
              static_cast<double>(c.activeSetpoint), static_cast<double>(c.room),
              static_cast<double>(c.pidOutput), static_cast<double>(c.tSet),
              Bool(c.ch), Bool(c.cool), Bool(c.overrideActive));
}

ThermostatPanel::Outcome ThermostatPanel::ClimateSet(const char* line, FixBuf& b)
{
    // The panel's rules, as the thermostat's handler applies them: the setpoint first,
    // then the mode, so a request carrying both ends where it asked. An unknown mode
    // leaves the mode alone and is answered with the status anyway.
    char mode[8] = {};
    const bool hasMode = ExtractJsonString(line, "mode", mode, sizeof(mode));
    double setpoint = 0;
    const bool hasSetpoint = ReadNumber(line, "setpoint", setpoint);

    {
        LOCK(mutex_);
        const ThermostatSimulator::Panel p = sim_->ReadPanel();
        coolingSupported_ = p.coolingSupported;
        if (hasSetpoint)
            SetUserSetpointLocked(static_cast<float>(setpoint), p.setpoint);
        Mode m;
        if (hasMode && ParseMode(mode, m))
            SetPickLocked(m);
        if (hasMode)
            snprintf(lastSet_, sizeof(lastSet_), "%s", mode);
    }

    // The simulator asks in the new mode from its next status on, and the answer below
    // already shows what it will ask. A request only comes over BLE, so BLE is on.
    Apply(true);
    WriteClimate(b);
    return Outcome::Finish;
}

ThermostatPanel::Outcome ThermostatPanel::Ping(const char* line, FixBuf& b, char* reason, size_t reasonCap)
{
    double seq = 0;
    if (!ReadNumber(line, "seq", seq))
    {
        snprintf(reason, reasonCap, "missing required argument: seq");
        return Outcome::Reject;
    }

    // "seq" is never last: the gateway matches `"seq":N,` so ping 12 is not taken for
    // an echo of ping 123. No site test runs here, so "test" is false and the gateway
    // keeps its slow pace.
    b.appendf("{\"ok\":true,\"seq\":%lu,\"test\":false,\"pad\":\"%s\"}",
              static_cast<unsigned long>(seq), PING_PAD);
    return Outcome::Finish;
}

// ── Firmware push ────────────────────────────────────────────

const char* ThermostatPanel::ResolveSlot(const char* label) const
{
    // Called under the lock (running_).
    if (strcmp(label, SLOTS[0]) != 0 && strcmp(label, SLOTS[1]) != 0)
        return "unknown partition";
    if (strcmp(label, running_) == 0)
        return "partition is running";
    return nullptr;
}

void ThermostatPanel::WritePartitionStatus(FixBuf& b)
{
    char firmware[40];
    Firmware(firmware, sizeof(firmware));
    LOCK(mutex_);
    const char* next = strcmp(running_, SLOTS[0]) == 0 ? SLOTS[1] : SLOTS[0];
    b.append("{\"firmware\":");
    AppendString(b, firmware);
    b.appendf(",\"running\":\"%s\",\"nextSlot\":\"%s\"}", running_, next);
}

void ThermostatPanel::ResetPushLocked(const char* slot)
{
    push_ = Push{};
    snprintf(push_.slot, sizeof(push_.slot), "%s", slot);
}

ThermostatPanel::Outcome ThermostatPanel::PartitionClear(const char* line, FixBuf& b, char* reason, size_t reasonCap)
{
    char label[17] = {};
    if (!ExtractJsonString(line, "partition", label, sizeof(label)))
    {
        snprintf(reason, reasonCap, "missing required argument: partition");
        return Outcome::Reject;
    }

    LOCK(mutex_);
    if (const char* err = ResolveSlot(label))
    {
        b.appendf("{\"ok\":false,\"error\":\"%s\"}", err);
        return Outcome::Finish;
    }
    ResetPushLocked(label);
    push_.state = Push::State::Cleared;
    b.append("{\"ok\":true}");
    return Outcome::Finish;
}

// Writes are addressed, so the image is followed by where each begins: the next block
// extends the hash; the last block again (a retry after a lost answer) puts back the
// hash from before it and takes it anew; anything else leaves a gap the hash cannot
// cover, and the image is then not one that would boot.
void ThermostatPanel::BeginWriteLocked(const char* slot, uint32_t offset)
{
    if (strcmp(push_.slot, slot) != 0)
        ResetPushLocked(slot);   // written without a clear: the record starts here

    push_.state = Push::State::Writing;
    push_.writes++;
    if (!push_.gap && offset == push_.bytes)
    {
        push_.beforeLast = push_.sha;
        push_.lastOffset = offset;
        hashing_ = true;
    }
    else if (!push_.gap && offset == push_.lastOffset && offset < push_.bytes)
    {
        push_.sha = push_.beforeLast;
        push_.bytes = offset;
        push_.rewrites++;
        hashing_ = true;
    }
    else
    {
        push_.gap = true;
        hashing_ = false;
    }
    writePos_ = offset;
}

void ThermostatPanel::AccountLocked(const uint8_t* data, size_t len)
{
    if (writePos_ < HEAD_LEN)
    {
        const size_t n = (len < HEAD_LEN - writePos_) ? len : HEAD_LEN - writePos_;
        memcpy(push_.head + writePos_, data, n);
    }
    if (hashing_)
    {
        push_.sha.Update(data, len);
        push_.bytes += static_cast<uint32_t>(len);
    }
    writePos_ += static_cast<uint32_t>(len);
}

ThermostatPanel::Outcome ThermostatPanel::PartitionWrite(const char* line, Body& body, Reply& out, FixBuf& b,
                                                         char* reason, size_t reasonCap)
{
    char label[17] = {};
    if (!ExtractJsonString(line, "partition", label, sizeof(label)))
    {
        snprintf(reason, reasonCap, "missing required argument: partition");
        return Outcome::Reject;
    }
    // No offset is the thermostat's one-shot upload from 0; the gateway always sends one.
    double offsetArg = 0;
    const uint32_t offset = ReadNumber(line, "offset", offsetArg) ? static_cast<uint32_t>(offsetArg) : 0;

    // Refused before the body, as the thermostat refuses it: the rest of the body is
    // left for the transport to drop.
    {
        LOCK(mutex_);
        const char* err = ResolveSlot(label);
        if (err == nullptr && offset >= SLOT_SIZE)
            err = "offset beyond partition";
        if (err != nullptr)
        {
            b.appendf("{\"ok\":false,\"error\":\"%s\"}", err);
            return Outcome::Finish;
        }
        BeginWriteLocked(label, offset);
    }

    const uint8_t* data = nullptr;
    size_t n;
    uint32_t written = 0, reported = 0;
    while ((n = body.Next(data)) > 0)
    {
        if (offset + written + n > SLOT_SIZE)
        {
            LOCK(mutex_);
            push_.state = Push::State::Failed;
            snprintf(push_.error, sizeof(push_.error), "write past the slot");
            b.append("{\"ok\":false,\"error\":\"write failed\"}");
            return Outcome::Finish;
        }
        {
            LOCK(mutex_);
            AccountLocked(data, n);
        }
        written += static_cast<uint32_t>(n);
        if (written - reported >= REPORT_EVERY)
        {
            char progress[24];
            const int len = snprintf(progress, sizeof(progress), "{\"p\":%lu}", static_cast<unsigned long>(written));
            out.Write(progress, static_cast<size_t>(len));
            out.Flush();
            reported = written;
        }
    }

    // A stream that broke ended the way a complete one does; only the transport knows.
    if (body.Failed())
    {
        LOCK(mutex_);
        push_.state = Push::State::Failed;
        snprintf(push_.error, sizeof(push_.error), "stream failed at %lu bytes", static_cast<unsigned long>(written));
        b.appendf("{\"ok\":false,\"error\":\"stream failed at %lu bytes\"}", static_cast<unsigned long>(written));
        return Outcome::Finish;
    }

    b.appendf("{\"ok\":true,\"offset\":%lu,\"size\":%lu}",
              static_cast<unsigned long>(offset), static_cast<unsigned long>(written));
    return Outcome::Finish;
}

void ThermostatPanel::ImageVersionLocked(char* out, size_t cap) const
{
    // A real app image names itself in its app description; anything else is named by
    // its hash.
    const uint8_t* d = push_.head + APP_DESC_OFFSET;
    const uint32_t magic = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8)
                         | (static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);
    if (push_.bytes >= HEAD_LEN && magic == APP_DESC_MAGIC)
    {
        char version[APP_VERSION_LEN + 1] = {};
        memcpy(version, push_.head + APP_VERSION_OFFSET, APP_VERSION_LEN);
        if (version[0] != '\0')
        {
            snprintf(out, cap, "%s", version);
            return;
        }
    }
    char hex[2 * Sha256::DIGEST_LEN + 1];
    push_.sha.DigestHex(hex);
    snprintf(out, cap, "pushed-%.8s", hex);
}

ThermostatPanel::Outcome ThermostatPanel::PartitionActivate(const char* line, FixBuf& b, char* reason, size_t reasonCap)
{
    char label[17] = {};
    if (!ExtractJsonString(line, "partition", label, sizeof(label)))
    {
        snprintf(reason, reasonCap, "missing required argument: partition");
        return Outcome::Reject;
    }

    LOCK(mutex_);
    if (const char* err = ResolveSlot(label))
    {
        b.appendf("{\"ok\":false,\"error\":\"%s\"}", err);
        return Outcome::Finish;
    }

    // What the bootloader would check of it, as far as a count and a hash can: a whole
    // image, from 0, starting the way an app image starts.
    const bool written = push_.state == Push::State::Writing || push_.state == Push::State::Activated;
    const bool valid = strcmp(label, push_.slot) == 0 && written
                    && !push_.gap && push_.bytes > 0 && push_.head[0] == IMAGE_MAGIC;
    if (!valid)
    {
        if (strcmp(label, push_.slot) == 0)
        {
            push_.state = Push::State::Failed;
            snprintf(push_.error, sizeof(push_.error), "image validation failed");
        }
        b.append("{\"ok\":false,\"error\":\"image validation failed\"}");
        return Outcome::Finish;
    }

    ImageVersionLocked(push_.image, sizeof(push_.image));
    push_.state = Push::State::Activated;
    ESP_LOGI(TAG, "Image of %lu bytes activated in %s: %s",
             static_cast<unsigned long>(push_.bytes), push_.slot, push_.image);
    b.append("{\"ok\":true}");
    return Outcome::Finish;
}

// ── Faults ───────────────────────────────────────────────────

void ThermostatPanel::SetDelay(uint32_t ms, const char* type)
{
    LOCK(mutex_);
    delayMs_ = ms;
    snprintf(delayType_, sizeof(delayType_), "%s", ms > 0 ? type : "");
}

void ThermostatPanel::SetReject(const char* type)
{
    LOCK(mutex_);
    snprintf(rejectType_, sizeof(rejectType_), "%s", type);
}

// ── Report ───────────────────────────────────────────────────

void ThermostatPanel::Report(ReplyObject& out, uint32_t nowMs)
{
    const Climate c = ReadClimate();
    char firmware[40];
    Firmware(firmware, sizeof(firmware));

    LOCK(mutex_);
    out.field("mode", ModeName(pick_));
    out.field("modeChoice", modeChoice_);
    out.field("reportedMode", ModeName(c.inForce));
    out.field("coolingSupported", c.coolingSupported);
    out.field("firmware", firmware);

    {
        auto o = out.object("climate");
        o.field("userSetpoint", c.userSetpoint);
        o.field("activeSetpoint", c.activeSetpoint);
        o.field("roomTemp", c.room);
        o.field("chEnable", c.ch);
        o.field("coolEnable", c.cool);
        o.field("overrideActive", c.overrideActive);
    }
    {
        auto o = out.object("requests");
        for (size_t i = 0; i < TYPE_COUNT; ++i)
            o.field(TYPES[i], counts_[i]);
        o.field("other", counts_[TYPE_COUNT]);
    }
    out.field("rejected", rejected_);
    out.field("lastRequest", lastRequest_);
    if (lastRequestMs_ != 0)
        out.field("lastRequestAgoMs", nowMs - lastRequestMs_);
    out.field("lastSet", lastSet_);
    {
        auto o = out.object("faults");
        o.field("delayMs", delayMs_);
        o.field("delayCommand", delayType_);
        o.field("reject", rejectType_);
    }
    {
        auto o = out.object("push");
        const char* state = "idle";
        switch (push_.state)
        {
        case Push::State::Idle:      state = "idle"; break;
        case Push::State::Cleared:   state = "cleared"; break;
        case Push::State::Writing:   state = "writing"; break;
        case Push::State::Activated: state = "activated"; break;
        case Push::State::Booted:    state = "booted"; break;
        case Push::State::Failed:    state = "failed"; break;
        }
        o.field("state", state);
        o.field("running", running_);
        o.field("slot", push_.slot);
        o.field("bytes", push_.bytes);
        o.field("writes", push_.writes);
        o.field("rewrites", push_.rewrites);
        o.field("gap", push_.gap);
        char hex[2 * Sha256::DIGEST_LEN + 1];
        push_.sha.DigestHex(hex);
        o.field("sha256", hex);
        if (push_.image[0] != '\0')
            o.field("image", push_.image);
        if (push_.error[0] != '\0')
            o.field("error", push_.error);
    }
}
