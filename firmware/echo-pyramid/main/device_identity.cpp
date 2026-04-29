#include "device_identity.h"

#include <nvs.h>
#include <string.h>

namespace {

constexpr const char *kNamespace = "hermes_iot";
constexpr const char *kAuthTokenKey = "auth_token";
constexpr const char *kSignalUrlKey = "signal_url";
constexpr const char *kConversationKey = "conversation";

esp_err_t open_identity_store(nvs_handle_t *out_handle, nvs_open_mode_t mode) {
  if (out_handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return nvs_open(kNamespace, mode, out_handle);
}

esp_err_t load_string(nvs_handle_t handle, const char *key, char *buffer,
                      size_t buffer_size) {
  size_t required_size = buffer_size;
  esp_err_t result = nvs_get_str(handle, key, buffer, &required_size);
  if (result != ESP_OK) {
    return result;
  }
  if (required_size == 0 || required_size > buffer_size) {
    return ESP_ERR_INVALID_SIZE;
  }
  buffer[buffer_size - 1] = '\0';
  return ESP_OK;
}

}  // namespace

bool device_identity_is_saved(void) {
  GatewayBootstrapInfo bootstrap = {};
  return device_identity_load(&bootstrap) == ESP_OK;
}

esp_err_t device_identity_load(GatewayBootstrapInfo *out_info) {
  if (out_info == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t handle = 0;
  esp_err_t result = open_identity_store(&handle, NVS_READONLY);
  if (result != ESP_OK) {
    return result;
  }

  memset(out_info, 0, sizeof(*out_info));
  result = load_string(handle, kAuthTokenKey, out_info->auth_token,
                       sizeof(out_info->auth_token));
  if (result == ESP_OK) {
    result = load_string(handle, kSignalUrlKey, out_info->signaling_url,
                         sizeof(out_info->signaling_url));
  }
  if (result == ESP_OK) {
    result = load_string(handle, kConversationKey, out_info->conversation,
                         sizeof(out_info->conversation));
  }

  nvs_close(handle);
  return result;
}

esp_err_t device_identity_save(const GatewayBootstrapInfo *info) {
  if (info == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t handle = 0;
  esp_err_t result = open_identity_store(&handle, NVS_READWRITE);
  if (result != ESP_OK) {
    return result;
  }

  result = nvs_set_str(handle, kAuthTokenKey, info->auth_token);
  if (result == ESP_OK) {
    result = nvs_set_str(handle, kSignalUrlKey, info->signaling_url);
  }
  if (result == ESP_OK) {
    result = nvs_set_str(handle, kConversationKey, info->conversation);
  }
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }

  nvs_close(handle);
  return result;
}

esp_err_t device_identity_clear(void) {
  nvs_handle_t handle = 0;
  esp_err_t result = open_identity_store(&handle, NVS_READWRITE);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (result != ESP_OK) {
    return result;
  }

  esp_err_t auth_result = nvs_erase_key(handle, kAuthTokenKey);
  if (auth_result != ESP_OK && auth_result != ESP_ERR_NVS_NOT_FOUND) {
    result = auth_result;
  }
  if (result == ESP_OK) {
    esp_err_t signal_result = nvs_erase_key(handle, kSignalUrlKey);
    if (signal_result != ESP_OK && signal_result != ESP_ERR_NVS_NOT_FOUND) {
      result = signal_result;
    }
  }
  if (result == ESP_OK) {
    esp_err_t conversation_result = nvs_erase_key(handle, kConversationKey);
    if (conversation_result != ESP_OK &&
        conversation_result != ESP_ERR_NVS_NOT_FOUND) {
      result = conversation_result;
    }
  }
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }

  nvs_close(handle);
  return result;
}
