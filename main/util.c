#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "util.h"

void delay_ms(unsigned int t)
{
	vTaskDelay((TickType_t) t / portTICK_PERIOD_MS);
}

