#include "ui.h"
#include "splash.h"
#include "logo_anim.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdint.h>
#include <string.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"

LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

struct UsageScreen {
    lv_obj_t* container;
    lv_obj_t* lbl_title;
    lv_obj_t* usage_group;
    lv_obj_t* pair_group;
    lv_obj_t* idle_group;
    lv_obj_t* bar_session;
    lv_obj_t* lbl_session_pct;
    lv_obj_t* lbl_session_label;
    lv_obj_t* lbl_session_reset;
    lv_obj_t* bar_weekly;
    lv_obj_t* lbl_weekly_pct;
    lv_obj_t* lbl_weekly_label;
    lv_obj_t* lbl_weekly_reset;
    lv_obj_t* lbl_anim;
    int view_state;
};

static UsageScreen usage_screens[3];
static lv_obj_t* battery_img;
static lv_obj_t* header_imgs[3];
static lv_image_dsc_t battery_dscs[5];
static lv_image_dsc_t logo_dsc;
static lv_image_dsc_t codex_header_dsc;
static lv_image_dsc_t cursor_header_dsc;
static uint8_t codex_header_buf[80 * 80 * 3];
static uint8_t cursor_header_buf[80 * 80 * 3];

struct SelectorScreen {
    lv_obj_t* container;
    lv_obj_t* tiles[3];
};
static SelectorScreen selector = {};

static uint32_t last_data_ms = 0;
static bool data_received = false;
static const uint32_t DATA_FRESH_MS = 90000;

static screen_t current_screen = SCREEN_SELECTOR;
static bool s_ble_connected = false;
static uint32_t connected_at_ms = 0;

static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS 4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages_claude[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_CLAUDE_COUNT (sizeof(anim_messages_claude) / sizeof(anim_messages_claude[0]))

static const char* SERVICE_TITLES[] = { "Claude", "Codex", "Cursor" };

static void global_click_cb(lv_event_t* e);
static void selector_tile_click_cb(lv_event_t* e);
static void apply_battery_visibility(void);
static void apply_header_visibility(screen_t screen);
static UsageScreen* usage_for_screen(screen_t s);
static int service_index_for_screen(screen_t s);
static bool is_usage_screen(screen_t s);
static bool is_splash_anim_screen(screen_t s);
static screen_t splash_for_usage(screen_t usage);
static screen_t usage_for_splash(screen_t splash);
static void update_view_state(UsageScreen* us);
static void populate_usage_panel(UsageScreen* us, float session_pct, int session_reset,
                                 float weekly_pct, int weekly_reset, bool ok);

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

static void build_pair_group(UsageScreen* us) {
    us->pair_group = lv_obj_create(us->container);
    lv_obj_set_size(us->pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(us->pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(us->pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(us->pair_group, 0, 0);
    lv_obj_set_style_pad_all(us->pair_group, 0, 0);
    lv_obj_clear_flag(us->pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(us->pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(us->pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(us->pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(us->pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(us->pair_group, LV_OBJ_FLAG_HIDDEN);
}

static void selector_bob_cb(void* var, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t*)var, v, 0);
}

// Gentle staggered float so the three logos feel alive as a group.
static void start_selector_bob(lv_obj_t* icon, uint32_t delay_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, icon);
    lv_anim_set_exec_cb(&a, selector_bob_cb);
    lv_anim_set_values(&a, -5, 5);
    lv_anim_set_duration(&a, 1700);
    lv_anim_set_playback_duration(&a, 1700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_start(&a);
}

static lv_obj_t* make_selector_zone(lv_obj_t* parent, int y, int h, screen_t target) {
    lv_obj_t* zone = lv_obj_create(parent);
    lv_obj_set_pos(zone, 0, y);
    lv_obj_set_size(zone, L.scr_w, h);
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zone, 0, 0);
    lv_obj_set_style_pad_all(zone, 0, 0);
    lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, selector_tile_click_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)target);
    return zone;
}

static void build_selector_screen(lv_obj_t* scr) {
    memset(&selector, 0, sizeof(selector));

    selector.container = lv_obj_create(scr);
    lv_obj_set_size(selector.container, L.scr_w, L.scr_h);
    lv_obj_set_pos(selector.container, 0, 0);
    lv_obj_set_style_bg_opa(selector.container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(selector.container, 0, 0);
    lv_obj_set_style_pad_all(selector.container, 0, 0);
    lv_obj_clear_flag(selector.container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(selector.container);
    lv_label_set_text(title, "Tokenmeter");
    lv_obj_set_style_text_font(title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.title_y);

    // Three icon-only zones: full-width hit areas, animated pixel logos centered.
    const int top = L.title_y + lv_font_get_line_height(&font_tiempos_34) + 12;
    const int bottom = L.margin;
    const int available_h = L.scr_h - top - bottom;
    const int zone_h = available_h / 3;
    const int y0 = top + (available_h - zone_h * 3) / 2;
    const screen_t targets[3] = { SCREEN_SPLASH, SCREEN_SPLASH_CODEX, SCREEN_SPLASH_CURSOR };

    for (int i = 0; i < 3; i++) {
        lv_obj_t* zone = make_selector_zone(selector.container,
                                            y0 + i * zone_h, zone_h,
                                            targets[i]);
        selector.tiles[i] = zone;

        lv_obj_t* icon;
        if (i == 0) {
            icon = splash_mini_create(zone, "idle look around", 100);
        } else {
            icon = logo_mini_create(zone, i == 1 ? LOGO_SCREEN_CODEX : LOGO_SCREEN_CURSOR, 100);
        }
        if (icon) {
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
            lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
            start_selector_bob(icon, (uint32_t)i * 280);
        }
    }

    lv_obj_add_flag(selector.container, LV_OBJ_FLAG_HIDDEN);
}

static void build_idle_group(UsageScreen* us, int svc) {
    us->idle_group = lv_obj_create(us->container);
    lv_obj_set_size(us->idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(us->idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(us->idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(us->idle_group, 0, 0);
    lv_obj_set_style_pad_all(us->idle_group, 0, 0);
    lv_obj_clear_flag(us->idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(us->idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Brand-correct idle visual: Clawd only on the Claude screen.
    lv_obj_t* creature;
    if (svc == 0) {
        creature = splash_mini_create(us->idle_group, "expression sleep", 160);
    } else {
        creature = logo_mini_create(us->idle_group,
                                    svc == 1 ? LOGO_SCREEN_CODEX : LOGO_SCREEN_CURSOR, 160);
    }
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(us->idle_group, LV_OBJ_FLAG_HIDDEN);
}

static void init_service_usage_screen(lv_obj_t* scr, UsageScreen* us, const char* title, int svc) {
    memset(us, 0, sizeof(*us));
    us->view_state = -1;

    // Codex/Cursor drop the bottom status line, so push the panels down a bit
    // to give the 80px header logo clearance above the usage meters.
    int panel_y = L.content_y + (svc == 0 ? 0 : 14);

    us->container = lv_obj_create(scr);
    lv_obj_set_size(us->container, L.scr_w, L.scr_h);
    lv_obj_set_pos(us->container, 0, 0);
    lv_obj_set_style_bg_opa(us->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(us->container, 0, 0);
    lv_obj_set_style_pad_all(us->container, 0, 0);
    lv_obj_clear_flag(us->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(us->container, global_click_cb, LV_EVENT_CLICKED, NULL);

    us->lbl_title = lv_label_create(us->container);
    lv_label_set_text(us->lbl_title, title);
    lv_obj_set_style_text_font(us->lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(us->lbl_title, COL_TEXT, 0);
    lv_obj_align(us->lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    us->usage_group = lv_obj_create(us->container);
    lv_obj_set_size(us->usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(us->usage_group, 0, 0);
    lv_obj_set_style_bg_opa(us->usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(us->usage_group, 0, 0);
    lv_obj_set_style_pad_all(us->usage_group, 0, 0);
    lv_obj_clear_flag(us->usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(us->usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    make_usage_panel(us->usage_group, panel_y, "Current",
                     &us->lbl_session_pct, &us->lbl_session_label,
                     &us->bar_session, &us->lbl_session_reset);
    make_usage_panel(us->usage_group,
                     panel_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &us->lbl_weekly_pct, &us->lbl_weekly_label,
                     &us->bar_weekly, &us->lbl_weekly_reset);

    build_pair_group(us);
    build_idle_group(us, svc);

    // The whimsical spinner line is Claude Code branding — Claude screen only.
    if (svc == 0) {
        us->lbl_anim = lv_label_create(us->container);
        lv_label_set_text(us->lbl_anim, "");
        lv_obj_set_style_text_font(us->lbl_anim, &font_mono_32, 0);
        lv_obj_set_style_text_color(us->lbl_anim, COL_ACCENT, 0);
        lv_obj_align(us->lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
    }

    lv_obj_add_flag(us->container, LV_OBJ_FLAG_HIDDEN);
}

static bool is_usage_screen(screen_t s) {
    return s == SCREEN_USAGE_CLAUDE || s == SCREEN_USAGE_CODEX || s == SCREEN_USAGE_CURSOR;
}

static bool is_splash_anim_screen(screen_t s) {
    return s == SCREEN_SPLASH || s == SCREEN_SPLASH_CODEX || s == SCREEN_SPLASH_CURSOR;
}

static UsageScreen* usage_for_screen(screen_t s) {
    switch (s) {
    case SCREEN_USAGE_CLAUDE: return &usage_screens[0];
    case SCREEN_USAGE_CODEX:  return &usage_screens[1];
    case SCREEN_USAGE_CURSOR: return &usage_screens[2];
    default: return NULL;
    }
}

static int service_index_for_screen(screen_t s) {
    switch (s) {
    case SCREEN_USAGE_CLAUDE: return 0;
    case SCREEN_USAGE_CODEX:  return 1;
    case SCREEN_USAGE_CURSOR: return 2;
    default: return 0;
    }
}

static screen_t splash_for_usage(screen_t usage) {
    switch (usage) {
    case SCREEN_USAGE_CLAUDE: return SCREEN_SPLASH;
    case SCREEN_USAGE_CODEX:  return SCREEN_SPLASH_CODEX;
    case SCREEN_USAGE_CURSOR: return SCREEN_SPLASH_CURSOR;
    default: return SCREEN_SELECTOR;
    }
}

static screen_t usage_for_splash(screen_t splash) {
    switch (splash) {
    case SCREEN_SPLASH:        return SCREEN_USAGE_CLAUDE;
    case SCREEN_SPLASH_CODEX:  return SCREEN_USAGE_CODEX;
    case SCREEN_SPLASH_CURSOR: return SCREEN_USAGE_CURSOR;
    default: return SCREEN_SELECTOR;
    }
}

static void update_view_state(UsageScreen* us) {
    if (!us || !us->usage_group || !us->pair_group || !us->idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;
    } else {
        v = 1;
    }
    if (v == us->view_state) return;
    us->view_state = v;
    lv_obj_add_flag(us->pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(us->idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(us->usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? us->pair_group : v == 1 ? us->idle_group : us->usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

static void populate_usage_panel(UsageScreen* us, float session_pct, int session_reset,
                                 float weekly_pct, int weekly_reset, bool ok) {
    if (!ok) {
        lv_label_set_text(us->lbl_session_pct, "---%");
        lv_bar_set_value(us->bar_session, 0, LV_ANIM_OFF);
        lv_label_set_text(us->lbl_session_reset, "---");
        lv_label_set_text(us->lbl_weekly_pct, "---%");
        lv_bar_set_value(us->bar_weekly, 0, LV_ANIM_OFF);
        lv_label_set_text(us->lbl_weekly_reset, "---");
        return;
    }

    int s_pct = (int)(session_pct + 0.5f);
    lv_label_set_text_fmt(us->lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(us->bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(us->bar_session, pct_color(session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(session_reset, buf, sizeof(buf));
    lv_label_set_text(us->lbl_session_reset, buf);

    int w_pct = (int)(weekly_pct + 0.5f);
    lv_label_set_text_fmt(us->lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(us->bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(us->bar_weekly, pct_color(weekly_pct), LV_PART_INDICATOR);

    format_reset_time(weekly_reset, buf, sizeof(buf));
    lv_label_set_text(us->lbl_weekly_reset, buf);
}

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    logo_build_image_dsc(LOGO_SCREEN_CODEX, 80, &codex_header_dsc, codex_header_buf);
    logo_build_image_dsc(LOGO_SCREEN_CURSOR, 80, &cursor_header_dsc, cursor_header_buf);
    init_battery_icons();

    build_selector_screen(scr);

    for (int i = 0; i < 3; i++) {
        init_service_usage_screen(scr, &usage_screens[i], SERVICE_TITLES[i], i);
    }

    splash_init(scr);
    logo_anim_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }
    if (logo_anim_get_root()) {
        lv_obj_add_event_cb(logo_anim_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    const lv_image_dsc_t* header_srcs[] = { &logo_dsc, &codex_header_dsc, &cursor_header_dsc };
    for (int i = 0; i < 3; i++) {
        header_imgs[i] = lv_image_create(scr);
        lv_image_set_src(header_imgs[i], header_srcs[i]);
        lv_obj_set_pos(header_imgs[i], L.margin, L.title_y - 10);
        lv_obj_add_flag(header_imgs[i], LV_OBJ_FLAG_HIDDEN);
    }

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms = lv_tick_get();
    data_received = true;

    populate_usage_panel(&usage_screens[0], data->session_pct, data->session_reset_mins,
                         data->weekly_pct, data->weekly_reset_mins, data->ok);

    if (data->has_svc) {
        populate_usage_panel(&usage_screens[1], data->cx.session_pct, data->cx.session_reset_mins,
                             data->cx.weekly_pct, data->cx.weekly_reset_mins, data->cx.ok);
        populate_usage_panel(&usage_screens[2], data->cu.session_pct, data->cu.session_reset_mins,
                             data->cu.weekly_pct, data->cu.weekly_reset_mins, data->cu.ok);
    } else {
        populate_usage_panel(&usage_screens[1], 0, -1, 0, -1, false);
        populate_usage_panel(&usage_screens[2], 0, -1, 0, -1, false);
    }

    for (int i = 0; i < 3; i++) update_view_state(&usage_screens[i]);
}

static void tick_usage_anim(UsageScreen* us) {
    if (!us) return;
    update_view_state(us);
    if (us->view_state == 1) {
        splash_mini_tick();
        logo_mini_tick();
    }

    if (!us->lbl_anim) return;  // Codex/Cursor have no status line

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_CLAUDE_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";
    } else if (us->view_state == 1) {
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages_claude[anim_msg_idx % ANIM_MSG_CLAUDE_COUNT];
    }

    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(us->lbl_anim, buf);
}

void ui_tick_anim(void) {
    if (is_usage_screen(current_screen)) {
        tick_usage_anim(usage_for_screen(current_screen));
    }
    if (current_screen == SCREEN_SPLASH_CODEX || current_screen == SCREEN_SPLASH_CURSOR) {
        logo_anim_tick();
    }
    if (current_screen == SCREEN_SELECTOR) {
        splash_mini_tick();
        logo_mini_tick();
    }
}

static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SELECTOR || is_splash_anim_screen(current_screen)) {
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_header_visibility(screen_t screen) {
    for (int i = 0; i < 3; i++) {
        if (!header_imgs[i]) continue;
        lv_obj_add_flag(header_imgs[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!is_usage_screen(screen)) return;
    int idx = service_index_for_screen(screen);
    if (header_imgs[idx]) lv_obj_clear_flag(header_imgs[idx], LV_OBJ_FLAG_HIDDEN);
}

static void selector_tile_click_cb(lv_event_t* e) {
    screen_t target = (screen_t)(intptr_t)lv_event_get_user_data(e);
    ui_show_screen(target);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (is_splash_anim_screen(current_screen)) {
        ui_show_screen(usage_for_splash(current_screen));
    } else if (is_usage_screen(current_screen)) {
        ui_show_screen(SCREEN_SELECTOR);
    }
}

void ui_show_screen(screen_t screen) {
    splash_hide();
    logo_anim_hide();
    if (selector.container) lv_obj_add_flag(selector.container, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        lv_obj_add_flag(usage_screens[i].container, LV_OBJ_FLAG_HIDDEN);
    }

    switch (screen) {
    case SCREEN_SELECTOR:
        if (selector.container) lv_obj_clear_flag(selector.container, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_SPLASH:
        splash_show();
        break;
    case SCREEN_SPLASH_CODEX:
        logo_anim_show(LOGO_SCREEN_CODEX);
        break;
    case SCREEN_SPLASH_CURSOR:
        logo_anim_show(LOGO_SCREEN_CURSOR);
        break;
    case SCREEN_USAGE_CLAUDE:
    case SCREEN_USAGE_CODEX:
    case SCREEN_USAGE_CURSOR: {
        UsageScreen* us = usage_for_screen(screen);
        if (us) lv_obj_clear_flag(us->container, LV_OBJ_FLAG_HIDDEN);
        break;
    }
    default:
        break;
    }

    apply_header_visibility(screen);
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (is_usage_screen(current_screen)) {
        ui_show_screen(splash_for_usage(current_screen));
    } else if (is_splash_anim_screen(current_screen)) {
        ui_show_screen(usage_for_splash(current_screen));
    } else {
        ui_show_screen(SCREEN_SELECTOR);
    }
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name;
    (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    for (int i = 0; i < 3; i++) update_view_state(&usage_screens[i]);
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
