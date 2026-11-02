// Xbox, PlayStation and keyboard button prompts (XCX_PROMPTS). XC2 draws its prompts from texture layouts in bf2.ard
// (menu/image/button.wilay for the [ML:icon] tags of its text, the battle palette's mnu032_* layouts and a few more).
// tools/xc2_prompts.py redraws them from the player's own archive into prompts/<set>/ next to xcx.exe, listed in
// prompts/manifest.tsv. The game asks every add-on content mount for a menu file before it reads bf2.ard, so fs_host.c
// serves these files to those probes. Without add-on content the game has no mount to ask, and its own prompts stay.
#pragma once
#include <stddef.h>

// The host file of the chosen set for rel (a path inside an add-on content mount, "menu/image/button.wilay"); 0 when
// the set has no such file or no set is chosen. The set is picked at the first call (the game loads its layouts at
// start, so a change of XCX_PROMPTS or of the gamepad takes a restart): XCX_PROMPTS=auto takes the open gamepad's
// labels (PlayStation, Xbox, the game's own for Nintendo's) or the keyboard without one; the Xbox and PlayStation sets
// follow XCX_PAD_LAYOUT. Any thread.
int prompts_file(const char *rel, char *out, size_t cap);
