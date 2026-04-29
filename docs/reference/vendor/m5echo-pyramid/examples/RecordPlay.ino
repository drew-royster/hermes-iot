/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @brief M5EchoPyramid 5-second Record & Playback Demo with Rainbow RGB Effect
 *
 * This demo performs:
 *  - Continuous rainbow RGB animation at startup
 *  - Touch button trigger to record 5 seconds of audio
 *  - Automatic playback after recording
 *
 * Features:
 *  - No SPIFFS
 *  - No SD card
 *  - No PSRAM required
 *  - Uses dynamic heap allocation only
 *
 * Audio Format:
 *  - 44.1kHz
 *  - 16-bit
 *  - Mono
 *
 * Approximate RAM usage: ~430 KB
 *
 * Host type
 * AtomS3R
 */

#include "M5Unified.h"
#include <M5EchoPyramid.h>

/* ============================================================
 *                      Audio Configuration
 * ============================================================ */

/** @brief Audio sample rate (Hz) */
#define SAMPLE_RATE 44100

/** @brief Recording duration in seconds */
#define RECORD_SECONDS 5

/** @brief Audio frame size per read/write */
#define FRAME_SIZE 256

/** @brief Total number of audio samples */
#define TOTAL_FRAMES (SAMPLE_RATE * RECORD_SECONDS)

/* ============================================================
 *                      Global Variables
 * ============================================================ */

/** @brief EchoPyramid object */
M5EchoPyramid ep;

/** @brief Pointer to dynamically allocated recording buffer */
int16_t *recordBuffer = nullptr;

/** @brief Recording state flag */
bool recordingBusy = false;

/** @brief Global hue offset for rainbow animation */
float hueOffset = 0;

/* ============================================================
 *                      Utility Functions
 * ============================================================ */

/**
 * @brief Convert HSV color to RGB.
 *
 * @param h Hue (0.0 - 1.0)
 * @param s Saturation (0.0 - 1.0)
 * @param v Value/Brightness (0.0 - 1.0)
 * @param r Output red value (0-255)
 * @param g Output green value (0-255)
 * @param b Output blue value (0-255)
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

/**
 * @brief Generate continuous rainbow RGB animation.
 *
 * This function updates both RGB rings with a rotating HSV spectrum.
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

    hueOffset += 0.003;
    if (hueOffset > 1) hueOffset = 0;
}

/* ============================================================
 *                      Audio Functions
 * ============================================================ */

/**
 * @brief Record 5 seconds of audio and then play it back.
 *
 * Process:
 *  1. Capture microphone data into heap buffer.
 *  2. After recording finishes, playback the buffer.
 *
 * This function blocks until playback is complete.
 */
void recordAndPlay()
{
    if (recordingBusy) return;
    recordingBusy = true;

    Serial.println("Start Recording...");

    int16_t mic[FRAME_SIZE];
    int16_t ref[FRAME_SIZE];

    int recorded = 0;

    /* ---------------- Recording ---------------- */
    while (recorded < TOTAL_FRAMES) {
        ep.read(mic, ref, FRAME_SIZE);

        memcpy(recordBuffer + recorded, mic, FRAME_SIZE * sizeof(int16_t));

        recorded += FRAME_SIZE;
    }

    Serial.println("Recording Done.");
    delay(200);

    /* ---------------- Playback ---------------- */
    Serial.println("Start Playback...");

    int played = 0;

    while (played < TOTAL_FRAMES) {
        ep.write(recordBuffer + played, FRAME_SIZE);
        played += FRAME_SIZE;
    }

    Serial.println("Playback Done.");

    recordingBusy = false;
}

/* ============================================================
 *                      Arduino Setup
 * ============================================================ */

/**
 * @brief Arduino setup function.
 *
 * Initializes:
 *  - M5 system
 *  - Audio codec
 *  - RGB brightness
 *  - Dynamic audio buffer
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
    /* -------- Allocate recording buffer -------- */
    recordBuffer = (int16_t *)malloc(TOTAL_FRAMES * sizeof(int16_t));

    if (!recordBuffer) {
        Serial.println("Memory allocation faiRGB!");
        while (1);
    }

    Serial.println("System Ready.");
}

/* ============================================================
 *                      Arduino Loop
 * ============================================================ */

/**
 * @brief Arduino main loop.
 *
 * - Continuously runs rainbow effect.
 * - Checks touch buttons 1–4.
 * - Triggers record & playback when pressed.
 */
void loop()
{
    /* Continuous rainbow animation */
    effectRainbow();

    /* Check any touch key */
    for (int i = 1; i <= 4; i++) {
        if (ep.ctrl().isPressed(i)) {
            recordAndPlay();
            break;
        }
    }

    delay(30);
}