#include "locale.h"
#include <stdio.h>
#include <string.h>

/* ======================================================================
 * 3DSRadio i18n - Chinese (Simplified) translations
 * ====================================================================== */

static Language current_lang = LANG_AUTO;
static C2D_Font chinese_font = NULL;

/* Detect system language from 3DS settings */
static Language detect_system_language(void) {
    /* Check if we can detect - default to Chinese */
    u8 lang = 1; /* English fallback */
    cfguInit();
    CFGU_GetSystemLanguage(&lang);
    cfguExit();

    /* 0=JP, 1=EN, 2=FR, 3=DE, 4=IT, 5=ES, 6=ZH, 7=KO, 8=NL, 9=PT, 10=RU, 11=TW */
    if (lang == 6 || lang == 11) {
        return LANG_ZH_CN;
    }
    return LANG_EN;
}

void locale_set_language(Language lang) {
    current_lang = lang;
}

Language locale_get_language(void) {
    if (current_lang == LANG_AUTO) {
        current_lang = detect_system_language();
    }
    return current_lang;
}

bool locale_init_fonts(void) {
    /* Try to load Chinese font from romfs */
    chinese_font = C2D_FontLoad("romfs:/chinese.bcfnt");
    if (chinese_font) {
        return true;
    }
    return false;
}

C2D_Font locale_get_font(void) {
    if (chinese_font) {
        return chinese_font;
    }
    return NULL; /* Use system font */
}

/* Check if we should return Chinese or English */
static bool is_chinese(void) {
    Language lang = locale_get_language();
    return lang == LANG_ZH_CN;
}

/* ======================================================================
 * String translations
 * ====================================================================== */

const char *tr_main_title(void) {
    return is_chinese() ? "3DS 网络收音机" : "3DS Radio";
}

const char *tr_main_subtitle(void) {
    return is_chinese() ? "全球数千电台，尽在掌握" : "Thousands of stations worldwide";
}

const char *tr_menu_browse_genre(void) {
    return is_chinese() ? "按音乐风格浏览" : "Browse by Genre";
}

const char *tr_menu_top_stations(void) {
    return is_chinese() ? "热门电台" : "Top Stations";
}

const char *tr_menu_search(void) {
    return is_chinese() ? "搜索电台" : "Search Stations";
}

const char *tr_menu_about(void) {
    return is_chinese() ? "关于" : "About";
}

const char *tr_genre_header(void) {
    return is_chinese() ? "按风格浏览" : "Browse by Genre";
}

const char *tr_genre_subtitle(void) {
    return is_chinese() ? "选择音乐风格，探索全球电台" : "Select a genre to explore stations";
}

const char *tr_stations_header(void) {
    return is_chinese() ? "电台列表" : "Stations";
}

const char *tr_stations_found(int count) {
    static char buf[64];
    if (is_chinese()) {
        snprintf(buf, sizeof(buf), "找到 %d 个电台", count);
    } else {
        snprintf(buf, sizeof(buf), "Found: %d stations", count);
    }
    return buf;
}

const char *tr_select_station(void) {
    return is_chinese() ? "选择电台播放" : "Select a station to play";
}

const char *tr_loading(void) {
    return is_chinese() ? "加载中..." : "Loading...";
}

const char *tr_now_playing(void) {
    return is_chinese() ? "正在播放" : "Now Playing";
}

const char *tr_playing(void) {
    return is_chinese() ? "播放中" : "Playing";
}

const char *tr_paused(void) {
    return is_chinese() ? "已暂停" : "Paused";
}

const char *tr_controls(void) {
    return is_chinese() ? "播放控制" : "Controls";
}

const char *tr_play_pause(void) {
    return is_chinese() ? "播放/暂停" : "Play/Pause";
}

const char *tr_stop_back(void) {
    return is_chinese() ? "停止并返回" : "Stop & Back";
}

const char *tr_vol_down(void) {
    return is_chinese() ? "音量减" : "Vol Down";
}

const char *tr_vol_up(void) {
    return is_chinese() ? "音量加" : "Vol Up";
}

const char *tr_volume(void) {
    return is_chinese() ? "音量" : "Volume";
}

const char *tr_stream_url(void) {
    return is_chinese() ? "流地址" : "Stream URL";
}

const char *tr_search_header(void) {
    return is_chinese() ? "搜索电台" : "Search Stations";
}

const char *tr_search_prompt(void) {
    return is_chinese() ? "输入电台名称搜索" : "Find stations by name";
}

const char *tr_search_hint(void) {
    return is_chinese() ? "输入电台名称..." : "Type a station name...";
}

const char *tr_search_action(void) {
    return is_chinese() ? "搜索" : "Search";
}

const char *tr_searching(void) {
    return is_chinese() ? "搜索中..." : "Searching...";
}

const char *tr_no_results(void) {
    return is_chinese() ? "没有找到结果" : "No stations found";
}

const char *tr_back(void) {
    return is_chinese() ? "返回" : "Back";
}

const char *tr_select(void) {
    return is_chinese() ? "选择" : "Select";
}

const char *tr_info(void) {
    return is_chinese() ? "详情" : "Info";
}

const char *tr_navigate(void) {
    return is_chinese() ? "导航" : "Navigate";
}

const char *tr_wifi_connected(void) {
    return is_chinese() ? "WiFi 已连接" : "WiFi: Connected";
}

const char *tr_wifi_disconnected(void) {
    return is_chinese() ? "WiFi 未连接" : "WiFi: Disconnected";
}

const char *tr_connecting_stream(void) {
    return is_chinese() ? "连接流中..." : "Connecting to stream...";
}

const char *tr_station_info(void) {
    return is_chinese() ? "电台信息" : "Station Info";
}

const char *tr_name(void) {
    return is_chinese() ? "名称:" : "Name:";
}

const char *tr_country(void) {
    return is_chinese() ? "国家:" : "Country:";
}

const char *tr_codec(void) {
    return is_chinese() ? "编码:" : "Codec:";
}

const char *tr_bitrate(void) {
    return is_chinese() ? "码率:" : "Bitrate:";
}

const char *tr_language(void) {
    return is_chinese() ? "语言:" : "Language:";
}

const char *tr_votes(void) {
    return is_chinese() ? "投票:" : "Votes:";
}

const char *tr_clicks(void) {
    return is_chinese() ? "点击:" : "Clicks:";
}

const char *tr_tags(void) {
    return is_chinese() ? "标签:" : "Tags:";
}

const char *tr_about_title(void) {
    return is_chinese() ? "3DS 网络收音机" : "3DS Radio";
}

const char *tr_about_desc(void) {
    return is_chinese() ? "一款专为 Nintendo 3DS 打造的网络收音机" : "Internet Radio Player for Nintendo 3DS";
}

const char *tr_about_powered(void) {
    return is_chinese() ? "由 radio-browser.info 提供技术支持" : "Powered by radio-browser.info";
}
