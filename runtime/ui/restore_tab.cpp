// The Restorations tab of the in-game menu (restore.h): a line on what they are, then an on/off row per declared
// restoration with its one-line description; the description pane adds what this start did with it. The edits apply
// at start (romfs.c overlay_init), so a switch takes effect at the next one.
#include <stdio.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "restore.h"
#include "ui_text.h"
}

extern "C" void tab_restore(float scale) {
    opt_begin("##restore", scale);
    opt_text(UI_MENU_RESTORE_INTRO);
    static char desc[1024], state[160], key[96];
    for (int i = 0, n = restore_count(); i < n; i++) {
        const Restoration *r = restore_at(i);
        int applied, skipped;
        restore_started(r, &applied, &skipped);
        bool on = restore_on(r) != 0, started = applied + skipped > 0;
        if (on != started) snprintf(state, sizeof state, "%s", menu_text(UI_MENU_AFTER_RESTART));
        else if (!on) state[0] = 0;
        else if (!skipped) snprintf(state, sizeof state, menu_text(UI_MENU_RESTORE_APPLIED), applied);
        else if (applied) snprintf(state, sizeof state, menu_text(UI_MENU_RESTORE_PARTLY), applied, applied + skipped);
        else snprintf(state, sizeof state, "%s", menu_text(UI_MENU_RESTORE_NONE));
        snprintf(desc, sizeof desc, "%s%s%s", r->desc, state[0] ? "\n\n" : "", state);
        snprintf(key, sizeof key, "NX2NAVI_RESTORE_%s", r->key);   // the row's id, and what the environment check reads
        if (opt_toggle_text(key, r->name, desc, on)) {
            restore_set(r, !on);
            opt_save_soon();
        }
    }
    opt_end();
}
