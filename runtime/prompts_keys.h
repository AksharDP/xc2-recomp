// The keyboard button prompts drawn for the keys bound (XC2_BIND_*). tools/xc2_prompts.py draws the keyboard set for
// the default keys at setup; when the bind_ lines change what a prompt shows, prompts.c asks this file to draw the set
// again at the game's start from prompts/keys/ (xc2_prompts.py key_bank: the game's own layouts and the icon of every
// key). The drawing is xc2_prompts.py's, ported: the same icons, styles and cells, so the defaults draw the same bytes.
#pragma once
#include <stddef.h>

// Draws the keyboard set's layouts for the bindings now in xc2.ini into out_dir/<path> (out_dir is created), from
// prompts_dir/keys/. 1 when every layout was written. 0 when the bindings show the default icons (the setup's set
// fits), when prompts/keys/ is missing (a setup before it) or a file has not the expected shape, or on a write error;
// all but the first are logged. Any thread, one call at a time: the icon bank and the keys shown are this file's own
// (prompts.c init calls it once, under plat_once).
int prompts_keys_draw(const char *prompts_dir, const char *out_dir);
