// Prints the text catalog for tests/ui_text_test.py: per id and language one record, "name <US> language <US> text <RS>",
// with no text when the row has none of its own. ui_text.c is included whole, with stubs for the four functions it takes
// from the rest of the runtime. The test generates ui_text_ids.h, the ids' names.
#include <stdio.h>

#include "ui_text.c"

unsigned cfg_version(void) { return 0; }
const char *cfg_value(const char *name) { return (void)name, ""; }
const char *ov_language(void) { return "en-US"; }
const char *plat_ui_language(void) { return ""; }

static const struct { UiTextId id; const char *name; } ids[] = {
#include "ui_text_ids.h"
};

int main(void) {
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
        for (size_t l = 0; l < sizeof rows / sizeof *rows; l++)
            printf("%s\x1f%s\x1f%s\x1e", ids[i].name, rows[l].lang,
                   ui_text_defined(rows[l].lang, ids[i].id) ? rows[l].text[ids[i].id] : "");
    return 0;
}
