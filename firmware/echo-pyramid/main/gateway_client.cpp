#include "gateway_client.h"

#include <cJSON.h>
#include <esp_check.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#include "main.h"

namespace {

constexpr int kHttpTimeoutMs = 10000;

typedef struct {
  char *buffer;
  size_t capacity;
  size_t length;
} HttpResponseBuffer;

esp_err_t http_event_handler(esp_http_client_event_t *event) {
  auto *response = static_cast<HttpResponseBuffer *>(event->user_data);
  switch (event->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
      if (response != nullptr && response->buffer != nullptr) {
        response->length = 0;
        memset(response->buffer, 0, response->capacity);
      }
      break;
    case HTTP_EVENT_ON_DATA:
      if (response == nullptr || response->buffer == nullptr ||
          esp_http_client_is_chunked_response(event->client)) {
        break;
      }
      if (response->length >= response->capacity) {
        break;
      }
      {
        const size_t remaining = response->capacity - response->length - 1;
        const size_t copy_length =
            remaining < static_cast<size_t>(event->data_len)
                ? remaining
                : static_cast<size_t>(event->data_len);
        if (copy_length > 0) {
          memcpy(response->buffer + response->length, event->data, copy_length);
          response->length += copy_length;
          response->buffer[response->length] = '\0';
        }
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

esp_err_t build_url(const char *path, char *out_url, size_t out_url_size) {
  const int written = snprintf(out_url, out_url_size, "%s%s",
                               HERMES_IOT_GATEWAY_BASE_URL, path);
  if (written < 0 || static_cast<size_t>(written) >= out_url_size) {
    return ESP_ERR_INVALID_SIZE;
  }
  return ESP_OK;
}

esp_err_t perform_request(esp_http_client_method_t method, const char *url,
                          const char *body, const char *bearer_token,
                          HttpResponseBuffer *response) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.method = method;
  config.timeout_ms = kHttpTimeoutMs;
  config.event_handler = http_event_handler;
  config.user_data = response;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  if (body != nullptr) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
  }
  if (bearer_token != nullptr && bearer_token[0] != '\0') {
    char auth_header[320] = {0};
    const int written = snprintf(auth_header, sizeof(auth_header), "Bearer %s",
                                 bearer_token);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(auth_header)) {
      esp_http_client_cleanup(client);
      return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);
  }

  const esp_err_t perform_result = esp_http_client_perform(client);
  const int status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (perform_result != ESP_OK) {
    return perform_result;
  }
  if (status_code < 200 || status_code >= 300) {
    ESP_LOGE(LOG_TAG, "Gateway request failed: status=%d body=%s", status_code,
             response != nullptr && response->buffer != nullptr ? response->buffer
                                                                : "");
    if (status_code == 401) {
      return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
  }
  return ESP_OK;
}

}  // namespace

bool gateway_client_is_configured(void) {
  return strlen(HERMES_IOT_GATEWAY_BASE_URL) > 0;
}

esp_err_t gateway_client_health_check(void) {
  char url[256] = {0};
  char response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
  HttpResponseBuffer response = {
      .buffer = response_buffer,
      .capacity = sizeof(response_buffer),
      .length = 0,
  };

  ESP_RETURN_ON_ERROR(build_url("/health", url, sizeof(url)), LOG_TAG,
                      "Failed to build health URL");
  ESP_RETURN_ON_ERROR(
      perform_request(HTTP_METHOD_GET, url, nullptr, nullptr, &response),
                      LOG_TAG, "Gateway health check failed");
  ESP_LOGI(LOG_TAG, "Gateway health OK: %s", response_buffer);
  return ESP_OK;
}

esp_err_t gateway_client_claim_device(GatewayBootstrapInfo *out_info) {
  if (out_info == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  char url[256] = {0};
  char response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
  HttpResponseBuffer response = {
      .buffer = response_buffer,
      .capacity = sizeof(response_buffer),
      .length = 0,
  };

  cJSON *request = cJSON_CreateObject();
  if (request == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  const char *capabilities[] = {"speaker", "mic", "touch", "rgb"};
  cJSON *capability_array = cJSON_AddArrayToObject(request, "capabilities");
  if (capability_array == nullptr) {
    cJSON_Delete(request);
    return ESP_ERR_NO_MEM;
  }
  for (size_t i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); ++i) {
    cJSON *item = cJSON_CreateString(capabilities[i]);
    if (item == nullptr) {
      cJSON_Delete(request);
      return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(capability_array, item);
  }

  if (cJSON_AddStringToObject(request, "device_id", HERMES_IOT_DEVICE_ID) ==
          nullptr ||
      cJSON_AddStringToObject(request, "firmware_version",
                              HERMES_IOT_FIRMWARE_VERSION) == nullptr) {
    cJSON_Delete(request);
    return ESP_ERR_NO_MEM;
  }

  char *request_body = cJSON_PrintUnformatted(request);
  cJSON_Delete(request);
  if (request_body == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = build_url("/v1/pair/claim", url, sizeof(url));
  if (result == ESP_OK) {
    result = perform_request(HTTP_METHOD_POST, url, request_body, nullptr,
                             &response);
  }
  cJSON_free(request_body);
  ESP_RETURN_ON_ERROR(result, LOG_TAG, "Gateway claim request failed");

  cJSON *json = cJSON_Parse(response_buffer);
  if (json == nullptr) {
    return ESP_FAIL;
  }

  const cJSON *auth_token = cJSON_GetObjectItemCaseSensitive(json, "auth_token");
  const cJSON *signaling_url =
      cJSON_GetObjectItemCaseSensitive(json, "signaling_url");
  const cJSON *conversation =
      cJSON_GetObjectItemCaseSensitive(json, "conversation");
  if (!cJSON_IsString(auth_token) || !cJSON_IsString(signaling_url) ||
      !cJSON_IsString(conversation) || auth_token->valuestring == nullptr ||
      signaling_url->valuestring == nullptr ||
      conversation->valuestring == nullptr) {
    cJSON_Delete(json);
    return ESP_FAIL;
  }

  memset(out_info, 0, sizeof(*out_info));
  strncpy(out_info->auth_token, auth_token->valuestring,
          sizeof(out_info->auth_token) - 1);
  strncpy(out_info->signaling_url, signaling_url->valuestring,
          sizeof(out_info->signaling_url) - 1);
  strncpy(out_info->conversation, conversation->valuestring,
          sizeof(out_info->conversation) - 1);
  cJSON_Delete(json);

  ESP_LOGI(LOG_TAG, "Claimed device %s conversation=%s", HERMES_IOT_DEVICE_ID,
           out_info->conversation);
  return ESP_OK;
}

esp_err_t gateway_client_post_offer(const GatewayBootstrapInfo *bootstrap,
                                    const char *offer_sdp,
                                    GatewayOfferResponse *out_response) {
  if (bootstrap == nullptr || offer_sdp == nullptr || out_response == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  char *response_buffer = static_cast<char *>(calloc(1, MAX_SIGNALING_SDP_BUFFER));
  if (response_buffer == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  HttpResponseBuffer response = {
      .buffer = response_buffer,
      .capacity = MAX_SIGNALING_SDP_BUFFER,
      .length = 0,
  };

  cJSON *request = cJSON_CreateObject();
  cJSON *hello = cJSON_AddObjectToObject(request, "hello");
  cJSON *caps = hello != nullptr ? cJSON_AddArrayToObject(hello, "capabilities")
                                 : nullptr;
  if (request == nullptr || hello == nullptr || caps == nullptr) {
    cJSON_Delete(request);
    free(response_buffer);
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddStringToObject(request, "type", "offer");
  cJSON_AddStringToObject(request, "sdp", offer_sdp);
  cJSON_AddStringToObject(hello, "device_id", HERMES_IOT_DEVICE_ID);
  cJSON_AddStringToObject(hello, "firmware_version",
                          HERMES_IOT_FIRMWARE_VERSION);
  cJSON_AddStringToObject(hello, "transport", "webrtc");
  cJSON_AddNumberToObject(hello, "sample_rate_hz", 16000);
  cJSON_AddStringToObject(hello, "codec", "pcm16");
  cJSON_AddItemToArray(caps, cJSON_CreateString("speaker"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("mic"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("touch"));
  cJSON_AddItemToArray(caps, cJSON_CreateString("rgb"));

  char *body = cJSON_PrintUnformatted(request);
  cJSON_Delete(request);
  if (body == nullptr) {
    free(response_buffer);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = perform_request(HTTP_METHOD_POST, bootstrap->signaling_url,
                                     body, bootstrap->auth_token, &response);
  cJSON_free(body);
  if (result != ESP_OK) {
    free(response_buffer);
    return result;
  }

  cJSON *json = cJSON_Parse(response_buffer);
  free(response_buffer);
  if (json == nullptr) {
    return ESP_FAIL;
  }

  const cJSON *sdp = cJSON_GetObjectItemCaseSensitive(json, "sdp");
  const cJSON *session_id = cJSON_GetObjectItemCaseSensitive(json, "session_id");
  const cJSON *conversation =
      cJSON_GetObjectItemCaseSensitive(json, "conversation");
  if (!cJSON_IsString(sdp) || sdp->valuestring == nullptr) {
    cJSON_Delete(json);
    return ESP_FAIL;
  }

  memset(out_response, 0, sizeof(*out_response));
  strncpy(out_response->sdp, sdp->valuestring, sizeof(out_response->sdp) - 1);
  if (cJSON_IsString(session_id) && session_id->valuestring != nullptr) {
    strncpy(out_response->session_id, session_id->valuestring,
            sizeof(out_response->session_id) - 1);
  }
  if (cJSON_IsString(conversation) && conversation->valuestring != nullptr) {
    strncpy(out_response->conversation, conversation->valuestring,
            sizeof(out_response->conversation) - 1);
  }
  cJSON_Delete(json);
  return ESP_OK;
}
