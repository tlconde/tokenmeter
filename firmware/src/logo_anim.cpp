#include "logo_anim.h"
#include "logo_animations.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

#define GRID 20
#define COL_EMPTY 0x0000

static int  cell = 24;
static int  canvas_w = GRID * 24;
static int  canvas_h = GRID * 24;

static lv_obj_t*  container = NULL;
static lv_obj_t*  canvas = NULL;
static uint16_t*  canvas_buf = NULL;
static uint16_t*  row_buf = NULL;

static logo_screen_t active_screen = LOGO_SCREEN_CODEX;
static bool active = false;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;

static int anim_index_named(const char* want) {
    for (int i = 0; i < LOGO_ANIM_COUNT; i++) {
        if (logo_anims[i].name && strcmp(logo_anims[i].name, want) == 0) return i;
    }
    return -1;
}

static int idle_anim_index_for(logo_screen_t which) {
    const char* want = (which == LOGO_SCREEN_CODEX) ? "codex idle" : "cursor idle";
    int idx = anim_index_named(want);
    if (idx >= 0) return idx;
    return (which == LOGO_SCREEN_CODEX) ? 0 : 1;
}

static const logo_anim_def_t* current_anim(void) {
    if (LOGO_ANIM_COUNT == 0) return NULL;
    int idx = active_screen == LOGO_SCREEN_CURSOR
        ? anim_index_named("cursor splash")
        : idle_anim_index_for(active_screen);
    if (idx < 0) idx = idle_anim_index_for(active_screen);
    if (idx < 0 || idx >= LOGO_ANIM_COUNT) idx = 0;
    return &logo_anims[idx];
}

static void render_frame(const uint8_t* cells, const uint16_t* palette) {
    if (!row_buf || !canvas_buf) return;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < LOGO_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t* p = &row_buf[gx * cell];
            for (int i = 0; i < cell; i++) p[i] = color;
        }
        for (int dy = 0; dy < cell; dy++) {
            memcpy(&canvas_buf[(gy * cell + dy) * canvas_w], row_buf, canvas_w * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}

void logo_anim_init(lv_obj_t* parent) {
    const BoardCaps& c = board_caps();
    int min_dim = (c.width < c.height) ? c.width : c.height;
    cell = min_dim / GRID;
    if (cell < 4) cell = 4;

#ifdef BOARD_HAS_PSRAM
    const uint32_t canvas_caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t canvas_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const int MAX_CELL_NO_PSRAM = 10;
    if (cell > MAX_CELL_NO_PSRAM) cell = MAX_CELL_NO_PSRAM;
#endif

    canvas_w = GRID * cell;
    canvas_h = GRID * cell;

    canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, canvas_caps);
    row_buf = (uint16_t*)heap_caps_malloc(canvas_w * 2, canvas_caps);
    if (!canvas_buf || !row_buf) {
        Serial.println("logo_anim: failed to alloc canvas buffer");
        return;
    }

    container = lv_obj_create(parent);
    lv_obj_set_size(container, c.width, c.height);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(container);
    lv_canvas_set_buffer(canvas, canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    const logo_anim_def_t* a = current_anim();
    if (a && a->frame_count > 0) {
        render_frame(a->frames[0], a->palette);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

void logo_anim_tick(void) {
    if (!active) return;
    const logo_anim_def_t* a = current_anim();
    if (!a || a->frame_count == 0) return;

    uint16_t hold = a->holds[cur_frame];
    if (millis() - frame_started_ms >= hold) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms = millis();
        render_frame(a->frames[cur_frame], a->palette);
    }
}

void logo_anim_show(logo_screen_t which) {
    active_screen = which;
    cur_frame = 0;
    frame_started_ms = millis();
    const logo_anim_def_t* a = current_anim();
    if (a && a->frame_count > 0) render_frame(a->frames[0], a->palette);
    if (container) lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void logo_anim_hide(void) {
    if (container) lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

bool logo_anim_is_active(void) {
    return active;
}

lv_obj_t* logo_anim_get_root(void) {
    return container;
}

// ---- Mini animated logos for embedding (selector tiles, idle screens) ----
#define LOGO_MINI_MAX 4

struct LogoMini {
    lv_obj_t* canvas;
    uint16_t* buf;
    int       cell;
    int       w;
    const logo_anim_def_t* anim;
    uint16_t  frame;
    uint32_t  started;
};
static LogoMini logo_minis[LOGO_MINI_MAX] = {};
static int logo_mini_count = 0;

static void logo_mini_render(LogoMini* m) {
    if (!m->buf || !m->anim) return;
    const uint8_t* cells = m->anim->frames[m->frame];
    const uint16_t* pal = m->anim->palette;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (pal && code < LOGO_PALETTE_SIZE) ? pal[code] : COL_EMPTY;
            for (int dy = 0; dy < m->cell; dy++) {
                uint16_t* dst = &m->buf[(gy * m->cell + dy) * m->w + gx * m->cell];
                for (int dx = 0; dx < m->cell; dx++) dst[dx] = color;
            }
        }
    }
    if (m->canvas) lv_obj_invalidate(m->canvas);
}

lv_obj_t* logo_mini_create(lv_obj_t* parent, logo_screen_t which, int px) {
    if (logo_mini_count >= LOGO_MINI_MAX) return NULL;
    int idx = idle_anim_index_for(which);
    if (idx < 0 || idx >= LOGO_ANIM_COUNT) return NULL;
    const logo_anim_def_t* anim = &logo_anims[idx];
    if (!anim->frames || anim->frame_count == 0) return NULL;

    LogoMini* m = &logo_minis[logo_mini_count];
    m->anim = anim;
    m->cell = px / GRID;
    if (m->cell < 1) m->cell = 1;
    m->w = GRID * m->cell;
#ifdef BOARD_HAS_PSRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    m->buf = (uint16_t*)heap_caps_malloc(m->w * m->w * 2, caps);
    if (!m->buf) return NULL;
    m->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(m->canvas, m->buf, m->w, m->w, LV_COLOR_FORMAT_RGB565);
    m->frame = 0;
    m->started = millis();
    logo_mini_render(m);
    logo_mini_count++;
    return m->canvas;
}

void logo_mini_tick(void) {
    for (int i = 0; i < logo_mini_count; i++) {
        LogoMini* m = &logo_minis[i];
        if (!m->buf || !m->anim || m->anim->frame_count == 0) continue;
        if (millis() - m->started < m->anim->holds[m->frame]) continue;
        m->started = millis();
        m->frame = (m->frame + 1) % m->anim->frame_count;
        logo_mini_render(m);
    }
}

bool logo_build_image_dsc(logo_screen_t which, int px, lv_image_dsc_t* dsc, uint8_t* buf) {
    if (!dsc || !buf || px < GRID) return false;
    int idx = idle_anim_index_for(which);
    if (idx < 0 || idx >= LOGO_ANIM_COUNT) return false;
    const logo_anim_def_t* a = &logo_anims[idx];
    if (!a->frames || a->frame_count == 0) return false;

    int c = px / GRID;
    int w = GRID * c;
    int h = w;
    const uint8_t* cells = a->frames[0];
    const uint16_t* palette = a->palette;

    uint16_t* rgb = (uint16_t*)buf;
    uint8_t* alpha = buf + w * h * 2;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < LOGO_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint8_t a_px = (color == COL_EMPTY) ? 0 : 255;
            for (int dy = 0; dy < c; dy++) {
                for (int dx = 0; dx < c; dx++) {
                    int ix = gx * c + dx;
                    int iy = gy * c + dy;
                    int i = iy * w + ix;
                    rgb[i] = color;
                    alpha[i] = a_px;
                }
            }
        }
    }

    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = buf;
    dsc->data_size = w * h * 3;
    return true;
}
