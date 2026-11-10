// Restoration: the Wolfos ambush in the first room of the Forest Temple springs when Link is close, as in the original.
// The room's logic (Logic/ForestTemple_Level_Room_00_Prefab_6f65, bound to its objects by the AiGroups of
// ForestTemple_Static) creates the pair at z 56.23, which faces the entrance, when the player touches the trigger box
// Area_8aa4: centre z 61.5, half-depth 7.06 (an Area's Scale is half its extent: its boxes in this room abut, 8aa4's
// far side at 54.44 meeting Area_1649's near side). Coming in from the entrance (z 77) Link touches it at z 68.56,
// 12.3 m before the pair, and in this release the Wolfos stand up far ahead of him. This moves the box, at its size, so
// that its near side is AMBUSH_M before the pair; its far side then reaches z 46.1, inside Area_1649. Walking back
// through the room from the inner door with this on was not tried.
//
// AMBUSH_M = 4: a Wolfos lunges about 2 m (WolfosParam Attack1MaxAdvanceDistance 2.0), so the pair comes up just out of
// reach. Measured on 1.1.1 walking in from the entrance: the pair's clouds rise when Link is at z 60.1 to 60.8 (about
// 4 m before them) with this on, at z 68.5 (12.3 m) with it off.
#include "restore.h"

#define AMBUSH_M 4.0
#define PAIR_Z 56.2305       // Translate z of the pair (Enemy_Wolfos 6047898332190259725 and 16159197261179024807)
#define HALF_DEPTH 7.06227   // Area_8aa4's Scale z

static const RestoreEdit edits[] = {
    RESTORE_SET("Banc/ForestTemple/ForestTemple_Static.bcett.byml.zs", "Area", 9574074458810239257ull, 0.085, 101.522,
                61.5, "Translate/2", 61.5, PAIR_Z + AMBUSH_M - HALF_DEPTH),
};

RESTORATION(wolfos_ambush, "WOLFOS_AMBUSH", "Close Wolfos ambush",
            "The two Wolfos in the Forest Temple's first room appear when Link gets close, not 12 m away.", edits);
