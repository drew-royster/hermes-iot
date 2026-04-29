#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_err.h"

typedef struct {
  i2c_port_num_t i2c_port;
  gpio_num_t i2c_sda;
  gpio_num_t i2c_scl;
  i2c_port_num_t backlight_i2c_port;
  gpio_num_t backlight_sda;
  gpio_num_t backlight_scl;
  i2s_port_t i2s_port;
  gpio_num_t i2s_bclk;
  gpio_num_t i2s_lrck;
  gpio_num_t i2s_dout;
  gpio_num_t i2s_din;
  spi_host_device_t display_host;
  gpio_num_t display_sclk;
  gpio_num_t display_mosi;
  gpio_num_t display_cs;
  gpio_num_t display_dc;
  gpio_num_t display_reset;
  uint16_t display_width;
  uint16_t display_height;
  uint16_t display_gap_x;
  uint16_t display_gap_y;
  gpio_num_t user_button;
  uint8_t backlight_addr;
  uint8_t backlight_brightness;
  uint8_t io_expander_addr;
  uint8_t speaker_enable_pin;
  uint8_t si5351_addr;
  uint8_t aw87559_addr;
  uint8_t touch_addr;
} EchoPyramidBoardConfig;

extern const EchoPyramidBoardConfig kEchoPyramidBoardConfig;

typedef enum {
  STATUS_BOOT = 0,
  STATUS_WIFI_CONNECTING,
  STATUS_DISCONNECTED,
  STATUS_SIGNALING,
  STATUS_CONNECTED_IDLE,
  STATUS_LISTENING,
  STATUS_BOT_SPEAKING,
  STATUS_MEDIA_PLAYING,
  STATUS_ERROR,
} StatusState;

typedef struct {
  uint32_t read_count;
  int32_t mic_peak;
  int32_t ref_peak;
  int32_t aec_peak;
  uint8_t volume_percent;
  bool output_enabled;
  bool aec_enabled;
  int aec_frame_samples;
} BoardAudioStats;

typedef struct {
  uint32_t duration_ms;
  uint32_t frames;
  uint32_t frequency_hz;
  int32_t mic_peak;
  int32_t ref_peak;
  int32_t aec_peak;
  int32_t slot_peaks[4];
  float mic_rms;
  float ref_rms;
  float aec_rms;
  float slot_rms[4];
  float slot_corr[4];
  float mic_ref_corr;
  float aec_ref_corr;
  float suppression_db;
} BoardAudioAecProbeResult;

esp_err_t board_audio_init(void);
esp_err_t board_audio_read(void *dest, size_t size);
esp_err_t board_audio_read_raw(void *dest, size_t size);
esp_err_t board_audio_write(const void *data, size_t size);
esp_err_t board_audio_set_output_enabled(bool enabled);
esp_err_t board_audio_set_volume(uint8_t volume_percent);
uint8_t board_audio_get_volume(void);
void board_audio_get_stats(BoardAudioStats *stats);
esp_err_t board_audio_run_loopback_test(uint32_t duration_ms);
esp_err_t board_audio_run_aec_probe(uint32_t duration_ms,
                                    uint32_t frequency_hz,
                                    float amplitude,
                                    BoardAudioAecProbeResult *result);
void board_audio_dump_diagnostics(void);
esp_err_t board_audio_play_self_test(void);
esp_err_t board_audio_play_wake_tone(void);
esp_err_t board_audio_play_listening_tone(void);
esp_err_t board_audio_play_thinking_tone(void);
esp_err_t board_audio_play_tool_tone(void);
esp_err_t board_audio_play_ready_tone(void);
esp_err_t board_audio_play_failure_tone(void);
esp_err_t board_audio_play_timer_done_tone(void);
esp_err_t board_audio_play_beep(uint32_t frequency_hz, uint32_t duration_ms);
void board_controls_poll(void);
void board_lights_set_state(StatusState state);
void board_lights_set_effect(const char *color, const char *pattern);
void board_lights_tick(void);

esp_err_t board_status_init(void);
void board_status_set_state(StatusState state, const char *detail);
StatusState board_status_get_state(void);
void board_status_show_volume(uint8_t volume_percent);
void board_status_show_text(const char *text);
void board_status_show_timer(void);
void board_status_set_talk_enabled(bool enabled);
void board_status_tick(void);

esp_err_t board_timer_set(uint32_t duration_ms, const char *label);
void board_timer_cancel(const char *label);
void board_timer_cancel_all(void);
void board_timer_tick(void);
bool board_timer_is_set(void);
bool board_timer_is_expired(void);
uint32_t board_timer_remaining_ms(void);
uint32_t board_timer_duration_ms(void);
const char *board_timer_label(void);
uint8_t board_timer_count(void);

void board_dance_mode_set(bool enabled);
bool board_dance_mode_active(void);
