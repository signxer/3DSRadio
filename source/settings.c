#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SETTINGS_PATH "sdmc:/3ds/3DSRadio/settings.cfg"
#define SETTINGS_VERSION 1

struct SettingsFile {
    unsigned int version;
    unsigned int theme;
    int language;
    int buffer_size;
};

void settings_defaults(AppSettings *settings) {
    if (!settings) return;
    settings->theme = UI_THEME_LIGHT;
    settings->language = 2; /* LANG_ZH_CN */
    settings->buffer_size = 1; /* STREAM_BUF_MEDIUM */
}

static bool settings_valid(const AppSettings *settings) {
    return settings &&
           (settings->theme == UI_THEME_LIGHT || settings->theme == UI_THEME_DARK) &&
           settings->language >= 0 && settings->language <= 2 &&
           settings->buffer_size >= 0 && settings->buffer_size <= 2;
}

bool settings_load(AppSettings *settings) {
    struct SettingsFile file;
    FILE *fp;

    if (!settings) return false;
    settings_defaults(settings);

    fp = fopen(SETTINGS_PATH, "rb");
    if (!fp) return false;
    bool ok = fread(&file, sizeof(file), 1, fp) == 1;
    fclose(fp);

    if (!ok || file.version != SETTINGS_VERSION) return false;

    AppSettings loaded = {
        .theme = (UiTheme)file.theme,
        .language = file.language,
        .buffer_size = file.buffer_size,
    };
    if (!settings_valid(&loaded)) return false;
    *settings = loaded;
    return true;
}

bool settings_save(const AppSettings *settings) {
    struct SettingsFile file;
    FILE *fp;

    if (!settings_valid(settings)) return false;

    /* The standard 3DS SD layout normally has /3ds already. */
    (void)mkdir("sdmc:/3ds/3DSRadio", 0777);
    fp = fopen(SETTINGS_PATH ".tmp", "wb");
    if (!fp) return false;

    file.version = SETTINGS_VERSION;
    file.theme = (unsigned int)settings->theme;
    file.language = settings->language;
    file.buffer_size = settings->buffer_size;

    bool ok = fwrite(&file, sizeof(file), 1, fp) == 1;
    fclose(fp);
    if (!ok) return false;

    /* Rename is atomic on the SD filesystem and avoids half-written prefs. */
    remove(SETTINGS_PATH);
    return rename(SETTINGS_PATH ".tmp", SETTINGS_PATH) == 0;
}
