// Restoration: the long sinking platform of the Fire Trial in Ganon's Castle sinks at the original's pace. Timed on the
// same run in both, the original's platform is under the lava after 9.68 s, this release's after 15.71 s.
// Obj_InsideGanonCastle_SinkFloor_A_01 (one in InsideGanonCastle, room 10) takes its speed from its pack's
// SinkFloorParam, MoveSpeed 0.15; the two Obj_FireTemple_SinkFloor_A_01 of the same room (0.5) and the Fire Temple's
// are left as they are. The time to sink is the depth over the speed, so the original's time asks for
// 0.15 x 15.71 / 9.68 = 0.2434.
//
// Measured on 1.1.1 with Link standing on the platform from the same save: it carried him down at 0.167 m/s with this
// off and 0.272 m/s with it on (1.62 times as fast; 15.71 / 9.68 is 1.62), and he reached the lava after 13.5 s
// against 8.1 s.
#include "restore.h"

static const RestoreEdit edits[] = {
    RESTORE_SET_MEMBER("Pack/Actor/Obj_InsideGanonCastle_SinkFloor_A_01.pack.zs",
                       "GameParameter/SinkFloorParam/"
                       "Obj_InsideGanonCastle_SinkFloor_A_01.game__object__SinkFloorParam.bgyml",
                       "MoveSpeed", 0.15, 0.15 * 15.71 / 9.68),
};

RESTORATION(sink_floor, "FIRE_TRIAL_SINKING", "Fire Trial platform pace",
            "The long sinking platform in the Fire Trial of Ganon's Castle sinks at the original's pace, about 1.6 times "
            "as fast as in this release.",
            edits);
