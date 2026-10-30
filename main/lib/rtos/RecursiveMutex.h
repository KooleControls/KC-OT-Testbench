#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "IMutex.h"


class RecursiveMutex : public IMutex
{
	constexpr static const char* TAG = "RecursiveMutex";
	SemaphoreHandle_t handle = NULL;
public:
	RecursiveMutex(const char* msg = NULL)
	{
		handle = xSemaphoreCreateRecursiveMutex();
	}

	~RecursiveMutex() override
	{
		if(handle != NULL)
			vSemaphoreDelete(handle);
	}

	bool Take(TickType_t timeout = portMAX_DELAY) const override
	{
		return xSemaphoreTakeRecursive(handle, timeout) == pdTRUE;
	}

	bool Give() const override
	{
		return xSemaphoreGiveRecursive(handle) == pdTRUE;
	}
};
