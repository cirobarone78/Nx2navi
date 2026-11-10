// BYML documents (byml.c): the parameter and placement files of the game (.byml, .bgyml, .bcett.byml), versions 2 to 7,
// little-endian ("YB", as this release's files are), read into a tree whose numbers can be set and written back.
// restore.c edits the player's own files with it.
//
// Strings of a parsed tree point into the bytes it was read from, so those bytes must outlive the tree. The parser
// refuses a dict whose keys are not in strictly increasing order and a hash dict whose hashes are not (the game looks
// them up sorted). byml_write lays the tree out the way the game's files are: sorted key and string tables, containers
// 4-aligned, 64-bit values 8-aligned, equal containers and values written once.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    BYML_STRING = 0xa0, BYML_BINARY = 0xa1, BYML_ARRAY = 0xc0, BYML_DICT = 0xc1, BYML_HASH32 = 0x20, BYML_BOOL = 0xd0,
    BYML_INT = 0xd1, BYML_FLOAT = 0xd2, BYML_UINT = 0xd3, BYML_INT64 = 0xd4, BYML_UINT64 = 0xd5, BYML_DOUBLE = 0xd6,
    BYML_NULL = 0xff,
};

typedef struct BymlNode BymlNode;
struct BymlNode {
    uint8_t type;
    uint64_t bits;                // scalars: the value's bits (32-bit types in the low half)
    const char *str;              // BYML_STRING: the text; BYML_BINARY: the bytes
    uint32_t len;                 // BYML_BINARY: the byte count
    uint32_t n;                   // containers: the item count
    BymlNode *items;              // containers: the items (dict values)
    const char **keys;            // BYML_DICT: the key of each item, in increasing order
    uint32_t *hashes;             // BYML_HASH32: the hash of each item, in increasing order
};

typedef struct {
    BymlNode root;
    uint16_t version;
} Byml;

// The document in p[0..n); 0 with why (a short text) when it is not one this reader knows.
int byml_parse(Byml *b, const uint8_t *p, size_t n, const char **why);
void byml_free(Byml *b);
// The file of the tree (malloc'd); NULL when out of memory or when it is too large for 32-bit offsets.
uint8_t *byml_write(const Byml *b, size_t *n);
// 1 when the two trees hold the same values (strings compared by text, floats by bits).
int byml_equal(const BymlNode *a, const BymlNode *b);

// Dict lookup; NULL when node is not a dict or lacks key.
BymlNode *byml_get(const BymlNode *node, const char *key);
// The node below node at path: keys and array indices separated by '/' ("Translate/2"); NULL when one is missing.
BymlNode *byml_path(BymlNode *node, const char *path);
// Scalars: the value as a double (NAN for a node that is not a number or bool), and set from a double in the node's
// own type (integers rounded); 0 when the node is not a number or bool or the value does not fit.
double byml_number(const BymlNode *node);
int byml_set_number(BymlNode *node, double v);
