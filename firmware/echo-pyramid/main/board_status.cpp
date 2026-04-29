#include "board.h"

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nous_status_sprites.h"

namespace {

constexpr const char *TAG = "board_status";
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xffff;
constexpr uint16_t kNousGold = 0xfea0;
constexpr uint16_t kPillDark = 0x18e3;
constexpr uint16_t kStatusRed = 0xf800;
constexpr uint16_t kStatusCyan = 0x07ff;
constexpr uint16_t kStatusBlue = 0x041f;
constexpr uint16_t kStatusGreen = 0x07e0;
constexpr uint16_t kStatusAmber = 0xfd20;
constexpr TickType_t kVolumeOverlayDurationTicks = pdMS_TO_TICKS(1400);

struct PanelInitCommand {
  uint8_t command;
  uint8_t data[14];
  uint8_t data_size;
  uint16_t delay_ms;
};

bool s_display_ready = false;
bool s_talk_enabled = true;
bool s_volume_overlay_active = false;
TickType_t s_volume_overlay_deadline = 0;
uint8_t s_volume_overlay_percent = 0;
esp_lcd_panel_io_handle_t s_panel_io = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
i2c_master_bus_handle_t s_backlight_bus = nullptr;
i2c_master_dev_handle_t s_backlight_dev = nullptr;
uint16_t *s_framebuffer = nullptr;
StatusState s_current_state = STATUS_BOOT;

static const PanelInitCommand kGc9107InitCommands[] = {
    {.command = 0xfe, .data = {}, .data_size = 0, .delay_ms = 5},
    {.command = 0xef, .data = {}, .data_size = 0, .delay_ms = 5},
    {.command = 0xb0, .data = {0xc0}, .data_size = 1, .delay_ms = 0},
    {.command = 0xb2, .data = {0x2f}, .data_size = 1, .delay_ms = 0},
    {.command = 0xb3, .data = {0x03}, .data_size = 1, .delay_ms = 0},
    {.command = 0xb6, .data = {0x19}, .data_size = 1, .delay_ms = 0},
    {.command = 0xb7, .data = {0x01}, .data_size = 1, .delay_ms = 0},
    {.command = 0xac, .data = {0xcb}, .data_size = 1, .delay_ms = 0},
    {.command = 0xab, .data = {0x0e}, .data_size = 1, .delay_ms = 0},
    {.command = 0xb4, .data = {0x04}, .data_size = 1, .delay_ms = 0},
    {.command = 0xa8, .data = {0x19}, .data_size = 1, .delay_ms = 0},
    {.command = 0xb8, .data = {0x08}, .data_size = 1, .delay_ms = 0},
    {.command = 0xe8, .data = {0x24}, .data_size = 1, .delay_ms = 0},
    {.command = 0xe9, .data = {0x48}, .data_size = 1, .delay_ms = 0},
    {.command = 0xea, .data = {0x22}, .data_size = 1, .delay_ms = 0},
    {.command = 0xc6, .data = {0x30}, .data_size = 1, .delay_ms = 0},
    {.command = 0xc7, .data = {0x18}, .data_size = 1, .delay_ms = 0},
    {.command = 0xf0,
     .data = {0x01, 0x2b, 0x23, 0x3c, 0xb7, 0x12, 0x17,
              0x60, 0x00, 0x06, 0x0c, 0x17, 0x12, 0x1f},
     .data_size = 14,
     .delay_ms = 0},
    {.command = 0xf1,
     .data = {0x05, 0x2e, 0x2d, 0x44, 0xd6, 0x15, 0x17,
              0xa0, 0x02, 0x0d, 0x0d, 0x1a, 0x18, 0x1f},
     .data_size = 14,
     .delay_ms = 0},
    {.command = 0x11, .data = {}, .data_size = 0, .delay_ms = 120},
    {.command = 0x29, .data = {}, .data_size = 0, .delay_ms = 0},
};

static const uint8_t kGlyphSpace[7] = {0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00};
static const uint8_t kGlyphA[7] = {0x0e, 0x11, 0x11, 0x1f,
                                   0x11, 0x11, 0x11};
static const uint8_t kGlyphB[7] = {0x1e, 0x11, 0x11, 0x1e,
                                   0x11, 0x11, 0x1e};
static const uint8_t kGlyphC[7] = {0x0e, 0x11, 0x10, 0x10,
                                   0x10, 0x11, 0x0e};
static const uint8_t kGlyphD[7] = {0x1e, 0x11, 0x11, 0x11,
                                   0x11, 0x11, 0x1e};
static const uint8_t kGlyphE[7] = {0x1f, 0x10, 0x10, 0x1e,
                                   0x10, 0x10, 0x1f};
static const uint8_t kGlyphF[7] = {0x1f, 0x10, 0x10, 0x1e,
                                   0x10, 0x10, 0x10};
static const uint8_t kGlyphG[7] = {0x0f, 0x10, 0x10, 0x17,
                                   0x11, 0x11, 0x0f};
static const uint8_t kGlyphH[7] = {0x11, 0x11, 0x11, 0x1f,
                                   0x11, 0x11, 0x11};
static const uint8_t kGlyphI[7] = {0x1f, 0x04, 0x04, 0x04,
                                   0x04, 0x04, 0x1f};
static const uint8_t kGlyphJ[7] = {0x1f, 0x02, 0x02, 0x02,
                                   0x12, 0x12, 0x0c};
static const uint8_t kGlyphK[7] = {0x11, 0x12, 0x14, 0x18,
                                   0x14, 0x12, 0x11};
static const uint8_t kGlyphL[7] = {0x10, 0x10, 0x10, 0x10,
                                   0x10, 0x10, 0x1f};
static const uint8_t kGlyphM[7] = {0x11, 0x1b, 0x15, 0x15,
                                   0x11, 0x11, 0x11};
static const uint8_t kGlyphN[7] = {0x11, 0x19, 0x15, 0x13,
                                   0x11, 0x11, 0x11};
static const uint8_t kGlyphO[7] = {0x0e, 0x11, 0x11, 0x11,
                                   0x11, 0x11, 0x0e};
static const uint8_t kGlyphP[7] = {0x1e, 0x11, 0x11, 0x1e,
                                   0x10, 0x10, 0x10};
static const uint8_t kGlyphQ[7] = {0x0e, 0x11, 0x11, 0x11,
                                   0x15, 0x12, 0x0d};
static const uint8_t kGlyphR[7] = {0x1e, 0x11, 0x11, 0x1e,
                                   0x14, 0x12, 0x11};
static const uint8_t kGlyphS[7] = {0x0f, 0x10, 0x10, 0x0e,
                                   0x01, 0x01, 0x1e};
static const uint8_t kGlyphT[7] = {0x1f, 0x04, 0x04, 0x04,
                                   0x04, 0x04, 0x04};
static const uint8_t kGlyphU[7] = {0x11, 0x11, 0x11, 0x11,
                                   0x11, 0x11, 0x0e};
static const uint8_t kGlyphV[7] = {0x11, 0x11, 0x11, 0x11,
                                   0x11, 0x0a, 0x04};
static const uint8_t kGlyphW[7] = {0x11, 0x11, 0x11, 0x15,
                                   0x15, 0x15, 0x0a};
static const uint8_t kGlyphX[7] = {0x11, 0x11, 0x0a, 0x04,
                                   0x0a, 0x11, 0x11};
static const uint8_t kGlyphY[7] = {0x11, 0x11, 0x0a, 0x04,
                                   0x04, 0x04, 0x04};
static const uint8_t kGlyphZ[7] = {0x1f, 0x01, 0x02, 0x04,
                                   0x08, 0x10, 0x1f};

const uint8_t *glyph_for(char ch) {
  switch (ch) {
    case 'A':
      return kGlyphA;
    case 'B':
      return kGlyphB;
    case 'C':
      return kGlyphC;
    case 'D':
      return kGlyphD;
    case 'E':
      return kGlyphE;
    case 'F':
      return kGlyphF;
    case 'G':
      return kGlyphG;
    case 'H':
      return kGlyphH;
    case 'I':
      return kGlyphI;
    case 'J':
      return kGlyphJ;
    case 'K':
      return kGlyphK;
    case 'L':
      return kGlyphL;
    case 'M':
      return kGlyphM;
    case 'N':
      return kGlyphN;
    case 'O':
      return kGlyphO;
    case 'P':
      return kGlyphP;
    case 'Q':
      return kGlyphQ;
    case 'R':
      return kGlyphR;
    case 'S':
      return kGlyphS;
    case 'T':
      return kGlyphT;
    case 'U':
      return kGlyphU;
    case 'V':
      return kGlyphV;
    case 'W':
      return kGlyphW;
    case 'X':
      return kGlyphX;
    case 'Y':
      return kGlyphY;
    case 'Z':
      return kGlyphZ;
    default:
      return kGlyphSpace;
  }
}

const char *label_for_state(StatusState state) {
  switch (state) {
    case STATUS_BOOT:
      return "BOOT";
    case STATUS_WIFI_CONNECTING:
      return "WIFI";
    case STATUS_DISCONNECTED:
      return "OFFLINE";
    case STATUS_SIGNALING:
      return "THINK";
    case STATUS_CONNECTED_IDLE:
      return "READY";
    case STATUS_LISTENING:
      return "LISTEN";
    case STATUS_BOT_SPEAKING:
      return "SPEAK";
    case STATUS_MEDIA_PLAYING:
      return "PLAY";
    case STATUS_ERROR:
    default:
      return "ERROR";
  }
}

uint16_t panel_color(uint16_t rgb565) {
  return static_cast<uint16_t>(~rgb565);
}

uint16_t accent_for_state(StatusState state) {
  switch (state) {
    case STATUS_ERROR:
    case STATUS_DISCONNECTED:
      return kStatusRed;
    case STATUS_LISTENING:
      return kStatusBlue;
    case STATUS_SIGNALING:
    case STATUS_WIFI_CONNECTING:
    case STATUS_BOOT:
      return kStatusCyan;
    case STATUS_BOT_SPEAKING:
      return kStatusAmber;
    case STATUS_MEDIA_PLAYING:
      return kStatusGreen;
    case STATUS_CONNECTED_IDLE:
    default:
      return kNousGold;
  }
}

void fill_screen(uint16_t color) {
  const uint16_t converted = panel_color(color);
  const size_t pixel_count =
      kEchoPyramidBoardConfig.display_width * kEchoPyramidBoardConfig.display_height;
  for (size_t i = 0; i < pixel_count; ++i) {
    s_framebuffer[i] = converted;
  }
}

void put_pixel(int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= kEchoPyramidBoardConfig.display_width ||
      y >= kEchoPyramidBoardConfig.display_height) {
    return;
  }
  s_framebuffer[y * kEchoPyramidBoardConfig.display_width + x] =
      panel_color(color);
}

void draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  while (true) {
    put_pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void draw_rect(int x, int y, int w, int h, uint16_t color) {
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) {
      put_pixel(xx, yy, color);
    }
  }
}

void draw_circle(int cx, int cy, int radius, uint16_t color) {
  const int radius_sq = radius * radius;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      if ((x * x) + (y * y) <= radius_sq) {
        put_pixel(cx + x, cy + y, color);
      }
    }
  }
}

void draw_rounded_rect(int x, int y, int w, int h, int radius,
                       uint16_t color) {
  draw_rect(x + radius, y, w - (radius * 2), h, color);
  draw_rect(x, y + radius, w, h - (radius * 2), color);
  draw_circle(x + radius, y + radius, radius, color);
  draw_circle(x + w - radius - 1, y + radius, radius, color);
  draw_circle(x + radius, y + h - radius - 1, radius, color);
  draw_circle(x + w - radius - 1, y + h - radius - 1, radius, color);
}

void draw_bitmap_rgb565(int x, int y, int width, int height,
                        const uint16_t *pixels) {
  if (pixels == nullptr) {
    return;
  }
  for (int yy = 0; yy < height; ++yy) {
    for (int xx = 0; xx < width; ++xx) {
      put_pixel(x + xx, y + yy, pixels[(yy * width) + xx]);
    }
  }
}

void draw_char(int x, int y, char ch, int scale, uint16_t color) {
  const uint8_t *glyph = glyph_for(ch);
  for (int row = 0; row < 7; ++row) {
    for (int col = 0; col < 5; ++col) {
      if ((glyph[row] >> (4 - col)) & 0x01) {
        draw_rect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

int text_width(const char *text, int scale) {
  const int len = strlen(text);
  const int char_width = 6 * scale;
  return len > 0 ? (len * char_width) - scale : 0;
}

void draw_text_centered(const char *text, int y, int scale, uint16_t color) {
  const int char_width = 6 * scale;
  const int len = strlen(text);
  const int width = text_width(text, scale);
  const int x = (kEchoPyramidBoardConfig.display_width - width) / 2;
  for (int i = 0; i < len; ++i) {
    draw_char(x + i * char_width, y, text[i], scale, color);
  }
}

void draw_text_centered_shadowed(const char *text, int y, int scale,
                                 uint16_t color) {
  draw_text_centered(text, y + 1, scale, kBlack);
  draw_text_centered(text, y - 1, scale, kBlack);
  draw_text_centered(text, y, scale, color);
}

NousStatusSpriteIndex sprite_for_state(StatusState state) {
  switch (state) {
    case STATUS_SIGNALING:
    case STATUS_WIFI_CONNECTING:
    case STATUS_BOOT:
      return NOUS_STATUS_SPRITE_THINKING;
    case STATUS_LISTENING:
      return NOUS_STATUS_SPRITE_LISTENING;
    case STATUS_BOT_SPEAKING:
      return NOUS_STATUS_SPRITE_SPEAKING;
    case STATUS_MEDIA_PLAYING:
      return NOUS_STATUS_SPRITE_READY;
    case STATUS_ERROR:
    case STATUS_DISCONNECTED:
      return NOUS_STATUS_SPRITE_ERROR;
    case STATUS_CONNECTED_IDLE:
    default:
      return NOUS_STATUS_SPRITE_ASLEEP;
  }
}

void draw_nous_status_sprite(StatusState state) {
  const int sprite_x =
      (kEchoPyramidBoardConfig.display_width - kNousStatusSpriteWidth) / 2;
  const int sprite_y =
      (kEchoPyramidBoardConfig.display_height - kNousStatusSpriteHeight) / 2;
  draw_bitmap_rgb565(sprite_x, sprite_y, kNousStatusSpriteWidth,
                     kNousStatusSpriteHeight,
                     kNousStatusSprites[sprite_for_state(state)]);
}

void draw_warning_badge(void) {
  const int x0 = 8;
  const int y0 = 117;
  const int x1 = 20;
  const int y1 = 94;
  const int x2 = 32;
  const int y2 = 117;

  for (int y = y1 + 2; y <= y0 - 2; ++y) {
    const int half_width = ((y - y1) * 10) / (y0 - y1);
    draw_line(20 - half_width, y, 20 + half_width, y, kWhite);
  }
  for (int offset = 0; offset < 2; ++offset) {
    draw_line(x0 + offset, y0 - offset, x1, y1 + offset, kStatusRed);
    draw_line(x1, y1 + offset, x2 - offset, y2 - offset, kStatusRed);
    draw_line(x0 + offset, y0 - offset, x2 - offset, y2 - offset, kStatusRed);
  }
  draw_rect(19, 102, 3, 9, kBlack);
  draw_rect(19, 114, 3, 3, kBlack);
}

void draw_status_caption(StatusState state, const char *text, int scale) {
  const int text_w = text_width(text, scale);
  const int pill_w = std::max(58, std::min(118, text_w + 28));
  const int pill_h = scale > 1 ? 22 : 18;
  const int pill_x = (kEchoPyramidBoardConfig.display_width - pill_w) / 2;
  const int pill_y = kEchoPyramidBoardConfig.display_height - pill_h - 5;
  const int text_y = pill_y + (scale > 1 ? 4 : 6);

  draw_rounded_rect(pill_x, pill_y, pill_w, pill_h, 7, kBlack);
  draw_rounded_rect(pill_x + 2, pill_y + 2, pill_w - 4, pill_h - 4, 5,
                    kPillDark);
  draw_circle(pill_x + 9, pill_y + (pill_h / 2), 3, accent_for_state(state));
  draw_text_centered_shadowed(text, text_y, scale, kWhite);
}

void flush_framebuffer(void) {
  if (!s_display_ready || s_framebuffer == nullptr) {
    return;
  }
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, kEchoPyramidBoardConfig.display_width,
                            kEchoPyramidBoardConfig.display_height,
                            s_framebuffer);
}

void render_state(StatusState state) {
  if (!s_display_ready || s_framebuffer == nullptr) {
    return;
  }

  s_current_state = state;
  fill_screen(kWhite);
  draw_nous_status_sprite(state);
  if (state == STATUS_ERROR || state == STATUS_DISCONNECTED) {
    draw_warning_badge();
  }
  draw_status_caption(state, label_for_state(state), 1);
  flush_framebuffer();
}

void render_volume_overlay(uint8_t volume_percent) {
  if (!s_display_ready || s_framebuffer == nullptr) {
    return;
  }
  fill_screen(kWhite);
  draw_nous_status_sprite(s_current_state);
  draw_rounded_rect(14, 106, 100, 17, 7, kBlack);
  draw_rounded_rect(17, 109, 94, 11, 5, kPillDark);
  draw_rounded_rect(17, 109, (94 * volume_percent) / 100, 11, 5, kNousGold);
  draw_text_centered_shadowed("VOLUME", 91, 1, kWhite);
  flush_framebuffer();
}

void render_text_overlay(const char *text) {
  if (!s_display_ready || s_framebuffer == nullptr) {
    return;
  }
  char line[12] = {};
  size_t out = 0;
  for (size_t i = 0; text != nullptr && text[i] != '\0' && out < sizeof(line) - 1; ++i) {
    char ch = text[i];
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
    if ((ch >= 'A' && ch <= 'Z') || ch == ' ') {
      line[out++] = ch;
    }
  }
  if (out == 0) {
    strncpy(line, "READY", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
  }

  fill_screen(kWhite);
  draw_nous_status_sprite(s_current_state);
  draw_status_caption(s_current_state, line, out <= 6 ? 2 : 1);
  flush_framebuffer();
}

bool volume_overlay_expired() {
  return static_cast<int32_t>(xTaskGetTickCount() - s_volume_overlay_deadline) >=
         0;
}

esp_err_t write_backlight_reg(uint8_t reg, uint8_t value) {
  if (s_backlight_dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  const uint8_t payload[2] = {reg, value};
  return i2c_master_transmit(s_backlight_dev, payload, sizeof(payload), 100);
}

esp_err_t init_backlight(void) {
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = kEchoPyramidBoardConfig.backlight_i2c_port;
  bus_cfg.sda_io_num = kEchoPyramidBoardConfig.backlight_sda;
  bus_cfg.scl_io_num = kEchoPyramidBoardConfig.backlight_scl;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.intr_priority = 0;
  bus_cfg.trans_queue_depth = 0;
  bus_cfg.flags.enable_internal_pullup = 1;
  bus_cfg.flags.allow_pd = 0;
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_backlight_bus), TAG,
                      "Failed to create backlight I2C bus");

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = kEchoPyramidBoardConfig.backlight_addr;
  dev_cfg.scl_speed_hz = 400000;
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_backlight_bus, &dev_cfg,
                                                &s_backlight_dev),
                      TAG, "Failed to add backlight controller");
  ESP_RETURN_ON_ERROR(write_backlight_reg(0x00, 0x40), TAG,
                      "Failed to enable backlight controller");
  vTaskDelay(pdMS_TO_TICKS(1));
  ESP_RETURN_ON_ERROR(write_backlight_reg(0x08, 0x01), TAG,
                      "Failed to enable backlight channel");
  ESP_RETURN_ON_ERROR(write_backlight_reg(0x70, 0x00), TAG,
                      "Failed to configure backlight output");
  ESP_RETURN_ON_ERROR(
      write_backlight_reg(0x0e, kEchoPyramidBoardConfig.backlight_brightness),
      TAG, "Failed to set backlight brightness");
  return ESP_OK;
}

esp_err_t apply_gc9107_init_sequence(void) {
  for (const PanelInitCommand &command : kGc9107InitCommands) {
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(
            s_panel_io, command.command,
            command.data_size > 0 ? command.data : nullptr, command.data_size),
        TAG, "GC9107 panel command failed");
    if (command.delay_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(command.delay_ms));
    }
  }
  return ESP_OK;
}

}  // namespace

esp_err_t board_status_init(void) {
  if (s_display_ready) {
    return ESP_OK;
  }

  s_framebuffer = static_cast<uint16_t *>(
      heap_caps_malloc(kEchoPyramidBoardConfig.display_width *
                           kEchoPyramidBoardConfig.display_height *
                           sizeof(uint16_t),
                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (s_framebuffer == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = kEchoPyramidBoardConfig.display_mosi;
  bus_cfg.miso_io_num = -1;
  bus_cfg.sclk_io_num = kEchoPyramidBoardConfig.display_sclk;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.data4_io_num = -1;
  bus_cfg.data5_io_num = -1;
  bus_cfg.data6_io_num = -1;
  bus_cfg.data7_io_num = -1;
  bus_cfg.max_transfer_sz =
      kEchoPyramidBoardConfig.display_width *
      kEchoPyramidBoardConfig.display_height * sizeof(uint16_t);
  ESP_RETURN_ON_ERROR(
      spi_bus_initialize(kEchoPyramidBoardConfig.display_host, &bus_cfg,
                         SPI_DMA_CH_AUTO),
      TAG, "Failed to initialize display SPI bus");

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = kEchoPyramidBoardConfig.display_cs;
  io_cfg.dc_gpio_num = kEchoPyramidBoardConfig.display_dc;
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = 40 * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_spi(
          (esp_lcd_spi_bus_handle_t)kEchoPyramidBoardConfig.display_host,
          &io_cfg, &s_panel_io),
      TAG, "Failed to create panel IO");

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = kEchoPyramidBoardConfig.display_reset;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
  panel_cfg.bits_per_pixel = 16;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel),
                      TAG, "Failed to create display panel");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG,
                      "Failed to reset panel");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG,
                      "Failed to initialize panel");
  ESP_RETURN_ON_ERROR(apply_gc9107_init_sequence(), TAG,
                      "Failed to apply GC9107 panel init sequence");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, kEchoPyramidBoardConfig.display_gap_x,
                                            kEchoPyramidBoardConfig.display_gap_y),
                      TAG, "Failed to set display gap");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                      "Failed to enable panel");
  ESP_RETURN_ON_ERROR(init_backlight(), TAG, "Failed to initialize backlight");

  s_display_ready = true;
  render_state(STATUS_BOOT);
  ESP_LOGI(TAG, "Echo Pyramid display initialized");
  return ESP_OK;
}

void board_status_set_state(StatusState state, const char *detail) {
  s_current_state = state;
  board_lights_set_state(state);
  ESP_LOGI(TAG, "State -> %s (%s)", label_for_state(state),
           detail != nullptr ? detail : "");
  if (!s_volume_overlay_active) {
    render_state(state);
  }
}

StatusState board_status_get_state(void) { return s_current_state; }

void board_status_show_volume(uint8_t volume_percent) {
  s_volume_overlay_percent = volume_percent;
  s_volume_overlay_active = true;
  s_volume_overlay_deadline = xTaskGetTickCount() + kVolumeOverlayDurationTicks;
  render_volume_overlay(volume_percent);
}

void board_status_show_text(const char *text) {
  s_volume_overlay_active = true;
  s_volume_overlay_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
  render_text_overlay(text);
}

void board_status_show_timer(void) {
  ESP_LOGI(TAG, "Timer overlay requested");
}

void board_status_set_talk_enabled(bool enabled) {
  s_talk_enabled = enabled;
  s_volume_overlay_active = false;
  render_state(s_current_state);
}

void board_status_tick(void) {
  if (!s_volume_overlay_active) {
    return;
  }
  if (!volume_overlay_expired()) {
    return;
  }
  s_volume_overlay_active = false;
  render_state(s_current_state);
}
