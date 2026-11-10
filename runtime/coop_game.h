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

// What joining the room means, for the player to accept: the other members (by the names the relay shows; heard: a
// message of theirs passed the room key's check) and the room's rules. Nothing from a room is applied to the game before
// the player accepted its offer and a copy of the save was made (<profile>/coop-backup/<time>/, docs/coop.md). An accepted
// offer stays accepted while every member is one of the names accepted and the rules stay the same; a new member or
// other rules make a new offer, and nothing more is applied until that one is accepted. The COOP_ACCEPT setting accepts
// an offer by its number, or every offer with "always"; the menu calls coop_game_accept.
typedef struct {
    uint32_t token;     // the offer's number; 0: none (not in a room, or its rules are not known yet)
    int accepted;
    int n;              // the other members
    struct { int id, heard, maker; char name[COOP_NAME_MAX + 1]; } member[COOP_MAX_MEMBERS];
    CoopRules rules;
} CoopOffer;
int coop_game_offer(CoopOffer *out);   // 1 when there is an offer
int coop_game_accept(uint32_t token);  // 1 when token is the current offer's, which is accepted from now on
