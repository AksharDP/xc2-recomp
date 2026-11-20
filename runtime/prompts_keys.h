// Remappable keyboard prompts: the runtime side of tools/xc2_prompts.py's keys.dat. The keyboard set's layouts are
// drawn at install time with the default keys; this swaps the encoded blocks of each prompt cell for the key the
// player bound to that action. stick clusters and d-pads keep their four-key art.
#pragma once
#include <stddef.h>

// When rel is one of the layouts keys.dat can patch, writes a copy with the current bindings under the profile and
// returns 1 with its path in out. Otherwise returns 0 and the caller serves the original layout. Any thread.
int prompts_keys_file(const char *rel, const char *set_dir, const char *src, char *out, size_t cap);
