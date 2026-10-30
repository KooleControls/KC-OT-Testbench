#include "SystemManager.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "NetworkManager.h"
#include "DateTime.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

SystemManager::SystemManager(StruxProvider& strux)
    : strux_(strux)
{
}

CommandEntry SystemManager::pingCommand_{
    "system ping", &InvokeCommand<&SystemManager::Cmd_Ping>,
    "Check the device is answering. Takes nothing, returns {\"pong\":true}."
};

CommandEntry SystemManager::infoCommand_{
    "system info", &InvokeCommand<&SystemManager::Cmd_Info>,
    "Report this device's runtime state: name, firmware build, chip, clock, IP "
    "address, free heap and the device's own clock."
};

CommandEntry SystemManager::rebootCommand_{
    "system reboot", &InvokeCommand<&SystemManager::Cmd_Reboot>,
    "Restart the device. The reply is written first, then the device restarts "
    "about half a second later and every connection drops."
};

CommandEntry SystemManager::describeCommand_{
    "system describe", &InvokeCommand<&SystemManager::Cmd_Describe>,
    "What this device IS: its name, firmware, one-line description and its "
    "full instructions - how it is meant to be driven. Pair it with "
    "'help', which is the same question about the commands."
};

void SystemManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    strux_.getSettingsManager().Register({ &name_ });
    strux_.getCommandManager().Register(this, {
        &pingCommand_,
        &infoCommand_,
        &rebootCommand_,
        &describeCommand_,
    });

    // Logged rather than only served by `system info`, because a fork changing the
    // power settings is flashing a board over serial when it wants to know whether
    // the change took, and may have no network to ask over yet.
    char cpu[48] = {};
    DescribeCpu(cpu, sizeof(cpu));
    ESP_LOGI(TAG, "CPU: %s", cpu);

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void SystemManager::DescribeCpu(char* out, size_t maxLen)
{
    out[0] = '\0';
#if CONFIG_PM_ENABLE
    // The bounds startup code actually installed, which is the whole point of asking:
    // WiFi holds an APB_FREQ_MAX lock while the radio is up, so the effective floor at
    // runtime is higher than the minimum reported here. See sdkconfig.defaults.
    esp_pm_config_t pm = {};
    if (esp_pm_get_configuration(&pm) == ESP_OK)
    {
        snprintf(out, maxLen, "%d MHz, scaling down to %d MHz",
                 pm.max_freq_mhz, pm.min_freq_mhz);
    }
#endif
    if (out[0] == '\0')
        snprintf(out, maxLen, "%d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
}

void SystemManager::SetDocumentation(const char* description, const char* instructions)
{
    description_  = description;
    instructions_ = instructions;
}

void SystemManager::GetDeviceName(char* out, size_t maxLen)
{
    name_.Get(out, maxLen);
    if (out[0] == '\0')
        snprintf(out, maxLen, "%s", esp_app_get_description()->project_name);
}

// ──────────────────────────────────────────────────────────────
// WebSocket commands
// ──────────────────────────────────────────────────────────────

CommandResult SystemManager::Cmd_Ping(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    resp.field("pong", true);
    return CommandResult::Ok;
}

CommandResult SystemManager::Cmd_Info(CommandContext& ctx)
{
    auto resp = ctx.reply.object();

    const esp_app_desc_t* app = esp_app_get_description();

    char deviceName[32] = {};
    GetDeviceName(deviceName, sizeof(deviceName));
    resp.field("name", deviceName);

    resp.field("project", app->project_name);
    resp.field("firmware", app->version);
    resp.field("idf", app->idf_ver);
    resp.field("date", app->date);
    resp.field("time", app->time);
    resp.field("chip", CONFIG_IDF_TARGET);

    char cpu[48] = {};
    DescribeCpu(cpu, sizeof(cpu));
    resp.field("cpu", cpu);

    // Empty when there is no address yet — the wording for that belongs to
    // whoever renders it, not here.
    char ip[16] = {};
    strux_.getNetworkManager().GetIpv4(ip, sizeof(ip));
    resp.field("ip", ip);

    resp.field("heapFree", static_cast<uint32_t>(esp_get_free_heap_size()));
    resp.field("heapMin", static_cast<uint32_t>(esp_get_minimum_free_heap_size()));

    char deviceTimeStr[32] = "Not synced";
    DateTime now = DateTime::Now();
    if (now.YearLocal() >= 2020)
        now.ToStringLocal(deviceTimeStr, sizeof(deviceTimeStr), "%F %T");
    resp.field("deviceTime", deviceTimeStr);
    return CommandResult::Ok;
}

CommandResult SystemManager::Cmd_Describe(CommandContext& ctx)
{
    const esp_app_desc_t* app = esp_app_get_description();

    char deviceName[48] = {};
    GetDeviceName(deviceName, sizeof(deviceName));

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("name", deviceName);
    resp.field("project", app->project_name);
    resp.field("firmware", app->version);
    resp.field("commit", STRUX_GIT_COMMIT);

    // Absent, not empty, when the application registered nothing: a reader can then
    // tell a device that has no instructions from one whose instructions are blank.
    // The strings stream straight to the transport — nothing here buffers a README.
    if (description_ != nullptr && description_[0] != '\0')
        resp.field("description", description_);
    if (instructions_ != nullptr && instructions_[0] != '\0')
        resp.field("instructions", instructions_);

    return CommandResult::Ok;
}

CommandResult SystemManager::Cmd_Reboot(CommandContext& ctx)
{
    {
        auto resp = ctx.reply.object();
        resp.field("ok", true);
    }   // close the scope BEFORE restarting so the reply is complete

    // Delay to allow WS response to be sent before restarting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
