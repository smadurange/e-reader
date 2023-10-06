#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_tls.h>
#include <esp_http_client.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include <nvs_flash.h>
#include <sys/param.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

#include "epd.h"
#include "wifi.h"

#define PAGE_LEN  3
#define PAGE_SIZE 48000

#define IO_SLEEP_PIN  GPIO_NUM_15
#define IO_PAGE_PREV  GPIO_NUM_21
#define IO_PAGE_NEXT  GPIO_NUM_22
#define IO_INTR_FLAG_DEFAULT    0

#define EBM_ARCH_URL  CONFIG_EBM_ARCH_URL

#define MUTEX_TIMEOUT ((TickType_t) 5000 / portTICK_PERIOD_MS)

extern const char git_srht_cert_pem_start[] asm("_binary_git_srht_cert_pem_start");
extern const char git_srht_cert_pem_end[]   asm("_binary_git_srht_cert_pem_end");

static const char* TAG = "app";

static SemaphoreHandle_t mutex;

static RTC_NOINIT_ATTR size_t c_page_num;
static RTC_NOINIT_ATTR size_t n_page_num;
static RTC_NOINIT_ATTR size_t p_page_num;

static uint8_t *pages[PAGE_LEN];

static QueueHandle_t http_evt_queue;
static esp_http_client_handle_t http_client;

struct http_user_data {
	char *res_buf;
	int *res_len;
};

static esp_err_t http_evt_handler(esp_http_client_event_t *evt)
{
	static int read_len;

	switch(evt->event_id) {
		case HTTP_EVENT_ON_HEADER:
			ESP_LOGI(TAG, "received header, key=%s, value=%s",
			              evt->header_key,
			              evt->header_value);
			break;
		case HTTP_EVENT_ON_DATA:
			int copy_len = 0;
			struct http_user_data *user_data = (struct http_user_data *) evt->user_data;

			if (user_data && user_data->res_buf) {
				copy_len = MIN(evt->data_len, (PAGE_SIZE - read_len));
				if (copy_len) {
					memcpy(user_data->res_buf + read_len, evt->data, copy_len);
					read_len += copy_len;
					ESP_LOGD(TAG, "download progress = %d", read_len);
					*user_data->res_len = read_len;
				}
			}
			break;
		case HTTP_EVENT_ON_FINISH:
			read_len = 0;
			break;
		case HTTP_EVENT_DISCONNECTED:
			int mbedtls_err = 0;
			esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t) evt->data,
			                                                 &mbedtls_err,
			                                                 NULL);
			if (err != 0) {
				ESP_LOGE(TAG, "disconnected, last esp error code: 0x%x", err);
				ESP_LOGI(TAG, "disconnected, last mbedtls failure: 0x%x", mbedtls_err);
			}
			read_len = 0;
			break;
		default:
			break;
	}
	return ESP_OK;
}

static int http_get_page(size_t page_n, char buf[PAGE_SIZE])
{
	struct http_user_data user_data;
	
	int res_len = 0;
	int r0 = ((page_n - 1) * PAGE_SIZE);
	int rn = page_n * PAGE_SIZE - 1;
	int rh_buflen = snprintf(NULL, 0, "bytes=%d-%d", r0, rn) + 1;

	char *rh_buf = malloc(sizeof(char) * rh_buflen);

	if (rh_buf) {
		snprintf(rh_buf, rh_buflen, "bytes=%d-%d", r0, rn);
		esp_http_client_set_header(http_client, "Range", rh_buf);

		user_data.res_len = &res_len;
		user_data.res_buf = buf;
		esp_http_client_set_user_data(http_client, &user_data);

		esp_http_client_perform(http_client);

		ESP_LOGI(TAG, "HTTP status = %d, content length = %d",
		              esp_http_client_get_status_code(http_client),
		              res_len);

		free(rh_buf);
	}

	return res_len == PAGE_SIZE;
}

struct page_info {
	size_t page_num;
	uint8_t *page_buf;
};

static void http_task(void* arg)
{
	struct page_info pg;

	for (;;) {
		if (xQueueReceive(http_evt_queue, &pg, portMAX_DELAY)) {
			if (http_get_page(pg.page_num, (char *) pg.page_buf)) {
				if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
					if (pg.page_num > c_page_num)
						n_page_num = pg.page_num;
					else if (pg.page_num < c_page_num)
						p_page_num = pg.page_num;
					xSemaphoreGive(mutex);
				} else {
					ESP_LOGE(TAG, "failed to acquire mutex");
				}
			} else {
				ESP_LOGE(TAG, "page fetch error");
			}
		}
	}
}

static QueueHandle_t gpio_evt_queue = NULL;

static void gpio_task(void* arg)
{
	uint32_t io_num;
	struct page_info pg;

	for (;;) {
		if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
			if (io_num == IO_PAGE_NEXT) {
				if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
					if (c_page_num < n_page_num) {
						c_page_num++;

						pg.page_num = c_page_num + 2;
						pg.page_buf = pages[(c_page_num + 1) % PAGE_LEN];

						xSemaphoreGive(mutex);

						xQueueSend(http_evt_queue, &pg, portMAX_DELAY);

						epd_draw_async(pages[c_page_num % PAGE_LEN], PAGE_SIZE);
						epd_draw_await();
					} else {
						xSemaphoreGive(mutex);
						ESP_LOGI(TAG, "page not ready");
					}
				} else {
					ESP_LOGE(TAG, "failed to acquire mutex");
				}
			} else if (io_num == IO_PAGE_PREV && c_page_num > 0) {
				if (xSemaphoreTake(mutex, MUTEX_TIMEOUT) == pdTRUE) {
					if (c_page_num > p_page_num) {
						c_page_num--;

						pg.page_num = c_page_num + 1;
						pg.page_buf = pages[(c_page_num - 1) % PAGE_LEN];

						xSemaphoreGive(mutex);

						xQueueSend(http_evt_queue, &pg, portMAX_DELAY);

						epd_draw_async(pages[c_page_num % PAGE_LEN], PAGE_SIZE);
						epd_draw_await();
					} else {
						xSemaphoreGive(mutex);
						ESP_LOGI(TAG, "page not ready");
					}
				} else {
					ESP_LOGE(TAG, "failed to acquire mutex");
				}
			} else if (io_num == IO_SLEEP_PIN) {
				epd_clear();
				epd_sleep();

				esp_wifi_stop();

				rtc_gpio_init(IO_SLEEP_PIN);
				rtc_gpio_pullup_dis(IO_SLEEP_PIN);
				rtc_gpio_pulldown_en(IO_SLEEP_PIN);

				esp_sleep_enable_ext0_wakeup(IO_SLEEP_PIN, 1);

				esp_deep_sleep_start();
			} else {
				vTaskDelay((TickType_t) 500 / portTICK_PERIOD_MS);
			}

			gpio_intr_enable(IO_PAGE_NEXT);
			gpio_intr_enable(IO_PAGE_PREV);
			gpio_intr_enable(IO_SLEEP_PIN);
		}
	}
}

static void gpio_isr_handler(void *arg)
{
	gpio_intr_disable(IO_PAGE_PREV);
	gpio_intr_disable(IO_PAGE_NEXT);
	gpio_intr_disable(IO_SLEEP_PIN);

	uint32_t gpio_num = (uint32_t) arg;
	if (gpio_num != IO_SLEEP_PIN)
		xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
	else
		xQueueSendToFrontFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void app_main(void)
{
	if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
		p_page_num = 0;
		c_page_num = 0;
		n_page_num = 0;
	}

	mutex = xSemaphoreCreateMutex();
	if (mutex == NULL) {
		ESP_LOGE(TAG, "xSemaphoreCreateMutex() failed");
		return;
	}

	ESP_ERROR_CHECK(esp_event_loop_create_default());

	ESP_ERROR_CHECK(rtc_gpio_deinit(IO_SLEEP_PIN));

	gpio_config_t io_cfg = {
		.pin_bit_mask = ((1ULL << IO_PAGE_PREV) |
		                 (1ULL << IO_PAGE_NEXT |
		                 (1ULL << IO_SLEEP_PIN))),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = true,
		.intr_type = GPIO_INTR_NEGEDGE
	};

	ESP_ERROR_CHECK(gpio_config(&io_cfg));
	ESP_ERROR_CHECK(gpio_set_pull_mode(IO_SLEEP_PIN, GPIO_PULLDOWN_ONLY));
	ESP_ERROR_CHECK(gpio_set_intr_type(IO_SLEEP_PIN, GPIO_INTR_POSEDGE));

	ESP_ERROR_CHECK(gpio_install_isr_service(IO_INTR_FLAG_DEFAULT));

	ESP_ERROR_CHECK(gpio_isr_handler_add(IO_SLEEP_PIN,
	                                     gpio_isr_handler,
	                                     (void *) IO_SLEEP_PIN));

	gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	xTaskCreatePinnedToCore(gpio_task, "gpio_task", 4096, NULL, 10, NULL, 0);

	esp_err_t rc = nvs_flash_init();
	if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	}

	ESP_ERROR_CHECK(esp_netif_init());

	wifi_connect();

	pages[0] = heap_caps_malloc(sizeof(uint8_t) * PAGE_SIZE, MALLOC_CAP_DMA);
	pages[1] = heap_caps_malloc(sizeof(uint8_t) * PAGE_SIZE, MALLOC_CAP_DMA);
	pages[2] = heap_caps_malloc(sizeof(uint8_t) * PAGE_SIZE, MALLOC_CAP_DMA);

	int http_rc;

	if (pages[0] && pages[1] && pages[2]) {
		epd_init();

		esp_http_client_config_t http_client_cfg = {
			.url = EBM_ARCH_URL,
			.is_async = false,
			.timeout_ms = 5000,
			.disable_auto_redirect = true,
			.event_handler = http_evt_handler,
			.cert_pem = git_srht_cert_pem_start
		};

		http_client = esp_http_client_init(&http_client_cfg);
		esp_http_client_set_header(http_client, "Accept", "application/octet-stream");

		char *c_page_buf = (char *) pages[c_page_num % PAGE_LEN];
		http_rc = http_get_page(c_page_num + 1, c_page_buf);
		if (!http_rc) {
			ESP_LOGE(TAG, "failed to download the first page");
			return;
		}

		epd_draw_async(pages[c_page_num % PAGE_LEN], PAGE_SIZE);

		if (c_page_num > 0) {
			char *p_page_buf = (char *) pages[(c_page_num - 1) % PAGE_LEN];
			http_rc = http_get_page(c_page_num, p_page_buf);
			p_page_num = http_rc ? c_page_num - 1 : c_page_num;
		}

		char *n_page_buf = (char *) pages[(c_page_num) + 1 % PAGE_LEN];
		http_rc = http_get_page(c_page_num + 2, n_page_buf);
		n_page_num = http_rc ? c_page_num + 1 : c_page_num;

		epd_draw_await();

		http_evt_queue = xQueueCreate(10, sizeof(struct page_info));
		xTaskCreatePinnedToCore(http_task, "http_task", 4096, NULL, 10, NULL, 1);

		ESP_ERROR_CHECK(gpio_isr_handler_add(IO_PAGE_PREV,
		                                     gpio_isr_handler,
		                                     (void *) IO_PAGE_PREV));

		ESP_ERROR_CHECK(gpio_isr_handler_add(IO_PAGE_NEXT,
		                                     gpio_isr_handler,
		                                     (void *) IO_PAGE_NEXT));

		ESP_LOGI(TAG, "enabled GPIO for user input");
	} else {
		ESP_LOGE(TAG, "malloc() failed for pages");
	}
}

