#include "ModuleFirmwareManager.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "drivers/LpcIsp/LpcIsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstring>

// The PCB1246 image, linked in by main/CMakeLists.txt when the build found one.
#if PCB1246_FIRMWARE_EMBEDDED
extern const uint8_t pcb1246BinStart[] asm("_binary_pcb1246_bin_start");
extern const uint8_t pcb1246BinEnd[]   asm("_binary_pcb1246_bin_end");
#endif

namespace {

uint32_t ReadU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr CommandArg<bool> forceArg{
    "force", "true flashes even when the module already runs the carried image. "
             "Absent or false flashes only when it differs.",
    Presence::Optional };

} // namespace

CommandEntry ModuleFirmwareManager::statusCommand_{
    "module status", &InvokeCommand<&ModuleFirmwareManager::Cmd_Status>,
    "Report the PCB1246 firmware this gateway carries and what the last check or "
    "update of the module found, with progress while one is flashing."
};

CommandEntry ModuleFirmwareManager::updateCommand_{
    "module update", &InvokeCommand<&ModuleFirmwareManager::Cmd_Update>,
    "Check the PCB1246 against the firmware this gateway carries and flash it when it "
    "differs. Answers at once; follow it with 'module status'. The module is reset, and "
    "its OpenTherm lines are down until it is running again.",
    { &forceArg }
};

ModuleFirmwareManager::ModuleFirmwareManager(AppProvider& app)
    : app_(app)
{
}

void ModuleFirmwareManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    StruxProvider& strux = app_.getStrux();
    strux.getSettingsManager().Register({ &update_ });
    strux.getCommandManager().Register(this, { &statusCommand_, &updateCommand_ });

    if (!LoadImage())
    {
        state_ = State::NoImage;
        initAttempt.SetReady();
        return;
    }

    ESP_LOGI(TAG, "Carrying PCB1246 firmware %s (%u bytes, crc32 %08lx)",
             imageVersion_, static_cast<unsigned>(imageLen_),
             static_cast<unsigned long>(imageCrc_));

    state_ = update_.Get() ? State::Checking : State::Disabled;
    if (!update_.Get())
        ESP_LOGI(TAG, "module.update is off - not checking the module");

    task_.Init("module-fw", 3, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
    {
        ESP_LOGE(TAG, "Failed to start the module firmware task");
        state_ = State::Failed;
    }

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

bool ModuleFirmwareManager::LoadImage()
{
#if PCB1246_FIRMWARE_EMBEDDED
    image_ = pcb1246BinStart;
    imageLen_ = static_cast<size_t>(pcb1246BinEnd - pcb1246BinStart);
#endif
    if (image_ == nullptr)
    {
        ESP_LOGW(TAG, "This build carries no PCB1246 firmware - the module is left as it is");
        return false;
    }

    // Refuse an image that cannot be told apart from another: without its info the
    // check would compare garbage, and every boot would flash.
    if (imageLen_ < INFO_ADDR + INFO_LEN || ReadU32(image_ + INFO_ADDR) != INFO_MAGIC
        || ReadU32(image_ + INFO_ADDR + 4) != imageLen_ || imageLen_ > LpcIsp::FLASH_SIZE)
    {
        ESP_LOGE(TAG, "The carried PCB1246 image has no valid image info - not using it");
        image_ = nullptr;
        return false;
    }

    imageCrc_ = ReadU32(image_ + INFO_ADDR + 8);
    memcpy(imageVersion_, image_ + INFO_ADDR + 12, 20);
    imageVersion_[20] = '\0';
    pagesTotal_ = static_cast<uint32_t>((imageLen_ + LpcIsp::PAGE - 1) / LpcIsp::PAGE);
    return true;
}

bool ModuleFirmwareManager::RequestUpdate(bool force)
{
    if (image_ == nullptr || IsBusy())
        return false;
    force_ = force;
    state_ = State::Checking;
    return task_.Notify(1);
}

void ModuleFirmwareManager::TaskLoop()
{
    // At boot, unless switched off. After that only when asked.
    if (state_ == State::Checking)
        Run(false);

    for (;;)
    {
        uint32_t bits = 0;
        task_.NotifyWait(&bits);
        Run(force_.exchange(false));
    }
}

// ── ISP entry and exit: the KC1245's sequence, timings included ─

void ModuleFirmwareManager::EnterIsp()
{
    ispSessions_++;
    OpenThermModule& module = app_.getBoard().GetOpenThermModule();
    module.SetIsp(true);
    module.SetReset(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    module.SetReset(false);
    vTaskDelay(pdMS_TO_TICKS(50));    // the ROM samples ISP as it leaves reset
    module.SetIsp(false);
}

void ModuleFirmwareManager::LeaveIsp()
{
    OpenThermModule& module = app_.getBoard().GetOpenThermModule();
    module.SetIsp(false);
    module.SetReset(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    module.SetReset(false);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void ModuleFirmwareManager::Run(bool force)
{
    state_ = State::Checking;
    pagesDone_ = 0;

    // The whole session, from the reset into ISP to the reset out of it.
    LOCK(uartMutex_);

    LpcIsp isp(app_.getBoard().GetOpenThermModule().Serial());
    auto fail = [&](State s) {
        failedStep_ = isp.LastStep();
        failedCode_ = isp.LastCode();
        state_ = s;
        ESP_LOGE(TAG, "%s at '%s' (ISP code %d)",
                 s == State::Unreachable ? "PCB1246 boot ROM did not answer" : "PCB1246 update failed",
                 failedStep_, failedCode_);
        LeaveIsp();
    };

    EnterIsp();
    if (!isp.Synchronize())
        return fail(State::Unreachable);

    if (!force)
    {
        switch (isp.CompareFlash(INFO_ADDR, image_ + INFO_ADDR, INFO_LEN))
        {
        case LpcIsp::Compare::Same:
            ESP_LOGI(TAG, "PCB1246 already runs %s", imageVersion_);
            LeaveIsp();
            state_ = State::UpToDate;
            return;
        case LpcIsp::Compare::Different:
            break;
        case LpcIsp::Compare::Error:
            return fail(State::Failed);
        }
    }

    ESP_LOGI(TAG, "Flashing PCB1246 with %s (%u pages)", imageVersion_,
             static_cast<unsigned>(pagesTotal_));
    state_ = State::Flashing;

    if (!isp.Unlock() || !isp.Erase(0, static_cast<uint32_t>(imageLen_ - 1)))
        return fail(State::Failed);

    // Every page but the first, then the first: it holds the vector table and the
    // image info, so the part is neither bootable nor "up to date" until the rest is in.
    for (uint32_t page = 1; page < pagesTotal_; ++page)
    {
        const uint32_t addr = page * LpcIsp::PAGE;
        const size_t len = std::min<size_t>(LpcIsp::PAGE, imageLen_ - addr);
        if (!isp.WritePage(addr, image_ + addr, len))
            return fail(State::Failed);
        pagesDone_ = page;
    }
    if (!isp.WritePage(0, image_, std::min<size_t>(LpcIsp::PAGE, imageLen_)))
        return fail(State::Failed);
    pagesDone_ = pagesTotal_;

    LeaveIsp();
    state_ = State::Updated;
    ESP_LOGI(TAG, "PCB1246 updated to %s", imageVersion_);
}

// ── Commands ─────────────────────────────────────────────────

const char* ModuleFirmwareManager::StateName(State s)
{
    switch (s)
    {
    case State::NoImage:     return "no image";
    case State::Disabled:    return "disabled";
    case State::Checking:    return "checking";
    case State::Flashing:    return "flashing";
    case State::UpToDate:    return "up to date";
    case State::Updated:     return "updated";
    case State::Unreachable: return "unreachable";
    case State::Failed:      return "failed";
    }
    return "unknown";
}

CommandResult ModuleFirmwareManager::Cmd_Status(CommandContext& ctx)
{
    const State s = state_.load();
    auto resp = ctx.reply.object();
    resp.field("state", StateName(s));
    resp.field("updateAtBoot", update_.Get());

    if (image_ != nullptr)
    {
        auto img = resp.object("image");
        img.field("version", imageVersion_);
        img.field("length", static_cast<uint32_t>(imageLen_));
        img.field("crc32", imageCrc_);
    }

    if (s == State::Flashing)
    {
        auto p = resp.object("progress");
        p.field("pages", pagesDone_.load());
        p.field("of", pagesTotal_);
    }

    if (s == State::Failed || s == State::Unreachable)
    {
        auto e = resp.object("error");
        e.field("step", failedStep_);
        e.field("code", static_cast<int32_t>(failedCode_));
    }
    return CommandResult::Ok;
}

CommandResult ModuleFirmwareManager::Cmd_Update(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    if (image_ == nullptr)
    {
        resp.field("ok", false);
        resp.field("error", "this build carries no PCB1246 firmware");
        return CommandResult::Ok;
    }
    if (!RequestUpdate(ctx.has(forceArg) && ctx.arg(forceArg)))
    {
        resp.field("ok", false);
        resp.field("error", "busy");
        return CommandResult::Ok;
    }
    resp.field("ok", true);
    return CommandResult::Ok;
}
