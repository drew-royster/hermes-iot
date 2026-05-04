#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gateway_client.h"

esp_err_t hermes_webrtc_start(const GatewayBootstrapInfo *bootstrap);
void hermes_webrtc_request_connection(void);
void hermes_webrtc_toggle_wake_sleep(void);
bool hermes_webrtc_connection_requested(void);
void hermes_webrtc_loop(void);
bool hermes_webrtc_connected(void);
bool hermes_webrtc_session_active(void);
void hermes_webrtc_send_volume(uint8_t volume_percent);
