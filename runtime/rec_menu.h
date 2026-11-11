// The F1 menu's capture buttons (ui/menu.cpp, Advanced tab) and the recorder that takes their captures (replay_rec.c).
//
// The recorder's journal is always on: the objects the back end holds are followed from the start, so a capture can begin
// at any frame, and no variable is needed. Nothing is written until a capture starts. "Capture frame" records the next
// frame into captures/frame-N/log.bin; "Capture checkpoint" records from the next frame into captures/checkpoint-N/log.bin
// until "Stop checkpoint" is pressed or the game ends. The folder is captures/ in the working folder, or XC2_RECORD=<folder>
// when that is set. Freezing is held off while a capture runs (vk_mem_capture_hold). The replay (xc2.exe --replay
// <folder>/frame-N) runs each capture.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { REC_MENU_FRAME = 1, REC_MENU_CHECKPOINT = 2, REC_MENU_STOP = 3 };

// 0 idle, REC_MENU_FRAME or REC_MENU_CHECKPOINT while a capture runs. Any thread.
int rec_menu_state(void);
// Asks for a capture: REC_MENU_FRAME or REC_MENU_CHECKPOINT starts at the next present (ignored while one runs),
// REC_MENU_STOP ends a checkpoint at the next present. Any thread.
void rec_menu_request(int kind);

#ifdef __cplusplus
}
#endif
