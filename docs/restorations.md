# Restorations

A restoration brings back something the original game did that this release changed. Each one has its own switch,
`restore_<key> = 1` in `nx2navi.ini` `[restorations]` (`NX2NAVI_RESTORE_<KEY>`) or its row on the *Restorations* tab of
the menu (F1). All are off by default, and a switch takes effect at the next start. No game file ships with the port:
a restoration is a list of edits, and the game applies them once at start to the player's own files.

| Key | What it restores |
|---|---|
| `WOLFOS_AMBUSH` | The two Wolfos in the Forest Temple's first room appear when Link is about 4 m away, not 12 m. |

The tab's own texts are in the menu's four languages; a restoration's name and description are English.

## How it works

`runtime/romfs.c` `overlay_init` calls `restore_apply` (`runtime/restore.c`) once, at start, after the mods' files are
chosen. For each file the switched-on restorations edit, the engine (`runtime/restore_engine.c`) takes the file as the
game would read it (a mod's version, else the original), unpacks it (zstd, with the dictionary of its id from
`Pack/ZsDic.pack.zs`), parses the BYML (`runtime/byml.c`), or each BYML member of a pack the edits name
(`runtime/sarc.c`), sets the numbers, writes the BYML back, reads it back to check it holds what was meant, and packs it
as the original was. `romfs.c` serves the result from `<profile>/restore/<folder>/` and raises the file's entry in the
resource size table when it grew. Several restorations on one file all apply to it.

An edit applies only when what it names is there as declared: an object by its `Hash`, `Gyaml` and `Translate` (within
1 cm), the number at the value the game ships. Anything else (another game version, a mod that changed the file) skips
that edit, and the log says which and why (`restore: <KEY>: <file>: ...`); the other edits still apply. Each document,
the file or one pack member, is checked on its own: one that does not read back as edited, or a member that cannot be
put back into its pack, loses its own edits, and the file is served with the other members' edits. When the file
itself cannot be read, unpacked, packed again or served, none of its edits apply and the game reads it as it is. The
menu row says how many edits the start applied. With every switch off nothing is read or written, and the log has no
`restore:` line.

The BYML reader takes the little-endian files this release has, and refuses a dict whose keys are out of order or
repeated (the game looks them up sorted). It allocates at most 8 Mi nodes (about 512 MiB) for one file, then gives up
and the file is served as it is.

`nx2navi.exe --probe-restore` (`./ocarina --probe-restore` on Linux) is the dry run: it applies every declared
restoration, switched on or not, to the files as the game would read them, serves nothing, prints a line per edit
(applied, or skipped and why) and exits 1 when one was skipped. `python tests/restore_check.py` tests the engine, the
BYML and SARC readers and writers and `restore.c`'s switches on files of its own (no game files), and runs the readers
on every cut length of its files and on thousands of copies with bits flipped.

## Adding one

One file, `runtime/restore_<name>.c`; the build picks it up and the registry (a linker section, like `HOST_OVERRIDE`)
lists it. Nothing else changes but the docs: a line in `nx2navi.ini.example` `[restorations]` and in the table above.

```c
// Restoration: what the original did, what the remake does, and how the files show it (object names, hashes, why
// these values).
#include "restore.h"

static const RestoreEdit edits[] = {
    // set a number below a placed object: file, Gyaml, Hash, Translate, then a path of keys and array indices, the
    // value shipped, ours
    RESTORE_SET("Banc/X/X_Static.bcett.byml.zs", "Area", 3333ull, 0.0, 101.5, 61.5, "Scale/2", 7.06, 3.0),
    // set a number in a pack member
    RESTORE_SET_MEMBER("Pack/Actor/Obj_W.pack.zs", "GameParameter/WParam/Obj_W.game__object__WParam.bgyml",
                       "MoveSpeed", 0.15, 0.24),
};

RESTORATION(name, "KEY", "Menu label", "One-line description for the menu.", edits);
```

- The key is upper case, digits and `_`; the setting becomes `restore_<key>`.
- Hashes and positions come from the player's files of the game version the port builds (`Banc/<Area>/<Area>_*.bcett.byml.zs`,
  `Pack/Actor/<Name>.pack.zs`); the edit list holds facts about the game, never its files.
- Run `--probe-restore` on a game build: every edit of the new restoration should say what it did.

Not yet possible: removing or adding a placed object, adding a key or changing a string, editing logic graphs
(`.ainb`), text (`Mals/`) or binary files.
