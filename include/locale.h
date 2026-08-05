#pragma once

#include <stdbool.h>
#include <citro2d.h>

/* ======================================================================
 * i18n / Localization system for 3DSRadio
 * Supports Chinese (Simplified) and English
 * ====================================================================== */

typedef enum {
    LANG_AUTO = 0,
    LANG_EN,
    LANG_ZH_CN,
} Language;

/* Set language. LANG_AUTO = try system locale, fall back to Chinese */
void locale_set_language(Language lang);

/* Get current language */
Language locale_get_language(void);

/* Initialize font system. Returns true if Chinese font was loaded */
bool locale_init_fonts(void);

/* Get the active font for rendering */
C2D_Font locale_get_font(void);

/* ======================================================================
 * Translated strings
 * ====================================================================== */

const char *tr_main_title(void);
const char *tr_main_subtitle(void);
const char *tr_menu_browse_genre(void);
const char *tr_menu_top_stations(void);
const char *tr_menu_search(void);
const char *tr_menu_about(void);

const char *tr_genre_header(void);
const char *tr_genre_subtitle(void);
const char *tr_stations_header(void);
const char *tr_stations_found(int count);
const char *tr_select_station(void);
const char *tr_loading(void);

const char *tr_now_playing(void);
const char *tr_playing(void);
const char *tr_paused(void);
const char *tr_controls(void);
const char *tr_play_pause(void);
const char *tr_stop_back(void);
const char *tr_vol_down(void);
const char *tr_vol_up(void);
const char *tr_volume(void);
const char *tr_stream_url(void);

const char *tr_search_header(void);
const char *tr_search_prompt(void);
const char *tr_search_hint(void);
const char *tr_search_action(void);
const char *tr_searching(void);
const char *tr_no_results(void);

const char *tr_back(void);
const char *tr_select(void);
const char *tr_info(void);
const char *tr_navigate(void);
const char *tr_wifi_connected(void);
const char *tr_wifi_disconnected(void);
const char *tr_connecting_stream(void);

const char *tr_station_info(void);
const char *tr_name(void);
const char *tr_country(void);
const char *tr_codec(void);
const char *tr_bitrate(void);
const char *tr_language(void);
const char *tr_votes(void);
const char *tr_clicks(void);
const char *tr_tags(void);

/* About screen */
const char *tr_about_title(void);
const char *tr_about_desc(void);
const char *tr_about_powered(void);
