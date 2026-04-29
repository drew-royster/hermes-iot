#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

typedef struct PeerConnection PeerConnection;

typedef struct {
  uint32_t sent_packets;
  uint32_t send_failures;
  uint32_t playback_underruns;
  uint32_t playback_overflows;
  uint32_t remote_silence_packets;
  int32_t encoder_input_peak;
  int32_t encoder_peak;
  int32_t capture_gain_q8;
  bool remote_playback_active;
} HermesMediaStats;

esp_err_t hermes_media_init(void);
bool hermes_media_handle_remote_audio(uint8_t *data, size_t size);
void hermes_media_reset_playback(void);
void hermes_media_prepare_encoder(void);
void hermes_media_send_audio(PeerConnection *peer_connection);
void hermes_media_get_stats(HermesMediaStats *stats);
void hermes_media_set_publish_enabled(bool enabled);
void hermes_media_set_playback_enabled(bool enabled);
