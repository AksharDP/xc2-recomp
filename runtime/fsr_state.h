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
// The same for frame generation, whatever the settings
int vk_fg_state(void);
int vk_fsr3_version4(void);                  // 1: AMD's library is there, offers FSR 4 on this GPU, and it has not failed
int vk_fsr3_shown(char *buf, size_t size);   // 1 and the version when FSR upscaled within the last second (the overlay)
float vk_fg_shown_fps(uint64_t now_ns);      // frames a second on screen, negative while frame generation is off
int vk_fg_running(void);                     // a picture was generated within the last half second
int vk_fsr1_usable(void);                    // FSR 1 (vk_fsr.c, the present's scaling) has not failed to be set up

#ifdef __cplusplus
}
#endif