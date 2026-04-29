#include "wifi_manager.h"

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string.h>

#include "board.h"
#include "main.h"

namespace {

constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailedBit = BIT1;
constexpr int kMaxRetries = 8;

EventGroupHandle_t s_wifi_events = nullptr;
esp_netif_t *s_station_netif = nullptr;
bool s_connected = false;
int s_retry_count = 0;

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id,
                        void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_connected = false;
    if (s_retry_count < kMaxRetries) {
      ++s_retry_count;
      ESP_LOGW(LOG_TAG, "Wi-Fi disconnected, retry %d/%d", s_retry_count,
               kMaxRetries);
      esp_wifi_connect();
    } else {
      xEventGroupSetBits(s_wifi_events, kWifiFailedBit);
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    s_retry_count = 0;
    s_connected = true;
    xEventGroupSetBits(s_wifi_events, kWifiConnectedBit);
    ESP_LOGI(LOG_TAG, "Wi-Fi connected with IP " IPSTR,
             IP2STR(&event->ip_info.ip));
  }
}

}  // namespace

bool wifi_manager_is_configured(void) {
  return strlen(HERMES_IOT_WIFI_SSID) > 0;
}

esp_err_t wifi_manager_initialize(void) {
  if (!wifi_manager_is_configured()) {
    ESP_LOGE(LOG_TAG, "Wi-Fi SSID is empty. Set CONFIG_HERMES_IOT_WIFI_SSID.");
    return ESP_ERR_INVALID_STATE;
  }

  if (s_wifi_events == nullptr) {
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }
  xEventGroupClearBits(s_wifi_events, kWifiConnectedBit | kWifiFailedBit);

  ESP_RETURN_ON_ERROR(esp_netif_init(), LOG_TAG, "Failed to init netif");
  if (s_station_netif == nullptr) {
    s_station_netif = esp_netif_create_default_wifi_sta();
    if (s_station_netif == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), LOG_TAG, "Failed to init Wi-Fi");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler,
                                 nullptr),
      LOG_TAG, "Failed to register Wi-Fi event handler");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 &wifi_event_handler, nullptr),
      LOG_TAG, "Failed to register IP event handler");

  wifi_config_t wifi_config = {};
  strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), HERMES_IOT_WIFI_SSID,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy(reinterpret_cast<char *>(wifi_config.sta.password),
          HERMES_IOT_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  board_status_set_state(STATUS_WIFI_CONNECTING, "WiFi");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), LOG_TAG,
                      "Failed to set Wi-Fi mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), LOG_TAG,
                      "Failed to disable Wi-Fi power save");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), LOG_TAG,
                      "Failed to set Wi-Fi config");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), LOG_TAG, "Failed to start Wi-Fi");

  const EventBits_t bits = xEventGroupWaitBits(
      s_wifi_events, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(30000));
  if ((bits & kWifiConnectedBit) != 0) {
    return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void) { return s_connected; }
