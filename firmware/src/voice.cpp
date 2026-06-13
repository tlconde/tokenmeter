#include "voice.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "hal/audio_hal.h"
#include "hal/board_caps.h"
#include "theme.h"
#include "idle.h"

LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);

// ---------------------------------------------------------------- protocol

#define VOICE_SERVICE_UUID "4c41555a-4465-7669-6365-000000000010"
#define AUDIO_OUT_UUID     "4c41555a-4465-7669-6365-000000000011"
#define CTRL_UUID          "4c41555a-4465-7669-6365-000000000012"
#define AUDIO_IN_UUID      "4c41555a-4465-7669-6365-000000000013"
#define EVT_UUID           "4c41555a-4465-7669-6365-000000000014"

#define CHUNK_PAYLOAD 180                 // ADPCM bytes per BLE frame
#define CHUNK_SAMPLES (CHUNK_PAYLOAD * 2) // 4-bit ADPCM → 2 samples/byte
#define ASK_TEXT_MAX  208

// Playback ring sized for ~45 s of ADPCM (16 kHz mono, 8 kB/s) in PSRAM.
#define PLAY_RING_SIZE (368 * 1024)

// ------------------------------------------------------------- IMA ADPCM
// Mirror of voice_approver/adpcm.py: standard IMA tables, predictor and
// step index carried across the whole stream (no block headers), initial
// predictor=0 index=0, FIRST sample in the LOW nibble of each byte.

static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

struct AdpcmState {
    int32_t predictor = 0;
    int16_t index = 0;
    void reset() { predictor = 0; index = 0; }
};

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}
static inline int16_t clamp_index(int32_t i) {
    if (i < 0) return 0;
    if (i > 88) return 88;
    return (int16_t)i;
}

static uint8_t adpcm_encode_sample(AdpcmState& st, int16_t sample) {
    int32_t step = STEP_TABLE[st.index];
    int32_t diff = sample - st.predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int32_t delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; delta += step; }
    if (code & 8) st.predictor -= delta;
    else          st.predictor += delta;
    st.predictor = clamp16(st.predictor);
    st.index = clamp_index(st.index + INDEX_TABLE[code]);
    return code;
}

static int16_t adpcm_decode_nibble(AdpcmState& st, uint8_t code) {
    int32_t step = STEP_TABLE[st.index];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    if (code & 8) st.predictor -= diff;
    else          st.predictor += diff;
    st.predictor = clamp16(st.predictor);
    st.index = clamp_index(st.index + INDEX_TABLE[code]);
    return (int16_t)st.predictor;
}

// ------------------------------------------------------------------ state

static bool audio_ok = false;

static NimBLECharacteristic* audio_in_char = nullptr;
static NimBLECharacteristic* evt_char = nullptr;

// Playback ring (PSRAM). Single producer (NimBLE host task) / single
// consumer (audio task).
static uint8_t* play_ring = nullptr;
static volatile uint32_t play_w = 0;
static volatile uint32_t play_r = 0;
static volatile bool play_last_seen = false;
static volatile bool playing = false;
static AdpcmState play_dec;

// Recording
static volatile bool recording = false;
static volatile bool rec_stop_req = false;
static volatile uint32_t rec_end_ms = 0;
static AdpcmState rec_enc;
static volatile uint8_t rec_seq = 0;

// Requests from BLE callbacks to the main loop (LVGL + notify context)
static volatile bool req_show_ask = false;
static volatile bool req_idle = false;
static volatile uint32_t req_rec_max_s = 0;  // nonzero → start recording
static char ask_text[ASK_TEXT_MAX];
static portMUX_TYPE voice_mux = portMUX_INITIALIZER_UNLOCKED;

// Outbound frames/events (produced on audio task, notified on main loop)
struct OutFrame {
    uint8_t len;       // payload bytes (ADPCM), 0 allowed for bare last-flag
    uint8_t seq;
    uint8_t flags;
    uint8_t data[CHUNK_PAYLOAD];
};
static QueueHandle_t out_frames = nullptr;   // OutFrame
static QueueHandle_t out_events = nullptr;   // const char* (static strings)

static const char EVT_PLAY_DONE[]  = "{\"evt\":\"play_done\"}";
static const char EVT_REC_DONE[]   = "{\"evt\":\"rec_done\"}";
static const char EVT_TOUCH_ALLOW[] = "{\"evt\":\"touch\",\"d\":\"allow\"}";
static const char EVT_TOUCH_DENY[]  = "{\"evt\":\"touch\",\"d\":\"deny\"}";

static void queue_event(const char* json) {
    if (out_events) xQueueSend(out_events, &json, 0);
}

// ------------------------------------------------------------ BLE callbacks

class AudioOutCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        NimBLEAttValue v = c->getValue();
        if (v.size() < 2) return;
        const uint8_t* d = v.data();
        uint8_t seq = d[0];
        uint8_t flags = d[1];
        size_t payload = v.size() - 2;

        if (!audio_ok) {
            // No speaker: keep the protocol responsive — ack the clip as
            // soon as the last chunk lands so the daemon doesn't stall.
            if (flags & 1) queue_event(EVT_PLAY_DONE);
            return;
        }

        portENTER_CRITICAL(&voice_mux);
        if (seq == 0) {  // new stream
            play_w = 0;
            play_r = 0;
            play_last_seen = false;
            play_dec.reset();
            playing = true;
        }
        uint32_t w = play_w;
        portEXIT_CRITICAL(&voice_mux);

        if (playing && play_ring && payload > 0) {
            for (size_t i = 0; i < payload; i++)
                play_ring[(w + i) % PLAY_RING_SIZE] = d[2 + i];
            portENTER_CRITICAL(&voice_mux);
            play_w = w + payload;
            if (flags & 1) play_last_seen = true;
            portEXIT_CRITICAL(&voice_mux);
        } else if (flags & 1) {
            portENTER_CRITICAL(&voice_mux);
            play_last_seen = true;
            portEXIT_CRITICAL(&voice_mux);
        }
    }
};

class CtrlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        NimBLEAttValue v = c->getValue();
        JsonDocument doc;
        if (deserializeJson(doc, (const char*)v.data(), v.size()) != DeserializationError::Ok)
            return;
        const char* cmd = doc["cmd"];
        if (!cmd) return;

        if (strcmp(cmd, "ask") == 0) {
            const char* text = doc["text"] | "";
            portENTER_CRITICAL(&voice_mux);
            strlcpy(ask_text, text, sizeof(ask_text));
            req_show_ask = true;
            portEXIT_CRITICAL(&voice_mux);
        } else if (strcmp(cmd, "rec") == 0) {
            uint32_t max_s = doc["max_s"] | 8;
            if (max_s > 30) max_s = 30;
            if (!audio_ok) {
                // No mic: report an empty recording right away.
                queue_event(EVT_REC_DONE);
            } else {
                req_rec_max_s = max_s;
            }
        } else if (strcmp(cmd, "stop") == 0) {
            rec_stop_req = true;
            portENTER_CRITICAL(&voice_mux);
            playing = false;
            portEXIT_CRITICAL(&voice_mux);
        } else if (strcmp(cmd, "idle") == 0) {
            req_idle = true;
            rec_stop_req = true;
            portENTER_CRITICAL(&voice_mux);
            playing = false;
            portEXIT_CRITICAL(&voice_mux);
        }
    }
};

void voice_register_ble(NimBLEServer* server) {
    NimBLEService* svc = server->createService(VOICE_SERVICE_UUID);

    NimBLECharacteristic* audio_out = svc->createCharacteristic(
        AUDIO_OUT_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    static AudioOutCallbacks audioOutCb;
    audio_out->setCallbacks(&audioOutCb);

    NimBLECharacteristic* ctrl = svc->createCharacteristic(
        CTRL_UUID, NIMBLE_PROPERTY::WRITE);
    static CtrlCallbacks ctrlCb;
    ctrl->setCallbacks(&ctrlCb);

    audio_in_char = svc->createCharacteristic(
        AUDIO_IN_UUID, NIMBLE_PROPERTY::NOTIFY);
    evt_char = svc->createCharacteristic(
        EVT_UUID, NIMBLE_PROPERTY::NOTIFY);

    svc->start();
}

// -------------------------------------------------------------- audio task

static void audio_task(void*) {
    static int16_t pcm[CHUNK_SAMPLES];

    for (;;) {
        // ---- playback ----
        if (playing) {
            audio_hal_set_amp(true);
            while (playing) {
                portENTER_CRITICAL(&voice_mux);
                uint32_t avail = play_w - play_r;
                bool last = play_last_seen;
                portEXIT_CRITICAL(&voice_mux);

                if (avail == 0) {
                    if (last) {
                        audio_hal_drain();
                        portENTER_CRITICAL(&voice_mux);
                        playing = false;
                        portEXIT_CRITICAL(&voice_mux);
                        queue_event(EVT_PLAY_DONE);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }

                uint32_t n = avail;
                if (n > CHUNK_PAYLOAD) n = CHUNK_PAYLOAD;
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t b = play_ring[(play_r + i) % PLAY_RING_SIZE];
                    pcm[2 * i]     = adpcm_decode_nibble(play_dec, b & 0x0F);
                    pcm[2 * i + 1] = adpcm_decode_nibble(play_dec, (b >> 4) & 0x0F);
                }
                portENTER_CRITICAL(&voice_mux);
                play_r += n;
                portEXIT_CRITICAL(&voice_mux);
                audio_hal_write(pcm, n * 2);  // blocks ≈ realtime
            }
            audio_hal_set_amp(false);
        }

        // ---- recording ----
        if (recording) {
            size_t got = audio_hal_read(pcm, CHUNK_SAMPLES);
            bool done = rec_stop_req || (int32_t)(millis() - rec_end_ms) >= 0;

            OutFrame f;
            f.seq = rec_seq;
            rec_seq = rec_seq + 1;
            f.flags = done ? 1 : 0;
            f.len = 0;
            for (size_t i = 0; i + 1 < got; i += 2) {
                uint8_t lo = adpcm_encode_sample(rec_enc, pcm[i]);
                uint8_t hi = adpcm_encode_sample(rec_enc, pcm[i + 1]);
                f.data[f.len++] = (uint8_t)(lo | (hi << 4));
            }
            if (f.len > 0 || done)
                xQueueSend(out_frames, &f, pdMS_TO_TICKS(50));
            if (done) {
                recording = false;
                rec_stop_req = false;
                queue_event(EVT_REC_DONE);
            }
            if (got == 0) vTaskDelay(pdMS_TO_TICKS(5));  // I2S hiccup guard
            continue;  // otherwise no delay — i2s read paces us
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ----------------------------------------------------------------- overlay

static lv_obj_t* ov_panel = nullptr;
static lv_obj_t* ov_label = nullptr;
static volatile int touch_pick = 0;  // 0 none, 1 allow, 2 deny

// Failsafe: if the daemon never sends "idle" (BLE drop mid-approval), the
// overlay must not trap the UI forever. Daemon-side timeout is 30 s.
static uint32_t overlay_shown_ms = 0;

#define OVERLAY_STALE_MS 45000UL

static void ov_btn_cb(lv_event_t* e) {
    touch_pick = (int)(intptr_t)lv_event_get_user_data(e);
}

static void overlay_build(void) {
    const int W = board_caps().width;
    const int H = board_caps().height;

    ov_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov_panel, W - 24, H - 24);
    lv_obj_center(ov_panel);
    lv_obj_set_style_bg_color(ov_panel, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(ov_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ov_panel, THEME_ACCENT, 0);
    lv_obj_set_style_border_width(ov_panel, 2, 0);
    lv_obj_set_style_radius(ov_panel, 16, 0);
    lv_obj_set_style_pad_all(ov_panel, 16, 0);
    lv_obj_clear_flag(ov_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(ov_panel);
    lv_label_set_text(title, "Approval needed");
    lv_obj_set_style_text_font(title, &font_styrene_24, 0);
    lv_obj_set_style_text_color(title, THEME_ACCENT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    ov_label = lv_label_create(ov_panel);
    lv_label_set_long_mode(ov_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ov_label, W - 24 - 32);
    lv_obj_set_style_text_font(ov_label, &font_styrene_20, 0);
    lv_obj_set_style_text_color(ov_label, THEME_TEXT, 0);
    lv_obj_align(ov_label, LV_ALIGN_TOP_MID, 0, 44);

    const int btn_w = (W - 24 - 32 - 12) / 2;
    struct { const char* txt; lv_color_t col; int pick; lv_align_t align; } defs[2] = {
        {"Deny",  THEME_RED,   2, LV_ALIGN_BOTTOM_LEFT},
        {"Allow", THEME_GREEN, 1, LV_ALIGN_BOTTOM_RIGHT},
    };
    for (auto& d : defs) {
        lv_obj_t* btn = lv_button_create(ov_panel);
        lv_obj_set_size(btn, btn_w, 64);
        lv_obj_align(btn, d.align, 0, 0);
        lv_obj_set_style_bg_color(btn, d.col, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_add_event_cb(btn, ov_btn_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)d.pick);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, d.txt);
        lv_obj_set_style_text_font(l, &font_styrene_24, 0);
        lv_obj_center(l);
    }

    lv_obj_add_flag(ov_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ov_panel, LV_OBJ_FLAG_CLICKABLE);
}

bool voice_overlay_active(void) {
    return ov_panel && !lv_obj_has_flag(ov_panel, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------------------- init

void voice_init(void) {
    out_frames = xQueueCreate(32, sizeof(OutFrame));
    out_events = xQueueCreate(8, sizeof(const char*));

    // Keep the codec dormant until voice is explicitly requested. Boot-time
    // codec setup invalidates the shared I2C controller used by touch.
    const bool audio_autostart = false;
    if (audio_autostart && board_caps().has_audio) {
        play_ring = (uint8_t*)heap_caps_malloc(PLAY_RING_SIZE, MALLOC_CAP_SPIRAM);
        if (!play_ring)
            play_ring = (uint8_t*)heap_caps_malloc(PLAY_RING_SIZE, MALLOC_CAP_DEFAULT);
        audio_ok = play_ring && audio_hal_init();
    }
    if (audio_ok) {
        xTaskCreatePinnedToCore(audio_task, "voice_audio", 6144, nullptr,
                                3, nullptr, 0);  // core 0; loop() runs on 1
    }
    overlay_build();
    Serial.printf("voice: ready (audio %s)\n", audio_ok ? "on" : "off");
}

// ------------------------------------------------------------------- tick

void voice_tick(void) {
    // Overlay show/hide (LVGL — main loop only)
    if (req_show_ask) {
        portENTER_CRITICAL(&voice_mux);
        req_show_ask = false;
        char buf[ASK_TEXT_MAX];
        strlcpy(buf, ask_text, sizeof(buf));
        portEXIT_CRITICAL(&voice_mux);
        if (ov_label) lv_label_set_text(ov_label, buf);
        if (ov_panel) {
            lv_obj_clear_flag(ov_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ov_panel, LV_OBJ_FLAG_CLICKABLE);
            overlay_shown_ms = millis();
        }
        touch_pick = 0;
        idle_note_activity();  // wake the panel so the ask is visible
    }
    if (req_idle) {
        req_idle = false;
        if (ov_panel) {
            lv_obj_add_flag(ov_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ov_panel, LV_OBJ_FLAG_CLICKABLE);
        }
        touch_pick = 0;
    }

    if (voice_overlay_active() && millis() - overlay_shown_ms > OVERLAY_STALE_MS) {
        Serial.println("voice: stale overlay auto-hidden");
        lv_obj_add_flag(ov_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ov_panel, LV_OBJ_FLAG_CLICKABLE);
        touch_pick = 0;
    }

    // Touch decision → EVT
    if (touch_pick) {
        queue_event(touch_pick == 1 ? EVT_TOUCH_ALLOW : EVT_TOUCH_DENY);
        touch_pick = 0;
        if (ov_panel) {
            lv_obj_add_flag(ov_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ov_panel, LV_OBJ_FLAG_CLICKABLE);
        }
        rec_stop_req = true;
        portENTER_CRITICAL(&voice_mux);
        playing = false;
        portEXIT_CRITICAL(&voice_mux);
    }

    // Start recording (worker-thread state owned here to keep ordering sane)
    if (req_rec_max_s && !recording) {
        uint32_t max_s = req_rec_max_s;
        req_rec_max_s = 0;
        rec_enc.reset();
        rec_seq = 0;
        rec_stop_req = false;
        rec_end_ms = millis() + max_s * 1000;
        recording = true;
    }

    // Outbound notifies (NimBLE — keep on one thread)
    OutFrame f;
    while (out_frames && xQueueReceive(out_frames, &f, 0) == pdTRUE) {
        if (!audio_in_char) continue;
        uint8_t frame[2 + CHUNK_PAYLOAD];
        frame[0] = f.seq;
        frame[1] = f.flags;
        memcpy(frame + 2, f.data, f.len);
        audio_in_char->setValue(frame, 2 + f.len);
        audio_in_char->notify();
    }
    const char* evt;
    while (out_events && xQueueReceive(out_events, &evt, 0) == pdTRUE) {
        if (!evt_char) continue;
        evt_char->setValue((const uint8_t*)evt, strlen(evt));
        evt_char->notify();
    }
}
