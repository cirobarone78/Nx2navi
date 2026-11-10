// The save files on the menu's Saves tab (ui/saves_page.cpp, fs_host.c section "save files"). Present path only (ImGui).
#pragma once

// Inside the page's opt_begin / opt_end: a row per save file (its name, child or adult, hearts, area and when it was
// last saved) and New save file. saves_after_page, after opt_end: the window a file's row opens, with Play this file
// (a restart into it), Copy, Rename and Delete, each asking first where a save could be lost or a name is wanted.
void saves_rows(float scale);
void saves_after_page(float scale);
// The window is open: B, Esc, L / R and the tab keys stay with it.
bool saves_modal(void);
// The menu opened or closed (menu.cpp): the window closes; opening, the list is read again at once.
void saves_cancel(void);
// NX2NAVI_MENU_TEST_SET pseudo-keys (menu.cpp test_apply_sets): SAVE_FILE_OPEN=<id> opens a file's window,
// SAVE_FILE_PAGE=play|copy|rename|delete|new turns it to that step, SAVE_FILE_PLAY=<id> does what Play this file's
// Restart does. 1 when key is one of them.
int saves_test_set(const char *key, const char *value);
