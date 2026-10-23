/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Time-signal station display. Replaces the instrument UI: shows local and
 * UTC time, synchronization, master level and audio health, and feeds the
 * MASTER knob to the engine's output gain. Control task only. */
#include "instrument.h"
#include "audio_output.h"
#include "clock.h"
#include "fm1_engine_control.h"
#include <string.h>

enum { BACK = 0x0000, TEXT = 0xef5d, ACCENT = 0x07ff, DIM = 0x738e, GREEN = 0x07e0, AMBER = 0xfd20 };
/* Panel button, LED and encoder numbers (see sdk/Simulator.js faceplate). */
enum { EDIT_BUTTON = 6, HOME_BUTTON = 8, SAVE_BUTTON = 9, PLAY_LED = 12, LEDS = 41, BAND_ROWS = 16 };
enum { HOUR_KNOB = 2, MINUTE_KNOB = 3, SECOND_KNOB = 4, ZONE_KNOB = 5 };
#define BUTTON(b) (1U << (b))
/* Five columns per glyph, bit 0 at the top. Digits followed by uppercase. */
static const uint8_t font[36][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0},       {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0, 0x41, 0x7f, 0x41, 0},       {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};
static const uint8_t blank[5], colon[5] = {0, 0x36, 0x36, 0, 0};
static const uint8_t minus[5] = {8, 8, 8, 8, 8}, plus[5] = {8, 8, 0x3e, 8, 8};
static const struct { uint8_t y, scale; } rows[] = {
    {14, 2}, {56, 4}, {98, 2}, {136, 2}, {166, 2}, {196, 2}, {220, 2}};
enum { TITLE, LOCAL, ZONE, UTC, SYNC, MASTER, HEALTH, LINES = sizeof rows / sizeof rows[0] };
static uint16_t band[FM1_SCREEN_WIDTH * BAND_ROWS];
static char shown[LINES][21];
static uint16_t shown_color[LINES];
static uint32_t last_ms, carry_ms, seconds, synced_at;
static unsigned led_on, sync_count, previous_buttons, setting, input_utc;
static uint32_t entered; /* SET TIME entry: second of day in the input zone */

static const uint8_t *glyph(char c)
{
    if (c >= '0' && c <= '9') return font[c - '0'];
    if (c >= 'A' && c <= 'Z') return font[10 + c - 'A'];
    return c == ':' ? colon : c == '-' ? minus : c == '+' ? plus : blank;
}
/* Redraw a full-width row with centred text, at most 240 / (6 * scale) characters. */
static void line(unsigned index, const char *s, uint16_t color)
{
    if (!strcmp(shown[index], s) && shown_color[index] == color) return;
    strcpy(shown[index], s);
    shown_color[index] = color;
    unsigned scale = rows[index].scale, n = (unsigned)strlen(s), w = 6U * scale, h = 7U * scale;
    unsigned x0 = (FM1_SCREEN_WIDTH + scale - n * w) / 2;
    for (unsigned top = 0; top < h; top += BAND_ROWS) {
        unsigned count = h - top < BAND_ROWS ? h - top : BAND_ROWS;
        for (unsigned r = 0; r < count; ++r) {
            unsigned mask = 1U << ((top + r) / scale);
            for (unsigned x = 0; x < FM1_SCREEN_WIDTH; ++x) {
                unsigned c = x < x0 ? n : (x - x0) / w, column = x < x0 ? 5 : (x - x0) % w / scale;
                band[r * FM1_SCREEN_WIDTH + x] =
                    c < n && column < 5 && (glyph(s[c])[column] & mask) ? color : BACK;
            }
        }
        fm1_screen_blit(0, rows[index].y + top, FM1_SCREEN_WIDTH, count, band,
                        FM1_SCREEN_WIDTH * count);
    }
}
/* Append value in decimal, zero-padded to at least digits places. */
static char *decimal(char *out, uint32_t value, unsigned digits)
{
    char reversed[10];
    unsigned n = 0;
    do { reversed[n++] = (char)('0' + value % 10U); value /= 10U; } while (value || n < digits);
    while (n) *out++ = reversed[--n];
    *out = 0;
    return out;
}
void groove_screen_init(void)
{
    fm1_controls_init();
    for (unsigned key = 0; key < LEDS; ++key) fm1_led_set(key, 0);
    led_on = setting = input_utc = 0;
    fm1_screen_init();
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, BACK);
    memset(shown, 0, sizeof shown);
    last_ms = fm1_millis();
    carry_ms = seconds = synced_at = 0;
    sync_count = clock_set_count();
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    previous_buttons = controls.buttons;
}
/* Append HH:MM:SS of a second of day. */
static char *clock_text(char *out, uint32_t second)
{
    out = decimal(out, second / 3600U, 2);
    *out++ = ':';
    out = decimal(out, second / 60U % 60U, 2);
    *out++ = ':';
    return decimal(out, second % 60U, 2);
}
/* Append UTC+H:MM or UTC-H:MM. */
static char *zone_text(char *out, int offset)
{
    unsigned magnitude = (unsigned)(offset < 0 ? -offset : offset);
    memcpy(out, "UTC", 3);
    out[3] = offset < 0 ? '-' : '+';
    out = decimal(out + 4, magnitude / 60U, 1);
    *out++ = ':';
    return decimal(out, magnitude % 60U, 2);
}
static uint32_t shift(uint32_t second, int32_t delta)
{
    return (uint32_t)(((int32_t)second + delta % CLOCK_DAY + CLOCK_DAY) % CLOCK_DAY);
}
/* Turn one HH:MM:SS field, wrapping within it without carrying. */
static uint32_t turn(uint32_t second, int32_t unit, int32_t range, int delta)
{
    int32_t field = (int32_t)second / unit % range;
    int32_t turned = ((field + delta) % range + range) % range;
    return (uint32_t)((int32_t)second + (turned - field) * unit);
}
static void set_leds(unsigned on)
{
    fm1_led_set(SAVE_BUTTON, (int)on);
    fm1_led_set(HOME_BUTTON, (int)on);
}
void groove_screen_control(void)
{
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    (void)groove_master_update(controls.knob);
    unsigned pressed = controls.buttons & ~previous_buttons;
    previous_buttons = controls.buttons;

    /* Uptime for the sync age; the millisecond clock wraps after 49 days. */
    uint32_t now = fm1_millis();
    carry_ms += now - last_ms;
    last_ms = now;
    seconds += carry_ms / 1000U;
    carry_ms %= 1000U;
    if (clock_set_count() != sync_count) { sync_count = clock_set_count(); synced_at = seconds; }

    unsigned tone = fm1_output_tone();
    if (tone != led_on) { fm1_led_set(PLAY_LED, (int)tone); led_on = tone; }

    struct clock_time time;
    clock_read(&time);
    int offset = clock_offset();
    uint32_t local = shift(time.second, offset * 60);
    char text[21], *at;

    /* SET TIME: EDIT opens it with the current time, frozen. KNOB 1-3 turn
     * hours, minutes and seconds; KNOB 4 selects local or UTC input, keeping
     * the same instant. SAVE sets the clock to the entered time at that
     * moment; HOME cancels. */
    if (!setting && (pressed & BUTTON(EDIT_BUTTON))) {
        setting = 1;
        input_utc = 0;
        entered = local;
        set_leds(1);
    } else if (setting && (pressed & (BUTTON(SAVE_BUTTON) | BUTTON(HOME_BUTTON)))) {
        if (pressed & BUTTON(SAVE_BUTTON))
            clock_set((input_utc ? entered : shift(entered, -offset * 60)) * 1000U);
        setting = 0;
        set_leds(0);
    } else if (setting) {
        entered = turn(entered, 3600, 24, controls.encoders[HOUR_KNOB]);
        entered = turn(entered, 60, 60, controls.encoders[MINUTE_KNOB]);
        entered = turn(entered, 1, 60, controls.encoders[SECOND_KNOB]);
        unsigned utc = controls.encoders[ZONE_KNOB] > 0 ? 1U : controls.encoders[ZONE_KNOB] < 0 ? 0U : input_utc;
        if (utc != input_utc) {
            entered = shift(entered, utc ? -offset * 60 : offset * 60);
            input_utc = utc;
        }
    }
    if (setting) {
        line(TITLE, "SET TIME", ACCENT);
        clock_text(text, entered);
        line(LOCAL, text, AMBER);
        if (input_utc)
            strcpy(text, "INPUT UTC");
        else {
            memcpy(text, "LOCAL ", 6);
            zone_text(text + 6, offset);
        }
        line(ZONE, text, TEXT);
        line(UTC, "K1 H  K2 M  K3 S", DIM);
        line(SYNC, "K4 LOCAL OR UTC", DIM);
        line(MASTER, "SAVE ACCEPT", GREEN);
        line(HEALTH, "HOME CANCEL", DIM);
        return;
    }

    line(TITLE, "HAIDA STANDARD TIME", ACCENT);
    clock_text(text, local);
    line(LOCAL, text, time.set ? TEXT : AMBER);
    zone_text(text, offset);
    line(ZONE, text, DIM);
    memcpy(text, "UTC ", 4);
    clock_text(text + 4, time.second);
    line(UTC, text, DIM);

    if (!time.set)
        line(SYNC, "TIME NOT SET", AMBER);
    else {
        uint32_t age = (seconds - synced_at) / 60U;
        memcpy(text, "SET ", 4);
        at = decimal(text + 4, age < 120U ? age : age / 60U, 1);
        strcpy(at, age < 120U ? "M AGO" : "H AGO");
        line(SYNC, text, GREEN);
    }

    unsigned level = fm1_engine_output_level();
    if (level) {
        memcpy(text, "MASTER ", 7);
        decimal(text + 7, (level * 100U + 63U) / 127U, 1);
    } else
        strcpy(text, "MASTER MUTED");
    line(MASTER, text, DIM);

    struct fm1_audio_stats dma;
    fm1_audio_stats_read(&dma);
    if (dma.overruns) {
        memcpy(text, "OVERRUNS ", 9);
        decimal(text + 9, dma.overruns, 1);
        line(HEALTH, text, AMBER);
    } else
        line(HEALTH, "AUDIO OK", DIM);
}
