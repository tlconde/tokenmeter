#include "logo_anim.h"
#include "logo_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

#define GRID LOGO_GRID_SIZE
#define COL_EMPTY 0x0000

static int  cell = 24;
static int  canvas_w = GRID * 24;
static int  canvas_h = GRID * 24;

static lv_obj_t*  container = NULL;
static lv_obj_t*  canvas = NULL;
static uint16_t*  canvas_buf = NULL;
static uint8_t    rendered_cells[GRID * GRID] = {};
static const uint16_t* rendered_palette = NULL;
static bool       rendered_valid = false;

static logo_screen_t active_screen = LOGO_SCREEN_CODEX;
static bool active = false;
static int active_anim_index = -1;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;

#define CODEX_ROTATE_INTERVAL_MS 20000
// v2 (deferred): the four-group rate-driven rotation lived here. tokenmeter v1
// collapses all four codex animations into a single "codex balanced" splash, so
// pick_for_current_rate() / idle_anim_index_for() ignore the rate group and
// always return "codex balanced". Re-introduce the rotation tables and the
// per-group resolve when distinct activity motions ship.

static int anim_index_named(const char* want) {
    for (int i = 0; i < LOGO_ANIM_COUNT; i++) {
        if (logo_anims[i].name && strcmp(logo_anims[i].name, want) == 0) return i;
    }
    return -1;
}

static int idle_anim_index_for(logo_screen_t which) {
    const char* want =
        (which == LOGO_SCREEN_CODEX) ? "codex balanced" : "cursor idle";
    int idx = anim_index_named(want);
    if (idx >= 0) return idx;
    return (which == LOGO_SCREEN_CODEX) ? 0 : 1;
}

static const logo_anim_def_t* current_anim(void) {
    if (LOGO_ANIM_COUNT == 0) return NULL;
    int idx = active_anim_index;
    if (idx < 0) {
        idx = active_screen == LOGO_SCREEN_CURSOR
            ? anim_index_named("cursor splash")
            : idle_anim_index_for(active_screen);
    }
    if (idx < 0) idx = idle_anim_index_for(active_screen);
    if (idx < 0 || idx >= LOGO_ANIM_COUNT) idx = 0;
    return &logo_anims[idx];
}

static bool is_splash_anim_for(int idx, logo_screen_t which) {
    if (idx < 0 || idx >= LOGO_ANIM_COUNT) return false;
    const char* name = logo_anims[idx].name;
    if (!name) return false;
    const char* prefix = (which == LOGO_SCREEN_CODEX) ? "codex " : "cursor ";
    const size_t plen = (which == LOGO_SCREEN_CODEX) ? 6 : 7;
    return strncmp(name, prefix, plen) == 0;
}

static void resolve_codex_groups(void) {
    // v1: no-op. Group rotation tables removed; "codex balanced" is the only
    // splash animation. Re-introduce when distinct activity motions ship.
}

static void render_frame(const uint8_t* cells, const uint16_t* palette) {
    if (!cells || !canvas_buf || !canvas) return;

    const bool full_redraw = !rendered_valid || palette != rendered_palette;
    int min_gx = GRID;
    int min_gy = GRID;
    int max_gx = -1;
    int max_gy = -1;

    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            const int index = gy * GRID + gx;
            const uint8_t code = cells[index];
            if (!full_redraw && rendered_cells[index] == code) continue;

            const uint16_t color =
                (palette && code < LOGO_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            for (int dy = 0; dy < cell; dy++) {
                uint16_t* dst =
                    &canvas_buf[(gy * cell + dy) * canvas_w + gx * cell];
                for (int dx = 0; dx < cell; dx++) dst[dx] = color;
            }
            rendered_cells[index] = code;
            if (gx < min_gx) min_gx = gx;
            if (gx > max_gx) max_gx = gx;
            if (gy < min_gy) min_gy = gy;
            if (gy > max_gy) max_gy = gy;
        }
    }

    rendered_valid = true;
    rendered_palette = palette;
    if (max_gx < 0) return;

    lv_area_t local_area = {
        .x1 = min_gx * cell,
        .y1 = min_gy * cell,
        .x2 = (max_gx + 1) * cell - 1,
        .y2 = (max_gy + 1) * cell - 1,
    };
    lv_draw_buf_flush_cache(lv_canvas_get_draw_buf(canvas), &local_area);

    lv_area_t canvas_coords;
    lv_obj_get_coords(canvas, &canvas_coords);
    lv_area_t screen_area = {
        .x1 = canvas_coords.x1 + local_area.x1,
        .y1 = canvas_coords.y1 + local_area.y1,
        .x2 = canvas_coords.x1 + local_area.x2,
        .y2 = canvas_coords.y1 + local_area.y2,
    };
    lv_obj_invalidate_area(canvas, &screen_area);
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
    if (!canvas_buf) {
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
    resolve_codex_groups();

    const logo_anim_def_t* a = current_anim();
    if (a && a->frame_count > 0) {
        render_frame(a->frames[0], a->palette);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

void logo_anim_tick(void) {
    if (!active) return;
    if (active_screen == LOGO_SCREEN_CODEX &&
        millis() - last_pick_ms >= CODEX_ROTATE_INTERVAL_MS) {
        logo_anim_pick_for_current_rate();
    }
    const logo_anim_def_t* a = current_anim();
    if (!a || a->frame_count == 0) return;

    const uint32_t now = millis();
    bool advanced = false;
    for (uint16_t skipped = 0; skipped < a->frame_count; skipped++) {
        uint16_t hold = a->holds[cur_frame];
        if (now - frame_started_ms < hold) break;
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms += hold;
        advanced = true;
    }
    if (advanced) render_frame(a->frames[cur_frame], a->palette);
}

void logo_anim_show(logo_screen_t which) {
    active_screen = which;
    if (which == LOGO_SCREEN_CURSOR) {
        active_anim_index = anim_index_named("cursor splash");
    } else {
        active_anim_index = idle_anim_index_for(LOGO_SCREEN_CODEX);
    }
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const logo_anim_def_t* a = current_anim();
    if (a && a->frame_count > 0) render_frame(a->frames[0], a->palette);
    if (which == LOGO_SCREEN_CODEX) {
        Serial.printf("Codex animation: %s\n", a && a->name ? a->name : "unknown");
    }
    if (container) lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

bool logo_anim_next(void) {
    if (!active) return false;

    int start = active_anim_index;
    if (start < 0) start = idle_anim_index_for(active_screen);
    for (int step = 1; step <= LOGO_ANIM_COUNT; step++) {
        int idx = (start + step) % LOGO_ANIM_COUNT;
        if (!is_splash_anim_for(idx, active_screen)) continue;
        active_anim_index = idx;
        cur_frame = 0;
        frame_started_ms = millis();
        last_pick_ms = frame_started_ms;
        const logo_anim_def_t* a = current_anim();
        if (a && a->frame_count > 0) render_frame(a->frames[0], a->palette);
        const char* label =
            (active_screen == LOGO_SCREEN_CODEX) ? "Codex" : "Cursor";
        Serial.printf("%s animation: %s\n", label, a && a->name ? a->name : "unknown");
        return idx != start;
    }
    return false;
}

bool logo_anim_keepalive(void) {
    if (!active) return false;
    last_pick_ms = millis();
    return true;
}

void logo_anim_pick_for_current_rate(void) {
    // v1: only one codex splash animation. Reset to its first frame.
    active_anim_index = idle_anim_index_for(LOGO_SCREEN_CODEX);
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const logo_anim_def_t* a = current_anim();
    if (a && a->frame_count > 0) render_frame(a->frames[0], a->palette);
    Serial.printf("Codex animation: %s\n", a && a->name ? a->name : "unknown");
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
#define LOGO_MINI_MAX 6

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
    if (m->canvas) {
        lv_draw_buf_flush_cache(lv_canvas_get_draw_buf(m->canvas), NULL);
        lv_obj_invalidate(m->canvas);
    }
}

lv_obj_t* logo_mini_create_named(lv_obj_t* parent, const char* anim_name, int px) {
    if (logo_mini_count >= LOGO_MINI_MAX) return NULL;
    int idx = anim_index_named(anim_name);
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

lv_obj_t* logo_mini_create(lv_obj_t* parent, logo_screen_t which, int px) {
    const char* anim_name =
        (which == LOGO_SCREEN_CODEX) ? "codex blink" : "cursor idle";
    return logo_mini_create_named(parent, anim_name, px);
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
