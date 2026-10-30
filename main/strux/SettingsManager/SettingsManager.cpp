#include "SettingsManager.h"
#include "CommandManager.h"
#include "ContextLock.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "nvs_flash.h"
#include <cstring>
#include <cstdlib>
#include <cassert>

// ──────────────────────────────────────────────────────────────
// Init
// ──────────────────────────────────────────────────────────────

SettingsManager::SettingsManager(StruxProvider& strux)
    : strux_(strux)
{
}

namespace {

constexpr CommandArg<const char*> keyArg{
    "key", "The setting's dotted key exactly as 'settings list' reports it, "
           "e.g. 'relay.url' or 'led.enabled'.",
    63 };

// A string whatever the setting's type is: which conversion applies is not known
// until `key` has been resolved, so the handler does it. A typed declaration here
// would have to be one type for every setting, which is the thing it cannot be.
constexpr CommandArg<const char*> valueArg{
    "value", "The new value as text: numbers in decimal, booleans as 'true' or "
             "'false'. Omit it to clear the setting to an empty value.",
    127, Presence::Optional };

} // namespace

CommandEntry SettingsManager::listCommand_{
    "settings list", &InvokeCommand<&SettingsManager::Cmd_GetSettings>,
    "List every setting registered on this device with its key, label, type "
    "and current value. This is the whole configuration surface - there are "
    "no hidden keys."
};

CommandEntry SettingsManager::setCommand_{
    "settings set", &InvokeCommand<&SettingsManager::Cmd_SetSetting>,
    "Change one setting's value in RAM. NOT durable on its own: call "
    "'settings save' afterwards or the change is lost at the next reboot.",
    { &keyArg, &valueArg }
};

CommandEntry SettingsManager::saveCommand_{
    "settings save", &InvokeCommand<&SettingsManager::Cmd_SaveSettings>,
    "Commit every setting changed with 'settings set' to NVS so they survive "
    "a reboot. Several settings only take effect after one."
};

void SettingsManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    // Registered before the NVS work so the commands exist even if NVS
    // fails to open (getSettings then reports defaults).
    strux_.getCommandManager().Register(this, {
        &listCommand_,
        &setCommand_,
        &saveCommand_,
    });

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    handle_ = nvs::open_nvs_handle(NVS_NAMESPACE, NVS_READWRITE, &err);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(err));
        initAttempt.SetReady();
        return;
    }

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// ──────────────────────────────────────────────────────────────
// Schema registration + iteration
// ──────────────────────────────────────────────────────────────

void SettingsManager::Register(std::initializer_list<Setting*> settings)
{
    LOCK(mutex_);
    for (Setting* s : settings)
    {
        // Chain-corruption class → FATAL (survives NDEBUG): re-linking a
        // registered entry would cycle the chain and hang every walk.
        if (s->registered)
            FATAL("setting '%s' registered twice", s->key);
        if (FindLocked(s->key) != nullptr)
            FATAL("duplicate setting key '%s'", s->key);

        // Sloppiness class → assert is fine. NVS caps keys at 15 chars;
        // key/label/string-default must be string literals (flash) so the
        // registry never holds a pointer that can dangle.
        assert(strlen(s->key) < NVS_KEY_NAME_MAX_SIZE && "NVS keys are max 15 chars");
        assert(esp_ptr_in_drom(s->key) && "setting key must be a string literal");
        assert(esp_ptr_in_drom(s->label) && "setting label must be a string literal");
        if (s->type == SettingType::String)
            assert(esp_ptr_in_drom(s->asString().def) && "string default must be a string literal");

        s->mgr = this;
        s->registered = true;
        s->next = head_;
        head_ = s;
    }
}

SettingIterator SettingsManager::begin()
{
    LOCK(mutex_);
    return SettingIterator(head_);
}

const Setting* SettingsManager::FindLocked(const char* key) const
{
    for (Setting* s = head_; s != nullptr; s = s->next)
        if (strcmp(key, s->key) == 0)
            return s;
    return nullptr;
}

// ──────────────────────────────────────────────────────────────
// Persistence
// ──────────────────────────────────────────────────────────────

bool SettingsManager::Save()
{
    if (!handle_) return false;
    esp_err_t err = handle_->commit();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Settings saved");
    return true;
}

bool SettingsManager::ResetToDefaults()
{
    if (!handle_) return false;

    // Defaults resolve at read, so erasing IS resetting.
    handle_->erase_all();
    handle_->commit();
    ESP_LOGI(TAG, "Reset to defaults");
    return true;
}

// ──────────────────────────────────────────────────────────────
// NVS primitives
// ──────────────────────────────────────────────────────────────

bool SettingsManager::ReadI32(const char* key, int32_t& out) const
{
    if (!handle_) return false;
    return handle_->get_item(key, out) == ESP_OK;
}

bool SettingsManager::WriteI32(const char* key, int32_t v)
{
    if (!handle_) return false;
    return handle_->set_item(key, v) == ESP_OK;
}

bool SettingsManager::ReadU32(const char* key, uint32_t& out) const
{
    if (!handle_) return false;
    return handle_->get_item(key, out) == ESP_OK;
}

bool SettingsManager::WriteU32(const char* key, uint32_t v)
{
    if (!handle_) return false;
    return handle_->set_item(key, v) == ESP_OK;
}

bool SettingsManager::ReadU8(const char* key, uint8_t& out) const
{
    if (!handle_) return false;
    return handle_->get_item(key, out) == ESP_OK;
}

bool SettingsManager::WriteU8(const char* key, uint8_t v)
{
    if (!handle_) return false;
    return handle_->set_item(key, v) == ESP_OK;
}

bool SettingsManager::ReadString(const char* key, char* out, size_t maxLen) const
{
    if (!handle_) return false;
    return handle_->get_string(key, out, maxLen) == ESP_OK;
}

bool SettingsManager::WriteString(const char* key, const char* v)
{
    if (!handle_) return false;
    return handle_->set_string(key, v) == ESP_OK;
}

// ──────────────────────────────────────────────────────────────
// WebSocket commands — the JSON converter. This is edge code: it
// walks the schema via iteration and the type tag; the core above
// knows nothing about JSON. Anyone wanting YAML writes their own.
// ──────────────────────────────────────────────────────────────

CommandResult SettingsManager::Cmd_GetSettings(CommandContext& ctx)
{
    auto root     = ctx.reply.object();
    auto settings = root.array("settings");

    for (const Setting& s : *this)
    {
        auto o = settings.object();
        o.field("key", s.key);
        o.field("label", s.label);
        o.field("type", SettingTypeToString(s.type));

        switch (s.type)   // NO default → new SettingType values must be handled here
        {
        case SettingType::Int32:  o.field("value", s.asInt32().Get());  break;
        case SettingType::UInt32: o.field("value", s.asUInt32().Get()); break;
        case SettingType::Float:  o.field("value", s.asFloat().Get());  break;
        case SettingType::Bool:   o.field("value", s.asBool().Get());   break;
        case SettingType::String:
        {
            char buf[128] = {};
            s.asString().Get(buf, sizeof(buf));
            o.field("value", buf);
            break;
        }
        }
    }   // each `o` closes at end of iteration; `settings` and `root` at return
    return CommandResult::Ok;
}

CommandResult SettingsManager::Cmd_SetSetting(CommandContext& ctx)
{
    const char* key   = ctx.arg(keyArg);
    const char* value = ctx.arg(valueArg);

    auto resp = ctx.reply.object();

    for (Setting& s : *this)
    {
        if (strcmp(s.key, key) != 0)
            continue;

        bool ok = false;
        switch (s.type)   // NO default → new SettingType values must be handled here
        {
        case SettingType::Int32:
            ok = s.asInt32().Set(static_cast<int32_t>(strtol(value, nullptr, 10)));
            break;
        case SettingType::UInt32:
            ok = s.asUInt32().Set(static_cast<uint32_t>(strtoul(value, nullptr, 10)));
            break;
        case SettingType::Float:
            ok = s.asFloat().Set(strtof(value, nullptr));
            break;
        case SettingType::Bool:
            ok = s.asBool().Set(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            break;
        case SettingType::String:
            ok = s.asString().Set(value);
            break;
        }

        resp.field("ok", ok);
        return CommandResult::Ok;
    }

    // An unrecognised setting key is MEANING, not form — the framework has no idea
    // which keys exist. So it is a reply, not a refusal.
    resp.field("ok", false);
    resp.field("error", "unknown key");
    return CommandResult::Ok;
}

CommandResult SettingsManager::Cmd_SaveSettings(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    resp.field("ok", Save());
    return CommandResult::Ok;
}
