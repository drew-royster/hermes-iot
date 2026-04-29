/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @brief WAV Playback + Flash Recording + Beat RGB Effects Demo
 *
 * =============================================================
 * Partition Table Reference (Custom)
 * =============================================================
 *
 * # Name,   Type, SubType, Offset,   Size,Flags
 * nvs,      data, nvs,     0x9000,   0x5000
 * otadata,  data, ota,     0xE000,   0x2000
 * app0,     app,  ota_0,   0x10000,  0x290000
 * spiffs,   data, spiffs,  0x2A0000, 0x400000
 * audio,    data, 0x99,    0x700000, 0x100000
 *
 * Description:
 * - "spiffs"  : SPIFFS for WAV files
 * - "audio"    : Raw flash area for PCM recording spiffs
 *
 * =============================================================
 *
 * Features:
 *  - WAV playback from SPIFFS
 *  - 5-second raw PCM recording to flash partition
 *  - Playback from raw flash
 *  - Real-time beat detection
 *  - Multiple RGB effects
 *  - FreeRTOS audio task (Core 0)
 *
 * Audio Format:
 *  - 44.1 kHz
 *  - 16-bit
 *  - Mono (internally converted from stereo WAV)
 */

#include "M5Unified.h"
#include <M5EchoPyramid.h>
#include "SPIFFS.h"
#include "esp_partition.h"

/* ============================================================
 *                      Audio Configuration
 * ============================================================ */

#define SAMPLE_RATE    44100  ///< Audio sample rate (Hz)
#define RECORD_SECONDS 5      ///< Recording duration (seconds)
#define FRAME_SIZE     256    ///< Audio frame block size
#define TOTAL_FRAMES   (SAMPLE_RATE * RECORD_SECONDS)
#define WAV_FILE_PATH  "/file_name.wav"
/* ============================================================
 *                      Global Variables
 * ============================================================ */

M5EchoPyramid ep;  ///< EchoPyramid device instance
TaskHandle_t audioTaskHandle = NULL;

volatile bool audioBusy = false;  ///< Audio engine busy flag

static const esp_partition_t *audioPartition = nullptr;  ///< Raw PCM flash partition

/* ============================================================
 *                      Audio Commands
 * ============================================================ */

/**
 * @brief Audio command types for AudioTask.
 */
enum AudioCmd {
    CMD_NONE,     ///< Idle
    CMD_RECORD,   ///< Record 5 seconds to flash and playback
    CMD_PLAY_WAV  ///< Play WAV file from SPIFFS
};

volatile AudioCmd audioCmd = CMD_NONE;
char wavPath[64];

/* ============================================================
 *                      RGB Effect Modes
 * ============================================================ */

/**
 * @brief RGB animation modes.
 */
enum EffectMode { EFFECT_NONE, EFFECT_RAINBOW, EFFECT_BREATH, EFFECT_GRADIENT, EFFECT_BEAT };

EffectMode currentEffect = EFFECT_NONE;

/* ============================================================
 *                      Beat Detection
 * ============================================================ */

#define BEAT_HISTORY 43

float energyHistory[BEAT_HISTORY] = {0};
int energyIndex                   = 0;
bool g_isBeat                     = false;
bool beatEnabRGB                  = false;

/**
 * @brief Simple energy-based beat detection.
 *
 * Uses short-time energy comparison against
 * rolling average to detect beat peaks.
 *
 * @param data    Audio sample buffer
 * @param samples Number of samples
 */
void detectBeat(int16_t *data, int samples)
{
    if (!beatEnabRGB) return;

    float energy = 0;

    for (int i = 0; i < samples; i++) {
        float s = data[i] / 32768.0f;
        energy += s * s;
    }

    energy /= samples;

    float sum = 0;
    for (int i = 0; i < BEAT_HISTORY; i++) sum += energyHistory[i];

    float avgEnergy = sum / BEAT_HISTORY;
    float threshold = avgEnergy * 1.6f;

    g_isBeat = (energy > threshold && energy > 0.0005f);

    energyHistory[energyIndex++] = energy;
    if (energyIndex >= BEAT_HISTORY) energyIndex = 0;
}

/* ============================================================
 *                      RGB Control
 * ============================================================ */

/**
 * @brief Set all RGBs to a specific RGB color.
 */
void setAll(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 14; i++) {
        ep.ctrl().setRGB(1, i, r, g, b);
        ep.ctrl().setRGB(2, i, r, g, b);
    }
}

/**
 * @brief Convert HSV to RGB.
 */
void hsv2rgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b)
{
    int i   = int(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    float r1, g1, b1;

    switch (i % 6) {
        case 0:
            r1 = v;
            g1 = t;
            b1 = p;
            break;
        case 1:
            r1 = q;
            g1 = v;
            b1 = p;
            break;
        case 2:
            r1 = p;
            g1 = v;
            b1 = t;
            break;
        case 3:
            r1 = p;
            g1 = q;
            b1 = v;
            break;
        case 4:
            r1 = t;
            g1 = p;
            b1 = v;
            break;
        default:
            r1 = v;
            g1 = p;
            b1 = q;
            break;
    }

    r = r1 * 255;
    g = g1 * 255;
    b = b1 * 255;
}

/* ============================================================
 *                      RGB Effects
 * ============================================================ */

float hueOffset = 0;
int breathValue = 0;
int breathDir   = 1;

/**
 * @brief Rainbow rotating effect.
 */
void effectRainbow()
{
    for (int i = 0; i < 14; i++) {
        float hue = (float)i / 14.0 + hueOffset;
        if (hue > 1) hue -= 1;

        uint8_t r, g, b;
        hsv2rgb(hue, 1.0, 1.0, r, g, b);

        ep.ctrl().setRGB(1, i, r, g, b);
        ep.ctrl().setRGB(2, i, r, g, b);
    }

    hueOffset += 0.002;
    if (hueOffset > 1) hueOffset = 0;
}

/**
 * @brief White breathing effect.
 */
void effectBreath()
{
    breathValue += breathDir * 5;

    if (breathValue >= 255) {
        breathValue = 255;
        breathDir   = -1;
    }
    if (breathValue <= 0) {
        breathValue = 0;
        breathDir   = 1;
    }

    setAll(breathValue, breathValue, breathValue);
}

/**
 * @brief Static red-blue gradient.
 */
void effectGradient()
{
    for (int i = 0; i < 14; i++) {
        uint8_t r = map(i, 0, 13, 255, 0);
        uint8_t b = map(i, 0, 13, 0, 255);
        ep.ctrl().setRGB(1, i, r, 0, b);
        ep.ctrl().setRGB(2, i, r, 0, b);
    }
}

/**
 * @brief Beat reactive RGB effect.
 */
void effectBeat()
{
    if (!beatEnabRGB) return;

    static float brightness = 0;
    static uint8_t r = 0, g = 0, b = 0;

    if (g_isBeat) {
        brightness = 1.0f;
        r          = random(256);
        g          = random(256);
        b          = random(256);
    } else {
        brightness *= 0.88f;
    }

    if (brightness < 0.02f) brightness = 0;

    setAll(r * brightness, g * brightness, b * brightness);
}

/* ============================================================
 *                      Audio FreeRTOS Task
 * ============================================================ */
void flushI2SSilence(int frames = FRAME_SIZE * 4)
{
    int16_t silence[FRAME_SIZE] = {0};
    for (int i = 0; i < 40; i++) {
        ep.write(silence, FRAME_SIZE);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ep.codec().mute(true);
}
/**
 * @brief Dedicated audio processing task.
 *
 * Handles:
 *  - WAV playback from SPIFFS
 *  - Recording PCM to flash partition
 *  - Playback from raw flash
 *
 * Runs on Core 0.
 */
void AudioTask(void *param)
{
    int16_t mic[FRAME_SIZE];
    int16_t ref[FRAME_SIZE];

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        audioBusy = true;

        /* ================= WAV Playback ================= */
        if (audioCmd == CMD_PLAY_WAV) {
            Serial.printf("\n▶️ Start WAV playback: %s\n", wavPath);
            ep.codec().mute(false);
            File file = SPIFFS.open(wavPath, "r");
            if (!file) {
                Serial.println("❌ WAV file not found!");
                audioBusy = false;
                continue;
            }

            file.seek(44);  // Skip WAV header

            int16_t buffer[FRAME_SIZE * 2];

            while (file.available()) {
                int bytesRead = file.read((uint8_t *)buffer, FRAME_SIZE * 2 * sizeof(int16_t));
                int frames    = bytesRead / (2 * sizeof(int16_t));

                if (frames > 0) {
                    int16_t mono[FRAME_SIZE];
                    for (int i = 0; i < frames; i++) {
                        mono[i] = ((int32_t)buffer[i * 2] + buffer[i * 2 + 1]) / 2;
                    }

                    detectBeat(mono, frames);
                    ep.write(mono, frames);
                }

                vTaskDelay(1);  ///< Yield CPU
            }

            file.close();

            beatEnabRGB = false;
            g_isBeat    = false;
            flushI2SSilence();
            Serial.println("⏹ WAV playback finished");
        }

        /* ================= Record + Playback ================= */
        else if (audioCmd == CMD_RECORD) {
            Serial.println("\n🎙 Start recording...");

            beatEnabRGB          = false;
            uint32_t writeOffset = 0;
            int recorded         = 0;
            uint32_t recordBytes = TOTAL_FRAMES * sizeof(int16_t);
            uint32_t eraseSize   = (recordBytes + 4095) & ~4095;

            esp_partition_erase_range(audioPartition, 0, eraseSize);

            while (recorded < TOTAL_FRAMES) {
                ep.read(mic, ref, FRAME_SIZE);
                esp_partition_write(audioPartition, writeOffset, mic, FRAME_SIZE * sizeof(int16_t));

                writeOffset += FRAME_SIZE * sizeof(int16_t);
                recorded += FRAME_SIZE;
            }

            Serial.println("✅ Recording finished");

            delay(200);
            ep.codec().mute(false);
            Serial.println("▶️ Start playback...");

            int16_t playBuf[FRAME_SIZE];
            uint32_t readOffset = 0;
            int played          = 0;

            while (played < TOTAL_FRAMES) {
                esp_partition_read(audioPartition, readOffset, playBuf, FRAME_SIZE * sizeof(int16_t));

                ep.write(playBuf, FRAME_SIZE);

                readOffset += FRAME_SIZE * sizeof(int16_t);
                played += FRAME_SIZE;
            }
            flushI2SSilence();
            Serial.println("⏹ Playback finished");
        }

        audioCmd  = CMD_NONE;
        audioBusy = false;
    }
}
/* ============================================================
 *                      Setup
 * ============================================================ */

/**
 * @brief Arduino setup function.
 *
 * Initializes:
 *  - M5 system
 *  - Audio codec
 *  - SPIFFS
 *  - Flash partition
 *  - Audio FreeRTOS task
 */
void setup()
{
    M5.begin();
    Serial.begin(115200);
    delay(2000);

    Wire1.end();

    ep.begin(&Wire1, 38, 39, 6, 8, 5, 7, SAMPLE_RATE);
    ep.codec().setVolume(50);
    ep.codec().mute(false);

    ep.ctrl().setBrightness(1, 100);
    ep.ctrl().setBrightness(2, 100);

    SPIFFS.begin(true);

    audioPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x99, "audio");

    xTaskCreatePinnedToCore(AudioTask, "AudioTask", 8192, NULL, 1, &audioTaskHandle, 0);
}

/* ============================================================
 *                      Main Loop
 * ============================================================ */

/**
 * @brief Main application loop.
 *
 * Handles:
 *  - Touch input
 *  - Effect switching
 *  - Audio command triggering
 */

// ================= loop =================
void loop()
{
    uint8_t key = 0;

    for (int i = 1; i <= 4; i++)
        if (ep.ctrl().isPressed(i)) {
            key = i;
            break;
        }

    if (key != 0 && !audioBusy) {
        switch (key) {
            case 1:
                currentEffect = EFFECT_RAINBOW;
                Serial.printf("touch1 EFFECT_RAINBOW\n");
                strcpy(wavPath, WAV_FILE_PATH);
                beatEnabRGB = false;
                audioCmd    = CMD_PLAY_WAV;
                xTaskNotifyGive(audioTaskHandle);
                break;

            case 2:
                currentEffect = EFFECT_BREATH;
                Serial.printf("touch2 EFFECT_BREATH\n");
                break;

            case 3:
                currentEffect = EFFECT_GRADIENT;
                Serial.printf("touch3 EFFECT_GRADIENT\n");
                audioCmd = CMD_RECORD;
                xTaskNotifyGive(audioTaskHandle);
                break;

            case 4:
                currentEffect = EFFECT_BEAT;
                Serial.printf("touch4 EFFECT_BEAT\n");
                strcpy(wavPath, WAV_FILE_PATH);
                beatEnabRGB = true;
                audioCmd    = CMD_PLAY_WAV;
                xTaskNotifyGive(audioTaskHandle);
                break;
        }
    }

    switch (currentEffect) {
        case EFFECT_RAINBOW:
            effectRainbow();
            break;
        case EFFECT_BREATH:
            effectBreath();
            break;
        case EFFECT_GRADIENT:
            effectGradient();
            break;
        case EFFECT_BEAT:
            effectBeat();
            break;
        default:
            break;
    }

    delay(30);
}