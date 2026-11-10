// Co-op puppets (coop_puppet.c): the other players of a co-op room shown in this game as Links of their own, game 1.1.1.
#pragma once

// nvnQueuePresentTexture, after coop_present: switches the puppets' frame hook (the patch set coop_puppet) on when there is
// work for it. With co-op off it reads one counter and returns.
void coop_puppet_present(void);
// At start, after coop_init: the puppets' handlers on the co-op thread (the other players' poses, the room's roster).
void coop_puppet_init(void);
