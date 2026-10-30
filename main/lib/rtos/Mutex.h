#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "IMutex.h"
#include "ContextLock.h"

class Mutex : public IMutex
{
	constexpr static const char* TAG = "Mutex";
	SemaphoreHandle_t handle = NULL;

public:
	Mutex()
	{
		handle = xSemaphoreCreateMutex();
	}

	~Mutex() override
	{
		if (handle != NULL)
			vSemaphoreDelete(handle);
	}

    bool Take(TickType_t timeout = portMAX_DELAY) const override
    {
        return xSemaphoreTake(handle, timeout) == pdTRUE;
    }

    bool Give() const override
    {
        return xSemaphoreGive(handle) == pdTRUE;
    }

    bool TakeFromISR(BaseType_t *pxHigherPriorityTaskWoken = NULL) const
    {
        return xSemaphoreTakeFromISR(handle, pxHigherPriorityTaskWoken) == pdTRUE;
    }

    bool GiveFromISR(BaseType_t *pxHigherPriorityTaskWoken = NULL) const
    {
        return xSemaphoreGiveFromISR(handle, pxHigherPriorityTaskWoken) == pdTRUE;
    }
};
