#pragma once

#include <stdbool.h>

/* Runtime preferences.  The file format is intentionally tiny and versioned
 * so a damaged or future-incompatible file can safely fall back to defaults. */
typedef enum {
    UI_THEME_LIGHT = 0,
    UI_THEME_DARK = 1,
} UiTheme;

typedef struct {
    UiTheme theme;
    int language;       /* Matches Language from locale.h without coupling the modules. */
    int buffer_size;    /* Matches StreamBufSize from stream_player.h. */
} AppSettings;

void settings_defaults(AppSettings *settings);
bool settings_load(AppSettings *settings);
bool settings_save(const AppSettings *settings);
