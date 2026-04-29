#include "board.h"

esp_err_t board_timer_set(uint32_t duration_ms, const char *label) {
  (void)duration_ms;
  (void)label;
  return ESP_ERR_NOT_SUPPORTED;
}

void board_timer_cancel(const char *label) { (void)label; }

void board_timer_cancel_all(void) {}

void board_timer_tick(void) {}

bool board_timer_is_set(void) { return false; }

bool board_timer_is_expired(void) { return false; }

uint32_t board_timer_remaining_ms(void) { return 0; }

uint32_t board_timer_duration_ms(void) { return 0; }

const char *board_timer_label(void) { return ""; }

uint8_t board_timer_count(void) { return 0; }

