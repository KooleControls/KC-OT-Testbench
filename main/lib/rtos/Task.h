#pragma once
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class Task
{
	const char* name = "New task";
	portBASE_TYPE priority = 0;
	portSHORT stackDepth = configMINIMAL_STACK_SIZE;
	TaskHandle_t taskHandle = nullptr;
	std::function<void()> callback;

	static void TaskFunction(void* parm)
	{
		Task* t = static_cast<Task*>(parm);
		if (t && t->callback) {
			t->callback();
		}
		t->taskHandle = nullptr;
		vTaskDelete(nullptr);
	}

	void DeleteTaskIfExists()
	{
		if (taskHandle != nullptr) {
			vTaskDelete(taskHandle);
			taskHandle = nullptr;
		}
	}

public:

	Task() = default;

	~Task()
	{
		DeleteTaskIfExists();
	}

	bool IsRunning() const { return taskHandle != nullptr; }

	bool Init(const char* name, portBASE_TYPE priority, portSHORT stackDepth)
	{
		this->name = name;
		this->priority = priority;
		this->stackDepth = (stackDepth < configMINIMAL_STACK_SIZE) ? configMINIMAL_STACK_SIZE : stackDepth;
		return true;
	}

	void SetHandler(std::function<void()> callback)
	{
		this->callback = std::move(callback);
	}

	bool Run(BaseType_t core = tskNO_AFFINITY)
	{
		DeleteTaskIfExists();
		BaseType_t result;
		if (core == tskNO_AFFINITY) {
			result = xTaskCreate(&TaskFunction, name, stackDepth, this, priority, &taskHandle);
		} else {
			result = xTaskCreatePinnedToCore(&TaskFunction, name, stackDepth, this, priority, &taskHandle, core);
		}
		if (result != pdPASS) {
			taskHandle = nullptr;
		}
		return result == pdPASS;
	}

#ifdef configUSE_TASK_NOTIFICATIONS

	bool NotifyWait(uint32_t* pulNotificationValue, TickType_t timeout = portMAX_DELAY)
	{
		return xTaskNotifyWait(0x0000, 0xFFFF, pulNotificationValue, timeout) == pdPASS;
	}

	bool Notify(uint32_t bits)
	{
		if (taskHandle == nullptr) return false;
		return xTaskNotify(taskHandle, bits, eSetBits) == pdPASS;
	}

	bool NotifyFromISR(uint32_t bits)
	{
		if (taskHandle == nullptr) return false;
		return xTaskNotifyFromISR(taskHandle, bits, eSetBits, nullptr) == pdPASS;
	}

	bool NotifyFromISR(uint32_t bits, BaseType_t* pxHigherPriorityTaskWoken)
	{
		if (taskHandle == nullptr) return false;
		return xTaskNotifyFromISR(taskHandle, bits, eSetBits, pxHigherPriorityTaskWoken) == pdPASS;
	}

#endif // configUSE_TASK_NOTIFICATIONS

	static int GetCurrentCoreID()
	{
		return xPortGetCoreID();
	}
};
