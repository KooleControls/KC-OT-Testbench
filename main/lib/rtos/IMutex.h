#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class IMutex
{
public:
	virtual ~IMutex() = default;
	virtual bool Take(TickType_t timeout = portMAX_DELAY) const = 0;
	virtual bool Give() const = 0;
};
