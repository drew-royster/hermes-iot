#include "hermes_media.h"

#include <opus.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "peer.h"

#include "board.h"
#include "main.h"

namespace {

constexpr int kPlaybackSampleRate = 48000;
constexpr int kCaptureSampleRate = 16000;
constexpr int kOpusBufferSize = 1276;
constexpr size_t kPlaybackFrameSamples = kPlaybackSampleRate / 50;
constexpr size_t kPlaybackFrameBytes = kPlaybackFrameSamples * sizeof(opus_int16);
constexpr size_t kCaptureFrameSamples = kCaptureSampleRate / 50;
constexpr size_t kCaptureFrameBytes = kCaptureFrameSamples * sizeof(opus_int16);
constexpr size_t kPcmMaxDecodeSamples = (kPlaybackSampleRate * 60) / 1000;
constexpr size_t kPlaybackPrebufferFrames = 10;
constexpr size_t kPlaybackQueueFrames = 48;
constexpr uint32_t kPlaybackResetUnderrunFrames = 15;
constexpr int kOpusEncoderBitrate = 30000;
constexpr int kOpusEncoderComplexity = 0;
constexpr int16_t kRemoteAudioActivityPeak = 32;
constexpr int32_t kCaptureAgcTargetPeak = 5000;
constexpr int32_t kCaptureAgcNoisePeakFloor = 4;
constexpr int32_t kCaptureAgcRawSpeechPeakFloor = 160;
constexpr int32_t kCaptureAgcPlaybackTargetPeak = 3000;
constexpr int32_t kCaptureAgcPlaybackNoisePeakFloor = 200;
constexpr int32_t kCaptureAgcUnityQ8 = 256;
constexpr int32_t kCaptureAgcMaxGainQ8 = 32 * kCaptureAgcUnityQ8;
constexpr int32_t kCaptureAgcIdleMaxGainQ8 = 8 * kCaptureAgcUnityQ8;
constexpr int32_t kCaptureAgcPlaybackMaxGainQ8 = 6 * kCaptureAgcUnityQ8;
constexpr TickType_t kPlaybackActiveHoldTicks = pdMS_TO_TICKS(300);

OpusDecoder *s_opus_decoder = nullptr;
OpusEncoder *s_opus_encoder = nullptr;
opus_int16 *s_decoder_buffer = nullptr;
opus_int16 *s_playback_accumulator = nullptr;
size_t s_playback_accumulator_samples = 0;
uint8_t *s_encoder_output_buffer = nullptr;
int16_t *s_read_buffer = nullptr;
RingbufHandle_t s_decoder_buffer_queue = nullptr;
StaticRingbuffer_t s_ringbuffer_struct = {};
uint32_t s_playback_queue_underruns = 0;
uint32_t s_playback_queue_overflows = 0;
uint32_t s_sent_packets = 0;
uint32_t s_send_failures = 0;
uint32_t s_remote_silence_packets = 0;
int32_t s_encoder_input_peak = 0;
int32_t s_encoder_peak = 0;
int32_t s_capture_gain_q8 = kCaptureAgcUnityQ8;
bool s_remote_playback_active = false;
bool s_initialized = false;
bool s_encoder_ready = false;
bool s_publish_enabled = true;
bool s_playback_enabled = true;
TickType_t s_remote_playback_active_until = 0;

int32_t sample_abs(int16_t sample) {
  const int32_t value = sample;
  return value < 0 ? -value : value;
}

int32_t peak_for_frame(const int16_t *samples, size_t sample_count) {
  int32_t peak = 0;
  for (size_t i = 0; i < sample_count; ++i) {
    peak = std::max(peak, sample_abs(samples[i]));
  }
  return peak;
}

int16_t scale_sample_q8(int16_t sample, int32_t gain_q8) {
  const int32_t scaled =
      (static_cast<int32_t>(sample) * gain_q8) / kCaptureAgcUnityQ8;
  if (scaled > INT16_MAX) {
    return INT16_MAX;
  }
  if (scaled < INT16_MIN) {
    return INT16_MIN;
  }
  return static_cast<int16_t>(scaled);
}

int32_t apply_capture_agc(int16_t *samples, size_t sample_count,
                          int32_t input_peak, int32_t raw_mic_peak,
                          bool remote_playback_active) {
  const int32_t target_peak = remote_playback_active
                                  ? kCaptureAgcPlaybackTargetPeak
                                  : kCaptureAgcTargetPeak;
  const int32_t noise_floor = remote_playback_active
                                  ? kCaptureAgcPlaybackNoisePeakFloor
                                  : kCaptureAgcNoisePeakFloor;
  int32_t max_gain_q8 = remote_playback_active ? kCaptureAgcPlaybackMaxGainQ8
                                               : kCaptureAgcMaxGainQ8;
  if (!remote_playback_active && raw_mic_peak < kCaptureAgcRawSpeechPeakFloor) {
    max_gain_q8 = kCaptureAgcIdleMaxGainQ8;
  }
  int32_t gain_q8 = kCaptureAgcUnityQ8;
  if (input_peak >= noise_floor && input_peak < target_peak) {
    gain_q8 =
        std::min(max_gain_q8, (target_peak * kCaptureAgcUnityQ8) / input_peak);
  }

  if (gain_q8 == kCaptureAgcUnityQ8) {
    return gain_q8;
  }

  for (size_t i = 0; i < sample_count; ++i) {
    samples[i] = scale_sample_q8(samples[i], gain_q8);
  }
  return gain_q8;
}

int play_audio(const void *data, size_t size) {
  if (!s_playback_enabled) {
    return static_cast<int>(size);
  }
  ESP_ERROR_CHECK_WITHOUT_ABORT(board_audio_write(data, size));
  return static_cast<int>(size);
}

void playback_task(void *) {
  size_t len = 0;
  bool playback_started = false;
  size_t prebuffered_frames = 0;
  uint8_t *prebuffer[kPlaybackPrebufferFrames] = {0};
  opus_int16 silence_frame[kPlaybackFrameSamples] = {0};
  uint32_t consecutive_underruns = 0;
  uint32_t played_frames = 0;

  auto release_prebuffer = [&](bool play) {
    for (size_t i = 0; i < prebuffered_frames; ++i) {
      if (play) {
        play_audio(prebuffer[i], kPlaybackFrameBytes);
      }
      vRingbufferReturnItem(s_decoder_buffer_queue, prebuffer[i]);
      prebuffer[i] = nullptr;
    }
    prebuffered_frames = 0;
  };

  while (true) {
    auto *audio_buffer = static_cast<uint8_t *>(
        xRingbufferReceive(s_decoder_buffer_queue, &len,
                           playback_started ? 0 : portMAX_DELAY));

    if (audio_buffer == nullptr) {
      if (playback_started) {
        play_audio(silence_frame, sizeof(silence_frame));
        ++s_playback_queue_underruns;
        ++consecutive_underruns;
        ESP_LOGD(LOG_TAG, "Playback queue underrun count=%lu",
                 static_cast<unsigned long>(s_playback_queue_underruns));
        if (consecutive_underruns < kPlaybackResetUnderrunFrames) {
          continue;
        }
      }

      release_prebuffer(false);
      playback_started = false;
      consecutive_underruns = 0;
      continue;
    }

    if (len != kPlaybackFrameBytes) {
      ESP_LOGW(LOG_TAG, "Unexpected playback frame size: %u",
               static_cast<unsigned>(len));
    }

    if (!playback_started) {
      prebuffer[prebuffered_frames++] = audio_buffer;
      if (prebuffered_frames < kPlaybackPrebufferFrames) {
        continue;
      }

      release_prebuffer(true);
      playback_started = true;
      consecutive_underruns = 0;
      continue;
    }

    consecutive_underruns = 0;
    play_audio(audio_buffer, len);
    ++played_frames;
    if (played_frames == 1 || (played_frames % 50) == 0) {
      ESP_LOGI(LOG_TAG, "Playback wrote frame count=%lu",
               static_cast<unsigned long>(played_frames));
    }
    vRingbufferReturnItem(s_decoder_buffer_queue, audio_buffer);
  }
}

esp_err_t init_decoder() {
  int decoder_error = 0;
  s_opus_decoder = opus_decoder_create(kPlaybackSampleRate, 1, &decoder_error);
  if (decoder_error != OPUS_OK || s_opus_decoder == nullptr) {
    ESP_LOGE(LOG_TAG, "opus_decoder_create failed: %d", decoder_error);
    return ESP_FAIL;
  }

  s_decoder_buffer = static_cast<opus_int16 *>(
      malloc(kPcmMaxDecodeSamples * sizeof(opus_int16)));
  s_playback_accumulator = static_cast<opus_int16 *>(
      malloc((kPcmMaxDecodeSamples + kPlaybackFrameSamples) * sizeof(opus_int16)));
  if (s_decoder_buffer == nullptr || s_playback_accumulator == nullptr) {
    ESP_LOGE(LOG_TAG, "No memory for decoder buffers");
    return ESP_ERR_NO_MEM;
  }

  const size_t ring_buffer_size =
      kPlaybackFrameBytes * kPlaybackQueueFrames + (kPlaybackQueueFrames * 10);
  auto *ring_storage = static_cast<uint8_t *>(malloc(ring_buffer_size));
  if (ring_storage == nullptr) {
    ESP_LOGE(LOG_TAG, "No memory for playback ring buffer");
    return ESP_ERR_NO_MEM;
  }

  s_decoder_buffer_queue = xRingbufferCreateStatic(
      ring_buffer_size, RINGBUF_TYPE_NOSPLIT, ring_storage,
      &s_ringbuffer_struct);
  if (s_decoder_buffer_queue == nullptr) {
    ESP_LOGE(LOG_TAG, "Failed to create playback ring buffer");
    return ESP_FAIL;
  }

  xTaskCreatePinnedToCore(playback_task, "hermes_playback", 4096, nullptr, 8,
                          nullptr, 1);
  return ESP_OK;
}

}  // namespace

esp_err_t hermes_media_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }

  if (board_audio_set_output_enabled(true) != ESP_OK) {
    ESP_LOGW(LOG_TAG, "Speaker output enable failed during media init");
  }

  esp_err_t result = init_decoder();
  if (result != ESP_OK) {
    return result;
  }

  s_initialized = true;
  return ESP_OK;
}

void hermes_media_prepare_encoder(void) {
  if (s_encoder_ready) {
    return;
  }

  int encoder_error = 0;
  s_opus_encoder =
      opus_encoder_create(kCaptureSampleRate, 1, OPUS_APPLICATION_VOIP, &encoder_error);
  if (encoder_error != OPUS_OK || s_opus_encoder == nullptr) {
    ESP_LOGE(LOG_TAG, "opus_encoder_create failed: %d", encoder_error);
    return;
  }

  if (opus_encoder_init(s_opus_encoder, kCaptureSampleRate, 1, OPUS_APPLICATION_VOIP) !=
      OPUS_OK) {
    ESP_LOGE(LOG_TAG, "opus_encoder_init failed");
    return;
  }

  opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(kOpusEncoderBitrate));
  opus_encoder_ctl(s_opus_encoder,
                   OPUS_SET_COMPLEXITY(kOpusEncoderComplexity));
  opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  s_read_buffer = static_cast<int16_t *>(
      heap_caps_malloc(kCaptureFrameBytes, MALLOC_CAP_DEFAULT));
  s_encoder_output_buffer = static_cast<uint8_t *>(malloc(kOpusBufferSize));
  if (s_read_buffer == nullptr || s_encoder_output_buffer == nullptr) {
    ESP_LOGE(LOG_TAG, "No memory for encoder buffers");
    return;
  }

  s_encoder_ready = true;
}

bool hermes_media_handle_remote_audio(uint8_t *data, size_t size) {
  static uint32_t s_remote_packets = 0;
  static uint32_t s_remote_decoded_packets = 0;

  if (!s_initialized || s_opus_decoder == nullptr || s_decoder_buffer_queue == nullptr) {
    return false;
  }
  if (!s_playback_enabled) {
    return false;
  }

  const bool packet_loss = (data == nullptr || size == 0);
  ++s_remote_packets;
  if (!packet_loss && size <= 3) {
    ++s_remote_silence_packets;
    if (s_remote_silence_packets == 1 ||
        (s_remote_silence_packets % 1000) == 0) {
      ESP_LOGD(LOG_TAG, "Dropped remote Opus silence packets=%lu",
               static_cast<unsigned long>(s_remote_silence_packets));
    }
    return false;
  }

  const int decoded_size =
      opus_decode(s_opus_decoder, packet_loss ? nullptr : data,
                  packet_loss ? 0 : static_cast<opus_int32>(size),
                  s_decoder_buffer, kPcmMaxDecodeSamples, 0);
  if (decoded_size < 0) {
    ESP_LOGW(LOG_TAG, "opus_decode failed: %d", decoded_size);
    return false;
  }

  if (decoded_size == 0) {
    return false;
  }

  const size_t decoded_samples = static_cast<size_t>(decoded_size);
  int16_t decoded_peak = 0;
  for (size_t i = 0; i < decoded_samples; ++i) {
    int16_t sample = s_decoder_buffer[i];
    if (sample < 0) {
      sample = static_cast<int16_t>(-sample);
    }
    if (sample > decoded_peak) {
      decoded_peak = sample;
    }
  }
  ++s_remote_decoded_packets;
  if (s_remote_decoded_packets == 1 || size > 3 ||
      (s_remote_decoded_packets % 50) == 0) {
    ESP_LOGI(LOG_TAG,
             "Remote audio decoded packets=%lu packet_bytes=%u samples=%u peak=%d",
             static_cast<unsigned long>(s_remote_decoded_packets),
             static_cast<unsigned>(size),
             static_cast<unsigned>(decoded_samples), decoded_peak);
  }
  if (decoded_peak >= kRemoteAudioActivityPeak) {
    s_remote_playback_active_until =
        xTaskGetTickCount() + kPlaybackActiveHoldTicks;
  }

  if ((s_playback_accumulator_samples + decoded_samples) >
      (kPcmMaxDecodeSamples + kPlaybackFrameSamples)) {
    s_playback_accumulator_samples = 0;
  }

  memcpy(s_playback_accumulator + s_playback_accumulator_samples,
         s_decoder_buffer, decoded_samples * sizeof(opus_int16));
  s_playback_accumulator_samples += decoded_samples;

  while (s_playback_accumulator_samples >= kPlaybackFrameSamples) {
    if (xRingbufferSend(s_decoder_buffer_queue, s_playback_accumulator,
                        kPlaybackFrameBytes, 0) != pdTRUE) {
      ++s_playback_queue_overflows;
      if ((s_playback_queue_overflows % 8) == 1) {
        ESP_LOGW(LOG_TAG, "Playback queue overflow count=%lu",
                 static_cast<unsigned long>(s_playback_queue_overflows));
      }
    }

    s_playback_accumulator_samples -= kPlaybackFrameSamples;
    if (s_playback_accumulator_samples > 0) {
      memmove(s_playback_accumulator, s_playback_accumulator + kPlaybackFrameSamples,
              s_playback_accumulator_samples * sizeof(opus_int16));
    }
  }

  return decoded_peak >= kRemoteAudioActivityPeak;
}

void hermes_media_reset_playback(void) {
  s_playback_accumulator_samples = 0;
  s_playback_queue_underruns = 0;
  s_playback_queue_overflows = 0;

  if (s_decoder_buffer_queue == nullptr) {
    return;
  }

  size_t len = 0;
  void *queued_frame = nullptr;
  while ((queued_frame = xRingbufferReceive(s_decoder_buffer_queue, &len, 0)) !=
         nullptr) {
    vRingbufferReturnItem(s_decoder_buffer_queue, queued_frame);
  }
}

void hermes_media_send_audio(PeerConnection *peer_connection) {
  if (peer_connection == nullptr || !s_encoder_ready || s_opus_encoder == nullptr ||
      s_encoder_output_buffer == nullptr || s_read_buffer == nullptr) {
    return;
  }
  if (!s_publish_enabled) {
    return;
  }

  if (board_audio_read(s_read_buffer, kCaptureFrameBytes) != ESP_OK) {
    ESP_LOGE(LOG_TAG, "board_audio_read failed");
    return;
  }

  const int32_t input_peak = peak_for_frame(s_read_buffer, kCaptureFrameSamples);
  BoardAudioStats board_stats = {};
  board_audio_get_stats(&board_stats);
  const bool remote_playback_active =
      xTaskGetTickCount() < s_remote_playback_active_until;
  const int32_t gain_q8 =
      apply_capture_agc(s_read_buffer, kCaptureFrameSamples, input_peak,
                        board_stats.mic_peak,
                        remote_playback_active);
  const int32_t output_peak = peak_for_frame(s_read_buffer, kCaptureFrameSamples);
  s_encoder_input_peak = input_peak;
  s_encoder_peak = output_peak;
  s_capture_gain_q8 = gain_q8;
  s_remote_playback_active = remote_playback_active;

  const int encoded_size =
      opus_encode(s_opus_encoder, s_read_buffer, kCaptureFrameSamples,
                  s_encoder_output_buffer, kOpusBufferSize);
  if (encoded_size <= 0) {
    return;
  }

  const int send_result =
      peer_connection_send_audio(peer_connection, s_encoder_output_buffer,
                                 encoded_size);
  if (send_result < 0) {
    ++s_send_failures;
    if ((s_send_failures % 25) == 1) {
      ESP_LOGW(LOG_TAG,
               "Audio send failed count=%lu peak=%ld input_peak=%ld "
               "gain_q8=%ld remote_playback=%d encoded=%d",
               static_cast<unsigned long>(s_send_failures),
               static_cast<long>(output_peak), static_cast<long>(input_peak),
               static_cast<long>(gain_q8), remote_playback_active,
               encoded_size);
    }
    return;
  }

  ++s_sent_packets;
  if ((s_sent_packets % 500) == 0) {
    ESP_LOGI(LOG_TAG,
             "Audio sent packets=%lu peak=%ld input_peak=%ld gain_q8=%ld "
             "remote_playback=%d encoded=%d",
             static_cast<unsigned long>(s_sent_packets),
             static_cast<long>(output_peak), static_cast<long>(input_peak),
             static_cast<long>(gain_q8), remote_playback_active, encoded_size);
  }
}

void hermes_media_get_stats(HermesMediaStats *stats) {
  if (stats == nullptr) {
    return;
  }

  stats->sent_packets = s_sent_packets;
  stats->send_failures = s_send_failures;
  stats->playback_underruns = s_playback_queue_underruns;
  stats->playback_overflows = s_playback_queue_overflows;
  stats->remote_silence_packets = s_remote_silence_packets;
  stats->encoder_input_peak = s_encoder_input_peak;
  stats->encoder_peak = s_encoder_peak;
  stats->capture_gain_q8 = s_capture_gain_q8;
  stats->remote_playback_active = s_remote_playback_active;
}

void hermes_media_set_publish_enabled(bool enabled) {
  s_publish_enabled = enabled;
}

void hermes_media_set_playback_enabled(bool enabled) {
  s_playback_enabled = enabled;
  if (!enabled) {
    hermes_media_reset_playback();
  }
}
