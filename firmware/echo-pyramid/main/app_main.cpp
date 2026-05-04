#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <peer.h>

#include "board.h"
#include "device_identity.h"
#include "gateway_client.h"
#include "hermes_media.h"
#include "hermes_webrtc.h"
#include "wifi_manager.h"

namespace {

constexpr TickType_t kGatewayBootstrapRetryTicks = pdMS_TO_TICKS(5000);

esp_err_t bootstrap_gateway(bool *out_wake_initialized) {
  if (out_wake_initialized == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  board_status_set_state(STATUS_SIGNALING, "Gateway");
  esp_err_t ret = gateway_client_health_check();
  if (ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Gateway health check failed: %s", esp_err_to_name(ret));
    board_status_set_state(STATUS_ERROR, "Health failed");
    return ret;
  }

  GatewayBootstrapInfo bootstrap = {};
  ret = gateway_client_claim_device(&bootstrap);
  if (ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Gateway claim failed: %s", esp_err_to_name(ret));
    board_status_set_state(STATUS_ERROR, "Claim failed");
    return ret;
  }

  esp_err_t save_result = device_identity_save(&bootstrap);
  if (save_result != ESP_OK) {
    ESP_LOGW(LOG_TAG, "Failed to save device identity: %s",
             esp_err_to_name(save_result));
  }
  ESP_LOGI(LOG_TAG, "Gateway ready: conversation=%s signaling=%s",
           bootstrap.conversation, bootstrap.signaling_url);
  board_status_set_state(STATUS_CONNECTED_IDLE, "Bootstrapped");
  ret = hermes_webrtc_start(&bootstrap);
  if (ret != ESP_OK) {
    ESP_LOGE(LOG_TAG, "WebRTC start failed: %s", esp_err_to_name(ret));
    board_status_set_state(STATUS_ERROR, "WebRTC failed");
    return ret;
  }

  if (!*out_wake_initialized) {
    ret = hermes_init_wake_word();
    if (ret != ESP_OK) {
      ESP_LOGE(LOG_TAG, "Wake word init failed: %s", esp_err_to_name(ret));
      board_status_set_state(STATUS_ERROR, "Wake failed");
      return ret;
    }
    *out_wake_initialized = true;
  }

  return ESP_OK;
}

}  // namespace

extern "C" void app_main(void) {
  if (board_status_init() != ESP_OK) {
    ESP_LOGW(LOG_TAG, "Status init failed, continuing without display");
  }
  board_status_set_state(STATUS_BOOT, "Boot");

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  if (peer_init() != 0) {
    ESP_LOGE(LOG_TAG, "Peer runtime init failed");
    board_status_set_state(STATUS_ERROR, "Peer init failed");
  }

  ret = board_audio_init();
  if (ret == ESP_OK) {
    ret = hermes_media_init();
    if (ret != ESP_OK) {
      ESP_LOGE(LOG_TAG, "Media init failed: %s", esp_err_to_name(ret));
      board_status_set_state(STATUS_ERROR, "Media init failed");
    }
    board_audio_dump_diagnostics();
#if CONFIG_HERMES_IOT_BOOT_AEC_PROBE
    BoardAudioAecProbeResult probe = {};
    ret = board_audio_run_aec_probe(5000, 0, 3500.0f, &probe);
    if (ret != ESP_OK) {
      ESP_LOGE(LOG_TAG, "Boot AEC probe failed: %s", esp_err_to_name(ret));
      board_status_set_state(STATUS_ERROR, "AEC probe failed");
    }
#endif
    board_status_set_state(STATUS_BOT_SPEAKING, "Self-test");
    ret = board_audio_play_self_test();
    if (ret != ESP_OK) {
      ESP_LOGE(LOG_TAG, "Self-test playback failed: %s", esp_err_to_name(ret));
      board_status_set_state(STATUS_ERROR, "Self-test failed");
    } else {
      board_status_set_state(STATUS_CONNECTED_IDLE, "Bring-up");
    }
  } else {
    ESP_LOGE(LOG_TAG, "Audio bring-up failed: %s", esp_err_to_name(ret));
    board_status_set_state(STATUS_ERROR, "Audio init failed");
  }

  GatewayBootstrapInfo cached_bootstrap = {};
  ret = device_identity_load(&cached_bootstrap);
  if (ret == ESP_OK) {
    ESP_LOGI(LOG_TAG, "Loaded saved device identity: conversation=%s signaling=%s",
             cached_bootstrap.conversation, cached_bootstrap.signaling_url);
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(LOG_TAG, "Saved device identity unavailable: %s",
             esp_err_to_name(ret));
  }

  bool gateway_ready = false;
  bool wake_initialized = false;
  TickType_t last_gateway_attempt_tick = 0;

  if (!wifi_manager_is_configured()) {
    ESP_LOGW(LOG_TAG, "Wi-Fi is not configured; skipping gateway bootstrap");
    board_status_set_state(STATUS_CONNECTED_IDLE, "Needs WiFi");
  } else if (!gateway_client_is_configured()) {
    ESP_LOGW(LOG_TAG, "Gateway is not configured; skipping bootstrap");
    board_status_set_state(STATUS_CONNECTED_IDLE, "Needs Gateway");
  } else {
    ret = wifi_manager_initialize();
    if (ret != ESP_OK) {
      ESP_LOGE(LOG_TAG, "Wi-Fi init failed: %s", esp_err_to_name(ret));
      board_status_set_state(STATUS_ERROR, "WiFi failed");
    } else {
      last_gateway_attempt_tick = xTaskGetTickCount();
      gateway_ready = bootstrap_gateway(&wake_initialized) == ESP_OK;
    }
  }

  while (1) {
    if (!gateway_ready && wifi_manager_is_configured() &&
        gateway_client_is_configured()) {
      TickType_t now = xTaskGetTickCount();
      if (last_gateway_attempt_tick == 0 ||
          now - last_gateway_attempt_tick >= kGatewayBootstrapRetryTicks) {
        last_gateway_attempt_tick = now;
        ESP_LOGI(LOG_TAG, "Retrying gateway bootstrap");
        gateway_ready = bootstrap_gateway(&wake_initialized) == ESP_OK;
      }
    }
    hermes_webrtc_loop();
    if (board_controls_poll()) {
      hermes_webrtc_request_connection();
    }
    board_lights_tick();
    board_status_tick();
    board_timer_tick();
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
