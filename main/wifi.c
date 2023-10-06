#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_wifi.h>

#include "wifi.h"

#define WIFI_SSID       CONFIG_ESP_WIFI_SSID
#define WIFI_PASSWORD   CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAX_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_ERROR_BIT      BIT1

static const char* TAG = "wifi";

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

void wifi_connect(void)
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
