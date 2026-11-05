// Xbox, PlayStation and keyboard button prompts (XC2_PROMPTS). XC2 draws its prompts from texture layouts in bf2.ard
// (menu/image/button.wilay for the [ML:icon] tags of its text, the battle palette's mnu032_* layouts and a few more).
// tools/xc2_prompts.py redraws them from the player's own archive into prompts/<set>/ next to xc2.exe, listed in
// prompts/manifest.tsv. The game asks every add-on content mount for a menu or text file before it reads bf2.ard, so
// fs_host.c serves these files to those probes. A player without add-on content gets an empty pack of the port's own
// to give the game a mount to ask (fs_host.c AOC_PORT_PACK). Every redrawn set also gets prompts/text/: the message
// tables whose stick clicks use the click's icon.
#pragma once
#include <stddef.h>

// Whether a set may be served: XC2_PROMPTS is not "switch" and prompts/manifest.tsv exists. Unlike prompts_file this
// picks no set, so it can run at boot, before the gamepads are open. Any thread.
int prompts_possible(void);

// The host file of the chosen set for rel (a path inside an add-on content mount, "menu/image/button.wilay"); 0 when
// the set has no such file or no set is chosen. The set is picked at the first call (the game loads its layouts at
// start, so a change of XC2_PROMPTS or of the gamepad takes a restart): XC2_PROMPTS=auto takes the open gamepad's
// labels (PlayStation, Xbox, the game's own for Nintendo's) or the keyboard without one; the Xbox and PlayStation sets
// follow XC2_PAD_LAYOUT. Any thread.
int prompts_file(const char *rel, char *out, size_t cap);
