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
#define MAX_VISIBLE_ITEMS 8
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
 * Async Loading System
 * Worker thread runs blocking network calls; main loop renders spinner.
 * Pattern follows stream_player.c's download thread.
 * ====================================================================== */

typedef enum {
    ASYNC_IDLE,
    ASYNC_LOADING,
    ASYNC_DONE,
    ASYNC_ERROR,
    ASYNC_TIMEOUT
} AsyncState;

typedef enum {
    ASYNC_REQ_LOAD_TAGS,
    ASYNC_REQ_LOAD_LANGUAGES,
    ASYNC_REQ_LOAD_STATIONS_BY_TAG,
    ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE,
    ASYNC_REQ_LOAD_TOP_STATIONS,
    ASYNC_REQ_SEARCH,
    ASYNC_REQ_PLAY_STATION,
} AsyncRequestType;

typedef struct {
    AsyncRequestType type;
    char param[256];       /* tag name, language, search query, station UUID */
} AsyncRequest;

typedef struct {
    volatile AsyncState state;
    volatile bool cancel_requested;
    Thread worker_thread;
    AsyncRequest request;
    u32 start_frame;       /* frame_count when loading began */
    int result_count;
    char error_msg[128];
} AsyncLoad;

/* Timeout thresholds in frames (~60fps on 3DS) */
#define ASYNC_TIMEOUT_LIST     900   /* 15s for tag/language lists */
#define ASYNC_TIMEOUT_STATIONS 1800  /* 30s for station/stream loads */

/* ======================================================================
 * UI State
 * ====================================================================== */

typedef enum {
    SCREEN_MAIN_MENU,
    SCREEN_TAG_LIST,
    SCREEN_LANGUAGE_LIST,
    SCREEN_STATION_LIST,
    SCREEN_PLAYING,
    SCREEN_SEARCH,
    SCREEN_STATION_INFO,
} AppScreen;

/* Menu items */
#define MAIN_MENU_COUNT 5

typedef struct {
    AppScreen screen;
    int selection;
    int scroll_offset;
    int prev_screen;
    int prev_selection;

    /* Tag data */
    RadioTag tags[MAX_TAGS];
    int tag_count;

    /* Language data */
    RadioLanguage languages[MAX_TAGS];
    int language_count;

    /* Station data */
    RadioStation stations[MAX_STATIONS];
    int station_count;
    int station_list_parent;  /* enum AppScreen; where B goes back to */

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

    /* Async loading */
    AsyncLoad async;

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
        0, TOP_HEIGHT - 26, 0.5f, TOP_WIDTH, 26, 8U, 4.0f);
    if (!skin.ready) {
        C2D_DrawRectSolid(0, TOP_HEIGHT - 26, 0.5f, TOP_WIDTH, 26, CLR_STATUSBAR);
    }

    draw_label(10, TOP_HEIGHT - 21, 0.5f, CLR_TEXT_DIM, "%s", tr_about_title());

    /* Center slot: transient status message while active, otherwise the
     * buffer-size indicator. Both fit on one 0.5f line without colliding
     * with the wifi label on the right. */
    u64 now = svcGetSystemTick();
    u64 elapsed = (strlen(app.status_text) > 0)
        ? (now - app.status_time) / CPU_TICKS_PER_MSEC : 9999;
    if (elapsed < 4000) {
        float alpha = 1.0f;
        if (elapsed > 3000) alpha = 1.0f - (float)(elapsed - 3000) / 1000.0f;
        u32 c = app.status_color;
        u8 a = (u8)((c & 0xFF) * alpha);
        c = (c & 0xFFFFFF00) | a;
        /* Rough centering: CJK glyphs are ~14px at 0.5f, latin narrower */
        float tw = (float)strlen(app.status_text) * 7.0f;
        draw_label((TOP_WIDTH - tw) / 2.0f, TOP_HEIGHT - 21, 0.5f, c,
                   "%s", app.status_text);
    } else {
        const char *buf_names[] = {tr_buffer_small(), tr_buffer_medium(), tr_buffer_large()};
        draw_label(TOP_WIDTH/2 - 40, TOP_HEIGHT - 21, 0.5f, CLR_TEXT_DIM,
                   "%s: %s", tr_buffer_size(), buf_names[app.buffer_size]);
    }

    const char *wifi_label = net_wifi_status() ? tr_wifi_connected() : tr_wifi_disconnected();
    u32 wifi_color = net_wifi_status() ? CLR_OK : CLR_ERR;
    draw_label(TOP_WIDTH - 130, TOP_HEIGHT - 21, 0.5f, wifi_color, "%s", wifi_label);
}

/* Top screen hero header */
static void draw_hero_header(const char *title, const char *subtitle) {
    select_top();
    draw_gradient(0, 0, TOP_WIDTH, 60, 0x2A2A44FF, 0x1C1C2E00);

    draw_label(20, 10, 0.9f, CLR_TEXT, "%s", title);
    if (subtitle) {
        draw_label(20, 40, 0.55f, CLR_TEXT_SEC, "%s", subtitle);
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
        tr_menu_browse_genre(), tr_menu_browse_language(),
        tr_menu_top_stations(), tr_menu_search(), tr_menu_about()
    };
    for (int i = 0; i < MAIN_MENU_COUNT; i++) {
        int y = 28 + i * 36;
        bool sel = (i == app.selection);

        draw_button(10, y, BOT_WIDTH - 20, 34, sel);

        if (sel) {
            C2D_DrawRectSolid(10, y, 0.5f, 3, 34, CLR_ACCENT);
        }

        draw_label(22, y + 7, 0.5f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", items[i]);
        draw_label(BOT_WIDTH - 30, y + 7, 0.5f, CLR_TEXT_DIM, ">");
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

    draw_label(15, 8, 0.55f, CLR_TEXT_DIM, "%s", tr_genres_available(app.tag_count));

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.tag_count) end = app.tag_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 28 + idx * 24;
        bool sel = (i == app.selection);

        if (sel) {
            draw_selection(5, y - 3, BOT_WIDTH - 10, 22);
        }

        char label[128];
        snprintf(label, sizeof(label), "%s", app.tags[i].name);
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", app.tags[i].stationcount);

        draw_label(15, y, 0.5f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", label);
        draw_label(BOT_WIDTH - 50, y, 0.4f, CLR_TEXT_DIM, "%s", count_str);
    }

    /* Hint bar using footer skin */
    draw_panel(5, BOT_HEIGHT - 22, BOT_WIDTH - 10, 18);
    draw_label(12, BOT_HEIGHT - 20, 0.4f, CLR_TEXT_DIM,
               "%s", tr_nav_hint_genres());

    draw_status_bar();
}

/* ======================================================================
 * Screen: Language List
 * ====================================================================== */

static void render_language_list(void) {
    draw_hero_header(tr_language_header(), tr_language_subtitle());

    select_bottom();
    clear_bottom();

    draw_label(15, 8, 0.55f, CLR_TEXT_DIM, "%s", tr_languages_available(app.language_count));

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.language_count) end = app.language_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 28 + idx * 24;
        bool sel = (i == app.selection);

        if (sel) {
            draw_selection(5, y - 3, BOT_WIDTH - 10, 22);
        }

        char label[128];
        snprintf(label, sizeof(label), "%s", app.languages[i].name);
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", app.languages[i].stationcount);

        draw_label(15, y, 0.5f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", label);
        draw_label(BOT_WIDTH - 50, y, 0.4f, CLR_TEXT_DIM, "%s", count_str);
    }

    /* Hint bar */
    draw_panel(5, BOT_HEIGHT - 22, BOT_WIDTH - 10, 18);
    draw_label(12, BOT_HEIGHT - 20, 0.4f, CLR_TEXT_DIM,
               "%s", tr_nav_hint_languages());

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
        int y = 5 + idx * 26;
        bool sel = (i == app.selection);

        if (sel) {
            draw_selection(3, y - 1, BOT_WIDTH - 6, 24);
        }

        RadioStation *s = &app.stations[i];

        /* Station name — keep it clear of the bitrate badge on the right */
        char name_buf[24];
        size_t name_len = strlen(s->name);
        if (name_len > 18) {
            memcpy(name_buf, s->name, 16);
            name_buf[16] = '.';
            name_buf[17] = '.';
            name_buf[18] = '.';
            name_buf[19] = '\0';
        } else {
            strcpy(name_buf, s->name);
        }
        draw_label(12, y + 2, 0.5f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", name_buf);

        /* Bitrate + codec badge using skin dot */
        if (s->bitrate > 0) {
            char badge[16];
            snprintf(badge, sizeof(badge), "%d kbps", s->bitrate);
            /* Small accent dot before bitrate */
            ui_skin_draw_tinted(&skin, UI_SKIN_DOT_CYAN,
                BOT_WIDTH - 72, y + 4, 0.5f, 10, 10, CLR_ACCENT);
            draw_label(BOT_WIDTH - 60, y + 2, 0.35f, CLR_ACCENT, "%s", badge);
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
            float tag_w = strlen(first_tag) * 7.0f + 16.0f;
            draw_button(20, 66, tag_w, 24, false);
            draw_label(28, 71, 0.45f, CLR_TEXT, "%s", first_tag);
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
    draw_label(20, 96, 0.5f, CLR_TEXT_SEC, "%s", info);

    /* Votes */
    draw_label(20, 112, 0.4f, CLR_TEXT_DIM, "%s",
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
        20, TOP_HEIGHT - 52, 0.5f, 12, 12,
        app.is_playing ? CLR_OK : CLR_WARN);
    draw_label(38, TOP_HEIGHT - 49, 0.55f,
               app.is_playing ? CLR_OK : CLR_WARN,
               "%s", app.is_playing ? tr_now_playing() : tr_paused());

    /* Volume */
    draw_label(TOP_WIDTH - 90, TOP_HEIGHT - 49, 0.45f, CLR_TEXT_DIM,
               "%s", tr_volume_level((int)(app.volume * 100)));

    /* Progress bar using skin */
    select_top();
    ui_skin_draw_nine_slice(&skin, UI_SKIN_PROGRESS,
        20, TOP_HEIGHT - 32, 0.5f, TOP_WIDTH - 40, 6, 4U, 2.0f);

    /* Bottom screen: controls */
    select_bottom();
    clear_bottom();

    draw_label(15, 12, 0.55f, CLR_TEXT_SEC, "%s", tr_controls());

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
        draw_label(x + 10, y + 17, 0.6f, controls[i].color, "%s", controls[i].key);
        draw_label(x + 10, y + 38, 0.45f, CLR_TEXT_DIM, "%s", controls[i].label);
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
        draw_panel(8, 118, BOT_WIDTH - 16, 42);
        draw_label(15, 126, 0.4f, CLR_TEXT_DIM, "%s", tr_stream_url());
        draw_label(15, 140, 0.4f, CLR_ACCENT, "%s", url_display);
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
static void async_launch_load(AsyncRequestType type, const char *param);

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

            if (touch_active && touch.py >= 28 && touch.py <= 28 + MAIN_MENU_COUNT * 36) {
                int idx = (touch.py - 28) / 36;
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
                    case 0: async_launch_load(ASYNC_REQ_LOAD_TAGS, ""); break;
                    case 1: async_launch_load(ASYNC_REQ_LOAD_LANGUAGES, ""); break;
                    case 2:
                        app.station_list_parent = SCREEN_MAIN_MENU;
                        async_launch_load(ASYNC_REQ_LOAD_TOP_STATIONS, ""); break;
                    case 3:
                        memset(app.search_query, 0, sizeof(app.search_query));
                        app.search_cursor = 0;
                        app.screen = SCREEN_SEARCH;
                        break;
                    case 4:
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
                int idx = (touch.py - 28) / 24 + app.scroll_offset;
                if (idx >= 0 && idx < app.tag_count) {
                    app.selection = idx;
                    if (touch.px >= 5 && touch.px <= BOT_WIDTH - 5)
                        kDown |= KEY_A;
                }
            }
            if (kDown & KEY_A) {
                app.station_list_parent = SCREEN_TAG_LIST;
                async_launch_load(ASYNC_REQ_LOAD_STATIONS_BY_TAG,
                                  app.tags[app.selection].name);
            }
            if (kDown & KEY_B) {
                app.screen = SCREEN_MAIN_MENU;
                app.selection = 1;
            }
            break;
        }

        case SCREEN_LANGUAGE_LIST: {
            if (kDown & KEY_DOWN) {
                if (app.selection < app.language_count - 1) {
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
                int idx = (touch.py - 28) / 24 + app.scroll_offset;
                if (idx >= 0 && idx < app.language_count) {
                    app.selection = idx;
                    if (touch.px >= 5 && touch.px <= BOT_WIDTH - 5)
                        kDown |= KEY_A;
                }
            }
            if (kDown & KEY_A) {
                app.station_list_parent = SCREEN_LANGUAGE_LIST;
                async_launch_load(ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE,
                                  app.languages[app.selection].name);
            }
            if (kDown & KEY_B) {
                app.screen = SCREEN_MAIN_MENU;
                app.selection = 1;
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
                int idx = (touch.py - 5) / 26 + app.scroll_offset;
                if (idx >= 0 && idx < app.station_count) {
                    app.selection = idx;
                    if (touch.px >= 3 && touch.px <= BOT_WIDTH - 3)
                        kDown |= KEY_A;
                }
            }
            if (kDown & KEY_A)
                async_launch_load(ASYNC_REQ_PLAY_STATION,
                                  app.stations[app.selection].stationuuid);
            if (kDown & KEY_B) {
                app.screen = app.station_list_parent;
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
                app.screen = app.station_list_parent;
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
                            app.screen = app.station_list_parent;
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
                    app.station_list_parent = SCREEN_MAIN_MENU;
                    async_launch_load(ASYNC_REQ_SEARCH, app.search_query);
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
 * Async Loading System
 * Worker thread — makes blocking network calls so the UI stays alive.
 * ====================================================================== */

static void async_worker_thread(void *arg) {
    AsyncRequest *req = &app.async.request;
    char error[128] = {0};
    int count = 0;

    switch (req->type) {
        case ASYNC_REQ_LOAD_TAGS:
            count = radio_fetch_tags(app.tags, MAX_TAGS, error, sizeof(error));
            if (count > 0) app.tag_count = count;
            break;

        case ASYNC_REQ_LOAD_LANGUAGES:
            count = radio_fetch_languages(app.languages, MAX_TAGS, error, sizeof(error));
            if (count > 0) app.language_count = count;
            break;

        case ASYNC_REQ_LOAD_STATIONS_BY_TAG:
            count = radio_fetch_by_tag(req->param, app.stations, MAX_STATIONS,
                                        error, sizeof(error));
            if (count > 0) app.station_count = count;
            break;

        case ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE:
            count = radio_fetch_by_language(req->param, app.stations, MAX_STATIONS,
                                             error, sizeof(error));
            if (count > 0) app.station_count = count;
            break;

        case ASYNC_REQ_LOAD_TOP_STATIONS:
            count = radio_fetch_topclick(app.stations, MAX_STATIONS, error, sizeof(error));
            if (count > 0) app.station_count = count;
            break;

        case ASYNC_REQ_SEARCH:
            count = radio_search_by_name(req->param, app.stations, MAX_STATIONS,
                                          error, sizeof(error));
            if (count > 0) app.station_count = count;
            break;

        case ASYNC_REQ_PLAY_STATION: {
            /* Get stream URL, then fall back to url_resolved / url */
            int idx = -1;
            for (int i = 0; i < app.station_count; i++) {
                if (strcmp(app.stations[i].stationuuid, req->param) == 0) {
                    idx = i; break;
                }
            }
            if (idx < 0) {
                snprintf(error, sizeof(error), "Station not found");
                count = -1;
                break;
            }
            app.current_station = &app.stations[idx];
            int ret = radio_get_stream_url(req->param, app.stream_url,
                                            sizeof(app.stream_url), error, sizeof(error));
            if (ret != NET_OK || strlen(app.stream_url) == 0) {
                if (strlen(app.current_station->url_resolved) > 0) {
                    strncpy(app.stream_url, app.current_station->url_resolved,
                            sizeof(app.stream_url) - 1);
                    count = 0; /* Success with fallback */
                } else if (strlen(app.current_station->url) > 0) {
                    strncpy(app.stream_url, app.current_station->url,
                            sizeof(app.stream_url) - 1);
                    count = 0;
                } else {
                    count = -1;
                    snprintf(error, sizeof(error), "No stream URL");
                }
            }
            break;
        }
    }

    /* Check for cancellation (B pressed during load) */
    if (app.async.cancel_requested) {
        app.async.state = ASYNC_IDLE;
        return;
    }

    /* Write results BEFORE setting state = DONE (ordering for main thread) */
    app.async.result_count = count;
    if (count < 0) {
        snprintf(app.async.error_msg, sizeof(app.async.error_msg), "%s",
                 error[0] ? error : "Unknown error");
        app.async.state = ASYNC_ERROR;
    } else {
        app.async.state = ASYNC_DONE;
    }
}

/* Launch a non-blocking load. Returns immediately; worker runs on another thread. */
static void async_launch_load(AsyncRequestType type, const char *param) {
    /* Guard: no concurrent loads */
    if (app.async.state != ASYNC_IDLE) return;

    /* WiFi check for network requests */
    if (type != ASYNC_REQ_PLAY_STATION || true) {
        if (!net_wifi_status() && type != ASYNC_REQ_PLAY_STATION) {
            set_status("%s", CLR_ERR, tr_wifi_error());
            return;
        }
    }

    /* Set status based on request type */
    switch (type) {
        case ASYNC_REQ_LOAD_TAGS:
            set_status("%s", CLR_INFO, tr_loading_genres()); break;
        case ASYNC_REQ_LOAD_LANGUAGES:
            set_status("%s", CLR_INFO, tr_loading_languages()); break;
        case ASYNC_REQ_LOAD_TOP_STATIONS:
            set_status("%s", CLR_INFO, tr_loading_stations()); break;
        case ASYNC_REQ_LOAD_STATIONS_BY_TAG:
        case ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE:
        case ASYNC_REQ_SEARCH:
            set_status("%s", CLR_INFO, tr_loading()); break;
        case ASYNC_REQ_PLAY_STATION:
            set_status("%s", CLR_INFO, tr_connecting_stream()); break;
    }

    /* Configure the request */
    app.async.request.type = type;
    if (param) {
        strncpy(app.async.request.param, param, sizeof(app.async.request.param) - 1);
        app.async.request.param[sizeof(app.async.request.param) - 1] = '\0';
    } else {
        app.async.request.param[0] = '\0';
    }

    app.async.cancel_requested = false;
    app.async.state = ASYNC_LOADING;
    app.async.start_frame = app.frame_count;

    /* Spawn worker thread (32KB stack, same as stream_player's download thread) */
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    app.async.worker_thread = threadCreate(async_worker_thread, NULL, 32768,
                                            prio - 1, -2, false);
}

/* Called from main loop when worker finishes */
static void async_handle_completion(void) {
    threadJoin(app.async.worker_thread, U64_MAX);
    threadFree(app.async.worker_thread);

    int count = app.async.result_count;

    switch (app.async.state) {
        case ASYNC_DONE:
            switch (app.async.request.type) {
                case ASYNC_REQ_LOAD_TAGS:
                    app.selection = 0;
                    app.scroll_offset = 0;
                    app.screen = SCREEN_TAG_LIST;
                    set_status("%s", CLR_OK, tr_genres_loaded(count));
                    break;

                case ASYNC_REQ_LOAD_LANGUAGES:
                    app.selection = 0;
                    app.scroll_offset = 0;
                    app.screen = SCREEN_LANGUAGE_LIST;
                    set_status("%s", CLR_OK, tr_languages_loaded(count));
                    break;

                case ASYNC_REQ_LOAD_STATIONS_BY_TAG:
                case ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE:
                case ASYNC_REQ_LOAD_TOP_STATIONS:
                case ASYNC_REQ_SEARCH:
                    app.selection = 0;
                    app.scroll_offset = 0;
                    app.screen = SCREEN_STATION_LIST;
                    set_status("%s", CLR_OK, tr_stations_found(count));
                    break;

                case ASYNC_REQ_PLAY_STATION:
                    if (app.stream_player && app.current_station) {
                        stream_player_stop(app.stream_player);
                        int r = stream_player_play(app.stream_player, app.stream_url);
                        if (r == 0) {
                            app.is_playing = true;
                            app.play_start_tick = svcGetSystemTick();
                            app.screen = SCREEN_PLAYING;
                            set_status("%s", CLR_OK, tr_streaming());
                        } else {
                            set_status("%s", CLR_ERR, tr_stream_failed());
                        }
                    }
                    break;
            }
            break;

        case ASYNC_ERROR:
            set_status("%s", CLR_ERR,
                       tr_failed(app.async.error_msg[0] ? app.async.error_msg
                                                         : tr_internet_radio()));
            break;

        case ASYNC_TIMEOUT:
            set_status("%s", CLR_WARN,
                       locale_get_language() == LANG_ZH_CN
                           ? "请求超时" : "Request timed out");
            break;

        default:
            break;
    }

    app.async.state = ASYNC_IDLE;
}

/* ======================================================================
 * Loading Spinner
 * 8-dot animated spinner overlay on the bottom screen.
 * ====================================================================== */

static void render_loading_spinner(void) {
    /* Semi-transparent overlay */
    C2D_DrawRectSolid(0, 0, 0.5f, BOT_WIDTH, BOT_HEIGHT,
                      C2D_Color32(0x00, 0x00, 0x00, 0xAA));

    int cx = BOT_WIDTH / 2;
    int cy = BOT_HEIGHT / 2 - 10;
    int radius = 18;

    /* 8 dots rotating around center */
    float base_angle = app.frame_count * 5.0f * (3.14159265f / 180.0f);

    for (int i = 0; i < 8; i++) {
        float angle = base_angle + i * (3.14159265f * 2.0f / 8.0f);
        int dx = (int)(radius * cosf(angle));
        int dy = (int)(radius * sinf(angle));

        /* Alpha fades based on position — chasing-dots effect */
        u8 alpha = (u8)(55 + (200 * i / 8));
        u32 color = C2D_Color32(0xFF, 0x44, 0x44, alpha);

        C2D_DrawCircleSolid(cx + dx, cy + dy, 0.5f, 4, color);
    }

    /* Status text below spinner */
    const char *msg = tr_loading();
    switch (app.async.request.type) {
        case ASYNC_REQ_LOAD_TAGS:       msg = tr_loading_genres(); break;
        case ASYNC_REQ_LOAD_LANGUAGES:   msg = tr_loading_languages(); break;
        case ASYNC_REQ_LOAD_STATIONS_BY_TAG:
        case ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE:
        case ASYNC_REQ_LOAD_TOP_STATIONS:
        case ASYNC_REQ_SEARCH:           msg = tr_loading(); break;
        case ASYNC_REQ_PLAY_STATION:     msg = tr_connecting_stream(); break;
    }
    draw_label(cx - 80, cy + 32, 0.5f, CLR_TEXT,
               "%-24s", msg);  /* pad for consistent textbuf lifetime */
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

        /* --- Async completion check --- */
        if (app.async.state == ASYNC_DONE ||
            app.async.state == ASYNC_ERROR ||
            app.async.state == ASYNC_TIMEOUT) {
            async_handle_completion();
        }

        /* --- Input (only when idle; B cancels during loading) --- */
        if (app.async.state == ASYNC_IDLE) {
            handle_input();
        } else {
            /* B cancels the current async load */
            u32 kDown = hidKeysDown();
            if (kDown & KEY_B) {
                app.async.cancel_requested = true;
                /* Worker checks flag and returns ASYNC_IDLE; completion handler discards */
            }
        }

        /* --- Timeout detection --- */
        if (app.async.state == ASYNC_LOADING) {
            u32 elapsed = app.frame_count - app.async.start_frame;
            u32 threshold = (app.async.request.type == ASYNC_REQ_PLAY_STATION ||
                             app.async.request.type == ASYNC_REQ_LOAD_STATIONS_BY_TAG ||
                             app.async.request.type == ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE ||
                             app.async.request.type == ASYNC_REQ_LOAD_TOP_STATIONS ||
                             app.async.request.type == ASYNC_REQ_SEARCH)
                                ? ASYNC_TIMEOUT_STATIONS
                                : ASYNC_TIMEOUT_LIST;
            if (elapsed > threshold) {
                app.async.cancel_requested = true;
                app.async.state = ASYNC_TIMEOUT;
            }
        }

        app.frame_count++;

        /* Render */
        draw_begin_frame();

        switch (app.screen) {
            case SCREEN_MAIN_MENU:    render_main_menu(); break;
            case SCREEN_TAG_LIST:     render_tag_list(); break;
            case SCREEN_LANGUAGE_LIST:render_language_list(); break;
            case SCREEN_STATION_LIST: render_station_list(); break;
            case SCREEN_PLAYING:      render_playing(); break;
            case SCREEN_SEARCH:       render_search(); break;
            case SCREEN_STATION_INFO: render_station_info(); break;
        }

        /* Spinner overlay on top of whatever screen is showing */
        if (app.async.state == ASYNC_LOADING) {
            select_bottom();
            render_loading_spinner();
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
