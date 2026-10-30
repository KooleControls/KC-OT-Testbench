#pragma once
#include "StruxProvider.h"
#include "InitState.h"
#include "TypedSettings.h"
#include "DateTime.h"

class TimeManager
{
    inline static constexpr const char *TAG = "TimeManager";

public:
    explicit TimeManager(StruxProvider &ctx);

    void Init();

    bool IsTimeSynced() const { return synced; }
    bool IsTimeValid() const;

private:
    StruxProvider &strux_;
    InitState initState;

    volatile bool synced = false;
    char ntpServer[64] = {};

    void ApplyTimezone();
    void LoadServerName();
    void StartSntp();

    static void TimeSyncCallback(struct timeval *tv);
    static TimeManager *instance;

    // ── Settings (registered with SettingsManager in Init) ──
    inline static StringSetting ntpServerSetting_{ "ntp.server",   "NTP Server",   "pool.ntp.org" };
    inline static StringSetting ntpTimezone_     { "ntp.timezone", "NTP Timezone", "UTC0"         };
};
