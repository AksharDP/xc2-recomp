// Adapted from totk-pc-recomp runtime/ui/cheats_tab.cpp @ ab1dee5b (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// The Cheats tab of the in-game menu: the port's own cheats (cheats.c), then the cheats of the mods' cheat files, a
// heading per mod in the Mods tab's order, a row per cheat in file order and a line per group label of the file; a
// cheat that cannot be switched here is greyed out with the reason. While code cheats wait for the setup to build
// them in, a line on top says so. The button at the end opens the mods folder, where the cheat files live.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cheats.h"
#include "mods.h"
#include "platform/platform.h"
#include "ui_text.h"
}

void tab_cheats(float scale) {
    opt_begin("##cheats", scale);
    static char line[512], desc[768], buttons[192];
    if (cheats_unbuilt()) opt_text(UI_MENU_CHEATS_UNBUILT, true);

    // the port's own cheats, above the mods'
    int nb = cheats_builtin_count();
    if (nb) {
        opt_group_text(menu_text(UI_MENU_CHEATS_BUILTIN));
        for (int i = 0; i < nb; i++) {
            const CheatBuiltin *b = cheats_builtin(i);
            if (opt_toggle_text(b->key, menu_text(b->name), menu_text(b->desc), cheats_builtin_on(i) != 0)) {
                cheats_builtin_set(i, !cheats_builtin_on(i));
                opt_save_soon();
            }
        }
    }

    int n = cheats_count();
    if (!n) {
        snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_NONE), cheats_file()[0] ? cheats_file() : "<build id>.txt");
        opt_text_str(line);
    }
    for (int i = 0; i < n; i++) {
        const CheatInfo *c = cheats_info(i);
        if (!i || strcmp(c->mod, cheats_info(i - 1)->mod)) {
            int listed = 0;
            for (int j = i; j < n && !strcmp(cheats_info(j)->mod, c->mod); j++)
                listed += cheats_info(j)->kind != CHEAT_HEADING;
            snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_MOD), c->mod, listed);
            opt_group_text(line);
        }
        UiTextId text = UI_MENU_CHEAT_CODE_DESC;
        switch (c->kind) {
        case CHEAT_HEADING:
            if (!strstr(c->name, "TID=")) opt_text_str(c->name);   // the file's title line only names the game
            continue;
        case CHEAT_CODE: break;
        case CHEAT_KEY: text = UI_MENU_CHEAT_KEY_DESC; break;
        case CHEAT_DATA: text = UI_MENU_CHEAT_DATA_DESC; break;
        case CHEAT_MASTER: text = UI_MENU_CHEAT_MASTER_DESC, opt_disable_next(UI_MENU_CHEAT_MASTER); break;
        case CHEAT_RESTORE: text = UI_MENU_CHEAT_RESTORE_DESC, opt_disable_next(UI_MENU_CHEAT_RESTORE); break;
        case CHEAT_REBUILD: text = UI_MENU_CHEAT_REBUILD_DESC, opt_disable_next(UI_MENU_CHEAT_REBUILD); break;
        case CHEAT_UNSUPPORTED:
            text = UI_MENU_CHEAT_UNSUPPORTED_DESC, opt_disable_next(UI_MENU_CHEAT_UNSUPPORTED);
            break;
        }
        if (c->kind == CHEAT_KEY)
            snprintf(desc, sizeof desc, menu_text(text), cheats_buttons(c->buttons, buttons, sizeof buttons));
        else if (c->kind == CHEAT_UNSUPPORTED)
            snprintf(desc, sizeof desc, "%s %s", menu_text(text), c->note);
        else
            snprintf(desc, sizeof desc, "%s", menu_text(text));
        if (opt_toggle_text(c->key, c->name, desc, cheats_on(i) != 0)) {
            cheats_set(i, !cheats_on(i));
            opt_save_soon();
        }
    }
    static uint64_t open_failed;   // plat_now_ns until which the button says it failed
    if (opt_button("##cheats_open", UI_MENU_CHEATS_OPEN, UI_MENU_CHEATS_OPEN_DESC,
                   plat_now_ns() < open_failed ? menu_text(UI_MENU_ACTION_FAILED) : "")) {
        bool ok = mods_dir()[0] != 0;
        if (ok) plat_file_mkdir_p(mods_dir());
        if (!ok || !plat_open_folder(mods_dir())) open_failed = plat_now_ns() + 3000000000ull;
    }
    opt_end();
}
