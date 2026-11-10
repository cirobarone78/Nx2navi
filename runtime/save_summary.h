// What a save holds at a glance, for the menu's list of save files (save_summary.c): read from the newest slot of a
// save folder (account.sav, option.sav, slot_NN/caption.sav and slot_NN/progress.sav), never written.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int slots;                           // slot_NN folders holding a progress.sav; 0: nothing saved yet
    int year, month, day, hour, minute;  // when the newest slot was saved, the game's clock (local time); year 0 unknown
    int adult;                           // the newest slot: 1 adult, 0 child, -1 unknown
    int life, max_life;                  // the newest slot's hearts in quarters (4 a heart); -1 unknown
    char area[64];                       // the newest slot's area as words ("Hyrule Field"); "" unknown
} SaveSummary;

// Reads the save folder dir into *out. The newest slot is the one whose caption.sav has the latest save time.
void save_summary(const char *dir, SaveSummary *out);

#ifdef __cplusplus
}
#endif
