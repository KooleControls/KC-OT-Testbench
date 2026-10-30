#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


class Timeout
{
public:
    explicit Timeout(TickType_t timeout)
        : remaining(timeout)
    {
        vTaskSetTimeOutState(&timeoutState);
    }

    Timeout(const Timeout&) = delete;
    Timeout& operator=(const Timeout&) = delete;

    TickType_t GetRemaining(TickType_t max = portMAX_DELAY)
    {
        if (xTaskCheckForTimeOut(&timeoutState, &remaining) == pdTRUE)
            return 0;

        return (remaining < max) ? remaining : max;
    }

private:
    TimeOut_t timeoutState{};
    TickType_t remaining{0};
};
