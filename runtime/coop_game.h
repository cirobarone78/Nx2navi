// Co-op progress sync, the game side (Ocarina 1.1.1): reads the game's flag database between frames, hands what changed
// to the merge engine (coop_state.c), and gives the game what the room has through the game's own set routines, so the
// game takes it as if the player had done it. Nothing is read or written while the game is on its title screen, loading
// or changing scene; what arrives meanwhile waits. docs/coop.md says what is shared.
#pragma once
#include "host.h"

// Registers the message handlers and the coop thread's hooks (main.c, after coop_init). Allocates nothing until the COOP
// setting is on.
void coop_game_init(void);
// nvnQueuePresentTexture, between frames on the game's thread with its core held (as res_present). With COOP off it reads
// one counter and returns.
void coop_present(Cpu *c);
