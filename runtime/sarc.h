// SARC archives (sarc.c), as the restorations read them: the game's actor packs (Pack/Actor/<name>.pack.zs once
// unpacked) and Pack/ZsDic.pack.zs, little-endian, members named.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Member `name` of the archive p[0..n): its start and size; 0 with why when the archive is not one, lacks it, or the
// member runs past the end. Only that member's node is checked.
int sarc_find(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size, const char **why);
// Member i of the archive (in node order): its name ("" for an unnamed one; name may be NULL when it is not wanted),
// start and size; 0 past the last one, when p[0..n) is not an archive, or when member i or its name runs past the end.
int sarc_at(const uint8_t *p, size_t n, unsigned i, const char **name, size_t *at, size_t *size);
// The archive with member `name`'s bytes replaced by data[0..size) (malloc'd); NULL with why, also when any member or
// name runs past the end. The other members keep their bytes and order; a member that grows past the next one moves
// the ones after it by a step that keeps the alignment of each of their offsets (up to 0x2000).
uint8_t *sarc_replace(const uint8_t *p, size_t n, const char *name, const uint8_t *data, size_t size, size_t *out_n,
                      const char **why);
