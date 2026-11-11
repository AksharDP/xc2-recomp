// The F1 menu's capture buttons (ui/menu.cpp, Advanced tab) and the recorder that takes their captures (replay_rec.c).
//
// The recorder's journal is always on: the objects the back end holds are followed from the start, so a capture
// can begin at any frame, and no variable is needed. Nothing is written until a capture starts. "Capture frame"
// records the state the next frame starts from and that one frame into captures/frame-N/log.bin. "Record frames"
// records the same state and then every frame into captures/clip-N/log.bin until "Stop recording" is pressed,
// the game ends, or the write fails. N is the number of the first frame taken.
// The folder is captures/ in the working folder, or XC2_RECORD=<folder> when that is set. Neither is a savestate:
// the game cannot be resumed from a capture; the replay (xc2.exe --replay <folder>/frame-N) runs the renderer alone
// on the recorded calls. Freezing is held off while a capture runs (vk_mem_capture_hold).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { REC_MENU_FRAME = 1, REC_MENU_CLIP = 2, REC_MENU_STOP = 3 };

// 0 idle, REC_MENU_FRAME or REC_MENU_CLIP while a capture runs. Any thread.
int rec_menu_state(void);
// Asks for a capture: REC_MENU_FRAME or REC_MENU_CLIP starts at the next present (ignored while one runs),
// REC_MENU_STOP ends a clip at the next present. Any thread.
void rec_menu_request(int kind);
// The folder the captures go to, as the recorder writes them (captures/, or XC2_RECORD). 1 when it is known. Any thread.
int rec_menu_folder(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
