#pragma once
#include <stdint.h>
#include <lvgl.h>

typedef enum {
    LOGO_SCREEN_CODEX = 0,
    LOGO_SCREEN_CURSOR = 1,
    LOGO_SCREEN_COUNT,
} logo_screen_t;

void logo_anim_init(lv_obj_t* parent);
void logo_anim_show(logo_screen_t which);
// Advance within the active service's splash-only animation catalog.
// Returns true when another animation exists for that service.
bool logo_anim_next(void);
bool logo_anim_keepalive(void);
void logo_anim_pick_for_current_rate(void);
void logo_anim_hide(void);
void logo_anim_tick(void);
bool logo_anim_is_active(void);
lv_obj_t* logo_anim_get_root(void);

// Render frame 0 of a logo animation into an RGB565A8 buffer for lv_image.
// buf must hold px*px*3 bytes. Returns false if the animation is missing.
bool logo_build_image_dsc(logo_screen_t which, int px, lv_image_dsc_t* dsc, uint8_t* buf);

// Mini animated logo for embedding elsewhere (selector tiles, idle screens).
// Renders the logo animation at ~px×px inside `parent`; returns the canvas
// (position it with lv_obj_align) or NULL on failure. Drive all minis with
// logo_mini_tick().
lv_obj_t* logo_mini_create(lv_obj_t* parent, logo_screen_t which, int px);
lv_obj_t* logo_mini_create_named(lv_obj_t* parent, const char* anim_name, int px);
void logo_mini_tick(void);
