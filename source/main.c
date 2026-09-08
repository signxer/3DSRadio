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
#include "settings.h"
#include "search_input.h"

/* ======================================================================
 * 3DSRadio - ClouDS-inspired dual-screen UI
 * Skin-based rendering and interaction hierarchy adapted from
 * ClouDS-Music-FA, with radio-specific controls and diagnostics.
 *
 * Design philosophy:
 * - Texture atlas (ui-skin-light.png) for all UI chrome
 * - Nine-slice scaling for buttons, panels, headers, selections
 * - Top screen: content/art/visualization (hero)
 * - Bottom screen: navigation/lists/controls (functional)
 * - Both themes use a high-contrast ink/accent system tuned for 3DS PPI
 * - Bottom screen behaves as a stable tabbed application shell
 * - Touch + button dual input
 * ====================================================================== */

/* Screen dimensions */
#define TOP_WIDTH  400
#define TOP_HEIGHT 240
#define BOT_WIDTH  320
#define BOT_HEIGHT 240

/* Maximum items */
#define MAX_VISIBLE_ITEMS 7
#define MAX_STATIONS 100
#define MAX_TAGS 50

/* ======================================================================
 * Apple-Light Color Palette
 * Inspired by iOS system colors — dark text on light for 3DS readability.
 * C2D_Color32(r,g,b,a) guarantees correct PICA200 RGBA8 byte order.
 * ====================================================================== */

/* Theme tokens.  They remain variables rather than preprocessor constants so
 * the user can switch the bundled light/dark skins without restarting. */
static u32 CLR_BG_TOP;
static u32 CLR_BG_BOT;
static u32 CLR_SURFACE;
static u32 CLR_SURFACE_LT;
static u32 CLR_TEXT;
static u32 CLR_TEXT_SEC;
static u32 CLR_TEXT_DIM;
static u32 CLR_ACCENT;
static u32 CLR_ACCENT2;
static u32 CLR_ACCENT3;
static u32 CLR_OK;
static u32 CLR_ERR;
static u32 CLR_WARN;
static u32 CLR_INFO;
static u32 CLR_STATUSBAR;

static void apply_theme_palette(UiTheme theme) {
    /* C2D_Color32 is an inline function in some devkitPro releases, so these
     * runtime assignments intentionally avoid non-constant file initializers. */
    CLR_ACCENT3 = C2D_Color32(0xFF, 0x3B, 0x30, 0xFF);
    CLR_OK = C2D_Color32(0x34, 0xC7, 0x59, 0xFF);
    CLR_ERR = C2D_Color32(0xFF, 0x3B, 0x30, 0xFF);
    CLR_WARN = C2D_Color32(0xFF, 0x95, 0x00, 0xFF);
    CLR_INFO = C2D_Color32(0x00, 0x7A, 0xFF, 0xFF);
    if (theme == UI_THEME_DARK) {
        /* ClouDS-inspired dark canvas: near-black stage, cool ink, teal
         * interaction accents and coral play/action colour. */
        CLR_BG_TOP = C2D_Color32(0x0B, 0x0D, 0x12, 0xFF);
        CLR_BG_BOT = C2D_Color32(0x08, 0x0A, 0x0F, 0xFF);
        CLR_SURFACE = C2D_Color32(0x14, 0x17, 0x22, 0xFF);
        CLR_SURFACE_LT = C2D_Color32(0x22, 0x27, 0x36, 0xFF);
        CLR_TEXT = C2D_Color32(0xF2, 0xF4, 0xF7, 0xFF);
        CLR_TEXT_SEC = C2D_Color32(0xA9, 0xB2, 0xC7, 0xFF);
        CLR_TEXT_DIM = C2D_Color32(0x69, 0x74, 0x8D, 0xFF);
        CLR_ACCENT = C2D_Color32(0x59, 0xD0, 0xD8, 0xFF);
        CLR_ACCENT2 = C2D_Color32(0xEB, 0x5B, 0x75, 0xFF);
        CLR_STATUSBAR = C2D_Color32(0x14, 0x17, 0x22, 0xFF);
    } else {
        CLR_BG_TOP = C2D_Color32(0xF0, 0xF4, 0xF5, 0xFF);
        CLR_BG_BOT = C2D_Color32(0xE6, 0xED, 0xF0, 0xFF);
        CLR_SURFACE = C2D_Color32(0xF8, 0xFA, 0xFA, 0xFF);
        CLR_SURFACE_LT = C2D_Color32(0xD7, 0xE7, 0xEB, 0xFF);
        CLR_TEXT = C2D_Color32(0x16, 0x32, 0x44, 0xFF);
        CLR_TEXT_SEC = C2D_Color32(0x48, 0x65, 0x72, 0xFF);
        CLR_TEXT_DIM = C2D_Color32(0x74, 0x8D, 0x98, 0xFF);
        CLR_ACCENT = C2D_Color32(0x2E, 0xA4, 0xB7, 0xFF);
        CLR_ACCENT2 = C2D_Color32(0xE9, 0x5B, 0x70, 0xFF);
        CLR_STATUSBAR = C2D_Color32(0xD8, 0xE5, 0xE8, 0xFF);
    }
}

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
    ASYNC_TIMEOUT,
    ASYNC_CANCELLED
} AsyncState;

/* Upper bound on how long the main loop waits for a worker thread to exit
 * before giving up and detaching it. Prevents a stuck network call (e.g.
 * after a long stream session) from freezing the whole UI forever. */
#define ASYNC_JOIN_TIMEOUT_NS 1000000LL  /* 1 ms; retry on the next frame */

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
    u32 load_generation;   /* bumped per load; lets an orphaned worker know
                              it was superseded and must not publish results */
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
    SCREEN_SETTINGS,
    SCREEN_STATION_INFO,
} AppScreen;

typedef struct {
    AppScreen screen;
    int main_tab;            /* 0 now playing, 1 discover, 2 settings */
    int selection;
    int scroll_offset;
    int prev_screen;
    int prev_selection;

    /* Tag data */
    RadioTag tags[MAX_TAGS];
    int tag_count;
    bool tags_loaded;

    /* Language data */
    RadioLanguage languages[MAX_TAGS];
    int language_count;
    bool languages_loaded;

    /* Station data */
    RadioStation stations[MAX_STATIONS];
    int station_count;
    int station_list_parent;  /* enum AppScreen; where B goes back to */
    RadioStation top_stations[MAX_STATIONS];
    int top_count;
    bool top_loaded;

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
    SearchInput search_input;
    int search_candidate_cursor;
    int search_candidate_page;
    bool search_candidate_focus;
    bool search_symbols;

    /* Runtime preferences */
    AppSettings settings;

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

static bool load_theme(UiTheme theme) {
    const char *path = theme == UI_THEME_DARK
        ? "romfs:/ui-skin-dark.png" : "romfs:/ui-skin-light.png";
    ui_skin_clear(&skin);
    ui_skin_init(&skin);
    apply_theme_palette(theme);
    return ui_skin_load(&skin, path);
}

/* ======================================================================
 * Drawing Primitives - Skin-based Apple-Light Style
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
    /* ClouDS-style active rail: it makes focus readable even when the skin
     * texture is dimmed by the dark theme or by a small 3DS screen. */
    C2D_DrawRectSolid(x, y + 4.0f, 0.5f, 3.0f, h - 8.0f, CLR_ACCENT);
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

/* ----------------------------------------------------------------------
 * ClouDS-style icon language
 * ----------------------------------------------------------------------
 * The atlas still supplies the small, polished system glyphs (search,
 * speaker, gear and keycaps). These vector marks fill the gaps that the old
 * UI represented with raw letters or a lonely "R". They are deliberately
 * built from rectangles/circles/triangles: no extra texture memory, crisp on
 * the 3DS and identical in light/dark themes.
 */
static void draw_icon_radio(float x, float y, float size, u32 color) {
    float r = size * 0.36f;
    C2D_DrawCircleSolid(x + size * 0.50f, y + size * 0.54f,
                        0.4f, r, color);
    C2D_DrawCircleSolid(x + size * 0.50f, y + size * 0.54f,
                        0.5f, r * 0.58f, CLR_SURFACE);
    C2D_DrawRectSolid(x + size * 0.18f, y + size * 0.76f,
                      0.5f, size * 0.64f, size * 0.10f, color);
    C2D_DrawRectSolid(x + size * 0.30f, y + size * 0.30f,
                      0.5f, size * 0.08f, size * 0.20f, color);
    C2D_DrawRectSolid(x + size * 0.46f, y + size * 0.20f,
                      0.5f, size * 0.08f, size * 0.30f, color);
    C2D_DrawRectSolid(x + size * 0.62f, y + size * 0.10f,
                      0.5f, size * 0.08f, size * 0.40f, color);
}

static void draw_icon_play(float x, float y, float size, u32 color) {
    C2D_DrawTriangle(x + size * 0.28f, y + size * 0.18f, color,
                     x + size * 0.28f, y + size * 0.82f, color,
                     x + size * 0.78f, y + size * 0.50f, color, 0.5f);
}

static void draw_icon_pause(float x, float y, float size, u32 color) {
    C2D_DrawRectSolid(x + size * 0.28f, y + size * 0.18f,
                      0.5f, size * 0.16f, size * 0.64f, color);
    C2D_DrawRectSolid(x + size * 0.56f, y + size * 0.18f,
                      0.5f, size * 0.16f, size * 0.64f, color);
}

static void draw_icon_stop(float x, float y, float size, u32 color) {
    C2D_DrawRectSolid(x + size * 0.24f, y + size * 0.24f,
                      0.5f, size * 0.52f, size * 0.52f, color);
}

static void draw_icon_globe(float x, float y, float size, u32 color) {
    C2D_DrawCircleSolid(x + size * 0.50f, y + size * 0.50f,
                        0.5f, size * 0.36f, color);
    C2D_DrawCircleSolid(x + size * 0.50f, y + size * 0.50f,
                        0.6f, size * 0.27f, CLR_SURFACE);
    C2D_DrawRectSolid(x + size * 0.16f, y + size * 0.46f,
                      0.5f, size * 0.68f, size * 0.08f, color);
    C2D_DrawRectSolid(x + size * 0.46f, y + size * 0.16f,
                      0.5f, size * 0.08f, size * 0.68f, color);
}

static void draw_icon_search(float x, float y, float size, u32 color) {
    C2D_DrawCircleSolid(x + size * 0.40f, y + size * 0.40f,
                        0.5f, size * 0.24f, color);
    C2D_DrawCircleSolid(x + size * 0.40f, y + size * 0.40f,
                        0.6f, size * 0.14f, CLR_SURFACE);
    C2D_DrawRectSolid(x + size * 0.58f, y + size * 0.58f,
                      0.5f, size * 0.28f, size * 0.10f, color);
}

static void draw_icon_list(float x, float y, float size, u32 color) {
    for (int row = 0; row < 3; row++) {
        float yy = y + size * (0.22f + row * 0.28f);
        C2D_DrawRectSolid(x + size * 0.10f, yy, 0.5f,
                          size * 0.10f, size * 0.10f, color);
        C2D_DrawRectSolid(x + size * 0.30f, yy, 0.5f,
                          size * 0.58f, size * 0.10f, color);
    }
}

static void draw_icon_gear(float x, float y, float size, u32 color) {
    C2D_DrawCircleSolid(x + size * 0.50f, y + size * 0.50f,
                        0.5f, size * 0.34f, color);
    C2D_DrawCircleSolid(x + size * 0.50f, y + size * 0.50f,
                        0.6f, size * 0.13f, CLR_SURFACE);
    for (int i = 0; i < 4; i++) {
        float xx = (i & 1) ? x + size * 0.86f : x + size * 0.06f;
        float yy = (i & 2) ? y + size * 0.86f : y + size * 0.06f;
        C2D_DrawRectSolid(xx, yy, 0.5f, size * 0.08f,
                          size * 0.08f, color);
    }
}

static void draw_icon_volume(float x, float y, float size, u32 color) {
    C2D_DrawTriangle(x + size * 0.16f, y + size * 0.42f, color,
                     x + size * 0.38f, y + size * 0.42f, color,
                     x + size * 0.38f, y + size * 0.18f, color, 0.5f);
    C2D_DrawRectSolid(x + size * 0.38f, y + size * 0.18f,
                      0.5f, size * 0.18f, size * 0.64f, color);
    C2D_DrawRectSolid(x + size * 0.68f, y + size * 0.30f,
                      0.5f, size * 0.08f, size * 0.40f, color);
}

static void draw_icon_chevron(float x, float y, float size, u32 color) {
    C2D_DrawRectSolid(x + size * 0.25f, y + size * 0.20f,
                      0.5f, size * 0.12f, size * 0.60f, color);
    C2D_DrawRectSolid(x + size * 0.25f, y + size * 0.20f,
                      0.5f, size * 0.48f, size * 0.12f, color);
}

static const char *stream_codec_name(StreamCodec codec) {
    switch (codec) {
        case STREAM_CODEC_MP3: return "MP3";
        case STREAM_CODEC_OGG: return "OGG/Vorbis";
        case STREAM_CODEC_AAC: return "AAC";
        default: return "?";
    }
}

static size_t utf8_codepoint_width(unsigned char first) {
    /* CJK glyphs are roughly two Latin columns at the bundled font size. */
    return first < 0x80 ? 1 : 2;
}

static size_t utf8_display_columns(const char *text) {
    size_t columns = 0;
    size_t length;
    if (!text) return 0;
    length = strlen(text);
    for (size_t i = 0; i < length; ) {
        unsigned char c = (unsigned char)text[i];
        size_t codepoint = (c < 0x80) ? 1 :
                           ((c & 0xE0) == 0xC0) ? 2 :
                           ((c & 0xF0) == 0xE0) ? 3 : 4;
        if (i + codepoint > length) break;
        i += codepoint;
        columns += utf8_codepoint_width(c);
    }
    return columns;
}

static void copy_utf8_ellipsis(char *dst, size_t dst_size,
                               const char *src, size_t max_columns) {
    size_t used = 0;
    size_t columns = 0;
    size_t len;
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;
    len = strlen(src);

    /* Preserve the complete string when it fits both the byte buffer and the
     * approximate pixel-width budget. */
    if (len < dst_size) {
        size_t scan = 0;
        size_t scan_columns = 0;
        while (scan < len) {
            unsigned char c = (unsigned char)src[scan];
            size_t codepoint = (c < 0x80) ? 1 :
                               ((c & 0xE0) == 0xC0) ? 2 :
                               ((c & 0xF0) == 0xE0) ? 3 : 4;
            if (scan + codepoint > len) break;
            scan += codepoint;
            scan_columns += utf8_codepoint_width(c);
        }
        if (scan == len && scan_columns <= max_columns) {
            memcpy(dst, src, len + 1);
            return;
        }
    }

    if (max_columns <= 3) max_columns = 4;
    max_columns -= 3;
    while (used < len && columns < max_columns) {
        unsigned char c = (unsigned char)src[used];
        size_t codepoint = (c < 0x80) ? 1 :
                           ((c & 0xE0) == 0xC0) ? 2 :
                           ((c & 0xF0) == 0xE0) ? 3 : 4;
        size_t width = utf8_codepoint_width(c);
        if (used + codepoint > len || columns + width > max_columns) break;
        used += codepoint;
        columns += width;
    }
    if (used + 3 >= dst_size) used = dst_size - 4;
    memcpy(dst, src, used);
    memcpy(dst + used, "...", 4);
}

/* Status bar at bottom of top screen */
static void draw_status_bar(void) {
    select_top();
    ui_skin_draw_nine_slice(&skin, UI_SKIN_FOOTER,
        0, TOP_HEIGHT - 26, 0.5f, TOP_WIDTH, 26, 8U, 4.0f);
    if (!skin.ready) {
        C2D_DrawRectSolid(0, TOP_HEIGHT - 26, 0.5f, TOP_WIDTH, 26, CLR_STATUSBAR);
    }

    if (skin.ready)
        ui_skin_draw_tinted(&skin, UI_SKIN_FOOTER_SPEAKER,
                            7, TOP_HEIGHT - 22, 0.4f, 16, 16, CLR_TEXT_DIM);
    draw_label(29, TOP_HEIGHT - 21, 0.40f, CLR_TEXT_DIM, "3DSRadio");

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
        char status[80];
        copy_utf8_ellipsis(status, sizeof(status), app.status_text, 28);
        float tw = (float)utf8_display_columns(status) * 7.0f;
        draw_label((TOP_WIDTH - tw) / 2.0f, TOP_HEIGHT - 21, 0.5f, c,
                   "%s", status);
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
    clear_top();
    draw_gradient(0, 0, TOP_WIDTH, TOP_HEIGHT,
                  app.settings.theme == UI_THEME_DARK ? 0x111827FF : 0xE3EFF1FF,
                  app.settings.theme == UI_THEME_DARK ? 0x0B0D12FF : 0xF4E5E0FF);
    C2D_DrawRectSolid(0, 0, 0.2f, TOP_WIDTH, 4, CLR_ACCENT2);
    draw_icon_radio(26, 38, 84, CLR_ACCENT);
    draw_label(132, 45, 0.82f, CLR_TEXT, "%s", title);
    if (subtitle)
        draw_label(134, 76, 0.40f, CLR_TEXT_SEC, "%s", subtitle);
    for (int i = 0; i < 10; i++) {
        float h = 4.0f + (float)((i * 9) % 18);
        C2D_DrawRectSolid(132 + i * 19, 143 - h, 0.3f,
                          10, h, i == 5 ? CLR_ACCENT2 : CLR_ACCENT);
    }
    draw_status_bar();
}

static const char *discover_label(void) {
    return locale_get_language() == LANG_ZH_CN ? "发现" : "Discover";
}

static void draw_app_nav(void) {
    /* The reference project treats the bottom screen as an application shell:
     * brand at left, three stable destinations, and a thin active rail. */
    select_bottom();
    C2D_DrawRectSolid(0, 0, 0.1f, BOT_WIDTH, 29, CLR_SURFACE);
    C2D_DrawRectSolid(0, 28, 0.2f, BOT_WIDTH, 1, CLR_SURFACE_LT);

    draw_label(8, 6, 0.43f, CLR_TEXT, "3DSRadio");

    const char *tabs[] = {tr_now_playing(), discover_label(),
                          tr_settings_header()};
    const int starts[] = {91, 164, 254};
    const int widths[] = {68, 80, 62};
    for (int i = 0; i < 3; i++) {
        bool active = app.main_tab == i;
        float text_width = (float)utf8_display_columns(tabs[i]) * 6.0f;
        draw_label(starts[i] + (widths[i] - text_width) * 0.5f, 7,
                   0.34f, active ? CLR_TEXT : CLR_TEXT_DIM, "%s", tabs[i]);
        if (active)
            C2D_DrawRectSolid(starts[i] + 5, 26, 0.3f,
                              widths[i] - 10, 3, CLR_ACCENT2);
    }
}

static void draw_footer_hints(const char *left, const char *right) {
    select_bottom();
    C2D_DrawRectSolid(0, BOT_HEIGHT - 25, 0.2f,
                      BOT_WIDTH, 25, CLR_STATUSBAR);
    if (skin.ready) {
        ui_skin_draw_tinted(&skin, UI_SKIN_KEY_A, 9, BOT_HEIGHT - 20,
                            0.4f, 14, 14, CLR_ACCENT);
        ui_skin_draw_tinted(&skin, UI_SKIN_KEY_B, 27, BOT_HEIGHT - 20,
                            0.4f, 14, 14, CLR_ACCENT2);
    }
    draw_label(47, BOT_HEIGHT - 19, 0.30f, CLR_TEXT_DIM, "%s", left);
    if (right && right[0])
        draw_label(190, BOT_HEIGHT - 19, 0.30f, CLR_TEXT_DIM, "%s", right);
}

static void draw_top_brand(void) {
    select_top();
    clear_top();
    /* Quiet horizontal bands keep the screen legible while giving it the
     * soft, illustrated atmosphere of the reference home screen. */
    draw_gradient(0, 0, TOP_WIDTH, TOP_HEIGHT,
                  app.settings.theme == UI_THEME_DARK ? 0x111827FF : 0xE7F1F3FF,
                  app.settings.theme == UI_THEME_DARK ? 0x0B0D12FF : 0xF6E6E0FF);
    for (int i = 0; i < 7; i++) {
        float x = 26.0f + i * 58.0f;
        float h = 10.0f + (float)((i * 13) % 28);
        C2D_DrawRectSolid(x, 156.0f - h, 0.3f, 20.0f, h,
                          i == 3 ? CLR_ACCENT2 : CLR_ACCENT);
    }
    draw_icon_radio(160, 30, 80, CLR_ACCENT);
    draw_label(20, 156, 1.15f, CLR_TEXT, "3DSRadio");
    draw_label(22, 186, 0.43f, CLR_TEXT_SEC, "%s", tr_main_subtitle());
    draw_label(22, 207, 0.32f, CLR_TEXT_DIM, "radio-browser.info  ·  v1.1");
}

static void draw_current_station_hero(void) {
    select_top();
    clear_top();
    draw_gradient(0, 0, TOP_WIDTH, TOP_HEIGHT,
                  app.settings.theme == UI_THEME_DARK ? 0x0D1720FF : 0xE5F0F1FF,
                  app.settings.theme == UI_THEME_DARK ? 0x12121AFF : 0xF6E2DFFF);
    draw_icon_radio(28, 34, 74, CLR_ACCENT);
    draw_label(122, 40, 0.35f, CLR_ACCENT2, "%s", tr_now_playing());
    char station[96];
    copy_utf8_ellipsis(station, sizeof(station), app.current_station->name, 30);
    draw_label(122, 62, 0.88f, CLR_TEXT, "%s", station);
    char details[120];
    snprintf(details, sizeof(details), "%s  ·  %d kbps",
             app.current_station->codec[0] ? app.current_station->codec : "STREAM",
             app.current_station->bitrate);
    draw_label(122, 91, 0.38f, CLR_TEXT_SEC, "%s", details);

    StreamPlayerState state = app.stream_player
        ? stream_player_get_state(app.stream_player) : STREAM_STATE_ERROR;
    bool active = state == STREAM_STATE_PLAYING;
    int bar_count = 28;
    for (int i = 0; i < bar_count; i++) {
        float phase = (float)(i * 7 + app.frame_count * 3);
        float height = active ? 5.0f + (sinf(phase * 0.09f) + 1.0f) * 11.0f : 4.0f;
        C2D_DrawRectSolid(22 + i * 13, 143 - height, 0.3f, 8, height,
                          i % 5 == 0 ? CLR_ACCENT2 : CLR_ACCENT);
    }
    u32 state_color = active ? CLR_OK : CLR_WARN;
    const char *state_text = active ? tr_playing() : tr_paused();
    if (state == STREAM_STATE_BUFFERING) {
        state_color = CLR_WARN;
        state_text = tr_buffering();
    } else if (state == STREAM_STATE_ERROR) {
        state_color = CLR_ERR;
        state_text = tr_stream_failed();
    }
    C2D_DrawCircleSolid(24, 184, 0.4f, 4, state_color);
    draw_label(34, 178, 0.42f, state_color, "%s", state_text);
    draw_label(245, 178, 0.35f, CLR_TEXT_DIM, "%s",
               tr_volume_level((int)(app.volume * 100)));
    draw_status_bar();
}

/* ======================================================================
 * Screen: Main Menu
 * ====================================================================== */

static void render_settings(void);

static void render_main_menu(void) {
    if (app.main_tab == 0 && app.current_station)
        draw_current_station_hero();
    else
        draw_top_brand();

    select_bottom();
    clear_bottom();
    draw_app_nav();

    if (app.main_tab == 0) {
        /* Now Playing is intentionally calm when no stream is active. */
        draw_panel(12, 48, BOT_WIDTH - 24, 106);
        draw_icon_play(32, 69, 36, CLR_ACCENT);
        draw_label(82, 64, 0.55f, CLR_TEXT, "%s", tr_now_playing());
        if (app.current_station) {
            draw_label(82, 88, 0.42f, CLR_TEXT_SEC, "%s",
                       app.current_station->name);
            draw_label(82, 110, 0.32f, CLR_TEXT_DIM, "%s",
                       tr_select_station());
        } else {
            draw_label(82, 91, 0.42f, CLR_TEXT_SEC, "%s",
                       locale_get_language() == LANG_ZH_CN ?
                       "还没有开始播放" : "Nothing is playing");
            draw_label(82, 114, 0.32f, CLR_TEXT_DIM, "%s",
                       locale_get_language() == LANG_ZH_CN ?
                       "切换到发现，选择一个电台" :
                       "Open Discover to choose a station");
        }
        draw_footer_hints("A 播放页", "← → 切换");
        return;
    }

    if (app.main_tab == 2) {
        render_settings();
        return;
    }

    /* Discover dashboard: four clear destinations instead of five cramped
     * text rows. The information hierarchy mirrors the reference app: one
     * heading, two-up cards, one persistent footer. */
    draw_label(14, 39, 0.36f, CLR_ACCENT, "%s", discover_label());
    draw_label(14, 54, 0.30f, CLR_TEXT_DIM,
               locale_get_language() == LANG_ZH_CN ?
               "按风格、语言或热门电台探索" :
               "Explore by mood, language or popularity");

    const char *labels[] = {tr_menu_browse_genre(), tr_menu_browse_language(),
                            tr_menu_top_stations(), tr_menu_search()};
    const char *subtitles[] = {"GENRE", "LANGUAGE", "POPULAR", "FIND"};
    for (int i = 0; i < 4; i++) {
        int col = i & 1;
        int row = i >> 1;
        float x = 10.0f + col * 152.0f;
        float y = 72.0f + row * 53.0f;
        bool selected = app.selection == i;
        draw_button(x, y, 144, 45, selected);
        if (i == 0) draw_icon_list(x + 12, y + 9, 24, selected ? CLR_ACCENT : CLR_TEXT_SEC);
        if (i == 1) draw_icon_globe(x + 12, y + 9, 24, selected ? CLR_ACCENT : CLR_TEXT_SEC);
        if (i == 2) draw_icon_radio(x + 12, y + 9, 24, selected ? CLR_ACCENT : CLR_TEXT_SEC);
        if (i == 3) draw_icon_search(x + 12, y + 9, 24, selected ? CLR_ACCENT : CLR_TEXT_SEC);
        draw_label(x + 44, y + 8, 0.35f,
                   selected ? CLR_TEXT : CLR_TEXT_SEC, "%s", subtitles[i]);
        draw_label(x + 44, y + 24, 0.30f,
                   selected ? CLR_ACCENT : CLR_TEXT_DIM, "%s", labels[i]);
        draw_icon_chevron(x + 126, y + 16, 11,
                          selected ? CLR_ACCENT2 : CLR_TEXT_DIM);
    }
    draw_footer_hints("A 打开", "L/R 切换页签");
}

/* ======================================================================
 * Screen: Tag List
 * ====================================================================== */

static void draw_empty_list_state(void) {
    draw_panel(12, 82, BOT_WIDTH - 24, 64);
    draw_icon_search(26, 97, 24, CLR_WARN);
    draw_label(62, 96, 0.42f, CLR_WARN, "%s", tr_no_stations());
    draw_label(62, 118, 0.30f, CLR_TEXT_DIM,
               locale_get_language() == LANG_ZH_CN ?
               "请返回并换一个筛选条件" : "Go back and try another filter");
}

static void render_tag_list(void) {
    draw_hero_header(tr_genre_header(), tr_genre_subtitle());

    select_bottom();
    clear_bottom();
    draw_app_nav();

    draw_label(15, 37, 0.36f, CLR_ACCENT, "%s", tr_genres_available(app.tag_count));

    if (app.tag_count <= 0) {
        draw_empty_list_state();
        draw_status_bar();
        return;
    }

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.tag_count) end = app.tag_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 56 + idx * 23;
        bool sel = (i == app.selection);

        if (sel) {
            draw_selection(5, y - 3, BOT_WIDTH - 10, 22);
        }

        char label[128];
        copy_utf8_ellipsis(label, sizeof(label), app.tags[i].name, 26);
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", app.tags[i].stationcount);

        draw_icon_list(9, y - 1, 15, sel ? CLR_ACCENT : CLR_TEXT_DIM);
        draw_label(30, y, 0.44f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", label);
        draw_label(BOT_WIDTH - 50, y, 0.4f, CLR_TEXT_DIM, "%s", count_str);
    }

    draw_footer_hints("A 打开", "B 返回");
}

/* ======================================================================
 * Screen: Language List
 * ====================================================================== */

static void render_language_list(void) {
    draw_hero_header(tr_language_header(), tr_language_subtitle());

    select_bottom();
    clear_bottom();
    draw_app_nav();

    draw_label(15, 37, 0.36f, CLR_ACCENT, "%s", tr_languages_available(app.language_count));

    if (app.language_count <= 0) {
        draw_empty_list_state();
        draw_status_bar();
        return;
    }

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.language_count) end = app.language_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 56 + idx * 23;
        bool sel = (i == app.selection);

        if (sel) {
            draw_selection(5, y - 3, BOT_WIDTH - 10, 22);
        }

        char label[128];
        copy_utf8_ellipsis(label, sizeof(label), app.languages[i].name, 26);
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", app.languages[i].stationcount);

        draw_icon_globe(9, y - 1, 15, sel ? CLR_ACCENT : CLR_TEXT_DIM);
        draw_label(30, y, 0.44f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", label);
        draw_label(BOT_WIDTH - 50, y, 0.4f, CLR_TEXT_DIM, "%s", count_str);
    }

    draw_footer_hints("A 打开", "B 返回");
}

/* ======================================================================
 * Screen: Station List
 * ====================================================================== */

static void render_station_list(void) {
    draw_hero_header(tr_stations_header(), tr_stations_found(app.station_count));

    select_bottom();
    clear_bottom();
    draw_app_nav();

    if (app.station_count <= 0) {
        draw_empty_list_state();
        draw_status_bar();
        return;
    }

    int start = app.scroll_offset;
    int end = start + MAX_VISIBLE_ITEMS;
    if (end > app.station_count) end = app.station_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int y = 37 + idx * 25;
        bool sel = (i == app.selection);

        if (sel) {
            draw_selection(3, y - 1, BOT_WIDTH - 6, 24);
        }

        RadioStation *s = &app.stations[i];

        /* Station name — keep it clear of the bitrate badge on the right */
        char name_buf[64];
        copy_utf8_ellipsis(name_buf, sizeof(name_buf), s->name, 24);
        draw_icon_radio(7, y + 1, 17, sel ? CLR_ACCENT : CLR_TEXT_DIM);
        draw_label(30, y + 2, 0.42f, sel ? CLR_TEXT : CLR_TEXT_SEC, "%s", name_buf);

        /* Codec + bitrate badge.  Unsupported formats remain visible but are
         * clearly marked so A never leads to a silent playback screen. */
        if (s->codec[0] || s->bitrate > 0) {
            char badge[24];
            const bool supported = stream_player_codec_supported(s->codec);
            const u32 badge_color = supported ? CLR_ACCENT : CLR_WARN;
            if (s->bitrate > 0)
                snprintf(badge, sizeof(badge), "%.8s %d", s->codec[0] ? s->codec : "?", s->bitrate);
            else
                snprintf(badge, sizeof(badge), "%.8s", s->codec[0] ? s->codec : "?");
            draw_label(BOT_WIDTH - 82, y + 2, 0.32f, badge_color, "%s", badge);
        }
    }

    draw_footer_hints("A 播放", "Y 详情  ·  B 返回");
}

/* ======================================================================
 * Screen: Now Playing
 * ====================================================================== */

static void render_playing(void) {
    StreamPlayerState player_state = stream_player_get_state(app.stream_player);
    const char *state_label = tr_paused();
    u32 state_color = CLR_WARN;
    if (player_state == STREAM_STATE_PLAYING) {
        state_label = tr_now_playing();
        state_color = CLR_OK;
    } else if (player_state == STREAM_STATE_BUFFERING) {
        state_label = tr_buffering();
        state_color = CLR_WARN;
    } else if (player_state == STREAM_STATE_RECONNECTING) {
        state_label = tr_reconnecting();
        state_color = CLR_INFO;
    } else if (player_state == STREAM_STATE_ERROR) {
        state_label = stream_player_error(app.stream_player);
        if (!state_label || !state_label[0]) state_label = tr_stream_failed();
        state_color = CLR_ERR;
    } else if (player_state == STREAM_STATE_CONNECTING) {
        state_label = tr_connecting_stream();
        state_color = CLR_INFO;
    } else if (player_state == STREAM_STATE_ENDED) {
        state_label = tr_stream_ended();
        state_color = CLR_WARN;
    }

    select_top();
    clear_top();

    if (!app.current_station) return;

    /* Large station name at top, clipped on UTF-8 boundaries. */
    char station_title[80];
    copy_utf8_ellipsis(station_title, sizeof(station_title),
                       app.current_station->name, 38);
    draw_label(20, 30, 1.2f, CLR_TEXT, "%s", station_title);

    /* Tags as chips using skin button */
    if (strlen(app.current_station->tags) > 0) {
        char first_tag[32] = {0};
        const char *comma = strchr(app.current_station->tags, ',');
        if (comma) {
            char tag_segment[64];
            size_t len = (size_t)(comma - app.current_station->tags);
            if (len >= sizeof(tag_segment)) len = sizeof(tag_segment) - 1;
            memcpy(tag_segment, app.current_station->tags, len);
            tag_segment[len] = '\0';
            copy_utf8_ellipsis(first_tag, sizeof(first_tag), tag_segment, 18);
        } else {
            copy_utf8_ellipsis(first_tag, sizeof(first_tag),
                               app.current_station->tags, 18);
        }
        if (strlen(first_tag) > 0) {
            float tag_w = (float)utf8_display_columns(first_tag) * 7.0f + 16.0f;
            if (tag_w > TOP_WIDTH - 40) tag_w = TOP_WIDTH - 40;
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
    char info_clipped[128];
    copy_utf8_ellipsis(info_clipped, sizeof(info_clipped), info, 54);
    draw_label(20, 96, 0.5f, CLR_TEXT_SEC, "%s", info_clipped);

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
        if (player_state != STREAM_STATE_PLAYING) height = 2.0f;

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
        player_state == STREAM_STATE_PLAYING ? UI_SKIN_DOT_GREEN : UI_SKIN_DOT_ORANGE,
        20, TOP_HEIGHT - 52, 0.5f, 12, 12,
        state_color);
    draw_label(38, TOP_HEIGHT - 49, 0.55f, state_color, "%s", state_label);

    /* Volume */
    draw_label(TOP_WIDTH - 90, TOP_HEIGHT - 49, 0.45f, CLR_TEXT_DIM,
               "%s", tr_volume_level((int)(app.volume * 100)));

    /* Progress bar using skin */
    select_top();
    ui_skin_draw_nine_slice(&skin, UI_SKIN_PROGRESS,
        20, TOP_HEIGHT - 32, 0.5f, TOP_WIDTH - 40, 6, 4U, 2.0f);
    int buffer_percent = stream_player_get_buffer_percent(app.stream_player);
    if (buffer_percent > 0) {
        if (buffer_percent > 100) buffer_percent = 100;
        C2D_DrawRectSolid(22, TOP_HEIGHT - 31, 0.5f,
                          (TOP_WIDTH - 44) * buffer_percent / 100.0f,
                          4.0f, state_color);
    }

    /* Bottom screen: controls */
    select_bottom();
    clear_bottom();
    C2D_DrawRectSolid(0, 0, 0.1f, BOT_WIDTH, 4, CLR_ACCENT2);
    draw_label(14, 12, 0.34f, CLR_ACCENT, "%s", tr_controls());
    draw_panel(10, 31, BOT_WIDTH - 20, 32);
    draw_icon_radio(18, 37, 22, CLR_ACCENT);
    char mini_name[64];
    copy_utf8_ellipsis(mini_name, sizeof(mini_name),
                       app.current_station->name, 25);
    draw_label(50, 37, 0.40f, CLR_TEXT, "%s", mini_name);
    draw_label(50, 51, 0.28f, CLR_TEXT_DIM, "%s", state_label);

    struct { const char *label; u32 color; } controls[] = {
        {tr_play_pause(), CLR_ACCENT}, {tr_stop_back(), CLR_ACCENT2},
        {tr_vol_down(), CLR_TEXT_SEC}, {tr_vol_up(), CLR_ACCENT2},
    };
    for (int i = 0; i < 4; i++) {
        int x = 8 + i * 78;
        int y = 72;
        bool selected = i == 0 && (player_state == STREAM_STATE_PLAYING ||
                                   player_state == STREAM_STATE_PAUSED);
        draw_button(x, y, 72, 52, selected);
        if (i == 0) {
            if (player_state == STREAM_STATE_PLAYING) draw_icon_pause(x + 25, y + 7, 23, controls[i].color);
            else draw_icon_play(x + 25, y + 7, 23, controls[i].color);
        } else if (i == 1) draw_icon_stop(x + 25, y + 7, 23, controls[i].color);
        else if (i == 2) draw_icon_volume(x + 24, y + 7, 24, controls[i].color);
        else draw_icon_volume(x + 24, y + 7, 24, controls[i].color);
        draw_label(x + 8, y + 34, 0.28f, controls[i].color, "%s", controls[i].label);
    }

    /* Diagnostic panel is useful when a stream is weak or unsupported, but
     * does not expose a long URL as the primary playback information. */
    const char *codec_name = stream_codec_name(
        stream_player_get_codec(app.stream_player));
    if (stream_player_get_codec(app.stream_player) == STREAM_CODEC_UNKNOWN &&
        app.current_station->codec[0])
        codec_name = app.current_station->codec;
    draw_panel(8, 132, BOT_WIDTH - 16, 49);
    draw_label(15, 140, 0.32f, CLR_TEXT_DIM, "%s: %s", tr_codec(), codec_name);
    draw_label(15, 154, 0.32f, state_color, "%s", state_label);
    if (player_state == STREAM_STATE_PLAYING ||
        player_state == STREAM_STATE_PAUSED) {
        draw_label(180, 140, 0.30f, CLR_TEXT_DIM, "%d Hz / %s",
                   stream_player_get_sample_rate(app.stream_player),
                   stream_player_get_channels(app.stream_player) == 1 ? "mono" : "stereo");
    }
    if (player_state == STREAM_STATE_BUFFERING ||
        player_state == STREAM_STATE_RECONNECTING)
        draw_label(180, 154, 0.32f, CLR_TEXT_DIM, "%d%%",
                   stream_player_get_buffer_percent(app.stream_player));
    if (player_state == STREAM_STATE_ERROR)
        draw_label(180, 154, 0.30f, CLR_ERR, "%s", tr_retry());

    draw_footer_hints("A 播放/暂停", "B 返回  ·  X/Y 音量");
}

/* ======================================================================
 * Screen: Search
 * ====================================================================== */

static const char *SEARCH_LETTER_ROWS[] = {
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
};
#define SEARCH_KEY_ROW_COUNT 3
#define SEARCH_ACTION_COUNT 5
#define SEARCH_CANDIDATES_PER_PAGE 4

static const char *SEARCH_SYMBOL_ROWS[] = {
    "1234567890",
    "-/:;()$&@\"",
    ".,?!'#+_%",
};

static const char *search_key_row(int row) {
    if (row < 0 || row >= SEARCH_KEY_ROW_COUNT) return "";
    return app.search_symbols ? SEARCH_SYMBOL_ROWS[row] : SEARCH_LETTER_ROWS[row];
}

static int search_character_key_count(void) {
    int count = 0;
    for (int i = 0; i < SEARCH_KEY_ROW_COUNT; i++)
        count += (int)strlen(search_key_row(i));
    return count;
}

static bool search_character_key_at(int index, char *out) {
    for (int i = 0; i < SEARCH_KEY_ROW_COUNT; i++) {
        const char *keys = search_key_row(i);
        int row_len = (int)strlen(keys);
        if (index < row_len) {
            if (out) *out = keys[index];
            return true;
        }
        index -= row_len;
    }
    return false;
}

static void render_search(void) {
    draw_hero_header(tr_search_header(), tr_search_prompt());

    select_bottom();
    clear_bottom();
    draw_app_nav();

    draw_panel(8, 34, BOT_WIDTH - 16, 25);
    draw_label(16, 40, 0.38f, CLR_ACCENT, "%s",
               search_input_query(&app.search_input));
    if (search_input_composition(&app.search_input)[0])
        draw_label(16 + (float)utf8_display_columns(
                   search_input_query(&app.search_input)) * 7.0f,
                   40, 0.36f, CLR_ACCENT2, "%s",
                   search_input_composition(&app.search_input));
    if (!search_input_query(&app.search_input)[0] &&
        !search_input_composition(&app.search_input)[0])
        draw_label(16, 40, 0.34f, CLR_TEXT_DIM, "%s", tr_search_hint());

    if (app.search_query[0] && app.station_count == 0) {
        draw_label(16, 63, 0.28f, CLR_WARN, "%s  %s",
                   tr_no_stations(), tr_search_action());
    }

    draw_panel(8, 63, BOT_WIDTH - 16, 24);
    int candidate_count = search_input_candidate_count(&app.search_input);
    int page_count = (candidate_count + SEARCH_CANDIDATES_PER_PAGE - 1) /
                     SEARCH_CANDIDATES_PER_PAGE;
    if (page_count > 0 && app.search_candidate_page >= page_count)
        app.search_candidate_page = page_count - 1;
    int page_start = app.search_candidate_page * SEARCH_CANDIDATES_PER_PAGE;
    int page_end = page_start + SEARCH_CANDIDATES_PER_PAGE;
    if (page_end > candidate_count) page_end = candidate_count;
    float candidate_x = 12.0f;
    for (int i = page_start; i < page_end; i++) {
        const char *candidate = search_input_candidate(&app.search_input, i);
        char candidate_text[32];
        copy_utf8_ellipsis(candidate_text, sizeof(candidate_text), candidate, 8);
        float width = 22.0f + (float)utf8_display_columns(candidate_text) * 8.0f;
        if (width < 42.0f) width = 42.0f;
        if (width > 78.0f) width = 78.0f;
        if (candidate_x + width > BOT_WIDTH - 8.0f) break;
        bool selected = i == app.search_candidate_cursor;
        if (selected && app.search_candidate_focus)
            draw_selection(candidate_x, 65, width - 2.0f, 19);
        draw_label(candidate_x + 7.0f, 69, 0.32f,
                   selected ? CLR_ACCENT : CLR_TEXT_SEC, "%s", candidate_text);
        candidate_x += width + 3.0f;
    }

    if (candidate_count > 0)
        draw_label(274, 69, 0.26f, CLR_TEXT_DIM, "%d/%d",
                   app.search_candidate_page + 1, page_count);

    int cursor = 0;
    for (int row = 0; row < SEARCH_KEY_ROW_COUNT; row++) {
        const char *keys = search_key_row(row);
        int key_count = (int)strlen(keys);
        float key_w = 29.0f;
        float start_x = row == 0 ? 4.0f : (row == 1 ? 19.0f : 50.0f);
        for (int col = 0; col < key_count; col++, cursor++) {
            int x = (int)(start_x + col * key_w);
            int y = 91 + row * 27;
            bool selected = !app.search_candidate_focus && cursor == app.search_cursor;
            draw_button((float)x, (float)y, key_w - 2, 29, selected);
            draw_label((float)x + 8, (float)y + 7, 0.38f,
                       selected ? CLR_ACCENT : CLR_TEXT_SEC, "%c", keys[col]);
        }
    }

    int action_base = search_character_key_count();
    const char *actions[] = {"拼音", "符号", "空格", "清除", "搜索"};
    const int action_x[] = {4, 76, 124, 192, 248};
    const int action_w[] = {68, 44, 64, 52, 68};
    const u32 action_colors[] = {CLR_ACCENT2, CLR_TEXT_SEC, CLR_TEXT_SEC,
                                 CLR_WARN, CLR_ACCENT};
    for (int i = 0; i < SEARCH_ACTION_COUNT; i++) {
        bool selected = !app.search_candidate_focus &&
                        app.search_cursor == action_base + i;
        draw_button((float)action_x[i], 174, (float)action_w[i], 34, selected);
        draw_label((float)action_x[i] + 8, 184, 0.30f,
                   selected ? CLR_ACCENT : action_colors[i], "%s", actions[i]);
    }

    draw_footer_hints("A 输入", "B 退格  ·  Y 符号");
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

    char station_name[100];
    copy_utf8_ellipsis(station_name, sizeof(station_name), s->name, 30);
    draw_hero_header(tr_station_info(), station_name);

    /* Info card using panel skin */
    select_top();
    draw_panel(10, 106, TOP_WIDTH - 20, 96);

    int y = 115;
    draw_label(20, y, 0.36f, CLR_TEXT_SEC, "%s", tr_country());
    char country[64];
    copy_utf8_ellipsis(country, sizeof(country),
                       s->country[0] ? s->country : tr_na(), 22);
    draw_label(112, y, 0.36f, CLR_TEXT, "%s", country);

    y += 20;
    draw_label(20, y, 0.36f, CLR_TEXT_SEC, "%s", tr_codec());
    draw_label(112, y, 0.36f, CLR_ACCENT, "%s", s->codec[0] ? s->codec : tr_na());

    y += 20;
    draw_label(20, y, 0.36f, CLR_TEXT_SEC, "%s", tr_bitrate());
    draw_label(112, y, 0.36f, CLR_TEXT, "%d kbps", s->bitrate);

    y += 20;
    draw_label(20, y, 0.36f, CLR_TEXT_SEC, "%s", tr_language());
    char language[64];
    copy_utf8_ellipsis(language, sizeof(language),
                       s->language[0] ? s->language : tr_na(), 22);
    draw_label(112, y, 0.36f, CLR_TEXT, "%s", language);

    select_bottom();
    clear_bottom();
    draw_app_nav();
    draw_label(14, 38, 0.36f, CLR_ACCENT, "%s", tr_station_details());

    /* Tags section */
    if (strlen(s->tags) > 0) {
        draw_panel(8, 52, BOT_WIDTH - 16, 50);
        draw_label(15, 58, 0.35f, CLR_TEXT_DIM, "%s", tr_tags());
            char tags_buf[120];
            copy_utf8_ellipsis(tags_buf, sizeof(tags_buf), s->tags, 42);
            draw_label(15, 72, 0.36f, CLR_ACCENT, "%s", tags_buf);
    }

    draw_panel(8, 112, BOT_WIDTH - 16, 42);
    draw_label(15, 120, 0.32f, CLR_TEXT_DIM, "%s", tr_votes_clicks(s->votes, s->clickcount));
    draw_label(15, 136, 0.30f, CLR_TEXT_DIM, "%s",
               locale_get_language() == LANG_ZH_CN ?
               "A 播放此电台" : "A play this station");

    draw_footer_hints("A 播放", "B 返回");
}

static const char *settings_theme_label(void) {
    return app.settings.theme == UI_THEME_DARK ? tr_theme_dark() : tr_theme_light();
}

static const char *settings_language_label(void) {
    switch (app.settings.language) {
        case LANG_EN: return tr_language_english();
        case LANG_ZH_CN: return tr_language_chinese();
        default: return tr_language_auto();
    }
}

static void render_settings(void) {
    draw_hero_header(tr_settings_header(), tr_about_tagline());

    select_bottom();
    clear_bottom();
    draw_app_nav();
    draw_label(14, 38, 0.36f, CLR_ACCENT, "%s", tr_settings_header());

    const char *labels[] = {tr_theme(), tr_buffer_size(), tr_language(), tr_about_title()};
    const char *values[] = {
        settings_theme_label(),
        app.settings.buffer_size == STREAM_BUF_SMALL ? tr_buffer_small() :
            app.settings.buffer_size == STREAM_BUF_LARGE ? tr_buffer_large() : tr_buffer_medium(),
        settings_language_label(),
        "v1.1",
    };

    for (int i = 0; i < 4; i++) {
        int y = 54 + i * 36;
        bool selected = app.selection == i;
        draw_button(10, y, BOT_WIDTH - 20, 31, selected);
        if (i == 0) draw_icon_gear(20, y + 5, 20, selected ? CLR_ACCENT : CLR_TEXT_DIM);
        if (i == 1) draw_icon_volume(20, y + 5, 20, selected ? CLR_ACCENT : CLR_TEXT_DIM);
        if (i == 2) draw_icon_globe(20, y + 5, 20, selected ? CLR_ACCENT : CLR_TEXT_DIM);
        if (i == 3) draw_icon_radio(20, y + 5, 20, selected ? CLR_ACCENT : CLR_TEXT_DIM);
        draw_label(49, y + 6, 0.42f, selected ? CLR_TEXT : CLR_TEXT_SEC,
                   "%s", labels[i]);
        draw_label(BOT_WIDTH - 126, y + 6, 0.36f, selected ? CLR_ACCENT : CLR_TEXT_DIM,
                   "%s", values[i]);
        draw_icon_chevron(BOT_WIDTH - 22, y + 10, 9,
                          selected ? CLR_ACCENT2 : CLR_TEXT_DIM);
    }
    draw_footer_hints("← → 修改", "B 返回");
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
            bool activate = (kDown & KEY_A) != 0;
            if (kDown & KEY_LEFT)
                app.main_tab = (app.main_tab + 2) % 3;
            if (kDown & KEY_RIGHT)
                app.main_tab = (app.main_tab + 1) % 3;

            if (touch_active && touch.py < 31) {
                if (touch.px >= 88 && touch.px < 164) app.main_tab = 0;
                else if (touch.px >= 164 && touch.px < 249) app.main_tab = 1;
                else if (touch.px >= 249) app.main_tab = 2;
                app.selection = 0;
                if (app.main_tab == 2) {
                    app.screen = SCREEN_SETTINGS;
                    app.selection = 0;
                }
            }

            if (app.main_tab == 1) {
                if (kDown & KEY_DOWN) app.selection = (app.selection + 1) % 4;
                if (kDown & KEY_UP) app.selection = (app.selection + 3) % 4;
                if (touch_active && touch.py >= 68 && touch.py < 180) {
                    int col = touch.px >= 160 ? 1 : 0;
                    int row = touch.py >= 125 ? 1 : 0;
                    app.selection = row * 2 + col;
                    activate = true;
                }
                if (activate) {
                    switch (app.selection) {
                        case 0:
                            if (app.tags_loaded) {
                                app.selection = 0;
                                app.scroll_offset = 0;
                                app.screen = SCREEN_TAG_LIST;
                            } else async_launch_load(ASYNC_REQ_LOAD_TAGS, "");
                            break;
                        case 1:
                            if (app.languages_loaded) {
                                app.selection = 0;
                                app.scroll_offset = 0;
                                app.screen = SCREEN_LANGUAGE_LIST;
                            } else async_launch_load(ASYNC_REQ_LOAD_LANGUAGES, "");
                            break;
                        case 2:
                            app.station_list_parent = SCREEN_MAIN_MENU;
                            if (app.top_loaded) {
                                memcpy(app.stations, app.top_stations,
                                       (size_t)app.top_count * sizeof(app.stations[0]));
                                app.station_count = app.top_count;
                                app.selection = 0;
                                app.scroll_offset = 0;
                                app.screen = SCREEN_STATION_LIST;
                            } else async_launch_load(ASYNC_REQ_LOAD_TOP_STATIONS, "");
                            break;
                        case 3:
                            search_input_destroy(&app.search_input);
                            search_input_init(&app.search_input);
                            app.search_query[0] = '\0';
                            app.search_cursor = 0;
                            app.search_candidate_cursor = 0;
                            app.search_candidate_page = 0;
                            app.search_candidate_focus = false;
                            app.search_symbols = false;
                            app.screen = SCREEN_SEARCH;
                            break;
                    }
                }
            } else if (app.main_tab == 0 && activate && app.current_station) {
                app.screen = SCREEN_PLAYING;
            } else if (app.main_tab == 2 && activate) {
                app.screen = SCREEN_SETTINGS;
                app.selection = 0;
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
            if (touch_active && touch.py >= 48 && touch.py < 215) {
                int idx = (touch.py - 48) / 23 + app.scroll_offset;
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
            if (touch_active && touch.py >= 48 && touch.py < 215) {
                int idx = (touch.py - 48) / 23 + app.scroll_offset;
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
            if (touch_active && touch.py >= 32 && touch.py < 215) {
                int idx = (touch.py - 32) / 25 + app.scroll_offset;
                if (idx >= 0 && idx < app.station_count) {
                    app.selection = idx;
                    if (touch.px >= 3 && touch.px <= BOT_WIDTH - 3)
                        kDown |= KEY_A;
                }
            }
            if (kDown & KEY_A) {
                if (!stream_player_codec_supported(app.stations[app.selection].codec)) {
                    set_status("%s", CLR_WARN, tr_codec_unsupported());
                } else {
                    async_launch_load(ASYNC_REQ_PLAY_STATION,
                                      app.stations[app.selection].stationuuid);
                }
            }
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
            if (kDown & KEY_SELECT) {
                StreamPlayerState state = stream_player_get_state(app.stream_player);
                if (state == STREAM_STATE_ERROR || state == STREAM_STATE_ENDED) {
                    int retry = stream_player_retry(app.stream_player);
                    set_status("%s", retry == 0 ? CLR_INFO : CLR_ERR,
                               retry == 0 ? tr_connecting_stream() : tr_stream_failed());
                }
            }
            if (kDown & KEY_B) {
                if (app.stream_player) {
                    stream_player_stop(app.stream_player);
                }
                app.is_playing = false;
                app.current_station = NULL;
                memset(app.stream_url, 0, sizeof(app.stream_url));
                /* Back always returns to the station list we came from,
                 * never to the tag/language category menu. */
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
            if (touch_active && touch.py >= 70 && touch.py <= 126) {
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
                            /* Same as B: back to the station list, not the category menu */
                            app.screen = SCREEN_STATION_LIST;
                            break;
                        case 2:
                            app.volume = fmax(0.0f, app.volume - 0.1f);
                            if (app.stream_player)
                                stream_player_set_volume(app.stream_player, app.volume);
                            set_status("%s", CLR_INFO,
                                       tr_volume_level((int)(app.volume * 100)));
                            break;
                        case 3:
                            app.volume = fmin(1.0f, app.volume + 0.1f);
                            if (app.stream_player)
                                stream_player_set_volume(app.stream_player, app.volume);
                            set_status("%s", CLR_INFO,
                                       tr_volume_level((int)(app.volume * 100)));
                            break;
                    }
                }
            }
            if (touch_active && touch.py >= 132 && touch.py <= 182 &&
                (stream_player_get_state(app.stream_player) == STREAM_STATE_ERROR ||
                 stream_player_get_state(app.stream_player) == STREAM_STATE_ENDED)) {
                if (stream_player_retry(app.stream_player) == 0)
                    set_status("%s", CLR_INFO, tr_connecting_stream());
            }
            break;
        }

        case SCREEN_SEARCH: {
            int char_count = search_character_key_count();
            int action_base = char_count;
            int total_keys = char_count + SEARCH_ACTION_COUNT;
            int candidate_count = search_input_candidate_count(&app.search_input);
            int candidate_pages = (candidate_count + SEARCH_CANDIDATES_PER_PAGE - 1) /
                                  SEARCH_CANDIDATES_PER_PAGE;
            bool activate = (kDown & KEY_A) != 0;
            if (candidate_count == 0)
                app.search_candidate_focus = false;

            if (kDown & KEY_Y) {
                app.search_symbols = !app.search_symbols;
                app.search_candidate_focus = false;
                set_status("%s", CLR_INFO,
                           app.search_symbols ? "符号键盘" : "字母 / 拼音键盘");
            }

            if (kDown & KEY_LEFT) {
                if (app.search_candidate_focus && candidate_count > 0) {
                    app.search_candidate_cursor--;
                    if (app.search_candidate_cursor < 0)
                        app.search_candidate_cursor = candidate_count - 1;
                    app.search_candidate_page =
                        app.search_candidate_cursor / SEARCH_CANDIDATES_PER_PAGE;
                } else {
                    app.search_cursor = (app.search_cursor - 1 + total_keys) % total_keys;
                }
            }
            if (kDown & KEY_RIGHT) {
                if (app.search_candidate_focus && candidate_count > 0) {
                    app.search_candidate_cursor =
                        (app.search_candidate_cursor + 1) % candidate_count;
                    app.search_candidate_page =
                        app.search_candidate_cursor / SEARCH_CANDIDATES_PER_PAGE;
                } else {
                    app.search_cursor = (app.search_cursor + 1) % total_keys;
                }
            }
            if (kDown & KEY_UP) {
                if (candidate_count > 0 && !app.search_candidate_focus) {
                    app.search_candidate_focus = true;
                    app.search_candidate_cursor = app.search_candidate_page *
                                                  SEARCH_CANDIDATES_PER_PAGE;
                } else if (app.search_candidate_focus && candidate_pages > 1) {
                    app.search_candidate_page =
                        (app.search_candidate_page + candidate_pages - 1) % candidate_pages;
                    app.search_candidate_cursor = app.search_candidate_page *
                                                  SEARCH_CANDIDATES_PER_PAGE;
                } else {
                    app.search_cursor = (app.search_cursor - 1 + total_keys) % total_keys;
                }
            }
            if (kDown & KEY_DOWN) {
                if (app.search_candidate_focus && candidate_pages > 1) {
                    app.search_candidate_page =
                        (app.search_candidate_page + 1) % candidate_pages;
                    app.search_candidate_cursor = app.search_candidate_page *
                                                  SEARCH_CANDIDATES_PER_PAGE;
                } else if (app.search_candidate_focus) {
                    app.search_candidate_focus = false;
                } else {
                    app.search_cursor = (app.search_cursor + 1) % total_keys;
                }
            }

            if (kDown & KEY_L && candidate_count > 0)
                app.search_candidate_cursor =
                    (app.search_candidate_cursor - 1 + candidate_count) % candidate_count;
            if (kDown & KEY_R && candidate_count > 0)
                app.search_candidate_cursor =
                    (app.search_candidate_cursor + 1) % candidate_count;
            if ((kDown & (KEY_L | KEY_R)) && candidate_count > 0) {
                app.search_candidate_page = app.search_candidate_cursor /
                                            SEARCH_CANDIDATES_PER_PAGE;
                app.search_candidate_focus = true;
            }

            if (touch_active) {
                if (touch.py >= 63 && touch.py < 88 && candidate_count > 0) {
                    float x = 12.0f;
                    for (int candidate = app.search_candidate_page *
                         SEARCH_CANDIDATES_PER_PAGE;
                         candidate < candidate_count && candidate <
                         (app.search_candidate_page + 1) * SEARCH_CANDIDATES_PER_PAGE;
                         candidate++) {
                        const char *text = search_input_candidate(&app.search_input, candidate);
                        char text_clipped[32];
                        copy_utf8_ellipsis(text_clipped, sizeof(text_clipped), text, 8);
                        float width = 22.0f +
                                      (float)utf8_display_columns(text_clipped) * 8.0f;
                        if (width < 42.0f) width = 42.0f;
                        if (width > 78.0f) width = 78.0f;
                        if ((float)touch.px >= x && (float)touch.px < x + width) {
                            app.search_candidate_cursor = candidate;
                            search_input_commit_candidate(&app.search_input, candidate);
                            app.search_candidate_focus = false;
                            app.search_candidate_page = 0;
                            break;
                        }
                        x += width + 3.0f;
                    }
                } else if (touch.py >= 91 && touch.py < 169) {
                    int row = (touch.py - 91) / 27;
                    if (row >= 0 && row < SEARCH_KEY_ROW_COUNT) {
                        int y = 91 + row * 27;
                        int count = (int)strlen(search_key_row(row));
                        float key_w = 29.0f;
                        float start_x = row == 0 ? 4.0f : (row == 1 ? 19.0f : 50.0f);
                        int col = (int)(((float)touch.px - start_x) / key_w);
                        if (touch.py < y + 29 && col >= 0 && col < count) {
                            app.search_cursor = 0;
                            for (int r = 0; r < row; r++)
                                app.search_cursor += (int)strlen(search_key_row(r));
                            app.search_cursor += col;
                            app.search_candidate_focus = false;
                            activate = true;
                        }
                    }
                } else if (touch.py >= 174 && touch.py < 210) {
                    const int action_x[] = {4, 76, 124, 192, 248};
                    const int action_w[] = {68, 44, 64, 52, 68};
                    for (int action = 0; action < SEARCH_ACTION_COUNT; action++) {
                        if (touch.px >= action_x[action] &&
                            touch.px < action_x[action] + action_w[action]) {
                            app.search_cursor = action_base + action;
                            app.search_candidate_focus = false;
                            activate = true;
                            break;
                        }
                    }
                }
            }

            if (activate) {
                if (app.search_candidate_focus && candidate_count > 0) {
                    if (search_input_commit_candidate(&app.search_input,
                                                      app.search_candidate_cursor)) {
                        app.search_candidate_focus = false;
                        app.search_candidate_page = 0;
                    }
                } else if (app.search_cursor < char_count) {
                    char key;
                    if (search_character_key_at(app.search_cursor, &key))
                        search_input_append_ascii(&app.search_input, key);
                    app.search_candidate_page = 0;
                    app.search_candidate_cursor = 0;
                } else if (app.search_cursor == action_base) {
                    app.search_symbols = false;
                } else if (app.search_cursor == action_base + 1) {
                    app.search_symbols = true;
                } else if (app.search_cursor == action_base + 2) {
                    if (candidate_count > 0)
                        search_input_commit_candidate(&app.search_input,
                                                      app.search_candidate_cursor);
                    else
                        search_input_append_ascii(&app.search_input, ' ');
                    app.search_candidate_page = 0;
                    app.search_candidate_cursor = 0;
                } else if (app.search_cursor == action_base + 3) {
                    search_input_clear(&app.search_input);
                    app.search_query[0] = '\0';
                    app.search_candidate_page = 0;
                    app.search_candidate_cursor = 0;
                } else {
                    if (candidate_count > 0)
                        search_input_commit_candidate(&app.search_input,
                                                      app.search_candidate_cursor);
                    if (strlen(search_input_query(&app.search_input)) > 0) {
                        strncpy(app.search_query, search_input_query(&app.search_input),
                                sizeof(app.search_query) - 1);
                        app.search_query[sizeof(app.search_query) - 1] = '\0';
                        app.station_list_parent = SCREEN_MAIN_MENU;
                        async_launch_load(ASYNC_REQ_SEARCH, app.search_query);
                    }
                }
            }
            if (kDown & KEY_B) {
                if (search_input_composition(&app.search_input)[0]) {
                    search_input_backspace(&app.search_input);
                    app.search_candidate_focus = false;
                    app.search_candidate_page = 0;
                    app.search_candidate_cursor = 0;
                } else {
                    app.screen = SCREEN_MAIN_MENU;
                    app.selection = 0;
                }
            }
            break;
        }

        case SCREEN_SETTINGS: {
            if (kDown & KEY_DOWN)
                app.selection = (app.selection + 1) % 4;
            if (kDown & KEY_UP)
                app.selection = (app.selection - 1 + 4) % 4;

            int direction = 0;
            if (kDown & KEY_LEFT) direction = -1;
            if (kDown & KEY_RIGHT) direction = 1;
            if (kDown & KEY_A && app.selection < 3) direction = 1;

            if (touch_active && touch.py >= 48 && touch.py < 205) {
                int idx = (touch.py - 48) / 36;
                if (idx >= 0 && idx < 4) {
                    app.selection = idx;
                    if (touch.px >= 10 && touch.px <= BOT_WIDTH - 10 && idx < 3)
                        direction = 1;
                }
            }

            if (direction != 0) {
                if (app.selection == 0) {
                    app.settings.theme = app.settings.theme == UI_THEME_DARK
                        ? UI_THEME_LIGHT : UI_THEME_DARK;
                    load_theme(app.settings.theme);
                } else if (app.selection == 1) {
                    int value = app.settings.buffer_size + direction;
                    if (value < STREAM_BUF_SMALL) value = STREAM_BUF_LARGE;
                    if (value > STREAM_BUF_LARGE) value = STREAM_BUF_SMALL;
                    app.settings.buffer_size = value;
                    if (app.stream_player) {
                        stream_player_destroy(app.stream_player);
                    }
                    app.stream_player = stream_player_create_with_bufsize(value);
                    if (app.stream_player)
                        stream_player_set_volume(app.stream_player, app.volume);
                    else
                        set_status("%s", CLR_ERR, tr_audio_init_failed());
                } else if (app.selection == 2) {
                    int value = app.settings.language + direction;
                    if (value < LANG_AUTO) value = LANG_ZH_CN;
                    if (value > LANG_ZH_CN) value = LANG_AUTO;
                    app.settings.language = value;
                    locale_set_language((Language)value);
                }
                settings_save(&app.settings);
                set_status("%s", CLR_OK, tr_saved());
            }
            if (kDown & KEY_B) {
                settings_save(&app.settings);
                app.screen = SCREEN_MAIN_MENU;
                app.main_tab = 1;
                app.selection = 0;
            }
            break;
        }

        case SCREEN_STATION_INFO: {
            if (touch_active && touch.py >= 110 && touch.py < 158)
                kDown |= KEY_A;
            if (kDown & KEY_A) {
                if (!stream_player_codec_supported(app.stations[app.selection].codec))
                    set_status("%s", CLR_WARN, tr_codec_unsupported());
                else
                    async_launch_load(ASYNC_REQ_PLAY_STATION,
                                      app.stations[app.selection].stationuuid);
            }
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

/* Cancellation callback installed in the radio layer while the worker runs.
 * curl calls it periodically; returning non-zero aborts the transfer. */
static int async_cancel_cb(void *data) {
    (void)data;
    return app.async.cancel_requested ? 1 : 0;
}

static void async_worker_thread(void *arg) {
    (void)arg;
    AsyncRequest req = app.async.request;
    char error[128] = {0};
    int count = 0;
    u32 gen = app.async.load_generation;

    /* The cancel hook is installed by async_launch_load (main thread) and
     * clears in async_handle_completion; it aborts this worker's in-flight
     * request when the user cancels or a timeout fires. */

    switch (req.type) {
        case ASYNC_REQ_LOAD_TAGS:
            count = radio_fetch_tags(app.tags, MAX_TAGS, error, sizeof(error));
            app.tag_count = count > 0 ? count : 0;
            break;

        case ASYNC_REQ_LOAD_LANGUAGES:
            count = radio_fetch_languages(app.languages, MAX_TAGS, error, sizeof(error));
            app.language_count = count > 0 ? count : 0;
            break;

        case ASYNC_REQ_LOAD_STATIONS_BY_TAG:
            count = radio_fetch_by_tag(req.param, app.stations, MAX_STATIONS,
                                       error, sizeof(error));
            app.station_count = count > 0 ? count : 0;
            break;

        case ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE:
            count = radio_fetch_by_language(req.param, app.stations, MAX_STATIONS,
                                             error, sizeof(error));
            app.station_count = count > 0 ? count : 0;
            break;

        case ASYNC_REQ_LOAD_TOP_STATIONS:
            count = radio_fetch_topclick(app.top_stations, MAX_STATIONS,
                                         error, sizeof(error));
            app.top_count = count > 0 ? count : 0;
            break;

        case ASYNC_REQ_SEARCH:
            count = radio_search_by_name(req.param, app.stations, MAX_STATIONS,
                                          error, sizeof(error));
            app.station_count = count > 0 ? count : 0;
            break;

        case ASYNC_REQ_PLAY_STATION: {
            /* Get stream URL, then fall back to url_resolved / url */
            int idx = -1;
            for (int i = 0; i < app.station_count; i++) {
                if (strcmp(app.stations[i].stationuuid, req.param) == 0) {
                    idx = i; break;
                }
            }
            if (idx < 0) {
                snprintf(error, sizeof(error), "Station not found");
                count = -1;
                break;
            }
            app.current_station = &app.stations[idx];
            int ret = radio_get_stream_url(req.param, app.stream_url,
                                            sizeof(app.stream_url), error, sizeof(error));
            if (ret != NET_OK || strlen(app.stream_url) == 0) {
                if (strlen(app.current_station->url_resolved) > 0) {
                    strncpy(app.stream_url, app.current_station->url_resolved,
                            sizeof(app.stream_url) - 1);
                    app.stream_url[sizeof(app.stream_url) - 1] = '\0';
                    count = 0; /* Success with fallback */
                } else if (strlen(app.current_station->url) > 0) {
                    strncpy(app.stream_url, app.current_station->url,
                            sizeof(app.stream_url) - 1);
                    app.stream_url[sizeof(app.stream_url) - 1] = '\0';
                    count = 0;
                } else {
                    count = -1;
                    snprintf(error, sizeof(error), "No stream URL");
                }
            }
            break;
        }
    }

    /* If this load was superseded (generation bumped when a new load started
     * after we were detached on a join timeout), don't publish stale results. */
    if (gen != app.async.load_generation) {
        return;
    }

    /* Check for cancellation (B pressed during load). Use a dedicated state
     * instead of going straight to IDLE so the main loop ALWAYS joins and
     * frees this thread — no leak, and no race with the timeout path. */
    if (app.async.cancel_requested) {
        app.async.state = ASYNC_CANCELLED;
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
    if (!net_wifi_status()) {
        set_status("%s", CLR_ERR, tr_wifi_error());
        return;
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

    /* Clear the target collection before a new request.  An empty response
     * must not leave the previous page visible as if it succeeded. */
    switch (type) {
        case ASYNC_REQ_LOAD_TAGS:
            app.tag_count = 0;
            app.tags_loaded = false;
            break;
        case ASYNC_REQ_LOAD_LANGUAGES:
            app.language_count = 0;
            app.languages_loaded = false;
            break;
        case ASYNC_REQ_LOAD_STATIONS_BY_TAG:
        case ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE:
        case ASYNC_REQ_SEARCH:
            app.station_count = 0;
            break;
        case ASYNC_REQ_LOAD_TOP_STATIONS:
            app.station_count = 0;
            app.top_count = 0;
            app.top_loaded = false;
            break;
        default:
            break;
    }

    app.async.cancel_requested = false;
    app.async.state = ASYNC_LOADING;
    app.async.start_frame = app.frame_count;
    app.async.load_generation++;  /* invalidate any previously-detached worker */

    /* Install the cancel hook so this load's requests can be aborted by the
     * user (B) or by the timeout handler. Cleared in async_handle_completion. */
    radio_set_cancel_hook(async_cancel_cb, NULL);

    /* Spawn worker thread (32KB stack, same as stream_player's download thread) */
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(async_worker_thread, NULL, 32768, prio - 1, -2, false);
    if (!t) {
        /* Can't spawn — surface an error instead of spinning forever. */
        app.async.worker_thread = 0;
        snprintf(app.async.error_msg, sizeof(app.async.error_msg), "%s",
                 locale_get_language() == LANG_ZH_CN ? "无法创建加载线程"
                                                     : "Failed to create loader thread");
        app.async.state = ASYNC_ERROR;
        return;
    }
    app.async.worker_thread = t;
}

/* Called from main loop when worker finishes */
static void async_handle_completion(void) {
    Thread t = app.async.worker_thread;
    if (t) {
        /* The worker publishes its terminal state immediately before return.
         * A very short join avoids freezing rendering while still keeping the
         * thread handle and cancellation hook alive until it really exits. */
        if (threadJoin(t, ASYNC_JOIN_TIMEOUT_NS) == 0) {
            threadFree(t);
            app.async.worker_thread = 0;
        } else {
            return;
        }
    }

    /* This load has ended; stop cancelling requests. */
    radio_set_cancel_hook(NULL, NULL);

    int count = app.async.result_count;

    switch (app.async.state) {
        case ASYNC_DONE:
            switch (app.async.request.type) {
                case ASYNC_REQ_LOAD_TAGS:
                    app.tags_loaded = count > 0;
                    if (count > 0) {
                        app.selection = 0;
                        app.scroll_offset = 0;
                        app.screen = SCREEN_TAG_LIST;
                        set_status("%s", CLR_OK, tr_genres_loaded(count));
                    } else {
                        set_status("%s", CLR_WARN, tr_no_stations());
                    }
                    break;

                case ASYNC_REQ_LOAD_LANGUAGES:
                    app.languages_loaded = count > 0;
                    if (count > 0) {
                        app.selection = 0;
                        app.scroll_offset = 0;
                        app.screen = SCREEN_LANGUAGE_LIST;
                        set_status("%s", CLR_OK, tr_languages_loaded(count));
                    } else {
                        set_status("%s", CLR_WARN, tr_no_stations());
                    }
                    break;

                case ASYNC_REQ_LOAD_STATIONS_BY_TAG:
                case ASYNC_REQ_LOAD_STATIONS_BY_LANGUAGE:
                case ASYNC_REQ_LOAD_TOP_STATIONS:
                case ASYNC_REQ_SEARCH:
                    if (count > 0) {
                        if (app.async.request.type == ASYNC_REQ_LOAD_TOP_STATIONS) {
                            memcpy(app.stations, app.top_stations,
                                   (size_t)count * sizeof(app.stations[0]));
                            app.station_count = count;
                            app.top_loaded = true;
                        }
                        app.selection = 0;
                        app.scroll_offset = 0;
                        app.screen = SCREEN_STATION_LIST;
                        set_status("%s", CLR_OK, tr_stations_found(count));
                    } else {
                        set_status("%s", CLR_WARN, tr_no_stations());
                    }
                    break;

                case ASYNC_REQ_PLAY_STATION:
                    if (app.stream_player && app.current_station) {
                        stream_player_stop(app.stream_player);
                        int r = stream_player_play_with_codec(
                            app.stream_player, app.stream_url,
                            app.current_station->codec);
                        if (r == 0) {
                            app.is_playing = true;
                            app.play_start_tick = svcGetSystemTick();
                            app.screen = SCREEN_PLAYING;
                            set_status("%s", CLR_OK, tr_streaming());
                        } else {
                            set_status("%s", CLR_ERR,
                                       r == -2 ? tr_codec_unsupported() : tr_stream_failed());
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

        case ASYNC_CANCELLED:
            /* Silent: the user backed out mid-load. Stay on the current
             * screen; no error, no screen transition. */
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
        u32 color = C2D_Color32(0x00, 0x7A, 0xFF, alpha);  /* accent blue, not error red */

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

    memset(&app, 0, sizeof(app));
    settings_load(&app.settings);
    locale_set_language((Language)app.settings.language);

    /* Initialize networking */
    int net_result = net_init();
    radio_init();

    /* Load the configured language font. */
    romfsInit();
    if (locale_init_fonts()) {
        active_font = locale_get_font();
    }

    /* Load UI skin texture atlas */
    ui_skin_init(&skin);
    if (!load_theme(app.settings.theme)) {
        /* Skin load failed — will use solid-color fallbacks throughout */
        set_status("%s", CLR_WARN, tr_skin_fallback());
    }

    /* App state */
    app.screen = SCREEN_MAIN_MENU;
    app.main_tab = 1; /* open on Discover, matching the reference shell */
    app.volume = 0.8f;
    app.buffer_size = (StreamBufSize)app.settings.buffer_size;

    if (net_result != NET_OK)
        set_status("%s", CLR_ERR, tr_wifi_error());
    else
        set_status("%s", CLR_INFO, tr_welcome());
    app.stream_player = stream_player_create_with_bufsize(app.buffer_size);
    if (!app.stream_player)
        set_status("%s", CLR_ERR, tr_audio_init_failed());

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
            app.async.state == ASYNC_TIMEOUT ||
            app.async.state == ASYNC_CANCELLED) {
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
            case SCREEN_SETTINGS:     render_settings(); break;
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
    search_input_destroy(&app.search_input);
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
