#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

#include "net.h"
#include "radio_api.h"
#include "json.h"
#include "locale.h"

/* ======================================================================
 * 3DSRadio - Flat Aero UI Design
 * Inspired by ClouDS-Music-FA
 *
 * Design philosophy:
 * - Flat Aero: Nintendo 3DS system settings hierarchy × Apple Music whitespace
 * - Top screen: content/art/visualization (hero)
 * - Bottom screen: navigation/lists/controls (functional)
 * - Dark mode with accent highlights, rounded corners, layered depth
 * - Touch + button dual input
 * ====================================================================== */

/* Screen dimensions */
#define TOP_WIDTH  400
#define TOP_HEIGHT 240
#define BOT_WIDTH  320
#define BOT_HEIGHT 240

/* Maximum items */
#define MAX_VISIBLE_ITEMS 10
#define MAX_STATIONS 100
#define MAX_TAGS 50

/* ======================================================================
 * Flat Aero Color Palette
 * ====================================================================== */

/* Background layers */
#define CLR_BG_TOP      0xFF1C1C2E  /* Deep navy top screen */
#define CLR_BG_BOT      0xFF16162A  /* Slightly deeper bottom */
#define CLR_SURFACE     0xFF2A2A40  /* Card/surface background */
#define CLR_SURFACE_LT  0xFF353550  /* Lighter surface for hover */

/* Text */
#define CLR_TEXT        0xFFF0F0F0  /* Primary text - near white */
#define CLR_TEXT_SEC    0xFFA0A0B8  /* Secondary text - muted */
#define CLR_TEXT_DIM    0xFF686880  /* Dim text - hints */

/* Accent - derived from radio theme */
#define CLR_ACCENT      0xFF5C9EFF  /* Soft blue accent */
#define CLR_ACCENT2     0xFF7C5CFF  /* Purple secondary accent */
#define CLR_ACCENT3     0xFFFF6B6B  /* Warm red accent for indicators */

/* System UI - 3DS beige tones */
#define CLR_SYS_BG      0xFFC8C0B0  /* Classic 3DS beige */
#define CLR_SYS_BTN     0xFFE0D8C8  /* Button surface */
#define CLR_SYS_BORDER  0xFFB0A898  /* Button border */

/* Status */
#define CLR_OK          0xFF4CD964  /* iOS green */
#define CLR_ERR         0xFFFF3B30  /* iOS red */
#define CLR_WARN        0xFFFFCC00  /* iOS yellow */
#define CLR_INFO         0xFF5AC8FA  /* iOS blue */

/* Status bar */
#define CLR_STATUSBAR   0xFF111122  /* Dark status bar */
#define CLR_DIVIDER     0xFF3A3A50  /* Subtle divider */

/* ======================================================================
 * UI State
 * ====================================================================== */

typedef enum {
    SCREEN_MAIN_MENU,
    SCREEN_TAG_LIST,
    SCREEN_STATION_LIST,
    SCREEN_PLAYING,
    SCREEN_SEARCH,
    SCREEN_STATION_INFO,
} AppScreen;

/* Menu items - loaded from locale */
#define MAIN_MENU_COUNT 4

typedef struct {
    AppScreen screen;
    int selection;
    int scroll_offset;
    int prev_screen;
    int prev_selection;

    /* Tag data */
    RadioTag tags[MAX_TAGS];
    int tag_count;

    /* Station data */
    RadioStation stations[MAX_STATIONS];
    int station_count;

    /* Playing state */
    RadioStation *current_station;
    bool is_playing;
    char stream_url[512];
    float volume;
    u32 play_start_tick;

    /* Search */
    char search_query[64];
    int search_cursor;

    /* Status */
    char status_text[128];
    u32 status_color;
    u64 status_time;

    /* UI animation */
    float highlight_alpha;
    int highlight_dir;
    u32 frame_count;
} App;

static App app;

/* Render targets */
static C3D_RenderTarget *top = NULL;
static C3D_RenderTarget *bottom = NULL;

/* ======================================================================
 * Drawing Primitives - Flat Aero Style
 * ====================================================================== */

/* Pre-allocated text buffer for efficiency */
static C2D_TextBuf global_text_buf = NULL;

static void draw_begin_frame(void) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

static void draw_end_frame(void) {
    C3D_FrameEnd(3);
}

static void select_top(void) {
    C2D_SceneBegin(top);
}

static void select_bottom(void) {
    C2D_SceneBegin(bottom);
}

static void clear_top(void) {
    C2D_TargetClear(top, CLR_BG_TOP);
}

static void clear_bottom(void) {
    C2D_TargetClear(bottom, CLR_BG_BOT);
}

/* Rounded rectangle - Flat Aero signature */
static void draw_rounded_rect(float x, float y, float w, float h, float r, u32 color) {
    if (r <= 0) {
        C2D_DrawRectSolid(x, y, 0.5f, w, h, color);
        return;
    }
    /* Core rectangle */
    C2D_DrawRectSolid(x + r, y, 0.5f, w - r * 2, h, color);
    C2D_DrawRectSolid(x, y + r, 0.5f, w, h - r * 2, color);
    /* Corner circles */
    C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, color);
    C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, color);
}

/* Draw a card-style surface */
static void draw_card(float x, float y, float w, float h, bool highlighted) {
    u32 color = highlighted ? CLR_SURFACE_LT : CLR_SURFACE;
    draw_rounded_rect(x, y, w, h, 6.0f, color);
    if (highlighted) {
        /* Glow effect on left edge */
        C2D_DrawRectSolid(x, y + 4, 0.6f, 3.0f, h - 8, CLR_ACCENT);
    }
}

/* Gradient bar (top to bottom) */
static void draw_gradient(float x, float y, float w, float h, u32 top_color, u32 bottom_color) {
    for (int i = 0; i < (int)h; i++) {
        float t = (float)i / h;
        u8 r = (u8)(((top_color >> 24) & 0xFF) * (1-t) + ((bottom_color >> 24) & 0xFF) * t);
        u8 g = (u8)(((top_color >> 16) & 0xFF) * (1-t) + ((bottom_color >> 16) & 0xFF) * t);
        u8 b = (u8)(((top_color >> 8) & 0xFF) * (1-t) + ((bottom_color >> 8) & 0xFF) * t);
        u8 a = (u8)((top_color & 0xFF) * (1-t) + (bottom_color & 0xFF) * t);
        u32 c = (r << 24) | (g << 16) | (b << 8) | a;
        C2D_DrawRectSolid(x, y + i, 0.5f, w, 1.0f, c);
    }
}

/* Draw text using global buffer */
static void draw_label(float x, float y, float size, u32 color, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    C2D_Text c2d_text;
    C2D_TextBufClear(global_text_buf);
    C2D_TextParse(&c2d_text, global_text_buf, buf);
    C2D_TextOptimize(&c2d_text);
    C2D_DrawText(&c2d_text, C2D_WithColor, x, y, 0.5f, size, size, color);
}

/* Status bar at bottom of each screen */
static void draw_status_bar(void) {
    select_top();
    draw_rounded_rect(0, TOP_HEIGHT - 20, TOP_WIDTH, 20, 0, CLR_STATUSBAR);

    /* Time placeholder */
    draw_label(10, TOP_HEIGHT - 18, 0.4f, CLR_TEXT_DIM, "3DSRadio v1.0");

    /* WiFi indicator */
    const char *wifi = net_wifi_status() ? "\x01 Wi-Fi" : "\x02 No Wi-Fi";
    u32 wifi_color = net_wifi_status() ? CLR_OK : CLR_ERR;
    draw_label(TOP_WIDTH - 90, TOP_HEIGHT - 18, 0.4f, wifi_color, wifi);

    /* Status text in center */
    if (strlen(app.status_text) > 0) {
        u64 now = svcGetSystemTick();
        u64 elapsed = (now - app.status_time) / CPU_TICKS_PER_MSEC;
        if (elapsed < 4000) {
            float alpha = 1.0f;
            if (elapsed > 3000) alpha = 1.0f - (float)(elapsed - 3000) / 1000.0f;
            u32 c = app.status_color;
            /* Apply alpha by modifying the alpha channel */
            u8 a = (u8)((c & 0xFF) * alpha);
            c = (c & 0xFFFFFF00) | a;
            draw_label(150, TOP_HEIGHT - 18, 0.4f, c, "%s", app.status_text);
        }
    }
}

/* Top screen hero header */
static void draw_hero_header(const char *title, const char *subtitle) {
    select_top();
    /* Gradient header area */
    draw_gradient(0, 0, TOP_WIDTH, 60, 0x2A2A44FF, 0x1C1C2E00);

    draw_label(20, 10, 0.9f, CLR_TEXT, "%s", title);
    if (subtitle) {
        draw_label(20, 34, 0.45f, CLR_TEXT_SEC, "%s", subtitle);
    }
}

/* ======================================================================
 * Screen: Main Menu
 * ====================================================================== */

static void render_main_menu(void) {
    select_top();
    clear_top();

    /* Hero area */
    draw_gradient(0, 0, TOP_WIDTH, 120, 0x25253DFF, 0x1C1C2E00);

    /* App logo area */
    draw_rounded_rect(TOP_WIDTH/2 - 50, 25, 100, 100, 16, CLR_SURFACE_LT);
    draw_label(TOP_WIDTH/2 - 30, 55, 1.8f, CLR_ACCENT, "R");

    /* Title */
    draw_label(20, 140, 0.7f, CLR_TEXT_SEC, "%s", tr_main_subtitle());
    draw_label(20, 162, 0.5f, CLR_TEXT_DIM, "%s", tr_about_powered());
    draw_label(20, 185, 0.4f, CLR_TEXT_DIM, "Browse thousands of stations worldwide");

    /* Bottom screen: menu */
    select_bottom();
    clear_bottom();

    draw_label(15, 8, 0.55f, CLR_TEXT_SEC, "Menu");

    const char *items[] = {
        tr_menu_browse_genre(), tr_menu_top_stations(),
        tr_menu_search(), tr_menu_about()
    };
    for (int i = 0; i < MAIN_MENU_COUNT; i++) {
        int y = 35 + i * 48;
        bool sel = (i == app.selection);

        draw_card(10, y, BOT_WIDTH - 20, 40, sel);

        if (sel) {
            C2D_DrawRectSolid(10, y, 0.6f, 3, 40, CLR_ACCENT);
        }

        draw_label(22, y + 10, 0.55f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", items[i]);
        draw_label(BOT_WIDTH - 30, y + 10, 0.5f, CLR_TEXT_DIM, ">");
    }

    draw_status_bar();
}

/* ======================================================================
 * Screen: Tag List
 * ====================================================================== */

static void render_tag_list(void) {
    draw_hero_header(tr_genre_header(), tr_genre_subtitle());

    select_bottom();
    clear_bottom();

    draw_label(15, 8, 0.5f, CLR_TEXT_DIM, "%d genres available", app.tag_count);

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.tag_count) end = app.tag_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 28 + idx * 20;
        bool sel = (i == app.selection);

        if (sel) {
            draw_rounded_rect(5, y - 2, BOT_WIDTH - 10, 18, 4, CLR_SURFACE_LT);
            C2D_DrawRectSolid(5, y, 0.6f, 3, 14, CLR_ACCENT);
        }

        char label[128];
        snprintf(label, sizeof(label), "%s", app.tags[i].name);
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", app.tags[i].stationcount);

        draw_label(15, y, 0.45f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", label);
        draw_label(BOT_WIDTH - 50, y, 0.35f, CLR_TEXT_DIM, "%s", count_str);
    }

    /* Hint bar */
    draw_rounded_rect(5, BOT_HEIGHT - 22, BOT_WIDTH - 10, 18, 4, CLR_SURFACE);
    draw_label(12, BOT_HEIGHT - 20, 0.35f, CLR_TEXT_DIM,
               "\x1E \x1F Navigate   A Select   B Back");

    draw_status_bar();
}

/* ======================================================================
 * Screen: Station List
 * ====================================================================== */

static void render_station_list(void) {
    draw_hero_header(tr_stations_header(), tr_stations_found(app.station_count));

    select_bottom();
    clear_bottom();

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.station_count) end = app.station_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 5 + idx * 22;
        bool sel = (i == app.selection);

        if (sel) {
            draw_rounded_rect(3, y, BOT_WIDTH - 6, 20, 4, CLR_SURFACE_LT);
            C2D_DrawRectSolid(3, y, 0.6f, 3, 20, CLR_ACCENT);
        }

        RadioStation *s = &app.stations[i];

        /* Station name */
        char name_buf[36];
        size_t name_len = strlen(s->name);
        if (name_len > 30) {
            memcpy(name_buf, s->name, 28);
            name_buf[28] = '.';
            name_buf[29] = '.';
            name_buf[30] = '.';
            name_buf[31] = '\0';
        } else {
            strcpy(name_buf, s->name);
        }
        draw_label(12, y + 1, 0.4f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", name_buf);

        /* Bitrate + codec badge */
        if (s->bitrate > 0) {
            char badge[16];
            snprintf(badge, sizeof(badge), "%d kbps", s->bitrate);
            draw_label(BOT_WIDTH - 60, y + 1, 0.3f, CLR_ACCENT, "%s", badge);
        }
    }

    draw_rounded_rect(5, BOT_HEIGHT - 22, BOT_WIDTH - 10, 18, 4, CLR_SURFACE);
    draw_label(12, BOT_HEIGHT - 20, 0.35f, CLR_TEXT_DIM,
               "\x1E \x1F Browse   A Play   Y Info   B Back");

    draw_status_bar();
}

/* ======================================================================
 * Screen: Now Playing
 * ====================================================================== */

static void render_playing(void) {
    select_top();
    clear_top();

    if (!app.current_station) return;

    /* Large station name at top */
    draw_label(20, 30, 1.2f, CLR_TEXT, "%s", app.current_station->name);

    /* Tags as chips */
    if (strlen(app.current_station->tags) > 0) {
        char first_tag[32] = {0};
        /* Extract first tag */
        const char *comma = strchr(app.current_station->tags, ',');
        if (comma) {
            size_t len = (size_t)(comma - app.current_station->tags);
            if (len > 20) len = 20;
            memcpy(first_tag, app.current_station->tags, len);
        } else {
            strncpy(first_tag, app.current_station->tags, 20);
        }
        if (strlen(first_tag) > 0) {
            draw_rounded_rect(20, 65, strlen(first_tag) * 6 + 16, 18, 9, CLR_ACCENT);
            draw_label(28, 67, 0.4f, CLR_TEXT, "%s", first_tag);
        }
    }

    /* Station info line */
    char info[128];
    if (strlen(app.current_station->country) > 0 && app.current_station->bitrate > 0) {
        snprintf(info, sizeof(info), "%s  \xb7  %d kbps  \xb7  %s",
                 app.current_station->country, app.current_station->bitrate,
                 app.current_station->codec);
    } else if (strlen(app.current_station->country) > 0) {
        snprintf(info, sizeof(info), "%s", app.current_station->country);
    } else {
        snprintf(info, sizeof(info), "Internet Radio");
    }
    draw_label(20, 95, 0.45f, CLR_TEXT_SEC, "%s", info);

    /* Votes */
    char stats[64];
    snprintf(stats, sizeof(stats), "\x02 %d votes  \xb7  %d clicks",
             app.current_station->votes, app.current_station->clickcount);
    draw_label(20, 118, 0.35f, CLR_TEXT_DIM, "%s", stats);

    /* Visualizer area - animated bars */
    int bar_count = 20;
    int bar_width = 12;
    int gap = 4;
    int total_w = bar_count * (bar_width + gap) - gap;
    int start_x = (TOP_WIDTH - total_w) / 2;
    int base_y = 155;

    for (int i = 0; i < bar_count; i++) {
        /* Pseudo-animated heights using frame count and sin */
        float phase = (float)(i * 3 + app.frame_count * 2);
        float height = 8.0f + sinf(phase * 0.1f) * 15.0f + sinf(phase * 0.05f) * 8.0f;
        if (!app.is_playing) height = 2.0f;

        /* Color gradient across bars */
        float t = (float)i / bar_count;
        u8 r = (u8)((1-t) * 0x5C + t * 0x7C);
        u8 g = (u8)((1-t) * 0x9E + t * 0x5C);
        u8 b = (u8)((1-t) * 0xFF + t * 0xFF);
        u32 bar_color = (r << 24) | (g << 16) | (b << 8) | 0xFF;

        C2D_DrawRectSolid(start_x + i * (bar_width + gap),
                          base_y - height, 0.5f,
                          bar_width, height, bar_color);
    }

    /* Playing/Paused indicator */
    draw_label(20, TOP_HEIGHT - 45, 0.5f,
               app.is_playing ? CLR_OK : CLR_WARN,
               app.is_playing ? "\x01 Now Playing" : "\x02 Paused");

    /* Volume */
    draw_label(TOP_WIDTH - 80, TOP_HEIGHT - 45, 0.35f, CLR_TEXT_DIM,
               "Vol: %.0f%%", app.volume * 100);

    /* Bottom screen: controls */
    select_bottom();
    clear_bottom();

    draw_label(15, 12, 0.5f, CLR_TEXT_SEC, "%s", tr_controls());

    /* Control buttons as cards */
    struct { const char *label; const char *key; u32 color; } controls[] = {
        {tr_play_pause(), "A", CLR_ACCENT},
        {tr_stop_back(), "B", CLR_ACCENT3},
        {tr_vol_down(), "X", CLR_TEXT_DIM},
        {tr_vol_up(), "Y", CLR_ACCENT2},
    };

    for (int i = 0; i < 4; i++) {
        int x = 8 + i * 78;
        int y = 45;
        draw_rounded_rect(x, y, 72, 55, 8, CLR_SURFACE);
        C2D_DrawRectSolid(x, y + 10, 0.6f, 72, 1, CLR_DIVIDER);
        draw_label(x + 10, y + 18, 0.55f, controls[i].color, "%s", controls[i].key);
        draw_label(x + 10, y + 38, 0.35f, CLR_TEXT_DIM, "%s", controls[i].label);
    }

    /* Stream info */
    if (strlen(app.stream_url) > 0) {
        char url_display[48];
        size_t len = strlen(app.stream_url);
        if (len > 40) {
            memcpy(url_display, app.stream_url, 37);
            url_display[37] = '.';
            url_display[38] = '.';
            url_display[39] = '.';
            url_display[40] = '\0';
        } else {
            strcpy(url_display, app.stream_url);
        }
        draw_rounded_rect(8, 120, BOT_WIDTH - 16, 35, 6, CLR_SURFACE);
        draw_label(15, 128, 0.3f, CLR_TEXT_DIM, "%s", tr_stream_url());
        draw_label(15, 140, 0.3f, CLR_ACCENT, "%s", url_display);
    }

    draw_status_bar();
}

/* ======================================================================
 * Screen: Search
 * ====================================================================== */

static void render_search(void) {
    draw_hero_header(tr_search_header(), tr_search_prompt());

    select_bottom();
    clear_bottom();

    /* Search input area */
    draw_rounded_rect(10, 20, BOT_WIDTH - 20, 40, 8, CLR_SURFACE);
    draw_label(20, 28, 0.35f, CLR_TEXT_DIM, "%s", tr_search_prompt());
    draw_label(20, 42, 0.5f, CLR_ACCENT,
               strlen(app.search_query) > 0 ? "%s_" : "Type a station name...",
               app.search_query);

    /* Hint */
    draw_rounded_rect(10, 80, BOT_WIDTH - 20, 55, 8, CLR_SURFACE);
    draw_label(20, 88, 0.35f, CLR_TEXT_DIM, "%s", tr_search_prompt());
    draw_label(20, 102, 0.35f, CLR_TEXT_DIM, "%s", tr_search_action());
    draw_label(20, 116, 0.35f, CLR_TEXT_DIM, "%s", tr_back());

    draw_status_bar();
}

/* ======================================================================
 * Screen: Station Info
 * ====================================================================== */

static void render_station_info(void) {
    if (app.selection < 0 || app.selection >= app.station_count) {
        app.screen = SCREEN_STATION_LIST;
        return;
    }

    RadioStation *s = &app.stations[app.selection];

    select_top();
    clear_top();

    draw_label(20, 20, 0.8f, CLR_TEXT, "%s", tr_station_info());

    /* Info card */
    draw_rounded_rect(10, 50, TOP_WIDTH - 20, 160, 8, CLR_SURFACE);

    int y = 60;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_name());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->name);

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_country());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->country[0] ? s->country : "N/A");

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_codec());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->codec[0] ? s->codec : "N/A");

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_bitrate());
    draw_label(120, y, 0.45f, CLR_TEXT, "%d kbps", s->bitrate);

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_language());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->language[0] ? s->language : "N/A");

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_votes());
    draw_label(120, y, 0.45f, CLR_TEXT, "%d", s->votes);

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_clicks());
    draw_label(120, y, 0.45f, CLR_TEXT, "%d", s->clickcount);

    select_bottom();
    clear_bottom();

    draw_label(15, 12, 0.5f, CLR_TEXT_SEC, "Station Details");

    /* Tags section */
    if (strlen(s->tags) > 0) {
        draw_rounded_rect(8, 40, BOT_WIDTH - 16, 50, 6, CLR_SURFACE);
        draw_label(15, 46, 0.35f, CLR_TEXT_DIM, "%s", tr_tags());
        draw_label(15, 60, 0.4f, CLR_ACCENT, "%s", s->tags);
    }

    draw_rounded_rect(8, BOT_HEIGHT - 40, BOT_WIDTH - 16, 32, 6, CLR_SURFACE);
    draw_label(15, BOT_HEIGHT - 35, 0.4f, CLR_TEXT_DIM, "%s", tr_back());

    draw_status_bar();
}

/* Forward declarations for functions used in handle_input */
static void set_status(const char *fmt, u32 color, ...);
static void load_tags(void);
static void load_top_stations(void);
static void load_stations_by_tag(const char *tag);
static void play_station(int index);

/* ======================================================================
 * Input Handling with Touch Support
 * ====================================================================== */

static void handle_input(void) {
    u32 kDown = hidKeysDown();
    touchPosition touch;
    hidTouchRead(&touch);

    bool touch_active = (kDown & KEY_TOUCH);

    switch (app.screen) {
        case SCREEN_MAIN_MENU: {
            if (kDown & KEY_DOWN)
                app.selection = (app.selection + 1) % MAIN_MENU_COUNT;
            if (kDown & KEY_UP)
                app.selection = (app.selection - 1 + MAIN_MENU_COUNT) % MAIN_MENU_COUNT;

            /* Touch selection for menu items */
            if (touch_active && touch.py >= 35 && touch.py <= 35 + MAIN_MENU_COUNT * 48) {
                int idx = (touch.py - 35) / 48;
                if (idx >= 0 && idx < MAIN_MENU_COUNT) {
                    app.selection = idx;
                    if (touch.px >= 10 && touch.px <= BOT_WIDTH - 10) {
                        /* Act as A press */
                        kDown |= KEY_A;
                    }
                }
            }

            if (kDown & KEY_A) {
                switch (app.selection) {
                    case 0: load_tags(); break;
                    case 1: load_top_stations(); break;
                    case 2:
                        memset(app.search_query, 0, sizeof(app.search_query));
                        app.search_cursor = 0;
                        app.screen = SCREEN_SEARCH;
                        break;
                    case 3:
                        set_status("3DSRadio v1.0 - Flat Aero Design", CLR_INFO);
                        break;
                }
            }
            break;
        }

        case SCREEN_TAG_LIST: {
            if (kDown & KEY_DOWN) {
                if (app.selection < app.tag_count - 1) {
                    app.selection++;
                    if (app.selection >= app.scroll_offset + MAX_VISIBLE_ITEMS)
                        app.scroll_offset++;
                }
            }
            if (kDown & KEY_UP) {
                if (app.selection > 0) {
                    app.selection--;
                    if (app.selection < app.scroll_offset)
                        app.scroll_offset--;
                }
            }
            /* Touch */
            if (touch_active && touch.py >= 28) {
                int idx = (touch.py - 28) / 20 + app.scroll_offset;
                if (idx >= 0 && idx < app.tag_count) {
                    app.selection = idx;
                    if (touch.px >= 5 && touch.px <= BOT_WIDTH - 5)
                        kDown |= KEY_A;
                }
            }
            if (kDown & KEY_A)
                load_stations_by_tag(app.tags[app.selection].name);
            if (kDown & KEY_B) {
                app.screen = SCREEN_MAIN_MENU;
                app.selection = 0;
            }
            break;
        }

        case SCREEN_STATION_LIST: {
            if (kDown & KEY_DOWN) {
                if (app.selection < app.station_count - 1) {
                    app.selection++;
                    if (app.selection >= app.scroll_offset + MAX_VISIBLE_ITEMS)
                        app.scroll_offset++;
                }
            }
            if (kDown & KEY_UP) {
                if (app.selection > 0) {
                    app.selection--;
                    if (app.selection < app.scroll_offset)
                        app.scroll_offset--;
                }
            }
            /* Touch */
            if (touch_active && touch.py >= 5) {
                int idx = (touch.py - 5) / 22 + app.scroll_offset;
                if (idx >= 0 && idx < app.station_count) {
                    app.selection = idx;
                    if (touch.px >= 3 && touch.px <= BOT_WIDTH - 3)
                        kDown |= KEY_A;
                }
            }
            if (kDown & KEY_A)
                play_station(app.selection);
            if (kDown & KEY_B) {
                app.screen = SCREEN_TAG_LIST;
                app.selection = 0;
                app.scroll_offset = 0;
            }
            if (kDown & KEY_Y) {
                app.screen = SCREEN_STATION_INFO;
            }
            break;
        }

        case SCREEN_PLAYING: {
            if (kDown & KEY_A) {
                app.is_playing = !app.is_playing;
                set_status(app.is_playing ? "Playing" : "Paused",
                          app.is_playing ? CLR_OK : CLR_WARN);
            }
            if (kDown & KEY_B) {
                app.is_playing = false;
                app.current_station = NULL;
                memset(app.stream_url, 0, sizeof(app.stream_url));
                app.screen = SCREEN_STATION_LIST;
            }
            if (kDown & KEY_X) {
                app.volume = fmax(0.0f, app.volume - 0.1f);
                set_status("Volume: %.0f%%", CLR_INFO, app.volume * 100);
            }
            if (kDown & KEY_Y) {
                app.volume = fmin(1.0f, app.volume + 0.1f);
                set_status("Volume: %.0f%%", CLR_INFO, app.volume * 100);
            }
            /* Touch on control cards */
            if (touch_active && touch.py >= 45 && touch.py <= 100) {
                int idx = (touch.px - 8) / 78;
                if (idx >= 0 && idx < 4) {
                    switch (idx) {
                        case 0: app.is_playing = !app.is_playing; break;
                        case 1:
                            app.is_playing = false;
                            app.current_station = NULL;
                            memset(app.stream_url, 0, sizeof(app.stream_url));
                            app.screen = SCREEN_STATION_LIST;
                            break;
                        case 2: app.volume = fmax(0.0f, app.volume - 0.1f); break;
                        case 3: app.volume = fmin(1.0f, app.volume + 0.1f); break;
                    }
                }
            }
            break;
        }

        case SCREEN_SEARCH: {
            if (kDown & KEY_A) {
                if (strlen(app.search_query) > 0) {
                    set_status("Searching...", CLR_INFO);
                    char error[128];
                    int count = radio_search_by_name(app.search_query, app.stations,
                                                       MAX_STATIONS, error, sizeof(error));
                    if (count > 0) {
                        app.station_count = count;
                        app.selection = 0;
                        app.scroll_offset = 0;
                        app.screen = SCREEN_STATION_LIST;
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Found %d stations", count);
                        set_status(msg, CLR_OK);
                    } else {
                        set_status("No stations found", CLR_WARN);
                    }
                }
            }
            if (kDown & KEY_B) {
                app.screen = SCREEN_MAIN_MENU;
                app.selection = 0;
            }
            break;
        }

        case SCREEN_STATION_INFO: {
            if (kDown & KEY_B) {
                app.screen = SCREEN_STATION_LIST;
            }
            break;
        }
    }
}

/* ======================================================================
 * Application Logic
 * ====================================================================== */

static void set_status(const char *fmt, u32 color, ...) {
    va_list args;
    va_start(args, color);
    vsnprintf(app.status_text, sizeof(app.status_text), fmt, args);
    va_end(args);
    app.status_color = color;
    app.status_time = svcGetSystemTick();
}

static void load_tags(void) {
    if (!net_wifi_status()) {
        set_status("WiFi not connected - check emulator network settings", CLR_ERR);
        return;
    }
    set_status("Loading genres...", CLR_INFO);

    char error[128];
    int count = radio_fetch_tags(app.tags, MAX_TAGS, error, sizeof(error));

    if (count > 0) {
        app.tag_count = count;
        app.selection = 0;
        app.scroll_offset = 0;
        app.screen = SCREEN_TAG_LIST;
        set_status("%d genres loaded", CLR_OK, count);
    } else {
        set_status("Failed: %s", CLR_ERR, error[0] ? error : "Network error");
    }
}

static void load_top_stations(void) {
    if (!net_wifi_status()) {
        set_status("WiFi not connected - check emulator network settings", CLR_ERR);
        return;
    }
    set_status("Loading top stations...", CLR_INFO);

    char error[128];
    int count = radio_fetch_topclick(app.stations, MAX_STATIONS, error, sizeof(error));

    if (count > 0) {
        app.station_count = count;
        app.selection = 0;
        app.scroll_offset = 0;
        app.screen = SCREEN_STATION_LIST;
        set_status("Top stations loaded", CLR_OK);
    } else {
        set_status("Failed: %s", CLR_ERR, error[0] ? error : "Network error");
    }
}

static void load_stations_by_tag(const char *tag) {
    if (!net_wifi_status()) {
        set_status("WiFi not connected - check emulator network settings", CLR_ERR);
        return;
    }
    set_status("Loading stations...", CLR_INFO);

    char error[128];
    int count = radio_fetch_by_tag(tag, app.stations, MAX_STATIONS, error, sizeof(error));

    if (count > 0) {
        app.station_count = count;
        app.selection = 0;
        app.scroll_offset = 0;
        app.screen = SCREEN_STATION_LIST;
        set_status("Found %d stations", CLR_OK, count);
    } else {
        set_status("Failed: %s", CLR_ERR, error[0] ? error : "No stations found");
    }
}

static void play_station(int index) {
    if (index < 0 || index >= app.station_count) return;

    app.current_station = &app.stations[index];
    app.is_playing = false;

    set_status("Connecting to stream...", CLR_INFO);

    char error[128];
    int ret = radio_get_stream_url(app.current_station->stationuuid,
                                    app.stream_url, sizeof(app.stream_url),
                                    error, sizeof(error));

    if (ret == NET_OK && strlen(app.stream_url) > 0) {
        app.is_playing = true;
        app.play_start_tick = svcGetSystemTick();
        app.screen = SCREEN_PLAYING;
        set_status("Streaming", CLR_OK);
    } else {
        if (strlen(app.current_station->url_resolved) > 0) {
            strncpy(app.stream_url, app.current_station->url_resolved, sizeof(app.stream_url) - 1);
            app.is_playing = true;
            app.screen = SCREEN_PLAYING;
            set_status("Streaming (direct)", CLR_OK);
        } else if (strlen(app.current_station->url) > 0) {
            strncpy(app.stream_url, app.current_station->url, sizeof(app.stream_url) - 1);
            app.is_playing = true;
            app.screen = SCREEN_PLAYING;
            set_status("Streaming (direct)", CLR_OK);
        } else {
            set_status("Failed to get stream URL", CLR_ERR);
        }
    }
}

/* ======================================================================
 * Main Entry Point
 * ====================================================================== */

int main(void) {
    /* Initialize */
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* Global text buffer */
    global_text_buf = C2D_TextBufNew(4096);

    /* Initialize networking */
    net_init();
    radio_init();

    /* Set Chinese language and try to load Chinese font */
    locale_set_language(LANG_ZH_CN);
    locale_init_fonts();

    /* App state */
    memset(&app, 0, sizeof(app));
    app.screen = SCREEN_MAIN_MENU;
    app.volume = 0.8f;
    app.highlight_alpha = 0.3f;
    app.highlight_dir = 1;

    set_status("Welcome to 3DSRadio", CLR_INFO);

    /* Main loop */
    while (aptMainLoop()) {
        hidScanInput();
        handle_input();

        app.frame_count++;

        /* Render */
        draw_begin_frame();

        switch (app.screen) {
            case SCREEN_MAIN_MENU:    render_main_menu(); break;
            case SCREEN_TAG_LIST:     render_tag_list(); break;
            case SCREEN_STATION_LIST: render_station_list(); break;
            case SCREEN_PLAYING:      render_playing(); break;
            case SCREEN_SEARCH:       render_search(); break;
            case SCREEN_STATION_INFO: render_station_info(); break;
        }

        draw_end_frame();
    }

    /* Cleanup */
    radio_exit();
    net_exit();
    C2D_TextBufDelete(global_text_buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return 0;
}
