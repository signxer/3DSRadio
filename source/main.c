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
#include "stream_player.h"
#include "ui_skin.h"

/* ======================================================================
 * 3DSRadio - Flat Aero UI Design
 * Skin-based rendering powered by ClouDS-Music-FA texture atlas
 *
 * Design philosophy:
 * - Texture atlas (ui-skin-dark.png) for all UI chrome
 * - Nine-slice scaling for buttons, panels, headers, selections
 * - Top screen: content/art/visualization (hero)
 * - Bottom screen: navigation/lists/controls (functional)
 * - Dark theme with skin-based depth and layered rendering
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

/* Background layers - deep dark for contrast with skin chrome */
#define CLR_BG_TOP      0xFF1C1C2E  /* Deep navy top screen */
#define CLR_BG_BOT      0xFF16162A  /* Slightly deeper bottom */
#define CLR_SURFACE     0xFF2A2A40  /* Card/surface background (fallback) */
#define CLR_SURFACE_LT  0xFF353550  /* Lighter surface for hover (fallback) */

/* Text */
#define CLR_TEXT        0xFFF0F0F0  /* Primary text - near white */
#define CLR_TEXT_SEC    0xFFA0A0B8  /* Secondary text - muted */
#define CLR_TEXT_DIM    0xFF686880  /* Dim text - hints */

/* Accent colors */
#define CLR_ACCENT      0xFF5C9EFF  /* Soft blue accent */
#define CLR_ACCENT2     0xFF7C5CFF  /* Purple secondary accent */
#define CLR_ACCENT3     0xFFFF6B6B  /* Warm red accent */

/* Status */
#define CLR_OK          0xFF4CD964  /* Green */
#define CLR_ERR         0xFFFF3B30  /* Red */
#define CLR_WARN        0xFFFFCC00  /* Yellow */
#define CLR_INFO        0xFF5AC8FA  /* Blue */

/* Status bar */
#define CLR_STATUSBAR   0xFF111122  /* Dark status bar */

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

/* Menu items */
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
    StreamPlayer *stream_player;
    float volume;
    u32 play_start_tick;
    StreamBufSize buffer_size;  /* Audio buffer preset */

    /* Search */
    char search_query[64];
    int search_cursor;

    /* Status */
    char status_text[128];
    u32 status_color;
    u64 status_time;

    /* UI animation */
    u32 frame_count;
} App;

static App app;

/* Render targets */
static C3D_RenderTarget *top = NULL;
static C3D_RenderTarget *bottom = NULL;

/* Skin */
static UiSkin skin;

/* ======================================================================
 * Drawing Primitives - Skin-based Flat Aero Style
 * ====================================================================== */

/* Pre-allocated text buffer for efficiency */
static C2D_TextBuf global_text_buf = NULL;
static C2D_Font active_font = NULL;

/* Corner radius helpers matching ClouDS-Music-FA aero style */
static float aero_corner(float h) {
    if (h >= 56.0f) return 13.0f;
    if (h >= 42.0f) return 11.0f;
    if (h >= 32.0f) return 8.0f;
    return 6.0f;
}

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

/* Draw a panel using 9-slice skin, with solid-color fallback */
static void draw_panel(float x, float y, float w, float h) {
    float corner = aero_corner(h);
    bool ok = ui_skin_draw_nine_slice(&skin, UI_SKIN_PANEL,
        x, y, 0.5f, w, h, 28U, corner);
    if (!ok) {
        /* Fallback: plain rounded rect */
        if (corner > 0) {
            C2D_DrawRectSolid(x + corner, y, 0.5f, w - corner * 2, h, CLR_SURFACE);
            C2D_DrawRectSolid(x, y + corner, 0.5f, w, h - corner * 2, CLR_SURFACE);
            C2D_DrawCircleSolid(x + corner, y + corner, 0.5f, corner, CLR_SURFACE);
            C2D_DrawCircleSolid(x + w - corner, y + corner, 0.5f, corner, CLR_SURFACE);
            C2D_DrawCircleSolid(x + corner, y + h - corner, 0.5f, corner, CLR_SURFACE);
            C2D_DrawCircleSolid(x + w - corner, y + h - corner, 0.5f, corner, CLR_SURFACE);
        } else {
            C2D_DrawRectSolid(x, y, 0.5f, w, h, CLR_SURFACE);
        }
    }
}

/* Draw a button using 9-slice skin, with highlight state */
static void draw_button(float x, float y, float w, float h, bool active) {
    float corner = aero_corner(h);
    UiSkinAsset asset = active ? UI_SKIN_BUTTON_ACTIVE : UI_SKIN_BUTTON;
    bool ok = ui_skin_draw_nine_slice(&skin, asset,
        x, y, 0.5f, w, h, 20U, corner);
    if (!ok) {
        u32 color = active ? CLR_SURFACE_LT : CLR_SURFACE;
        if (corner > 0) {
            C2D_DrawRectSolid(x + corner, y, 0.5f, w - corner * 2, h, color);
            C2D_DrawRectSolid(x, y + corner, 0.5f, w, h - corner * 2, color);
            C2D_DrawCircleSolid(x + corner, y + corner, 0.5f, corner, color);
            C2D_DrawCircleSolid(x + w - corner, y + corner, 0.5f, corner, color);
            C2D_DrawCircleSolid(x + corner, y + h - corner, 0.5f, corner, color);
            C2D_DrawCircleSolid(x + w - corner, y + h - corner, 0.5f, corner, color);
        } else {
            C2D_DrawRectSolid(x, y, 0.5f, w, h, color);
        }
    }
    /* Active glow layer */
    if (active) {
        ui_skin_draw_nine_slice_tinted_alpha(&skin, UI_SKIN_BUTTON_PRESSED,
            x, y, 0.5f, w, h, 20U, corner,
            CLR_ACCENT, 0.78f, 0.6f);
    }
}

/* Draw a selection row highlight */
static void draw_selection(float x, float y, float w, float h) {
    float corner = aero_corner(h);
    bool ok = ui_skin_draw_nine_slice(&skin, UI_SKIN_SELECTION,
        x, y, 0.5f, w, h, 16U, corner);
    if (!ok) {
        C2D_DrawRectSolid(x, y, 0.5f, w, h, CLR_SURFACE_LT);
    }
    /* Left edge accent bar */
    C2D_DrawRectSolid(x, y + 2, 0.5f, 3.0f, h - 4, CLR_ACCENT);
}

/* Draw a header/title bar */
static void draw_header(float x, float y, float w, float h) {
    float corner = aero_corner(h);
    ui_skin_draw_nine_slice(&skin, UI_SKIN_HEADER,
        x, y, 0.5f, w, h, 12U, corner);
}

/* Gradient bar (top to bottom) - no skin equivalent, keep raw */
static void draw_gradient(float x, float y, float w, float h,
                          u32 top_color, u32 bottom_color) {
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

/* Draw text using global buffer with active font */
static void draw_label(float x, float y, float size, u32 color,
                       const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    C2D_Text c2d_text;
    C2D_TextBufClear(global_text_buf);

    if (active_font) {
        C2D_TextFontParse(&c2d_text, active_font, global_text_buf, buf);
    } else {
        C2D_TextParse(&c2d_text, global_text_buf, buf);
    }

    C2D_TextOptimize(&c2d_text);
    /* All UI elements share z=0.5f. PICA200 GPU uses GPU_GEQUAL depth
     * test — LARGER z passes, smaller z is rejected. Render order
     * (bg → glow → accent → text) controls layering. Never use
     * z != 0.5f without checking depth test direction. */
    C2D_DrawText(&c2d_text, C2D_WithColor, x, y, 0.5f, size, size, color);
}

/* Status bar at bottom of top screen */
static void draw_status_bar(void) {
    select_top();
    ui_skin_draw_nine_slice(&skin, UI_SKIN_FOOTER,
        0, TOP_HEIGHT - 20, 0.5f, TOP_WIDTH, 20, 8U, 4.0f);
    if (!skin.ready) {
        C2D_DrawRectSolid(0, TOP_HEIGHT - 20, 0.5f, TOP_WIDTH, 20, CLR_STATUSBAR);
    }

    draw_label(10, TOP_HEIGHT - 18, 0.4f, CLR_TEXT_DIM, "%s", tr_about_title());
    /* Buffer size indicator */
    const char *buf_names[] = {tr_buffer_small(), tr_buffer_medium(), tr_buffer_large()};
    draw_label(TOP_WIDTH/2 - 40, TOP_HEIGHT - 18, 0.35f, CLR_TEXT_DIM,
               "%s: %s", tr_buffer_size(), buf_names[app.buffer_size]);

    const char *wifi_label = net_wifi_status() ? tr_wifi_connected() : tr_wifi_disconnected();
    u32 wifi_color = net_wifi_status() ? CLR_OK : CLR_ERR;
    draw_label(TOP_WIDTH - 130, TOP_HEIGHT - 18, 0.4f, wifi_color, "%s", wifi_label);

    if (strlen(app.status_text) > 0) {
        u64 now = svcGetSystemTick();
        u64 elapsed = (now - app.status_time) / CPU_TICKS_PER_MSEC;
        if (elapsed < 4000) {
            float alpha = 1.0f;
            if (elapsed > 3000) alpha = 1.0f - (float)(elapsed - 3000) / 1000.0f;
            u32 c = app.status_color;
            u8 a = (u8)((c & 0xFF) * alpha);
            c = (c & 0xFFFFFF00) | a;
            draw_label(150, TOP_HEIGHT - 18, 0.4f, c, "%s", app.status_text);
        }
    }
}

/* Top screen hero header */
static void draw_hero_header(const char *title, const char *subtitle) {
    select_top();
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

    /* Hero area with gradient */
    draw_gradient(0, 0, TOP_WIDTH, 120, 0x25253DFF, 0x1C1C2E00);

    /* App logo area using panel skin */
    draw_panel(TOP_WIDTH/2 - 50, 25, 100, 100);
    draw_label(TOP_WIDTH/2 - 30, 55, 1.8f, CLR_ACCENT, "R");

    /* Title */
    draw_label(20, 140, 0.7f, CLR_TEXT_SEC, "%s", tr_main_subtitle());
    draw_label(20, 162, 0.5f, CLR_TEXT_DIM, "%s", tr_about_powered());
    draw_label(20, 185, 0.4f, CLR_TEXT_DIM, "%s", tr_about_desc());

    /* Bottom screen: menu */
    select_bottom();
    clear_bottom();

    draw_label(15, 8, 0.55f, CLR_TEXT_SEC, "%s", tr_menu_header());

    const char *items[] = {
        tr_menu_browse_genre(), tr_menu_top_stations(),
        tr_menu_search(), tr_menu_about()
    };
    for (int i = 0; i < MAIN_MENU_COUNT; i++) {
        int y = 35 + i * 48;
        bool sel = (i == app.selection);

        draw_button(10, y, BOT_WIDTH - 20, 40, sel);

        if (sel) {
            C2D_DrawRectSolid(10, y, 0.5f, 3, 40, CLR_ACCENT);
        }

        draw_label(22, y + 10, 0.55f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", items[i]);
        draw_label(BOT_WIDTH - 30, y + 10, 0.5f, CLR_TEXT_DIM, ">");
    }

    /* Buffer size hint */
    draw_label(15, BOT_HEIGHT - 22, 0.32f, CLR_TEXT_DIM,
               "SELECT " "\x1E" " " "%s", tr_buffer_size());

    draw_status_bar();
}

/* ======================================================================
 * Screen: Tag List
 * ====================================================================== */

static void render_tag_list(void) {
    draw_hero_header(tr_genre_header(), tr_genre_subtitle());

    select_bottom();
    clear_bottom();

    draw_label(15, 8, 0.5f, CLR_TEXT_DIM, "%s", tr_genres_available(app.tag_count));

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.tag_count) end = app.tag_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 28 + idx * 20;
        bool sel = (i == app.selection);

        if (sel) {
            draw_selection(5, y - 2, BOT_WIDTH - 10, 18);
        }

        char label[128];
        snprintf(label, sizeof(label), "%s", app.tags[i].name);
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", app.tags[i].stationcount);

        draw_label(15, y, 0.45f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", label);
        draw_label(BOT_WIDTH - 50, y, 0.35f, CLR_TEXT_DIM, "%s", count_str);
    }

    /* Hint bar using footer skin */
    draw_panel(5, BOT_HEIGHT - 22, BOT_WIDTH - 10, 18);
    draw_label(12, BOT_HEIGHT - 20, 0.35f, CLR_TEXT_DIM,
               "%s", tr_nav_hint_genres());

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
            draw_selection(3, y, BOT_WIDTH - 6, 20);
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

        /* Bitrate + codec badge using skin dot */
        if (s->bitrate > 0) {
            char badge[16];
            snprintf(badge, sizeof(badge), "%d kbps", s->bitrate);
            /* Small accent dot before bitrate */
            ui_skin_draw_tinted(&skin, UI_SKIN_DOT_CYAN,
                BOT_WIDTH - 72, y + 3, 0.5f, 10, 10, CLR_ACCENT);
            draw_label(BOT_WIDTH - 60, y + 1, 0.3f, CLR_ACCENT, "%s", badge);
        }
    }

    draw_panel(5, BOT_HEIGHT - 22, BOT_WIDTH - 10, 18);
    draw_label(12, BOT_HEIGHT - 20, 0.35f, CLR_TEXT_DIM,
               "%s", tr_nav_hint_stations());

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

    /* Tags as chips using skin button */
    if (strlen(app.current_station->tags) > 0) {
        char first_tag[32] = {0};
        const char *comma = strchr(app.current_station->tags, ',');
        if (comma) {
            size_t len = (size_t)(comma - app.current_station->tags);
            if (len > 20) len = 20;
            memcpy(first_tag, app.current_station->tags, len);
        } else {
            strncpy(first_tag, app.current_station->tags, 20);
        }
        if (strlen(first_tag) > 0) {
            float tag_w = strlen(first_tag) * 6.0f + 16.0f;
            draw_button(20, 62, tag_w, 22, false);
            draw_label(28, 65, 0.4f, CLR_TEXT, "%s", first_tag);
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
        snprintf(info, sizeof(info), "%s", tr_internet_radio());
    }
    draw_label(20, 95, 0.45f, CLR_TEXT_SEC, "%s", info);

    /* Votes */
    draw_label(20, 118, 0.35f, CLR_TEXT_DIM, "%s",
               tr_votes_clicks(app.current_station->votes, app.current_station->clickcount));

    /* Visualizer area - animated bars */
    int bar_count = 20;
    int bar_width = 12;
    int gap = 4;
    int total_w = bar_count * (bar_width + gap) - gap;
    int start_x = (TOP_WIDTH - total_w) / 2;
    int base_y = 155;

    for (int i = 0; i < bar_count; i++) {
        float phase = (float)(i * 3 + app.frame_count * 2);
        float height = 8.0f + sinf(phase * 0.1f) * 15.0f + sinf(phase * 0.05f) * 8.0f;
        if (!app.is_playing) height = 2.0f;

        float t = (float)i / bar_count;
        u8 r = (u8)((1-t) * 0x5C + t * 0x7C);
        u8 g = (u8)((1-t) * 0x9E + t * 0x5C);
        u8 b = (u8)((1-t) * 0xFF + t * 0xFF);
        u32 bar_color = (r << 24) | (g << 16) | (b << 8) | 0xFF;

        C2D_DrawRectSolid(start_x + i * (bar_width + gap),
                          base_y - height, 0.5f,
                          bar_width, height, bar_color);
    }

    /* Playing/Paused indicator with skin dot */
    ui_skin_draw_tinted(&skin,
        app.is_playing ? UI_SKIN_DOT_GREEN : UI_SKIN_DOT_ORANGE,
        20, TOP_HEIGHT - 48, 0.5f, 12, 12,
        app.is_playing ? CLR_OK : CLR_WARN);
    draw_label(38, TOP_HEIGHT - 45, 0.5f,
               app.is_playing ? CLR_OK : CLR_WARN,
               "%s", app.is_playing ? tr_now_playing() : tr_paused());

    /* Volume */
    draw_label(TOP_WIDTH - 80, TOP_HEIGHT - 45, 0.35f, CLR_TEXT_DIM,
               "%s", tr_volume_level((int)(app.volume * 100)));

    /* Progress bar using skin */
    select_top();
    ui_skin_draw_nine_slice(&skin, UI_SKIN_PROGRESS,
        20, TOP_HEIGHT - 32, 0.5f, TOP_WIDTH - 40, 6, 4U, 2.0f);

    /* Bottom screen: controls */
    select_bottom();
    clear_bottom();

    draw_label(15, 12, 0.5f, CLR_TEXT_SEC, "%s", tr_controls());

    /* Control buttons as skin-based buttons */
    struct { const char *label; const char *key; u32 color; } controls[] = {
        {tr_play_pause(), "A", CLR_ACCENT},
        {tr_stop_back(), "B", CLR_ACCENT3},
        {tr_vol_down(), "X", CLR_TEXT_DIM},
        {tr_vol_up(), "Y", CLR_ACCENT2},
    };

    for (int i = 0; i < 4; i++) {
        int x = 8 + i * 78;
        int y = 45;
        draw_button(x, y, 72, 55, false);
        draw_label(x + 10, y + 18, 0.55f, controls[i].color, "%s", controls[i].key);
        draw_label(x + 10, y + 38, 0.35f, CLR_TEXT_DIM, "%s", controls[i].label);
    }

    /* Stream info panel */
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
        draw_panel(8, 120, BOT_WIDTH - 16, 35);
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

    /* Search input area using panel */
    draw_panel(10, 20, BOT_WIDTH - 20, 40);
    draw_label(20, 28, 0.35f, CLR_TEXT_DIM, "%s", tr_search_prompt());
    draw_label(20, 42, 0.5f, CLR_ACCENT,
               strlen(app.search_query) > 0 ? "%s_" : "%s",
               app.search_query[0] ? app.search_query : tr_search_hint());

    /* Hint panel */
    draw_panel(10, 80, BOT_WIDTH - 20, 55);
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

    /* Info card using panel skin */
    draw_panel(10, 50, TOP_WIDTH - 20, 160);

    int y = 60;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_name());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->name);

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_country());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->country[0] ? s->country : tr_na());

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_codec());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->codec[0] ? s->codec : tr_na());

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_bitrate());
    draw_label(120, y, 0.45f, CLR_TEXT, "%d kbps", s->bitrate);

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_language());
    draw_label(120, y, 0.45f, CLR_TEXT, "%s", s->language[0] ? s->language : tr_na());

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_votes());
    draw_label(120, y, 0.45f, CLR_TEXT, "%d", s->votes);

    y += 20;
    draw_label(20, y, 0.45f, CLR_TEXT_SEC, "%s", tr_clicks());
    draw_label(120, y, 0.45f, CLR_TEXT, "%d", s->clickcount);

    select_bottom();
    clear_bottom();

    draw_label(15, 12, 0.5f, CLR_TEXT_SEC, "%s", tr_station_details());

    /* Tags section */
    if (strlen(s->tags) > 0) {
        draw_panel(8, 40, BOT_WIDTH - 16, 50);
        draw_label(15, 46, 0.35f, CLR_TEXT_DIM, "%s", tr_tags());
        draw_label(15, 60, 0.4f, CLR_ACCENT, "%s", s->tags);
    }

    /* Back button */
    draw_panel(8, BOT_HEIGHT - 40, BOT_WIDTH - 16, 32);
    draw_label(15, BOT_HEIGHT - 35, 0.4f, CLR_TEXT_DIM, "%s", tr_back());

    draw_status_bar();
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

/* Forward declarations */
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

            if (touch_active && touch.py >= 35 && touch.py <= 35 + MAIN_MENU_COUNT * 48) {
                int idx = (touch.py - 35) / 48;
                if (idx >= 0 && idx < MAIN_MENU_COUNT) {
                    app.selection = idx;
                    if (touch.px >= 10 && touch.px <= BOT_WIDTH - 10) {
                        kDown |= KEY_A;
                    }
                }
            }

            /* SELECT cycles audio buffer size (Small → Medium → Large) */
            if (kDown & KEY_SELECT) {
                app.buffer_size = (app.buffer_size + 1) % 3;
                const char *size_names[] = {
                    tr_buffer_small(), tr_buffer_medium(), tr_buffer_large()
                };
                set_status("%s", CLR_INFO, tr_buffer_changed(size_names[app.buffer_size]));
                /* Recreate stream player with new buffer config */
                if (app.stream_player) {
                    stream_player_destroy(app.stream_player);
                }
                app.stream_player = stream_player_create_with_bufsize(app.buffer_size);
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
                        set_status("%s", CLR_INFO, tr_about_tagline());
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
                if (app.stream_player) {
                    stream_player_toggle_pause(app.stream_player);
                    bool paused = stream_player_is_paused(app.stream_player);
                    set_status("%s",
                              paused ? CLR_WARN : CLR_OK,
                              paused ? tr_paused() : tr_playing());
                }
            }
            if (kDown & KEY_B) {
                if (app.stream_player) {
                    stream_player_stop(app.stream_player);
                }
                app.is_playing = false;
                app.current_station = NULL;
                memset(app.stream_url, 0, sizeof(app.stream_url));
                app.screen = SCREEN_STATION_LIST;
            }
            if (kDown & KEY_X) {
                app.volume = fmax(0.0f, app.volume - 0.1f);
                if (app.stream_player)
                    stream_player_set_volume(app.stream_player, app.volume);
                set_status("%s", CLR_INFO, tr_volume_level((int)(app.volume * 100)));
            }
            if (kDown & KEY_Y) {
                app.volume = fmin(1.0f, app.volume + 0.1f);
                if (app.stream_player)
                    stream_player_set_volume(app.stream_player, app.volume);
                set_status("%s", CLR_INFO, tr_volume_level((int)(app.volume * 100)));
            }
            if (touch_active && touch.py >= 45 && touch.py <= 100) {
                int idx = (touch.px - 8) / 78;
                if (idx >= 0 && idx < 4) {
                    switch (idx) {
                        case 0:
                            if (app.stream_player)
                                stream_player_toggle_pause(app.stream_player);
                            break;
                        case 1:
                            if (app.stream_player)
                                stream_player_stop(app.stream_player);
                            app.is_playing = false;
                            app.current_station = NULL;
                            memset(app.stream_url, 0, sizeof(app.stream_url));
                            app.screen = SCREEN_STATION_LIST;
                            break;
                        case 2:
                            app.volume = fmax(0.0f, app.volume - 0.1f);
                            if (app.stream_player)
                                stream_player_set_volume(app.stream_player, app.volume);
                            break;
                        case 3:
                            app.volume = fmin(1.0f, app.volume + 0.1f);
                            if (app.stream_player)
                                stream_player_set_volume(app.stream_player, app.volume);
                            break;
                    }
                }
            }
            break;
        }

        case SCREEN_SEARCH: {
            if (kDown & KEY_A) {
                if (strlen(app.search_query) > 0) {
                    set_status("%s", CLR_INFO, tr_searching());
                    char error[128];
                    int count = radio_search_by_name(app.search_query, app.stations,
                                                       MAX_STATIONS, error, sizeof(error));
                    if (count > 0) {
                        app.station_count = count;
                        app.selection = 0;
                        app.scroll_offset = 0;
                        app.screen = SCREEN_STATION_LIST;
                        set_status("%s", CLR_OK, tr_stations_found(count));
                    } else {
                        set_status("%s", CLR_WARN, tr_no_results());
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
 * Data Loading Functions
 * ====================================================================== */

static void load_tags(void) {
    if (!net_wifi_status()) {
        set_status("%s", CLR_ERR, tr_wifi_error());
        return;
    }
    set_status("%s", CLR_INFO, tr_loading_genres());

    char error[128];
    int count = radio_fetch_tags(app.tags, MAX_TAGS, error, sizeof(error));

    if (count > 0) {
        app.tag_count = count;
        app.selection = 0;
        app.scroll_offset = 0;
        app.screen = SCREEN_TAG_LIST;
        set_status("%s", CLR_OK, tr_genres_loaded(count));
    } else {
        set_status("%s", CLR_ERR, tr_failed(error[0] ? error : tr_internet_radio()));
    }
}

static void load_top_stations(void) {
    if (!net_wifi_status()) {
        set_status("%s", CLR_ERR, tr_wifi_error());
        return;
    }
    set_status("%s", CLR_INFO, tr_loading_stations());

    char error[128];
    int count = radio_fetch_topclick(app.stations, MAX_STATIONS, error, sizeof(error));

    if (count > 0) {
        app.station_count = count;
        app.selection = 0;
        app.scroll_offset = 0;
        app.screen = SCREEN_STATION_LIST;
        set_status("%s", CLR_OK, tr_stations_loaded());
    } else {
        set_status("%s", CLR_ERR, tr_failed(error[0] ? error : tr_internet_radio()));
    }
}

static void load_stations_by_tag(const char *tag) {
    if (!net_wifi_status()) {
        set_status("%s", CLR_ERR, tr_wifi_error());
        return;
    }
    set_status("%s", CLR_INFO, tr_loading());

    char error[128];
    int count = radio_fetch_by_tag(tag, app.stations, MAX_STATIONS, error, sizeof(error));

    if (count > 0) {
        app.station_count = count;
        app.selection = 0;
        app.scroll_offset = 0;
        app.screen = SCREEN_STATION_LIST;
        set_status("%s", CLR_OK, tr_stations_found(count));
    } else {
        set_status("%s", CLR_ERR, tr_failed(error[0] ? error : tr_no_results()));
    }
}

static void play_station(int index) {
    if (index < 0 || index >= app.station_count) return;

    app.current_station = &app.stations[index];
    app.is_playing = false;

    set_status("%s", CLR_INFO, tr_connecting_stream());

    char error[128];
    int ret = radio_get_stream_url(app.current_station->stationuuid,
                                    app.stream_url, sizeof(app.stream_url),
                                    error, sizeof(error));

    const char *play_url = app.stream_url;
    if (ret != NET_OK || strlen(app.stream_url) == 0) {
        if (strlen(app.current_station->url_resolved) > 0) {
            strncpy(app.stream_url, app.current_station->url_resolved, sizeof(app.stream_url) - 1);
            play_url = app.stream_url;
        } else if (strlen(app.current_station->url) > 0) {
            strncpy(app.stream_url, app.current_station->url, sizeof(app.stream_url) - 1);
            play_url = app.stream_url;
        } else {
            set_status("%s", CLR_ERR, tr_stream_url_failed());
            return;
        }
    }

    if (app.stream_player) {
        stream_player_stop(app.stream_player);
        int r = stream_player_play(app.stream_player, play_url);
        if (r == 0) {
            app.is_playing = true;
            app.play_start_tick = svcGetSystemTick();
            app.screen = SCREEN_PLAYING;
            set_status("%s", CLR_OK, tr_streaming());
        } else {
            set_status("%s", CLR_ERR, tr_stream_failed());
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

    /* Global text buffer - larger for CJK glyph storage */
    global_text_buf = C2D_TextBufNew(32768);

    /* Initialize networking */
    net_init();
    radio_init();

    /* Set Chinese language and try to load Chinese font */
    locale_set_language(LANG_ZH_CN);
    romfsInit();
    if (locale_init_fonts()) {
        active_font = locale_get_font();
    }

    /* Load UI skin texture atlas */
    ui_skin_init(&skin);
    if (!ui_skin_load(&skin, "romfs:/ui-skin-dark.png")) {
        /* Skin load failed — will use solid-color fallbacks throughout */
        set_status("%s", CLR_WARN, tr_skin_fallback());
    }

    /* App state */
    memset(&app, 0, sizeof(app));
    app.screen = SCREEN_MAIN_MENU;
    app.volume = 0.8f;
    app.buffer_size = STREAM_BUF_MEDIUM;

    set_status("%s", CLR_INFO, tr_welcome());
    app.stream_player = stream_player_create_with_bufsize(app.buffer_size);

    /* Main loop */
    while (aptMainLoop()) {
        hidScanInput();
        /* Update audio playback */
        if (app.stream_player) {
            stream_player_update(app.stream_player);
            app.is_playing = stream_player_is_playing(app.stream_player);
        }
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
    stream_player_destroy(app.stream_player);
    ui_skin_clear(&skin);
    radio_exit();
    net_exit();
    romfsExit();
    C2D_TextBufDelete(global_text_buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return 0;
}
