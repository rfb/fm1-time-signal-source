/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Browser platform adapter for the time-signal station: in-memory display,
 * storage, controls and MIDI ports around the same APP sources as firmware.
 * The SDK engine is absent; it would be silent here, since the station
 * replaces its output. Instrument-only exports remain for sdk/Simulator.js
 * and report that no instrument is present. */
#include "instrument.h"
#include "fm_patch.h"
#include "fm1_engine_control.h"
#include "audio_output.h"
#include "fm1_audio_io.h"
#include <string.h>

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(name)))
#else
#define EXPORT(name)
#endif

enum { FLASH_BYTES = GROOVE_FLASH_BYTES, STATE_BYTES = 225 };
static struct fm1_controls controls;
static uint16_t pixels[240 * 240];
static uint8_t leds[41], flash[FLASH_BYTES], state[STATE_BYTES];
static uint8_t file_buffer[GROOVE_PROJECT_TUNED_BYTES];
static uint32_t now_ms, frame_revision, frame_dirty, flash_revision;
static uint64_t frames;
static uint8_t radio_features, radio_role;
static int32_t previous[FM1_BLOCK_WORDS];
static float output[FM1_BLOCK_WORDS];
/* Simulator.js reads 128 label bytes and a target table; both stay empty. */
static const uint8_t no_labels[128];
static const int32_t no_targets[16];

/* SDK engine and control-task services. */
static unsigned output_level, master_adc = 0xffffU;
int groove_master_update(unsigned adc)
{
    if (adc > 1023U) return 0;
    unsigned first = master_adc > 1023U;
    unsigned distance = adc > master_adc ? adc - master_adc : master_adc - adc;
    if (!first && adc > 4U && adc < 1019U && distance < 4U) return 0;
    unsigned level = adc <= 4U ? 0U : adc >= 1019U ? 127U : (adc * 127U + 511U) / 1023U;
    unsigned changed = output_level != level;
    master_adc = adc;
    output_level = level;
    return !first && changed;
}
uint32_t groove_master_state(void) { return master_adc | (output_level << 16); }
unsigned fm1_engine_output_level(void) { return output_level; }
int fm1_engine_render(int32_t *pcm, unsigned count)
{
    if (!pcm || count != FM1_BLOCK_FRAMES) return -1;
    memset(pcm, 0, FM1_BLOCK_WORDS * sizeof *pcm);
    return 0;
}
void fm1_engine_stats_read(struct fm1_engine_stats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    stats->submitted = stats->completed = (uint32_t)(frames / FM1_BLOCK_FRAMES);
}
void fm1_control_request(void) {}
void fm1_audio_stats_read(struct fm1_audio_stats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    stats->blocks = (uint32_t)(frames / FM1_BLOCK_FRAMES);
}
uint32_t fm1_millis(void) { return now_ms; }
void fm1_controls_init(void) {}
void fm1_controls_read(struct fm1_controls *value)
{
    *value = controls;
    memset(controls.encoders, 0, sizeof controls.encoders);
}
void fm1_led_set(unsigned key, int on) { if (key < sizeof leds) leds[key] = on != 0; }
/* Track 16-row bands until the browser consumes the next display update. */
static void screen_changed(unsigned y, unsigned h)
{
    if (!h) return;
    unsigned first = y / 16, bands = (y + h - 1) / 16 - first + 1;
    frame_dirty |= ((1U << bands) - 1U) << first;
    ++frame_revision;
}
void fm1_screen_init(void) { memset(pixels, 0, sizeof pixels); screen_changed(0, 240); }
void fm1_screen_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    if (x > 240 || y > 240 || w > 240 - x || h > 240 - y) return;
    for (unsigned r = 0; r < h; ++r)
        for (unsigned c = 0; c < w; ++c) pixels[(y + r) * 240 + x + c] = color;
    if (w) screen_changed(y, h);
}
int fm1_screen_blit(unsigned x, unsigned y, unsigned w, unsigned h,
                    const uint16_t *source, size_t count)
{
    if (!source || x > 240 || y > 240 || w > 240 - x || h > 240 - y || count < w * h) return -1;
    for (unsigned r = 0; r < h; ++r)
        memcpy(pixels + (y + r) * 240 + x, source + r * w, w * 2);
    if (w) screen_changed(y, h);
    return 0;
}
/* Settings work in the preview; discovery and links require the real radio. */
int fm1_bluetooth_configure(unsigned features)
{
    if (features & ~(FM1_BT_AUDIO | FM1_BT_MIDI)) return -1;
    radio_features = (uint8_t)features;
    return 0;
}
void fm1_bluetooth_get_status(struct fm1_bluetooth_status *value)
{
    if (!value) return;
    memset(value, 0, sizeof *value);
    value->features = radio_features;
}
int fm1_ble_set_role(enum fm1_ble_role role)
{
    if (role != FM1_BLE_PERIPHERAL && role != FM1_BLE_CENTRAL) return -1;
    radio_role = (uint8_t)role;
    return 0;
}
int fm1_ble_scan(void) { return -3; }
int fm1_ble_connect(unsigned index) { (void)index; return -3; }
int fm1_ble_disconnect(void) { return -3; }
void fm1_ble_get_status(struct fm1_ble_status *value)
{
    if (!value) return;
    memset(value, 0, sizeof *value);
    value->role = radio_role;
}
int fm1_ble_get_device(unsigned index, struct fm1_ble_device *value)
{ (void)index; if (value) memset(value, 0, sizeof *value); return -1; }
/* Retained-symbol stand-ins: the station never applies projects or tuning. */
int groove_project_apply_edit(const struct groove_project *p, unsigned operation)
{ (void)p; (void)operation; return -1; }
int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS]) { (void)t; return -1; }
void groove_audio_stats(struct fm1_render_stats *stats) { if (stats) memset(stats, 0, sizeof *stats); }
uint32_t fm1_flash_size(void) { return sizeof flash; }
int fm1_flash_read(uint32_t at, void *data, size_t bytes)
{
    if (!data || at > sizeof flash || bytes > sizeof flash - at) return FM1_FLASH_INVALID;
    memcpy(data, flash + at, bytes); return 0;
}
int fm1_flash_write(uint32_t at, const void *data, size_t bytes)
{
    if (!data || at > sizeof flash || bytes > sizeof flash - at) return FM1_FLASH_INVALID;
    const uint8_t *source = data;
    for (size_t i = 0; i < bytes; ++i)
        if ((flash[at + i] & source[i]) != source[i]) return FM1_FLASH_NEEDS_ERASE;
    memcpy(flash + at, data, bytes); ++flash_revision; return 0;
}
int fm1_flash_erase(uint32_t at)
{
    if (at % FM1_FLASH_SECTOR_BYTES || at > sizeof flash - FM1_FLASH_SECTOR_BYTES) return FM1_FLASH_INVALID;
    memset(flash + at, 255, FM1_FLASH_SECTOR_BYTES); ++flash_revision; return 0;
}
static unsigned midi_source = FM1_MIDI_USB;
unsigned fm1_midi_input_source(void) { return midi_source; }
/* Device replies, read back through sim_midi_output. */
static struct { uint8_t port, bytes, data[128]; } midi_capture[16];
static unsigned capture_read, capture_write;
int fm1_midi_send_port(unsigned port, const uint8_t *data, size_t bytes)
{
    if (port >= 3 || !data || !bytes || bytes > 128) return -1;
    if (capture_write - capture_read == 16) return -2;
    unsigned at = capture_write++ & 15U;
    midi_capture[at].port = (uint8_t)port; midi_capture[at].bytes = (uint8_t)bytes;
    memcpy(midi_capture[at].data, data, bytes);
    return 0;
}
/* The browser has no hardware streams; the same APP output stage still runs. */
int fm1_usb_audio_read(int32_t *pcm, unsigned count, uint16_t gains[2])
{
    if (!pcm || !gains || count != FM1_BLOCK_FRAMES) return -1;
    gains[0] = gains[1] = 0;
    return 0;
}
int fm1_bluetooth_audio_read(int16_t *pcm, unsigned count)
{ return pcm && count == FM1_BLOCK_FRAMES ? 0 : -1; }
int fm1_usb_audio_write(const int32_t *pcm, unsigned count)
{ return pcm && count == FM1_BLOCK_FRAMES ? 0 : -1; }

/* The SDK's startup order and control callback, as the firmware runs them. */
static void station_init(void)
{
    groove_fm_library_init();
    groove_samples_init();
    groove_project_init();
    groove_midi_init();
    groove_transfer_init();
    groove_screen_init();
}
static void station_control(void)
{
    groove_midi_poll();
    groove_samples_poll();
    groove_project_poll();
    groove_transfer_poll();
    groove_screen_control();
}

EXPORT("sim_boot") void sim_boot(unsigned clear)
{
    if (clear & 1U) { memset(flash, 255, sizeof flash); ++flash_revision; }
    memset(&controls, 0, sizeof controls); controls.battery = 900; controls.knob = 1023;
    memset(leds, 0, sizeof leds); memset(previous, 0, sizeof previous);
    midi_source = FM1_MIDI_USB; capture_read = capture_write = 0;
    now_ms = 0; frames = 0;
    master_adc = 0xffffU; output_level = 0;
    fm1_output_reset();
    radio_features = FM1_BT_AUDIO | FM1_BT_MIDI;
    radio_role = FM1_BLE_PERIPHERAL;
    station_init();
    station_control();
}
EXPORT("sim_control") void sim_control(unsigned kind, unsigned index, int value)
{
    if (kind == 0 && index < 14) {
        if (value) controls.buttons |= 1U << index; else controls.buttons &= ~(1U << index);
    } else if (kind == 1 && index < 27) {
        if (value) controls.keys |= 1U << index; else controls.keys &= ~(1U << index);
    } else if (kind == 2 && index < 7 && value >= -127 && value <= 127)
        controls.encoders[index] = (int16_t)value;
    else if (kind == 3 && index == 0 && value >= 0 && value <= 1023)
        controls.knob = (uint16_t)value;
    else return;
    station_control();
}
EXPORT("sim_release") void sim_release(void)
{
    controls.keys = controls.buttons = 0;
    memset(controls.encoders, 0, sizeof controls.encoders); station_control();
}
EXPORT("sim_clear") void sim_clear(void)
{
    unsigned level = controls.knob;
    sim_boot(1);
    sim_control(3, 0, (int)level);
}
EXPORT("sim_panic") void sim_panic(void) { sim_release(); }
EXPORT("sim_block_frames") unsigned sim_block_frames(void) { return FM1_BLOCK_FRAMES; }
EXPORT("sim_render") const float *sim_render(void)
{
    /* Preserve one block of output latency. */
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) output[i] = (float)previous[i] * (1.0f / 8388608.0f);
    (void)fm1_engine_render(previous, FM1_BLOCK_FRAMES);
    fm1_output_mix(previous);
    frames += FM1_BLOCK_FRAMES; uint32_t next = (uint32_t)(frames * 1000U / 44100U);
    unsigned poll = now_ms / 10U != next / 10U; now_ms = next;
    if (poll) station_control();
    return output;
}
EXPORT("sim_state") const uint8_t *sim_state(void)
{
    memset(state, 0, sizeof state);
    memcpy(state + 168, leds, sizeof leds);
    return state;
}
EXPORT("sim_pixels") const uint16_t *sim_pixels(void) { return pixels; }
EXPORT("sim_frame_revision") uint32_t sim_frame_revision(void) { return frame_revision; }
EXPORT("sim_frame_changes") unsigned sim_frame_changes(void)
{ unsigned dirty = frame_dirty; frame_dirty = 0; return dirty; }
EXPORT("sim_flash") uint8_t *sim_flash(void) { return flash; }
EXPORT("sim_flash_revision") uint32_t sim_flash_revision(void) { return flash_revision; }
EXPORT("sim_file") uint8_t *sim_file(void) { return file_buffer; }
EXPORT("sim_project_name") const char *sim_project_name(void)
{
    static char name[13]; groove_project_name(name); return name;
}
/* Application SysEx from the page reaches the same hook as on the device. */
EXPORT("sim_midi_from") int sim_midi_from(unsigned source, unsigned bytes)
{
    if (source >= 3 || !bytes || bytes > 1024) return -1;
    if (file_buffer[0] != 0xf0 || file_buffer[bytes - 1] != 0xf7) return bytes <= 3 ? 0 : -1;
    midi_source = source; fm1_app_sysex(file_buffer, bytes); midi_source = FM1_MIDI_USB;
    station_control(); return 0;
}
EXPORT("sim_midi") int sim_midi(unsigned bytes) { return sim_midi_from(FM1_MIDI_USB, bytes); }

/* Instrument features the station does not have. */
EXPORT("sim_tap_tempo") void sim_tap_tempo(void) {}
EXPORT("sim_open") int sim_open(unsigned target) { (void)target; return 0; }
EXPORT("sim_preset") int sim_preset(unsigned slot) { (void)slot; return -1; }
EXPORT("sim_select") int sim_select(unsigned track, unsigned source) { (void)track; (void)source; return -1; }
EXPORT("sim_step") int sim_step(unsigned step, unsigned toggle) { (void)step; (void)toggle; return -1; }
EXPORT("sim_step_first") unsigned sim_step_first(void) { return 0; }
EXPORT("sim_lcd") int sim_lcd(unsigned context, unsigned action, unsigned index, unsigned owner, int delta)
{ (void)context; (void)action; (void)index; (void)owner; (void)delta; return -1; }
EXPORT("sim_lcd_targets") const void *sim_lcd_targets(void) { return no_targets; }
EXPORT("sim_lcd_count") unsigned sim_lcd_count(void) { return 0; }
EXPORT("sim_lcd_context") unsigned sim_lcd_context(void) { return 0; }
EXPORT("sim_lcd_mask") unsigned sim_lcd_mask(void) { return 0; }
EXPORT("sim_knobs") const uint8_t *sim_knobs(void) { return no_labels; }
EXPORT("sim_preset_position") unsigned sim_preset_position(void) { return 0; }
EXPORT("sim_project_dirty") unsigned sim_project_dirty(void) { return 0; }
EXPORT("sim_edit_revision") uint32_t sim_edit_revision(void) { return 0; }
EXPORT("sim_project_revision") uint32_t sim_project_revision(void) { return 0; }
EXPORT("sim_count_in") unsigned sim_count_in(void) { return 0; }
EXPORT("sim_arp") unsigned sim_arp(unsigned track) { (void)track; return 255; }
EXPORT("sim_midi_capture") void sim_midi_capture(unsigned enable) { (void)enable; }
EXPORT("sim_midi_ports") void sim_midi_ports(unsigned ready, unsigned busy) { (void)ready; (void)busy; }
/* Encoded result: port in bits 8..9, byte count in bits 0..7; data in sim_file. */
EXPORT("sim_midi_output") unsigned sim_midi_output(void)
{
    if (capture_read == capture_write) return 0;
    unsigned at = capture_read++ & 15U, n = midi_capture[at].bytes;
    memcpy(file_buffer, midi_capture[at].data, n);
    return n | (unsigned)midi_capture[at].port << 8;
}
EXPORT("sim_midi_setting") unsigned sim_midi_setting(unsigned track, unsigned parameter)
{ (void)track; (void)parameter; return 0; }
EXPORT("sim_midi_edit") void sim_midi_edit(unsigned track, unsigned parameter, int delta)
{ (void)track; (void)parameter; (void)delta; }
EXPORT("sim_export") int sim_export(void) { return -1; }
EXPORT("sim_import") int sim_import(unsigned bytes) { (void)bytes; return -1; }
EXPORT("sim_library_read") int sim_library_read(unsigned bank) { (void)bank; return -1; }
EXPORT("sim_library_write") int sim_library_write(unsigned bank) { (void)bank; return -1; }
EXPORT("sim_factory_name") const char *sim_factory_name(unsigned index) { (void)index; return NULL; }
EXPORT("sim_factory_project") int sim_factory_project(unsigned index) { (void)index; return -1; }
EXPORT("sim_patch_export") int sim_patch_export(void) { return -1; }
EXPORT("sim_patch_import") int sim_patch_import(void) { return -1; }
EXPORT("sim_timbre_export") int sim_timbre_export(void) { return -1; }
EXPORT("sim_timbre_import") int sim_timbre_import(void) { return -1; }
EXPORT("sim_factory") const uint8_t *sim_factory(void) { return NULL; }
EXPORT("sim_factory_size") unsigned sim_factory_size(void) { return 0; }
EXPORT("sim_sample_alloc") int16_t *sim_sample_alloc(unsigned count) { (void)count; return NULL; }
EXPORT("sim_sample_encode") const uint8_t *sim_sample_encode(void) { return NULL; }
EXPORT("sim_sample_bytes") unsigned sim_sample_bytes(void) { return 0; }
EXPORT("sim_sample_bank") int sim_sample_bank(unsigned bytes) { (void)bytes; return -1; }
