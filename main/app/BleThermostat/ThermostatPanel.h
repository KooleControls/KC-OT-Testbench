#pragma once

#include "TypedSettings.h"
#include "Mutex.h"
#include "ThermostatSimulator.h"
#include "Sha256.h"
#include <cstddef>
#include <cstdint>

class SettingsManager;
class ReplyObject;
struct FixBuf;

// ──────────────────────────────────────────────────────────────
// What the KC thermostat answers a gateway over BLE, without the radio: the panel of
// a thermostat whose OpenTherm side is the ThermostatSimulator.
//
// A request is one line of JSON routed on its "type", and the answers are the KC
// thermostat's own, field for field and in its order, because the gateway reads them
// with strstr on `"key":"`: `system info`, `climate status` and `climate set`,
// `bletest ping`, and the firmware push (`partition status|clear|write|activate`,
// `system reboot`). Anything else is refused as the thermostat refuses it, with
// "unknown command".
//
// The mode follows the KC thermostat's ClimateManager rule for rule. The guest picks
// off, heat, cool or auto (ble.mode); whether that pick is in force is the mode choice,
// which only a gateway on the link can give: the BLE manager withdraws it after two
// minutes without one, and gives it back when the link is up. Without it the mode in
// force is the plant's one direction -- auto where it can cool, heat where it cannot --
// and Off holds the setpoint at 5 °C, the only way OpenTherm can say Off, until a
// raised setpoint (an override, or a hand at tsim.setpoint) ends it. A plant that
// cannot cool runs every pick but Off as heat. The mode in force is what the gateway
// reads, and what is handed to the simulator as its gate.
//
// A firmware push is taken and not written: the bytes are counted and hashed, the
// image's own version is read from its app description, and the fake reboot after
// `system reboot` comes back reporting it. Nothing touches this testbench's flash.
//
// Serve() runs on the BLE manager's task; Report() and the setters on whichever task
// serves a command, hence the lock.
// ──────────────────────────────────────────────────────────────
class ThermostatPanel
{
    static constexpr const char* TAG = "ThermostatPanel";

public:
    /// The guest's pick, numbered as the KC thermostat stores it (its climate.mode).
    enum class Mode : uint8_t { Off = 0, Heat = 1, Cool = 2, Auto = 3 };

    /// A request's body, after its JSON line: the rest of the first chunk, then every
    /// further chunk of the session until the FINAL one.
    class Body
    {
    public:
        /// The next piece; 0 at the end of the request, or when the link broke.
        virtual size_t Next(const uint8_t*& data) = 0;
        /// Whether the end was the link breaking rather than the request ending.
        virtual bool Failed() const = 0;

    protected:
        ~Body() = default;
    };

    /// Where a reply goes. Chunked and closed by the transport.
    class Reply
    {
    public:
        virtual void Write(const char* text, size_t len) = 0;
        /// What is written so far goes out now, as a chunk that is not the last.
        virtual void Flush() = 0;

    protected:
        ~Reply() = default;
    };

    /// How a request ends: a FINAL chunk, a REJECT, or no answer at all and the
    /// thermostat restarting -- the KC thermostat's `system reboot` buffers its reply
    /// and restarts before it is sent.
    enum class Outcome : uint8_t { Finish, Reject, Reboot };

    void Init(ThermostatSimulator& sim, SettingsManager& settings);

    /// One request; `line` is its JSON line without the newline. On Reject, `reason`
    /// holds the reason the thermostat would give.
    Outcome Serve(const char* line, Body& body, Reply& out, char* reason, size_t reasonCap);

    // ── The mode ─────────────────────────────────────────────
    bool ModeChoice();
    void SetModeChoice(bool choice);

    /// A mode tile pressed on the panel. The KC thermostat's SetMode: the pick only; a
    /// tile does not give back the choice, so without it heat, cool or auto waits.
    void SetPick(Mode mode);

    static bool ParseMode(const char* name, Mode& out);
    static const char* ModeName(Mode mode);

    /// Once per tick, from the BLE manager's task. `active`: whether there is a panel at
    /// all (BLE enabled); without one the simulator's gate is None. With one, it hands
    /// the simulator the mode in force, and ends Off once the setpoint is raised.
    void Apply(bool active);

    /// What a restart leaves: the choice back as at every boot, and an activated image
    /// running.
    void OnBoot();

    // ── Faults, not saved ────────────────────────────────────
    /// Answer `type` (every request when empty) `ms` late; 0 clears.
    void SetDelay(uint32_t ms, const char* type);
    /// Refuse `type` with "unknown command"; empty clears.
    void SetReject(const char* type);

    /// For `ble status`.
    void Report(ReplyObject& out, uint32_t nowMs);

private:
    static constexpr size_t TYPE_LEN = 32;
    static constexpr size_t LINE_LEN = 160;
    static constexpr size_t REPLY_LEN = 640;
    static constexpr size_t HEAD_LEN = 80;     // image header + segment header + app description up to its version
    static constexpr size_t TYPE_COUNT = 9;    // the types the thermostat answers; one more counts the rest

    struct Climate
    {
        Mode inForce = Mode::Auto;
        bool choice = true;
        float userSetpoint = 0, activeSetpoint = 0, room = 0, pidOutput = 0, tSet = 0;
        bool ch = false, cool = false, overrideActive = false, coolingSupported = false;
    };

    // A firmware push, as far as it got.
    struct Push
    {
        enum class State : uint8_t { Idle, Cleared, Writing, Activated, Booted, Failed };
        State state = State::Idle;
        char slot[8] = {};
        uint32_t bytes = 0;         // the image from offset 0 without a gap, all hashed
        uint32_t writes = 0, rewrites = 0;
        bool gap = false;           // a write that was neither the next block nor the last one again
        uint32_t lastOffset = 0;    // where the last write began, and the hash before it, for a retry
        Sha256 sha, beforeLast;
        uint8_t head[HEAD_LEN] = {};
        char image[33] = {};        // its version, once activated
        char error[40] = {};
    };

    ThermostatSimulator* sim_ = nullptr;
    SettingsManager* settings_ = nullptr;
    char* reply_ = nullptr;         // REPLY_LEN, in PSRAM: only Serve() builds into it

    Mutex mutex_;
    Mode pick_ = Mode::Auto;
    bool modeChoice_ = true;
    bool coolingSupported_ = false; // as of the last Apply()

    Push push_;
    char running_[8] = "ota_0";
    bool hashing_ = false;          // the write in progress extends the hash
    uint32_t writePos_ = 0;

    uint32_t counts_[TYPE_COUNT + 1] = {};
    uint32_t rejected_ = 0;
    char lastRequest_[LINE_LEN] = {};
    uint32_t lastRequestMs_ = 0;
    char lastSet_[8] = {};

    uint32_t delayMs_ = 0;
    char delayType_[TYPE_LEN] = {};
    char rejectType_[TYPE_LEN] = {};

    Mode InForceLocked() const;
    Mode DirectionLocked() const { return coolingSupported_ ? Mode::Auto : Mode::Heat; }
    void SetPickLocked(Mode mode);
    void SetUserSetpointLocked(float celsius, float current);
    void HoldFrostLocked();
    void StorePickLocked();
    Climate ReadClimate();

    void WriteSystemInfo(FixBuf& b);
    void WriteClimate(FixBuf& b);
    Outcome ClimateSet(const char* line, FixBuf& b);
    Outcome Ping(const char* line, FixBuf& b, char* reason, size_t reasonCap);
    void WritePartitionStatus(FixBuf& b);
    Outcome PartitionClear(const char* line, FixBuf& b, char* reason, size_t reasonCap);
    Outcome PartitionWrite(const char* line, Body& body, Reply& out, FixBuf& b, char* reason, size_t reasonCap);
    Outcome PartitionActivate(const char* line, FixBuf& b, char* reason, size_t reasonCap);
    const char* ResolveSlot(const char* label) const;
    void ResetPushLocked(const char* slot);
    void BeginWriteLocked(const char* slot, uint32_t offset);
    void AccountLocked(const uint8_t* data, size_t len);
    void ImageVersionLocked(char* out, size_t cap) const;
    void Firmware(char* out, size_t cap) const;

    // ── Settings (keys at most 15 characters) ──
    inline static UInt32Setting mode_{ "ble.mode", "BLE Thermostat Mode (0 off, 1 heat, 2 cool, 3 auto)",
                                       static_cast<uint32_t>(Mode::Auto) };
    inline static StringSetting firmware_{ "ble.fw", "BLE Thermostat Firmware Version", "SIM-1.0.0" };
};
