#ifndef SHIELD_LANGUAGE_CHOICE_H
#define SHIELD_LANGUAGE_CHOICE_H

#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Small, read-only-to-system language preference helper shared by Shield apps.
 * It reads only CFGU's selected UI language and stores the user's FR/EN choice
 * in that app's own SD folder. It never changes console settings. */
typedef struct {
    bool french;
    bool needs_confirmation;
    const char *path;
} ShieldLanguageChoice;

static inline void shield_language_choice_load(ShieldLanguageChoice *choice,
                                                const char *path,
                                                bool cfgu_ready) {
    if (!choice) return;
    choice->french = false;
    choice->needs_confirmation = false;
    choice->path = path;

    FILE *file = path ? fopen(path, "rb") : NULL;
    if (file) {
        int value = fgetc(file);
        fclose(file);
        if (value == 'F' || value == 'E') {
            choice->french = value == 'F';
            return;
        }
    }

    u8 system_language = CFG_LANGUAGE_EN;
    choice->french = cfgu_ready &&
        R_SUCCEEDED(CFGU_GetSystemLanguage(&system_language)) &&
        system_language == CFG_LANGUAGE_FR;
    choice->needs_confirmation = true;
}

static inline bool shield_language_choice_save(const ShieldLanguageChoice *choice) {
    if (!choice || !choice->path) return false;
    (void)mkdir("sdmc:/3ds", 0777);
    char parent[192];
    size_t length = strlen(choice->path);
    if (length >= sizeof(parent)) return false;
    memcpy(parent, choice->path, length + 1);
    char *slash = strrchr(parent, '/');
    if (slash && slash > parent + 5) {
        *slash = '\0';
        (void)mkdir(parent, 0777);
    }
    FILE *file = fopen(choice->path, "wb");
    if (!file) return false;
    const char value = choice->french ? 'F' : 'E';
    bool okay = fwrite(&value, 1, 1, file) == 1;
    if (fclose(file) != 0) okay = false;
    return okay;
}

static inline void shield_language_choice_accept(ShieldLanguageChoice *choice) {
    if (!choice) return;
    choice->needs_confirmation = false;
    (void)shield_language_choice_save(choice);
}

static inline void shield_language_choice_toggle(ShieldLanguageChoice *choice) {
    if (choice) choice->french = !choice->french;
}

#endif
