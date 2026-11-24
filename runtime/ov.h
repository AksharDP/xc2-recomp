// Shared by the system overrides: runtime/ov_system.c, ov_input.c, ov_media.c and ov_gfx.c, and their probe.
// oe_push_message, oe_set_focus and ov_nv_state are in gfx.h (the window posts focus messages, the NVN layer
// allocates through the recorded callbacks).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

// ---- ov_system.c
int ov_handheld(void);                     // XC2_OPERATION_MODE=handheld (default docked); fatal on another value
const char *ov_language(void);             // the game's language, a Switch language code ("en-US", "zh-CN", ...)
int ov_language_shipped(const char *code); // the installed game files have text for code (XC2_GAME_LANGS)
// The Switch codes offered to the game (GetAvailableLanguageCodes): up to max of them to out, or the count when out is NULL
int ov_offered_languages(char (*out)[8], int max);
const char *ov_shared_font_dir(char *buf, size_t n);   // XC2_SHARED_FONTS, else <romfs>/../../firmware/shared-fonts
uint64_t ov_bfttf_size(const uint8_t *p, uint64_t len); // decoded font size of a BFTTF image, 0 if it is not one
