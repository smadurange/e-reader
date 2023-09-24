#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <driver/gpio.h>
#include <esp_log.h>

#include "epd.h"
#include "util.h"
#include "doc.h"

#define IO_PG_PREV  GPIO_NUM_21
#define IO_PG_NEXT  GPIO_NUM_22
#define IO_INTR_FLAG_DEFAULT  0

static const char* TAG = "app";

static size_t page = 0;
static QueueHandle_t gpio_evt_queue = NULL;

static void gpio_task(void* arg)
{
	uint32_t io_num;

	for(;;) {
		if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
			if (io_num == IO_PG_NEXT) {
				if (page < data_len - 1) {
					epd_draw(data[++page]);
					gpio_intr_enable(IO_PG_NEXT);
					gpio_intr_enable(IO_PG_PREV);
				} else {
					epd_clear();
					epd_sleep();
				}
			}
			else if (io_num == IO_PG_PREV) {
				if (page > 0) {
					epd_draw(data[--page]);
					gpio_intr_enable(IO_PG_NEXT);
					gpio_intr_enable(IO_PG_PREV);
				} else {
					epd_clear();
					epd_sleep();
				}
			}
			else {
				delay_ms(500);
				gpio_intr_enable(IO_PG_NEXT);
				gpio_intr_enable(IO_PG_PREV);
			}
		}
	}
}

static void gpio_isr_handler(void *arg)
{
	gpio_intr_disable(IO_PG_PREV);
	gpio_intr_disable(IO_PG_NEXT);
	uint32_t gpio_num = (uint32_t) arg;
	xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void app_main(void)
{
	epd_init();
	ESP_LOGI(TAG, "initialized e-paper display");

	gpio_config_t io_cfg = {
		.pin_bit_mask = ((1ULL << IO_PG_PREV) | (1ULL << IO_PG_NEXT)),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = true,
		.intr_type = GPIO_INTR_NEGEDGE
	};
	ESP_ERROR_CHECK(gpio_config(&io_cfg));

	gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	xTaskCreate(gpio_task, "gpio_task", 2048, NULL, 10, NULL);

	gpio_install_isr_service(IO_INTR_FLAG_DEFAULT);
	gpio_isr_handler_add(IO_PG_PREV, gpio_isr_handler, (void *) IO_PG_PREV);
	gpio_isr_handler_add(IO_PG_NEXT, gpio_isr_handler, (void *) IO_PG_NEXT);

	epd_clear();
	delay_ms(500);
	epd_draw(data[0]);
}

