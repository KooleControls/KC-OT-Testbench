#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <functional>
#include <string>

class Timer
{
	std::function<void()> callback;
	TimerHandle_t xTimer = nullptr;

	static void tCallback(TimerHandle_t xTimer)
	{
		Timer* t = static_cast<Timer*>(pvTimerGetTimerID(xTimer));
		if (t && t->callback) {
			t->callback();
		}
	}

	void DeleteTimerIfExists()
	{
		if (xTimer != nullptr) {
			xTimerDelete(xTimer, 0);
			xTimer = nullptr;
		}
	}

public:
	Timer() = default;

	~Timer()
	{
		DeleteTimerIfExists();
	}

	bool SetHandler(const std::function<void()>& callback)
	{
		this->callback = callback;
		return true;
	}

	bool Init(const std::string& name, TickType_t rtosTicks, bool autoReload = true)
	{
		if (rtosTicks < 1) rtosTicks = 1;
		DeleteTimerIfExists();
		xTimer = xTimerCreate(name.c_str(), rtosTicks, autoReload, this, &tCallback);
		return xTimer != nullptr;
	}

	bool Start(int timeout = portMAX_DELAY)
	{
		return xTimer != nullptr && xTimerStart(xTimer, timeout) == pdPASS;
	}

	bool Stop(int timeout = portMAX_DELAY)
	{
		return xTimer != nullptr && xTimerStop(xTimer, timeout) == pdPASS;
	}

	bool Reset(int timeout = portMAX_DELAY)
	{
		return xTimer != nullptr && xTimerReset(xTimer, timeout) == pdPASS;
	}

	bool IsRunning(bool& value)
	{
		if (xTimer == nullptr) return false;
		value = xTimerIsTimerActive(xTimer) != pdFALSE;
		return true;
	}

	bool SetPeriod(TickType_t rtosTicks, TickType_t timeout = portMAX_DELAY)
	{
		if (xTimer == nullptr) return false;
		if (rtosTicks < 1) rtosTicks = 1;
		return xTimerChangePeriod(xTimer, rtosTicks, timeout) == pdPASS;
	}

	bool GetPeriod(TickType_t& period)
	{
		if (xTimer == nullptr) return false;
		period = xTimerGetPeriod(xTimer);
		return true;
	}
};
