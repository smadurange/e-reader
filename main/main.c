#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <esp_log.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_tls.h>
#include <esp_http_client.h>

#include <nvs_flash.h>
#include <sys/param.h>
#include <driver/gpio.h>

#include "epd.h"
#include "util.h"

#define PAGE_LEN  3
#define PAGE_SIZE 48000

#define IO_PAGE_PREV  GPIO_NUM_21
#define IO_PAGE_NEXT  GPIO_NUM_22
#define IO_INTR_FLAG_DEFAULT    0

#define WIFI_SSID       CONFIG_ESP_WIFI_SSID
#define WIFI_PASSWORD   CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAX_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_ERROR_BIT      BIT1

#define HTTP_URL_BASE    CONFIG_HTTP_BASE_URL
#define HTTP_URL_SUFFIX  ".ebm"

extern const char git_srht_cert_pem_start[] asm("_binary_git_srht_cert_pem_start");
extern const char git_srht_cert_pem_end[]   asm("_binary_git_srht_cert_pem_end");

static const char* TAG = "app";

static size_t page_num = 0;
static uint8_t *pages[PAGE_LEN];

static int http_get_page(size_t page_n, char buf[PAGE_SIZE])
{
	int rc;
	char *url;
	esp_http_client_handle_t client;

	rc = 0;
	url = NULL;
	client = NULL;

	int url_len = strlen(HTTP_URL_BASE) +
	                     snprintf(NULL, 0,"%02d", page_n) +
	                     strlen(HTTP_URL_SUFFIX) + 1;

	if ((url = malloc(sizeof(char) * url_len)) == NULL) {
		ESP_LOGE(TAG, "malloc() failed for URL");
		goto exit;
	}

	snprintf(url, url_len, "%s%02d%s", HTTP_URL_BASE, page_n,
	         HTTP_URL_SUFFIX);

	ESP_LOGI(TAG, "downloading, URL = %s", url);

	esp_http_client_config_t config = {
		.url = url,
		.timeout_ms = 5000,
		.disable_auto_redirect = true,
		.max_authorization_retries = -1,
		.cert_pem = git_srht_cert_pem_start
	};

	client = esp_http_client_init(&config);

	esp_err_t err = esp_http_client_open(client, 0);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "HTTP connection error: %s", esp_err_to_name(err));
		goto exit;
	}

	esp_http_client_fetch_headers(client);
	
	int read_len = esp_http_client_read(client, buf, PAGE_SIZE);
	if (read_len <= 0) {
		ESP_LOGE(TAG, "HTTP response read error");
		goto exit;
	}

	ESP_LOGI(TAG, "HTTP response status = %d, content length = %" PRId64,
	              esp_http_client_get_status_code(client),
	              esp_http_client_get_content_length(client));

	rc = read_len == PAGE_SIZE;

exit:
	free(url);
	esp_http_client_close(client);
	esp_http_client_cleanup(client);

	return rc;
}

static QueueHandle_t gpio_evt_queue = NULL;

static void gpio_task(void* arg)
{
	uint32_t io_num;

	for(;;) {
		if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
			if (io_num == IO_PAGE_NEXT) {
				page_num++;
				//epd_draw(pages[page_num % PAGE_LEN]);
				if(!http_get_page(page_num + 2,
				                  (char *) pages[(page_num + 1) % PAGE_LEN])) {
					page_num--;
				}
			}
			else if (io_num == IO_PAGE_PREV && page_num > 0) {
				page_num--;
				//epd_draw(pages[page_num % PAGE_LEN]);
				if(!http_get_page(page_num + 1,
					                (char *) pages[(page_num - 1) % PAGE_LEN])) {
					page_num++;
				}
			}
			else
				delay_ms(500);

			gpio_intr_enable(IO_PAGE_NEXT);
			gpio_intr_enable(IO_PAGE_PREV);
		}
	}
}

static void gpio_isr_handler(void *arg)
{
	gpio_intr_disable(IO_PAGE_PREV);
	gpio_intr_disable(IO_PAGE_NEXT);

	uint32_t gpio_num = (uint32_t) arg;
	xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static int wifi_retry_num = 0;
static EventGroupHandle_t wifi_evt_group;

static void wifi_evt_handler(void *arg,
                             esp_event_base_t eb, int32_t id,
                             void *data)
{
	if (eb == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		if (wifi_retry_num < WIFI_MAX_RETRY) {
			esp_wifi_connect();
			wifi_retry_num++;
			ESP_LOGI(TAG, "trying to connect to AP...");
		} else {
			ESP_LOGE(TAG,"connection to AP failed");
			xEventGroupSetBits(wifi_evt_group, WIFI_ERROR_BIT);
		}
	} else if (eb == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* evt = (ip_event_got_ip_t*) data;
		ESP_LOGI(TAG, "connected to AP with ip:" IPSTR, IP2STR(&evt->ip_info.ip));
		wifi_retry_num = 0;
		xEventGroupSetBits(wifi_evt_group, WIFI_CONNECTED_BIT);
	}
}

static inline void wifi_connect(void)
{
	wifi_evt_group = xEventGroupCreate();
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t any_id;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
	                                                    ESP_EVENT_ANY_ID,
	                                                    &wifi_evt_handler,
	                                                    NULL,
	                                                    &any_id));

	esp_event_handler_instance_t got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
	                                                    IP_EVENT_STA_GOT_IP,
	                                                    &wifi_evt_handler,
	                                                    NULL,
	                                                    &got_ip));

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = WIFI_SSID,
			.password = WIFI_PASSWORD,
			.threshold.authmode = WIFI_AUTH_WPA2_PSK
		},
	};

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );

	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_connect());

	xEventGroupWaitBits(wifi_evt_group,
	                    WIFI_CONNECTED_BIT | WIFI_ERROR_BIT,
	                    pdFALSE,
	                    pdFALSE,
	                    portMAX_DELAY);

	ESP_LOGI(TAG, "wifi station initialized");
}

void app_main(void)
{
	esp_err_t rc = nvs_flash_init();
	if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	}

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(esp_netif_init());

	gpio_config_t io_cfg = {
		.pin_bit_mask = ((1ULL << IO_PAGE_PREV) | (1ULL << IO_PAGE_NEXT)),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = true,
		.intr_type = GPIO_INTR_NEGEDGE
	};

	ESP_ERROR_CHECK(gpio_config(&io_cfg));

	gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 10, NULL);

	ESP_ERROR_CHECK(gpio_set_direction(IO_PAGE_PREV, GPIO_MODE_INPUT));
	ESP_ERROR_CHECK(gpio_set_direction(IO_PAGE_NEXT, GPIO_MODE_INPUT));

	wifi_connect();

	//epd_init();
	//epd_clear();
	//delay_ms(500);

	pages[0] = malloc(sizeof(uint8_t) * PAGE_SIZE);
	pages[1] = malloc(sizeof(uint8_t) * PAGE_SIZE);
	pages[2] = malloc(sizeof(uint8_t) * PAGE_SIZE);

	if (pages[0] && pages[1] && pages[2]) {
		if (http_get_page(page_num + 1,
		                  (char *) pages[page_num % PAGE_LEN])) {
			//epd_draw(pages[page_num % PAGE_LEN]);
			if (!http_get_page(page_num + 2,
			                   (char *) pages[(page_num + 1) % PAGE_LEN])) {
				page_num = -1;
			}

			gpio_install_isr_service(IO_INTR_FLAG_DEFAULT);
			gpio_isr_handler_add(IO_PAGE_PREV, gpio_isr_handler, (void *) IO_PAGE_PREV);
			gpio_isr_handler_add(IO_PAGE_NEXT, gpio_isr_handler, (void *) IO_PAGE_NEXT);

			ESP_LOGI(TAG, "GPIO for user input initialized");
		}
	} else {
		ESP_LOGE(TAG, "malloc() failed for pages");
	}
}

