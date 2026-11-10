// Co-op progress sync, the game side (Ocarina 1.1.1): reads the game's flag database between frames, hands what changed
// to the merge engine (coop_state.c), and gives the game what the room has through the game's own set routines, so the
// game takes it as if the player had done it. Nothing is read or written while the game is on its title screen, loading
// or changing scene; what arrives meanwhile waits. docs/coop.md says what is shared.
#pragma once
#include "coop_proto.h"
#include "coop_state.h"
#include "host.h"

// Registers the message handlers and the coop thread's hooks (main.c, after coop_init). Allocates nothing until the COOP
// setting is on.
void coop_game_init(void);
// nvnQueuePresentTexture, between frames on the game's thread with its core held (as res_present). With COOP off it reads
// one counter and returns.
void coop_present(Cpu *c);

// What joining the room means, for the player to accept: the engine's offer (coop_state.h: CoopOffer), with the
// relay's names for members not identified yet. Nothing from a room is applied to the game before the player accepted
// its offer and a copy of the save was made (<profile>/coop-backup/<time>/, docs/coop.md; the first copy for a room is
// "first-<time>" and is never pruned). An accepted offer stays accepted while every member is an identity accepted,
// the rules and the key that signs them stay the same and the same save is played; anything else is a new offer, and
// nothing more is applied until that one is accepted. The offer's number is random for each start of the game. The
// COOP_ACCEPT setting accepts every offer with "always" (from the environment only) or one offer by its number; the
// menu calls coop_game_accept.
int coop_game_offer(CoopOffer *out);   // 1 when there is an offer
int coop_game_accept(uint32_t token);  // 1 when token is the current offer's, which is accepted from now on
// The room's maker (its game holds the key that signs the rules) resets the pools: every game drops their records and
// joins them again with what it holds. 1 when done. A removal is coop_remove_member (coop.h).
int coop_game_reset_pools(void);
