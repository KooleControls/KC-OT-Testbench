#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Task.h"
#include "RecursiveMutex.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// Keeps the PCB1246 running the firmware this gateway build carries.
//
// The release build links the PCB1246's .bin into this app image (see the
// PCB1246_FIRMWARE block in main/CMakeLists.txt), so one OTA of the gateway is an
// update of both boards: the gateway is the only thing that can flash the LPC, and it
// holds the image that belongs with the code that talks to it.
//
// On boot, and on `module update`, a task of its own:
//
//   1. resets the module into its boot ROM's ISP mode (ISP held through a reset);
//   2. compares the 32-byte image info at 0x300 in the module's flash with the same
//      bytes of the carried image (the PCB1246's ImageInfo.c). The ROM compares them
//      itself, so nothing is read back. Equal means "already running this build";
//      anything else -- another build, the old KC2 firmware, an erased part -- is
//      flashed. A part that does not answer at all is reported, not retried;
//   3. if flashing: erases what the image covers, sector 0 included, writes every
//      page but the first, then the first. Until that last page lands the part has
//      no valid image, so its boot ROM stays in ISP, and an update cut short is simply
//      found different and done again on the next boot;
//   4. resets the module out of ISP into whatever it now holds.
//
// The module is reset on every gateway boot by step 1. Its OpenTherm lines are idle
// for that second, while the gateway that answers them is booting anyway.
//
// An application manager because it drives board hardware (the module's UART and its
// reset and ISP lines), which only the application layer has.
// ──────────────────────────────────────────────────────────────
class ModuleFirmwareManager
{
    static constexpr const char* TAG = "ModuleFirmware";

public:
    enum class State : uint8_t
    {
        NoImage,       // this build carries no PCB1246 firmware
        Disabled,      // module.update is off: nothing checked at boot
        Checking,
        Flashing,
        UpToDate,      // the module already ran the carried image
        Updated,       // the module was flashed with it
        Unreachable,   // the boot ROM never answered: no module, or ISP not reaching it
        Failed,        // it answered, and something went wrong after that
    };

    explicit ModuleFirmwareManager(AppProvider& app);

    ModuleFirmwareManager(const ModuleFirmwareManager&) = delete;
    ModuleFirmwareManager& operator=(const ModuleFirmwareManager&) = delete;
    ModuleFirmwareManager(ModuleFirmwareManager&&) = delete;
    ModuleFirmwareManager& operator=(ModuleFirmwareManager&&) = delete;

    void Init();

    State GetState() const { return state_.load(); }

    /// True while the module is held in ISP. Anything else that talks to the module
    /// over its UART has to wait for this to be false.
    bool IsBusy() const
    {
        const State s = state_.load();
        return s == State::Checking || s == State::Flashing;
    }

    /// Check the module now, and flash it when it differs -- or always, with `force`.
    /// False while a check is already running.
    bool RequestUpdate(bool force);

    /// The module's UART has two users: this manager, for the length of an ISP
    /// session, and ModuleLinkManager, for everything else. Whoever talks on it holds
    /// this; a session holds it from the reset into ISP until the reset out of it, so
    /// nothing else can put a byte on the line while the boot ROM is listening.
    /// The link takes it to send, and to ask whether the line is still its own.
    RecursiveMutex& UartMutex() { return uartMutex_; }

    /// How many ISP sessions there have been. Each one restarts the module, so a user
    /// of the link that sees this change knows everything it had settled is gone.
    uint32_t IspSessions() const { return ispSessions_.load(); }

private:
    // Where the image info sits and how much of it is compared. ImageInfo.c on the
    // PCB1246 side is the other half of this.
    static constexpr uint32_t INFO_ADDR = 0x300;
    static constexpr size_t   INFO_LEN = 32;
    static constexpr uint32_t INFO_MAGIC = 0x544F434B;   // "KCOT"

    static constexpr int TASK_STACK = 6144;

    AppProvider& app_;
    InitState initState_;
    Task task_;

    const uint8_t* image_ = nullptr;
    size_t imageLen_ = 0;
    char imageVersion_[21] = {};
    uint32_t imageCrc_ = 0;

    std::atomic<State> state_{ State::NoImage };
    std::atomic<bool> force_{ false };
    std::atomic<uint32_t> pagesDone_{ 0 };
    std::atomic<uint32_t> ispSessions_{ 0 };
    RecursiveMutex uartMutex_;
    uint32_t pagesTotal_ = 0;

    // What went wrong last, for `module status`: the ISP step and its return code.
    const char* failedStep_ = "";
    int failedCode_ = 0;

    bool LoadImage();
    void TaskLoop();
    void Run(bool force);

    void EnterIsp();
    void LeaveIsp();

    static const char* StateName(State s);

    inline static BoolSetting update_{ "module.update", "Update the PCB1246 at boot", true };

    // ── Commands ──
    CommandResult Cmd_Status(CommandContext& ctx);
    CommandResult Cmd_Update(CommandContext& ctx);
    static CommandEntry statusCommand_;
    static CommandEntry updateCommand_;
};
