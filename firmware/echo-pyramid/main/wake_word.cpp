#include "main.h"

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "board.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hermes_webrtc.h"
#include "model_path.h"

namespace {

constexpr const char *TAG = "wake_word";
constexpr const char *kModelPartitionLabel = "model";
constexpr const char *kWakeWordModelFilter = "heywillow";
constexpr TickType_t kWakeWordIdlePollTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kWakeWordCooldownTicks = pdMS_TO_TICKS(1500);
constexpr det_mode_t kWakeWordDetectionMode = DET_MODE_90;

srmodel_list_t *s_models = nullptr;
const esp_wn_iface_t *s_wakenet = nullptr;
model_iface_data_t *s_wakenet_data = nullptr;
char *s_model_name = nullptr;
char *s_wake_words = nullptr;
int16_t *s_audio_buffer = nullptr;
size_t s_audio_buffer_bytes = 0;
TaskHandle_t s_task_handle = nullptr;

void wake_word_task(void *) {
  while (true) {
    if (hermes_webrtc_connection_requested()) {
      vTaskDelay(kWakeWordIdlePollTicks);
      continue;
    }

    if (board_audio_read_raw(s_audio_buffer, s_audio_buffer_bytes) != ESP_OK) {
      ESP_LOGW(TAG, "Wake word mic read failed");
      vTaskDelay(kWakeWordIdlePollTicks);
      continue;
    }

    const wakenet_state_t state = s_wakenet->detect(s_wakenet_data, s_audio_buffer);
    if (state == WAKENET_DETECTED) {
      ESP_LOGI(TAG, "Wake word detected: %s source=voice",
               s_wake_words != nullptr ? s_wake_words : s_model_name);
      hermes_webrtc_request_connection();
      vTaskDelay(kWakeWordCooldownTicks);
    }
  }
}

}  // namespace

esp_err_t hermes_init_wake_word(void) {
  if (s_task_handle != nullptr) {
    return ESP_OK;
  }

  s_models = esp_srmodel_init(kModelPartitionLabel);
  if (s_models == nullptr) {
    ESP_LOGE(TAG, "Failed to load speech models from partition `%s`",
             kModelPartitionLabel);
    return ESP_ERR_NOT_FOUND;
  }

  s_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, kWakeWordModelFilter);
  if (s_model_name == nullptr) {
    s_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, nullptr);
  }
  if (s_model_name == nullptr) {
    ESP_LOGE(TAG, "No WakeNet model found in model partition");
    return ESP_ERR_NOT_FOUND;
  }

  s_wake_words = esp_srmodel_get_wake_words(s_models, s_model_name);
  s_wakenet = esp_wn_handle_from_name(s_model_name);
  if (s_wakenet == nullptr) {
    ESP_LOGE(TAG, "No WakeNet interface for model `%s`", s_model_name);
    return ESP_ERR_NOT_FOUND;
  }

  s_wakenet_data = s_wakenet->create(s_model_name, kWakeWordDetectionMode);
  if (s_wakenet_data == nullptr) {
    ESP_LOGE(TAG, "Failed to create WakeNet instance for `%s`", s_model_name);
    return ESP_ERR_NO_MEM;
  }

  const int chunk_samples = s_wakenet->get_samp_chunksize(s_wakenet_data);
  if (chunk_samples <= 0) {
    ESP_LOGE(TAG, "Invalid WakeNet chunk size for `%s`", s_model_name);
    return ESP_ERR_INVALID_SIZE;
  }

  s_audio_buffer_bytes = static_cast<size_t>(chunk_samples) * sizeof(int16_t);
  s_audio_buffer = static_cast<int16_t *>(
      heap_caps_malloc(s_audio_buffer_bytes, MALLOC_CAP_DEFAULT));
  if (s_audio_buffer == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate wake word buffer");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Wake word model=%s words=%s chunk_samples=%d rate=%d",
           s_model_name, s_wake_words != nullptr ? s_wake_words : "(unknown)",
           chunk_samples, s_wakenet->get_samp_rate(s_wakenet_data));

  xTaskCreatePinnedToCore(wake_word_task, "wake_word", 8192, nullptr, 5,
                          &s_task_handle, 1);
  if (s_task_handle == nullptr) {
    ESP_LOGE(TAG, "Failed to start wake word task");
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}
