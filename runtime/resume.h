// Restart in place (resume.c): the menu's Restart brings the player back where they stood, in a new process with the
// changed settings.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Cpu;

// What the Restart confirmation can promise now (resume_answer)
enum {
    RESUME_PLAIN,     // not in play on 1.1.1, an imported save waits, or no guest thread polled the pad for 2 s after
                      // the menu opened: Restart starts the game at its title
    RESUME_CHECKING,  // in play, the answer comes within a second
    RESUME_CAN,       // the game takes its autosave request now: Restart saves first and continues there
    RESUME_WAIT,      // the same once the game's cooldown ends, in *wait_s seconds
    RESUME_NO,        // the game cannot save here: Restart starts the game at its title
    RESUME_PENDING,   // Restart confirmed: waiting for the game's save (*wait_s: for its cooldown), Cancel cancels
    RESUME_GONE,      // the restart is under way: the window closes
};

// ---- the process Restart ends
void resume_probe_want(void);                       // the menu is open (each frame): keep the game's answer fresh
int resume_answer(int *wait_s);                     // RESUME_*, for the confirmation (present thread)
void resume_restart(void);                          // Restart confirmed (ui/menu.cpp): with RESUME_CAN or RESUME_WAIT
                                                    // asks for the game's autosave first, else restarts at once
void resume_cancel(void);                           // Cancel while RESUME_PENDING
int resume_menu_close(void);                        // the menu closes, any way: cancels a pending restart; 0 while the
                                                    // restart is under way, and the menu stays open
void resume_guest(struct Cpu *c);                   // a guest thread with its core (ov_input.c npad_states): the
                                                    // game's answer and the autosave request
void resume_save_file(const char *path, int write); // the game opened a file of its save mount (fs_host.c open_file)
void resume_present(void);                          // each presented game image (gfx_present.c), both processes

// ---- the process Restart starts (NX2NAVI_RESUME)
int resume_booting(void);                           // 1 until the player stands in the world again
uint64_t resume_buttons(void);                      // the A presses that take the title to the save (input.c input_poll),
                                                    // the only input the game gets while resume_booting

#ifdef __cplusplus
}
#endif
