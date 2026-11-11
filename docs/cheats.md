# Built-in cheats

The settings menu's Cheats tab offers built-in policies on game 1.1.1, identified by the main build ID
`E0F3CCFAF50065AE`. Other builds keep the existing imported-cheat interface without these built-ins. Every
policy defaults to off; sprint speed defaults to normal. Settings persist in `nx2navi.ini`.

| Group | Options |
|---|---|
| Link | Infinite health, magic and rupees, unlimited air jumps, sprint speed normal / 2× / 3.5× / 5×. |
| Inventory | Separate infinite magic beans and business seeds; contents of each unlocked bottle, up to five. |
| Ammunition | Separate infinite arrows, bombs, bombchus, Deku nuts, slingshot seeds and Deku sticks. |
| Timers | Freeze the supported shared countdown at its current remaining time. |

Resource policies preserve existing capacities and unlocks. Health does not revive a dead player. Magic requires
an existing magic meter; ammunition requires its existing capacity, with an additional obtained-item check for
bombchus. Magic beans and business seeds require their obtained flags and use native limits of 10 and 30.

Bottle rows appear only for unlocked slots. Each row selects empty, red/green/blue potion, blue fire, fish,
letter, Big Poe, Poe, bugs, fairy, milk or half milk. An edit updates the current and buffered contents once;
normal consumption continues afterward. Save in the game to retain the contents. Bottle validity and stock
are never changed.

Unlimited Jumps responds to another press of the configured jump action. Holding the button does not repeat
jumps. Sprint Speed scales the native sprint action without changing ordinary movement, gravity or global
game speed. Lower speeds can suit confined locations better. Text refers to actions rather than physical
controller buttons. Labels and help follow the selected menu language: English, Russian, Brazilian Portuguese
or Simplified Chinese.

Freeze timers acts only while the shared timer is running in countdown mode with a positive finite value.
Disabling it resumes normal counting. Count-up clocks, bomb fuses and unrelated temporary effects are not
modified. The shared clock was verified live in the Zora diving minigame on a local 0.7.1 Windows build;
references in other event scripts establish reuse of the interface, not testing of every event or its other
failure conditions.

## Implementation and checks

`runtime/cheats.c` validates the build, table shapes, key hashes, indices, readable memory and relevant object
identities before writing. Health and magic capture their live resource bindings through independent bits of
the existing function-entry observer; the resolution observer keeps its own bit. The sprint patch is declared
in `recomp/patches-v111.txt`: the generator validates the original instruction and emits the optional multiplier
path. Generated C is not edited by hand or checked in.

`python tests/cheats_check.py` compiles the real policies against synthetic guest memory, without game files.
Set `CC` to the UCRT64 clang executable when it is not on PATH. The test covers the build gate, health/death
and capacity guards, magic capacity, wallet limits, item ownership, legacy no-op policies, all 13 contents in
each of five bottles, locked-slot and invalid-enum rejection, countdown freezing, count-up exclusion and expired
or non-finite timers. `--probe-ui-text` checks the language catalogs and live menu-language selection. Changes
under `recomp/` also require the differential checks in [building.md](building.md).
