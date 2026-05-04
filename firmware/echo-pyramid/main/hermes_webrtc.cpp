#include "hermes_webrtc.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <peer.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "device_identity.h"
#include "hermes_media.h"
#include "main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *kDataChannelLabel = "control";
constexpr TickType_t kSessionIdleDisconnectTicks = pdMS_TO_TICKS(30000);
constexpr TickType_t kSessionHandshakeDisconnectTicks = pdMS_TO_TICKS(12000);
constexpr TickType_t kWakeAfterCloseCooldownTicks = pdMS_TO_TICKS(8000);
constexpr TickType_t kWakeAfterEndConversationCooldownTicks = pdMS_TO_TICKS(12000);

PeerConnection *s_peer_connection = nullptr;
GatewayBootstrapInfo s_bootstrap = {};
bool s_bootstrap_ready = false;
bool s_connection_requested = false;
bool s_connected = false;
bool s_connection_closed = false;
bool s_datachannel_created = false;
bool s_hello_sent = false;
bool s_audio_task_running = false;
bool s_audio_encoder_prepared = false;
bool s_audio_loopback_running = false;
bool s_audio_probe_running = false;
bool s_media_mode = false;
bool s_local_wake_active = false;
char s_last_assistant_state[16] = "";
TickType_t s_last_activity_tick = 0;
TickType_t s_wake_suppressed_until = 0;
StaticTask_t s_audio_task_buffer = {};
StackType_t *s_audio_task_stack = nullptr;

void stop_audio_task();

void note_session_activity() {
  s_last_activity_tick = xTaskGetTickCount();
}

void suppress_wake_for(TickType_t duration) {
  const TickType_t until = xTaskGetTickCount() + duration;
  if (s_wake_suppressed_until == 0 ||
      static_cast<int32_t>(until - s_wake_suppressed_until) > 0) {
    s_wake_suppressed_until = until;
  }
}

bool wake_suppressed() {
  if (s_wake_suppressed_until == 0) {
    return false;
  }
  if (static_cast<int32_t>(xTaskGetTickCount() - s_wake_suppressed_until) >= 0) {
    s_wake_suppressed_until = 0;
    return false;
  }
  return true;
}

bool remember_assistant_state(const char *state) {
  if (state == nullptr) {
    return false;
  }
  if (strncmp(s_last_assistant_state, state,
              sizeof(s_last_assistant_state)) == 0) {
    return false;
  }
  strncpy(s_last_assistant_state, state, sizeof(s_last_assistant_state) - 1);
  s_last_assistant_state[sizeof(s_last_assistant_state) - 1] = '\0';
  return true;
}

bool send_control_json(cJSON *root) {
  if (root == nullptr || s_peer_connection == nullptr || !s_connected ||
      !s_datachannel_created) {
    return false;
  }

  char *encoded = cJSON_PrintUnformatted(root);
  if (encoded == nullptr) {
    return false;
  }

  const int result = peer_connection_datachannel_send(
      s_peer_connection, encoded, strlen(encoded));
  cJSON_free(encoded);
  return result >= 0;
}

void send_audio_stats() {
  BoardAudioStats board_stats = {};
  HermesMediaStats media_stats = {};
  board_audio_get_stats(&board_stats);
  hermes_media_get_stats(&media_stats);

  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  if (root == nullptr || payload == nullptr) {
    cJSON_Delete(root);
    return;
  }

  cJSON_AddStringToObject(root, "type", "audio.stats");
  cJSON_AddNumberToObject(payload, "mic_peak", board_stats.mic_peak);
  cJSON_AddNumberToObject(payload, "ref_peak", board_stats.ref_peak);
  cJSON_AddNumberToObject(payload, "aec_peak", board_stats.aec_peak);
  cJSON_AddNumberToObject(payload, "read_count", board_stats.read_count);
  cJSON_AddNumberToObject(payload, "volume", board_stats.volume_percent);
  cJSON_AddBoolToObject(payload, "output_enabled", board_stats.output_enabled);
  cJSON_AddBoolToObject(payload, "aec_enabled", board_stats.aec_enabled);
  cJSON_AddNumberToObject(payload, "aec_frame_samples",
                          board_stats.aec_frame_samples);
  cJSON_AddNumberToObject(payload, "encoder_input_peak",
                          media_stats.encoder_input_peak);
  cJSON_AddNumberToObject(payload, "encoder_peak", media_stats.encoder_peak);
  cJSON_AddNumberToObject(payload, "capture_gain_q8",
                          media_stats.capture_gain_q8);
  cJSON_AddBoolToObject(payload, "remote_playback_active",
                        media_stats.remote_playback_active);
  cJSON_AddNumberToObject(payload, "sent_packets", media_stats.sent_packets);
  cJSON_AddNumberToObject(payload, "send_failures", media_stats.send_failures);
  cJSON_AddNumberToObject(payload, "playback_underruns",
                          media_stats.playback_underruns);
  cJSON_AddNumberToObject(payload, "playback_overflows",
                          media_stats.playback_overflows);
  cJSON_AddNumberToObject(payload, "remote_silence_packets",
                          media_stats.remote_silence_packets);
  send_control_json(root);
  cJSON_Delete(root);
}

void send_wake_detected_state() {
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  if (root == nullptr || payload == nullptr) {
    cJSON_Delete(root);
    return;
  }

  cJSON_AddStringToObject(root, "type", "device.state");
  cJSON_AddBoolToObject(payload, "wake_detected", true);
  cJSON_AddStringToObject(payload, "wake_word", "hey willow");
  if (s_media_mode) {
    cJSON_AddBoolToObject(payload, "media_barge_in", true);
  }

  send_control_json(root);
  cJSON_Delete(root);
}

void run_audio_loopback_task(void *arg) {
  const uint32_t duration_ms = reinterpret_cast<uintptr_t>(arg);
  hermes_media_set_publish_enabled(false);
  hermes_media_set_playback_enabled(false);
  vTaskDelay(pdMS_TO_TICKS(60));

  const esp_err_t result = board_audio_run_loopback_test(duration_ms);
  ESP_LOGI(LOG_TAG, "Local audio loopback result=%s", esp_err_to_name(result));

  hermes_media_set_playback_enabled(true);
  hermes_media_set_publish_enabled(true);
  s_audio_loopback_running = false;
  vTaskDelete(nullptr);
}

typedef struct {
  uint32_t duration_ms;
  uint32_t frequency_hz;
  float amplitude;
} AecProbeTaskConfig;

void send_aec_probe_result(const BoardAudioAecProbeResult &probe,
                           esp_err_t result) {
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  if (root == nullptr || payload == nullptr) {
    cJSON_Delete(root);
    return;
  }

  cJSON_AddStringToObject(root, "type", "audio.aec_probe");
  cJSON_AddStringToObject(payload, "result", esp_err_to_name(result));
  cJSON_AddNumberToObject(payload, "duration_ms", probe.duration_ms);
  cJSON_AddNumberToObject(payload, "frames", probe.frames);
  cJSON_AddNumberToObject(payload, "frequency_hz", probe.frequency_hz);
  cJSON_AddNumberToObject(payload, "mic_peak", probe.mic_peak);
  cJSON_AddNumberToObject(payload, "ref_peak", probe.ref_peak);
  cJSON_AddNumberToObject(payload, "aec_peak", probe.aec_peak);
  cJSON *slot_peaks = cJSON_AddArrayToObject(payload, "slot_peaks");
  cJSON *slot_rms = cJSON_AddArrayToObject(payload, "slot_rms");
  cJSON *slot_corr = cJSON_AddArrayToObject(payload, "slot_corr");
  for (size_t i = 0; i < 4; ++i) {
    if (slot_peaks != nullptr) {
      cJSON_AddItemToArray(slot_peaks,
                           cJSON_CreateNumber(probe.slot_peaks[i]));
    }
    if (slot_rms != nullptr) {
      cJSON_AddItemToArray(slot_rms, cJSON_CreateNumber(probe.slot_rms[i]));
    }
    if (slot_corr != nullptr) {
      cJSON_AddItemToArray(slot_corr, cJSON_CreateNumber(probe.slot_corr[i]));
    }
  }
  cJSON_AddNumberToObject(payload, "mic_rms", probe.mic_rms);
  cJSON_AddNumberToObject(payload, "ref_rms", probe.ref_rms);
  cJSON_AddNumberToObject(payload, "aec_rms", probe.aec_rms);
  cJSON_AddNumberToObject(payload, "mic_ref_corr", probe.mic_ref_corr);
  cJSON_AddNumberToObject(payload, "aec_ref_corr", probe.aec_ref_corr);
  cJSON_AddNumberToObject(payload, "suppression_db", probe.suppression_db);
  send_control_json(root);
  cJSON_Delete(root);
}

void run_aec_probe_task(void *arg) {
  auto *config = static_cast<AecProbeTaskConfig *>(arg);
  hermes_media_set_publish_enabled(false);
  hermes_media_set_playback_enabled(false);
  vTaskDelay(pdMS_TO_TICKS(60));

  BoardAudioAecProbeResult probe = {};
  const esp_err_t result = board_audio_run_aec_probe(
      config->duration_ms, config->frequency_hz, config->amplitude, &probe);
  ESP_LOGI(LOG_TAG, "AEC probe result=%s", esp_err_to_name(result));
  send_aec_probe_result(probe, result);

  hermes_media_set_playback_enabled(true);
  hermes_media_set_publish_enabled(true);
  free(config);
  s_audio_probe_running = false;
  vTaskDelete(nullptr);
}

void audio_publisher_task(void *) {
  if (!s_audio_encoder_prepared) {
    hermes_media_prepare_encoder();
    s_audio_encoder_prepared = true;
  }

  TickType_t last_stats_tick = 0;
  while (s_audio_task_running) {
    if (!s_connected || s_peer_connection == nullptr ||
        peer_connection_get_state(s_peer_connection) !=
            PEER_CONNECTION_COMPLETED) {
      vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
      continue;
    }
    if (!s_hello_sent) {
      vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
      continue;
    }
    hermes_media_send_audio(s_peer_connection);
    const TickType_t now = xTaskGetTickCount();
    if (s_hello_sent && (last_stats_tick == 0 ||
                         now - last_stats_tick >= pdMS_TO_TICKS(1000))) {
      last_stats_tick = now;
      send_audio_stats();
    }
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }

  s_audio_task_running = false;
  vTaskDelete(nullptr);
}

void reset_session_state() {
  s_connected = false;
  s_connection_closed = false;
  s_datachannel_created = false;
  s_hello_sent = false;
  s_last_assistant_state[0] = '\0';
}

void destroy_peer_connection() {
  stop_audio_task();
  hermes_media_reset_playback();
  if (s_peer_connection != nullptr) {
    peer_connection_destroy(s_peer_connection);
    s_peer_connection = nullptr;
  }
  reset_session_state();
  s_connection_requested = false;
  board_status_set_state(STATUS_CONNECTED_IDLE, "Say hey willow");
}

void ensure_audio_task_running() {
  if (s_audio_task_running) {
    return;
  }

  if (s_audio_task_stack == nullptr) {
    s_audio_task_stack = static_cast<StackType_t *>(
        heap_caps_malloc(30000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM));
  }
  if (s_audio_task_stack == nullptr) {
    ESP_LOGE(LOG_TAG, "Failed to allocate audio publisher stack");
    board_status_set_state(STATUS_ERROR, "Audio task");
    return;
  }

  s_audio_task_running = true;
  xTaskCreateStaticPinnedToCore(audio_publisher_task, "hermes_audio_pub", 30000,
                                nullptr, 7, s_audio_task_stack,
                                &s_audio_task_buffer, 0);
}

void stop_audio_task() {
  if (!s_audio_task_running) {
    return;
  }

  s_audio_task_running = false;
  for (int i = 0; i < 20 && s_audio_task_running; ++i) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void apply_assistant_state(const char *state) {
  if (state == nullptr) {
    return;
  }

  const bool state_changed = remember_assistant_state(state);

  if (s_media_mode &&
      !s_local_wake_active &&
      (strcmp(state, "idle") == 0 || strcmp(state, "listening") == 0)) {
    board_status_set_state(STATUS_MEDIA_PLAYING, "Playing");
    return;
  }

  if (strcmp(state, "listening") == 0) {
    board_status_set_state(STATUS_LISTENING, "Listening");
    if (state_changed) {
      board_status_show_text("LISTEN");
      board_audio_play_listening_tone();
    }
  } else if (strcmp(state, "speaking") == 0) {
    board_status_set_state(STATUS_BOT_SPEAKING, "Speaking");
    if (state_changed) {
      board_status_show_text("SPEAK");
    }
  } else if (strcmp(state, "thinking") == 0) {
    board_status_set_state(STATUS_SIGNALING, "Thinking");
    if (state_changed) {
      board_status_show_text("THINK");
    }
  } else if (strcmp(state, "tool") == 0) {
    board_status_set_state(STATUS_SIGNALING, "Tool");
    if (state_changed) {
      board_status_show_text("TOOL");
    }
  } else if (strcmp(state, "idle") == 0) {
    board_status_set_state(STATUS_CONNECTED_IDLE, "Connected");
  }
}

void handle_device_command(const cJSON *payload) {
  if (!cJSON_IsObject(payload)) {
    return;
  }

  const cJSON *command_type =
      cJSON_GetObjectItemCaseSensitive(payload, "type");
  if (!cJSON_IsString(command_type) || command_type->valuestring == nullptr) {
    return;
  }

  if (strcmp(command_type->valuestring, "beep") == 0) {
    const cJSON *frequency =
        cJSON_GetObjectItemCaseSensitive(payload, "frequency_hz");
    const cJSON *duration =
        cJSON_GetObjectItemCaseSensitive(payload, "duration_ms");
    const uint32_t frequency_hz =
        cJSON_IsNumber(frequency) ? static_cast<uint32_t>(frequency->valueint)
                                  : 1046;
    const uint32_t duration_ms =
        cJSON_IsNumber(duration) ? static_cast<uint32_t>(duration->valueint)
                                 : 90;
    board_audio_play_beep(frequency_hz, duration_ms);
  } else if (strcmp(command_type->valuestring, "audio_loopback") == 0 ||
             strcmp(command_type->valuestring, "audio.test") == 0) {
    if (s_audio_loopback_running) {
      ESP_LOGW(LOG_TAG, "Ignoring audio loopback request; already running");
      return;
    }
    const cJSON *duration =
        cJSON_GetObjectItemCaseSensitive(payload, "duration_ms");
    uint32_t duration_ms =
        cJSON_IsNumber(duration) ? static_cast<uint32_t>(duration->valueint)
                                 : 2000;
    duration_ms = duration_ms < 500 ? 500 : duration_ms;
    duration_ms = duration_ms > 3000 ? 3000 : duration_ms;
    s_audio_loopback_running = true;
    xTaskCreatePinnedToCore(run_audio_loopback_task, "audio_loopback", 4096,
                            reinterpret_cast<void *>(
                                static_cast<uintptr_t>(duration_ms)),
                            6, nullptr, 1);
  } else if (strcmp(command_type->valuestring, "audio.aec_probe") == 0) {
    if (s_audio_probe_running) {
      ESP_LOGW(LOG_TAG, "Ignoring AEC probe request; already running");
      return;
    }
    auto *config = static_cast<AecProbeTaskConfig *>(
        calloc(1, sizeof(AecProbeTaskConfig)));
    if (config == nullptr) {
      ESP_LOGE(LOG_TAG, "No memory for AEC probe config");
      return;
    }
    const cJSON *duration =
        cJSON_GetObjectItemCaseSensitive(payload, "duration_ms");
    const cJSON *frequency =
        cJSON_GetObjectItemCaseSensitive(payload, "frequency_hz");
    const cJSON *amplitude =
        cJSON_GetObjectItemCaseSensitive(payload, "amplitude");
    config->duration_ms =
        cJSON_IsNumber(duration) ? static_cast<uint32_t>(duration->valueint)
                                 : 2500;
    config->frequency_hz =
        cJSON_IsNumber(frequency) ? static_cast<uint32_t>(frequency->valueint)
                                  : 1000;
    config->amplitude =
        cJSON_IsNumber(amplitude) ? static_cast<float>(amplitude->valuedouble)
                                  : 2500.0f;
    s_audio_probe_running = true;
    BaseType_t created = xTaskCreatePinnedToCore(
        run_aec_probe_task, "aec_probe", 6144, config, 6, nullptr, 1);
    if (created != pdPASS) {
      ESP_LOGE(LOG_TAG, "Failed to start AEC probe task");
      free(config);
      s_audio_probe_running = false;
    }
  } else if (strcmp(command_type->valuestring, "set_led") == 0) {
    const cJSON *color = cJSON_GetObjectItemCaseSensitive(payload, "color");
    const cJSON *pattern = cJSON_GetObjectItemCaseSensitive(payload, "pattern");
    const char *color_value =
        cJSON_IsString(color) && color->valuestring != nullptr ? color->valuestring : "white";
    const char *pattern_value =
        cJSON_IsString(pattern) && pattern->valuestring != nullptr ? pattern->valuestring : "solid";
    ESP_LOGI(LOG_TAG, "Received set_led command color=%s pattern=%s",
             color_value, pattern_value);
    board_lights_set_effect(color_value, pattern_value);
  } else if (strcmp(command_type->valuestring, "media.mode") == 0) {
    const cJSON *playing = cJSON_GetObjectItemCaseSensitive(payload, "playing");
    s_media_mode = cJSON_IsTrue(playing);
    hermes_media_set_publish_enabled(!s_media_mode);
    if (s_media_mode) {
      s_local_wake_active = false;
      board_status_set_state(STATUS_MEDIA_PLAYING, "Playing");
      ESP_LOGI(LOG_TAG, "Media mode enabled; mic publishing paused");
    } else {
      s_local_wake_active = false;
      ESP_LOGI(LOG_TAG, "Media mode disabled; mic publishing enabled");
      hermes_media_set_publish_enabled(true);
      board_status_set_state(STATUS_CONNECTED_IDLE, "Connected");
    }
  } else if (strcmp(command_type->valuestring, "end_conversation") == 0) {
    ESP_LOGI(LOG_TAG, "Ending conversation; returning to local wake word");
    s_local_wake_active = false;
    s_media_mode = false;
    s_connection_requested = false;
    suppress_wake_for(kWakeAfterEndConversationCooldownTicks);
    hermes_media_set_publish_enabled(false);
    board_status_set_state(STATUS_CONNECTED_IDLE, "Say hey willow");
    board_status_show_text("READY");
    if (s_peer_connection != nullptr) {
      peer_connection_close(s_peer_connection);
      s_connection_closed = true;
      s_connected = false;
    }
  } else if (strcmp(command_type->valuestring, "display_text") == 0) {
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(payload, "text");
    if (cJSON_IsString(text) && text->valuestring != nullptr) {
      ESP_LOGI(LOG_TAG, "Display text: %s", text->valuestring);
      board_status_show_text(text->valuestring);
    }
  }
}

void handle_message(char *message, size_t length, void *, uint16_t) {
  if (message == nullptr || length == 0) {
    return;
  }

  char *buffer = static_cast<char *>(malloc(length + 1));
  if (buffer == nullptr) {
    ESP_LOGE(LOG_TAG, "No memory for data channel message");
    return;
  }

  memcpy(buffer, message, length);
  buffer[length] = '\0';

  cJSON *json = cJSON_Parse(buffer);
  if (json == nullptr) {
    free(buffer);
    return;
  }

  const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
  const cJSON *payload = cJSON_GetObjectItemCaseSensitive(json, "payload");

  if (cJSON_IsString(type) && type->valuestring != nullptr) {
    const bool is_audio_level =
        strcmp(type->valuestring, "audio.input.level") == 0;
    if (!is_audio_level) {
      ESP_LOGI(LOG_TAG, "DataChannel <- %s", buffer);
    }

    if (strcmp(type->valuestring, "assistant.state") == 0 &&
        cJSON_IsObject(payload)) {
      note_session_activity();
      const cJSON *state = cJSON_GetObjectItemCaseSensitive(payload, "state");
      if (cJSON_IsString(state) && state->valuestring != nullptr) {
        apply_assistant_state(state->valuestring);
      }
    } else if (strcmp(type->valuestring, "assistant.text.delta") == 0 &&
               cJSON_IsObject(payload)) {
      note_session_activity();
      const cJSON *text = cJSON_GetObjectItemCaseSensitive(payload, "text");
      if (cJSON_IsString(text) && text->valuestring != nullptr) {
        ESP_LOGI(LOG_TAG, "Assistant text: %s", text->valuestring);
      }
    } else if (strcmp(type->valuestring, "tool.progress") == 0 &&
               cJSON_IsObject(payload)) {
      note_session_activity();
      const cJSON *phase = cJSON_GetObjectItemCaseSensitive(payload, "phase");
      if (cJSON_IsString(phase) && phase->valuestring != nullptr) {
        ESP_LOGI(LOG_TAG, "Tool phase: %s", phase->valuestring);
        if (strcmp(phase->valuestring, "call") == 0) {
          board_status_set_state(STATUS_SIGNALING, "Tool");
          board_status_show_text("TOOL");
        } else if (strcmp(phase->valuestring, "output") == 0) {
          board_status_show_text("DONE");
        }
      }
    } else if (strcmp(type->valuestring, "audio.input.level") == 0 &&
               cJSON_IsObject(payload)) {
      const cJSON *has_transcript =
          cJSON_GetObjectItemCaseSensitive(payload, "has_transcript");
      const cJSON *frames_seen =
          cJSON_GetObjectItemCaseSensitive(payload, "frames_seen");
      const cJSON *pcm_peak =
          cJSON_GetObjectItemCaseSensitive(payload, "pcm_peak");
      const cJSON *pcm_rms =
          cJSON_GetObjectItemCaseSensitive(payload, "pcm_rms");
      if (cJSON_IsTrue(has_transcript)) {
        note_session_activity();
        ESP_LOGI(LOG_TAG, "Audio input has transcript activity");
      } else if ((cJSON_IsNumber(pcm_peak) && pcm_peak->valueint >= 150) ||
                 (cJSON_IsNumber(pcm_rms) && pcm_rms->valueint >= 35)) {
        note_session_activity();
        ESP_LOGI(LOG_TAG, "Audio input speech-like level: frames=%d peak=%d rms=%d",
                 cJSON_IsNumber(frames_seen) ? frames_seen->valueint : 0,
                 cJSON_IsNumber(pcm_peak) ? pcm_peak->valueint : 0,
                 cJSON_IsNumber(pcm_rms) ? pcm_rms->valueint : 0);
      } else if (cJSON_IsNumber(frames_seen)) {
        ESP_LOGI(LOG_TAG, "Audio input frames seen: %d", frames_seen->valueint);
      }
    } else if (strcmp(type->valuestring, "device.command") == 0) {
      note_session_activity();
      handle_device_command(payload);
    } else if (strcmp(type->valuestring, "error") == 0 && cJSON_IsObject(payload)) {
      note_session_activity();
      const cJSON *error_message =
          cJSON_GetObjectItemCaseSensitive(payload, "message");
      if (cJSON_IsString(error_message) && error_message->valuestring != nullptr) {
        ESP_LOGE(LOG_TAG, "Gateway error: %s", error_message->valuestring);
      }
      board_status_set_state(STATUS_ERROR, "Gateway error");
    }
  }

  cJSON_Delete(json);
  free(buffer);
}

void maybe_send_hello() {
  if (s_peer_connection == nullptr || !s_connected || !s_datachannel_created ||
      s_hello_sent) {
    return;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  cJSON *caps = cJSON_AddArrayToObject(payload, "capabilities");
  if (root == nullptr || payload == nullptr || caps == nullptr) {
    cJSON_Delete(root);
    return;
  }

  cJSON_AddStringToObject(root, "type", "hello");
  cJSON_AddStringToObject(payload, "device_id", HERMES_IOT_DEVICE_ID);
  cJSON_AddStringToObject(payload, "firmware_version",
                          HERMES_IOT_FIRMWARE_VERSION);
  cJSON_AddStringToObject(payload, "transport", "webrtc");
  cJSON_AddNumberToObject(payload, "sample_rate_hz", 16000);
  cJSON_AddStringToObject(payload, "codec", "opus");
  if (s_local_wake_active) {
    cJSON_AddBoolToObject(payload, "wake_detected", true);
    if (s_media_mode) {
      cJSON_AddBoolToObject(payload, "media_barge_in", true);
    }
  }
  cJSON_AddItemToArray(caps, cJSON_CreateString("speaker"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("mic"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("audio_reference"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("audio_stats"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("audio_loopback"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("touch"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("rgb"));

  const bool sent = send_control_json(root);
  cJSON_Delete(root);
  if (sent) {
    s_hello_sent = true;
    note_session_activity();
    ESP_LOGI(LOG_TAG, "Sent hello on control channel");
    ensure_audio_task_running();
    if (s_local_wake_active) {
      hermes_media_set_publish_enabled(true);
      apply_assistant_state("listening");
      send_wake_detected_state();
    } else if (!s_media_mode) {
      apply_assistant_state("listening");
    }
  }
}

}  // namespace

static esp_err_t create_peer_connection() {
  if (s_peer_connection != nullptr) {
    return ESP_OK;
  }
  reset_session_state();
  board_status_set_state(STATUS_SIGNALING, "Offer");

  PeerConfiguration config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack =
      [](uint8_t *data, size_t size, void *) -> void {
        if (size > 0 && hermes_media_handle_remote_audio(data, size)) {
          note_session_activity();
        }
      },
      .onvideotrack = nullptr,
      .on_request_keyframe = nullptr,
      .user_data = nullptr,
  };

  s_peer_connection = peer_connection_create(&config);
  if (s_peer_connection == nullptr) {
    board_status_set_state(STATUS_ERROR, "Peer create");
    return ESP_FAIL;
  }

  peer_connection_oniceconnectionstatechange(
      s_peer_connection,
      [](PeerConnectionState state, void *) {
        ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
                 peer_connection_state_to_string(state));
        if (state == PEER_CONNECTION_CONNECTED ||
            state == PEER_CONNECTION_COMPLETED) {
          s_connected = true;
          s_connection_requested = false;
          note_session_activity();
          board_status_set_state(s_media_mode ? STATUS_MEDIA_PLAYING
                                              : STATUS_SIGNALING,
                                 s_media_mode ? "Playing" : "Opening");
          hermes_media_reset_playback();
          return;
        }

        if (state == PEER_CONNECTION_FAILED ||
            state == PEER_CONNECTION_DISCONNECTED ||
            state == PEER_CONNECTION_CLOSED) {
          s_connection_closed = true;
    s_connected = false;
    stop_audio_task();
    hermes_media_reset_playback();
    suppress_wake_for(kWakeAfterCloseCooldownTicks);
    board_status_set_state(STATUS_ERROR, "Disconnected");
  }
      });

  peer_connection_onicecandidate(
      s_peer_connection,
      [](char *description, void *) {
        if (description == nullptr || s_peer_connection == nullptr) {
          return;
        }

        GatewayOfferResponse response = {};
        esp_err_t result = gateway_client_post_offer(&s_bootstrap, description, &response);
        if (result == ESP_ERR_INVALID_STATE) {
          ESP_LOGW(LOG_TAG, "Offer rejected by gateway; refreshing device claim");
          GatewayBootstrapInfo refreshed_bootstrap = {};
          result = gateway_client_claim_device(&refreshed_bootstrap);
          if (result == ESP_OK) {
            esp_err_t save_result = device_identity_save(&refreshed_bootstrap);
            if (save_result != ESP_OK) {
              ESP_LOGW(LOG_TAG, "Failed to save refreshed device identity: %s",
                       esp_err_to_name(save_result));
            }
            memcpy(&s_bootstrap, &refreshed_bootstrap, sizeof(s_bootstrap));
            result = gateway_client_post_offer(&s_bootstrap, description, &response);
          }
        }
        if (result != ESP_OK) {
          ESP_LOGE(LOG_TAG, "Offer exchange failed: %s", esp_err_to_name(result));
          board_status_set_state(STATUS_ERROR, "Offer failed");
          return;
        }

        peer_connection_set_remote_description(s_peer_connection, response.sdp,
                                               SDP_TYPE_ANSWER);
        ESP_LOGI(LOG_TAG, "Applied remote answer session=%s", response.session_id);
      });

  peer_connection_ondatachannel(
      s_peer_connection, handle_message,
      [](void *) {
        if (s_peer_connection == nullptr || s_datachannel_created) {
          return;
        }

        const int result = peer_connection_create_datachannel(
            s_peer_connection, DATA_CHANNEL_RELIABLE, 0, 0,
            const_cast<char *>(kDataChannelLabel), const_cast<char *>("json"));
        if (result >= 0) {
          s_datachannel_created = true;
          ESP_LOGI(LOG_TAG, "Created control data channel");
        } else {
          ESP_LOGE(LOG_TAG, "Failed to create control data channel");
          board_status_set_state(STATUS_ERROR, "Data channel");
        }
      },
      nullptr);

  peer_connection_create_offer(s_peer_connection);
  return ESP_OK;
}

esp_err_t hermes_webrtc_start(const GatewayBootstrapInfo *bootstrap) {
  if (bootstrap == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(&s_bootstrap, 0, sizeof(s_bootstrap));
  memcpy(&s_bootstrap, bootstrap, sizeof(s_bootstrap));
  s_bootstrap_ready = true;
  s_connection_requested = false;
  board_status_set_state(STATUS_CONNECTED_IDLE, "Say hey willow");
  ESP_LOGI(LOG_TAG, "WebRTC armed; waiting for local wake word");
  return ESP_OK;
}

void hermes_webrtc_request_connection(void) {
  if (!s_bootstrap_ready) {
    ESP_LOGW(LOG_TAG, "Ignoring wake request before gateway bootstrap");
    return;
  }
  if (wake_suppressed()) {
    ESP_LOGI(LOG_TAG, "Ignoring wake request during post-close cooldown");
    return;
  }
  if (s_peer_connection != nullptr && s_connected) {
    ESP_LOGI(LOG_TAG, "Wake requested on active WebRTC session");
    s_local_wake_active = true;
    hermes_media_set_publish_enabled(true);
    note_session_activity();
    board_status_set_state(STATUS_LISTENING, "Listening");
    strncpy(s_last_assistant_state, "listening",
            sizeof(s_last_assistant_state) - 1);
    s_last_assistant_state[sizeof(s_last_assistant_state) - 1] = '\0';
    board_status_show_text("LISTEN");
    board_audio_play_listening_tone();
    send_wake_detected_state();
    return;
  }
  if (s_connection_requested || s_peer_connection != nullptr) {
    return;
  }

  ESP_LOGI(LOG_TAG, "Wake requested WebRTC session");
  s_local_wake_active = true;
  s_connection_requested = true;
  note_session_activity();
  board_status_set_state(STATUS_SIGNALING, "Waking");
  board_status_show_text("WAKE");
}

bool hermes_webrtc_connection_requested(void) {
  return s_connection_requested || (s_peer_connection != nullptr && !s_media_mode);
}

void hermes_webrtc_loop(void) {
  if (s_connection_closed && s_peer_connection != nullptr) {
    destroy_peer_connection();
    s_local_wake_active = false;
    return;
  }

  if (s_connection_requested && s_peer_connection == nullptr) {
    hermes_media_set_publish_enabled(true);
    esp_err_t result = create_peer_connection();
    if (result != ESP_OK) {
      ESP_LOGE(LOG_TAG, "Failed to create WebRTC session: %s",
               esp_err_to_name(result));
      s_connection_requested = false;
      board_status_set_state(STATUS_ERROR, "WebRTC failed");
    }
  }

  if (s_peer_connection == nullptr) {
    return;
  }

  if (!s_hello_sent && s_last_activity_tick != 0 &&
      xTaskGetTickCount() - s_last_activity_tick >=
          kSessionHandshakeDisconnectTicks) {
    ESP_LOGW(LOG_TAG, "WebRTC handshake timed out; returning to wake word");
    peer_connection_close(s_peer_connection);
    s_connection_closed = true;
  } else
  if (s_connected && s_last_activity_tick != 0 &&
      xTaskGetTickCount() - s_last_activity_tick >=
          kSessionIdleDisconnectTicks) {
    ESP_LOGI(LOG_TAG, "WebRTC session idle; returning to wake word");
    peer_connection_close(s_peer_connection);
    s_connection_closed = true;
  }

  peer_connection_loop(s_peer_connection);
  maybe_send_hello();
}

bool hermes_webrtc_connected(void) { return s_connected; }

bool hermes_webrtc_session_active(void) {
  return s_peer_connection != nullptr && !s_connection_closed;
}

void hermes_webrtc_send_volume(uint8_t volume_percent) {
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  if (root == nullptr || payload == nullptr) {
    cJSON_Delete(root);
    return;
  }

  cJSON_AddStringToObject(root, "type", "volume.set");
  cJSON_AddNumberToObject(payload, "volume", volume_percent);
  if (!send_control_json(root)) {
    ESP_LOGD(LOG_TAG, "Skipped volume.set; control channel is not ready");
  }
  cJSON_Delete(root);
}
