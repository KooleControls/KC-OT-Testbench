#include "TimeManager.h"
#include "SettingsManager/SettingsManager.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include <ctime>

TimeManager *TimeManager::instance = nullptr;

TimeManager::TimeManager(StruxProvider &ctx)
    : strux_(ctx)
{
}

void TimeManager::Init()
{
    auto init = initState.TryBeginInit();
    if (!init)
        return;

    instance = this;

    strux_.getSettingsManager().Register({ &ntpServerSetting_, &ntpTimezone_ });

    ApplyTimezone();
    LoadServerName();
    StartSntp();

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void TimeManager::ApplyTimezone()
{
    char tz[64] = {};
    ntpTimezone_.Get(tz, sizeof(tz));
    if (tz[0] == '\0')
        snprintf(tz, sizeof(tz), "UTC0");

    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", tz);
}

void TimeManager::LoadServerName()
{
    ntpServerSetting_.Get(ntpServer, sizeof(ntpServer));
    if (ntpServer[0] == '\0')
        snprintf(ntpServer, sizeof(ntpServer), "pool.ntp.org");
}

void TimeManager::StartSntp()
{
    ESP_LOGI(TAG, "Starting SNTP with server: %s", ntpServer);

    // ntpServer is a member - pointer stays valid for the lifetime of the object
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntpServer);
    config.sync_cb = TimeSyncCallback;
    config.start = true;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
}

void TimeManager::TimeSyncCallback(struct timeval *tv)
{
    if (!instance)
        return;

    instance->synced = true;

    char buf[32];
    DateTime now = DateTime::Now();
    now.ToStringLocal(buf, sizeof(buf), "%F %T");
    ESP_LOGI(TAG, "Time synchronized: %s", buf);
}

bool TimeManager::IsTimeValid() const
{
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    return t.tm_year >= (2020 - 1900);
}
