// What the menu asks about AMD FSR (vk_fsr3.c) and its frame generation (vk_fg.c). On the present thread.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Whether AMD FSR can run here: with AMD's library or the built-in FSR 3.1 on this device, unless it failed in this run.
enum { VK_FSR_OK, VK_FSR_NO_DEVICE, VK_FSR_FAILED };
int vk_fsr3_state(void);
// What the menu offers: 1 once FSR, FSR 4 (as a choice) and frame generation have each been loaded here, by a check that
// the first question starts (it can take a few seconds) or by running, and have not failed since.
int vk_fsr3_ready(void);                     // -1 while the check runs
int vk_fsr3_version4(void);
int vk_fg_ready(void);
int vk_fsr3_shown(char *buf, size_t size);   // 1 and the version when FSR upscaled within the last second (the overlay)
float vk_fg_shown_fps(uint64_t now_ns);      // frames a second on screen, negative while frame generation is off
int vk_fg_running(void);                     // a picture was generated within the last half second
int vk_fsr1_usable(void);                    // FSR 1 (vk_fsr.c, the present's scaling) has not failed to be set up

#ifdef __cplusplus
}
#endif