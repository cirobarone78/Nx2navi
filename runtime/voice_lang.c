// The game's own Voice Language option (Ocarina 1.1.1), which the Game tab's Voice language row shows and changes.
//
// How the game picks its languages: the text comes from Mals/<region>.Product.<n>.sarc.zs of the language
// nn::oe::GetDesiredLanguage answers (ov_system.c ov_language). The voices follow an option of their own,
// OptionData.VoiceLanguage, an enum flag of the game's flag database with nine values (Japanese ... Portuguese_Brazil),
// saved in option.sav and offered on the game's own options screen as Voice Language. The game's voice manager fills
// it from the text language once when it starts (main+0x9b48c0, from the table at main+0x3f0fd28); a loaded option.sav
// then replaces it, so the text language decides the voices only while there is no option.sav. A voice file names the
// region of its language: Voice/Resource/<region>_EventFlowMsg-<event>.bars.zs with Voice/Resource/Stream/<region>_*.bwav
// for the spoken lines of events, Sound/Resource/NV_<region>_Custom_<character>.bars.zs and their streams for the
// characters' calls.
//
// Reading: the flag database is the manager at [[main+0x4c7e180]]. Its Enum container at +0x248 holds the entry count
// (+0x08), the entries of 0x38 bytes (+0x10) and an open-addressing table of {u32 hash, u32 entry} slots probed from
// hash % capacity (+0x18, capacity +0x24); an entry's current value, the hash of the value's name, is at +0x1c. The
// voice manager reads it the same way (main+0xa7ca30).
// Writing: as the game's own default does: GetStruct (main+0x9b3eb0) of OptionData into a reference on the guest stack,
// then SetEnumMember (main+0x9b3ba0) of VoiceLanguage with the value's hash, on the game's thread between frames.
// SetEnumMember puts the change on the database's queue and returns at once; the game's per-frame drain applies it, as
// it applies a change made on the game's options screen, and the voice manager sees the flag change and takes the new
// language.
// Storing: the game writes option.sav only when it saves (its autosaves, and when its options screen closes), so a
// change made only here would be gone at the next start. Once the flag holds the new value, the runtime asks the
// game's AutoSaveMgr for that save itself: main+0xee9230 (AutoSaveMgr) marks entry 1 of the manager's request list
// ([+0x128]: a count, then entries of 0x138 bytes), and the manager's update writes option.sav. The game calls the same
// routine from the sequence node SeqExecuteSaveGameData (main+0xf8cb58) and from main+0x3233d40 and +0x3233e08; the
// node's other routine, main+0xf8cb80, marks entry 3, which wrote account.sav when tried. The AutoSaveMgr is component
// 12 of the system at **[main+0x4c7e178] (main+0xba7880 returns it).
// Ready: the row takes changes only once the game has read option.sav (fs_host.c calls voice_lang_options_opened on
// its first open of the file, which a new profile makes too) and LOAD_FRAMES presents have passed, so a change is not
// made to the default value the file then replaces.
// Flags, structs, members and values are named by MurmurHash3 (x86, 32 bits, seed 0) of the strings below.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "fs.h"
#include "host.h"
#include "ui_text.h"
#include "voice_lang.h"

#define DB_HOLDER 0x7004c7e180ull       // [this] -> [+0] the flag database manager
#define GET_STRUCT 0x70009b3eb0ull      // (manager, reference *, struct hash) -> found
#define SET_ENUM_MEMBER 0x70009b3ba0ull // (manager, value hash, reference *, member hash)
#define SYSTEM_HOLDER 0x7004c7e178ull   // [[this]] the system the AutoSaveMgr is a component of
#define COMPONENT 0x7000ba7880ull       // (system, index) -> the component, 0 when absent
#define SAVE_OPTIONS 0x7000ee9230ull    // (AutoSaveMgr): marks the options entry of its request list

enum { ENUMS = 0x248, CN_COUNT = 0x08, CN_ENTRIES = 0x10, CN_TABLE = 0x18, CN_CAP = 0x24, ENUM_SIZE = 0x38,
       E_CURRENT = 0x1c, SETTLE_FRAMES = 120, LOAD_FRAMES = 30, AUTOSAVE_MGR = 12, AS_REQUESTS = 0x128,
       AS_OPTIONS_ENTRY = 1 };

// The option's values in the game's order, each with the Switch language whose voices it is and the region its voice
// files carry.
static const struct { const char *value, *code, *region; UiTextId name; } choices[VOICE_LANGS] = {
    {"Japanese", "ja", "JPja", UI_MENU_LANGNAME_JA},
    {"English", "en-US", "USen", UI_MENU_LANGNAME_EN},
    {"French_France", "fr", "EUfr", UI_MENU_LANGNAME_FR},
    {"French_Canada", "fr-CA", "USfr", UI_MENU_LANGNAME_FR_CA},
    {"German", "de", "EUde", UI_MENU_LANGNAME_DE},
    {"Spanish_Spain", "es", "EUes", UI_MENU_LANGNAME_ES},
    {"Spanish_LatinAmerica", "es-419", "USes", UI_MENU_LANGNAME_ES_419},
    {"Italian", "it", "EUit", UI_MENU_LANGNAME_IT},
    {"Portuguese_Brazil", "pt-BR", "USpt", UI_MENU_LANGNAME_PT_BR},
};

static uint32_t value_hash[VOICE_LANGS], flag_hash, struct_hash, member_hash;
static _Atomic uint64_t entry_at;      // the flag's entry, 0 while the database is not there
static _Atomic unsigned offered;       // a bit per choice whose voice files the RomFS index has
static _Atomic int scanned;
static _Atomic int requested = -1;     // the menu's choice, taken by the next present
static _Atomic int pending = -1;       // set, not yet seen in the flag
static _Atomic int options_opened;     // the game has opened save:/option.sav (fs_host.c)
static uint64_t seen_mgr, seen_table, seen_entries;   // the database last checked readable
static int logged = -2, pending_frames, load_frames, nofiles_logged = -2;

static uint32_t rotl(uint32_t x, int r) { return x << r | x >> (32 - r); }

static uint32_t murmur3(const char *s) {
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    size_t n = strlen(s), i = 0;
    uint32_t h = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t k = (uint32_t)(uint8_t)s[i] | (uint32_t)(uint8_t)s[i + 1] << 8 | (uint32_t)(uint8_t)s[i + 2] << 16 |
                     (uint32_t)(uint8_t)s[i + 3] << 24;
        h ^= rotl(k * c1, 15) * c2;
        h = rotl(h, 13) * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    for (size_t j = n - i; j > 0; j--) k = k << 8 | (uint8_t)s[i + j - 1];
    if (n - i) h ^= rotl(k * c1, 15) * c2;
    h ^= (uint32_t)n;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

static void names_once(void) {
    if (flag_hash) return;
    for (int i = 0; i < VOICE_LANGS; i++) value_hash[i] = murmur3(choices[i].value);
    struct_hash = murmur3("OptionData");
    member_hash = murmur3("VoiceLanguage");
    flag_hash = murmur3("OptionData.VoiceLanguage");
}

int voice_lang_supported(void) { return RT_V111; }
const char *voice_lang_code(int i) { return i >= 0 && i < VOICE_LANGS ? choices[i].code : ""; }
int voice_lang_name(int i) { return i >= 0 && i < VOICE_LANGS ? (int)choices[i].name : (int)UI_MENU_GAME_LANG_AUTO; }

int voice_lang_index(const char *code) {
    for (int i = 0; code && i < VOICE_LANGS; i++)
        if (!strcmp(code, choices[i].code)) return i;
    return -1;
}

static int index_of(uint32_t value) {
    for (int i = 0; i < VOICE_LANGS; i++)
        if (value == value_hash[i]) return i;
    return -1;
}

int voice_lang_state(int *cur, unsigned *bits) {
    uint64_t e = atomic_load(&entry_at);
    if (!e || !atomic_load(&scanned)) return 0;
    *cur = index_of(LD32(e + E_CURRENT));
    *bits = atomic_load(&offered);
    return 1;
}

int voice_lang_pending(void) {
    int r = atomic_load(&requested);
    return r >= 0 ? r : atomic_load(&pending);
}

void voice_lang_request(int i) {
    if (i >= 0 && i < VOICE_LANGS) atomic_store(&requested, i);
}

void voice_lang_options_opened(void) { atomic_store(&options_opened, 1); }

// The regions that have voice files: Voice/Resource/<region>_... (the spoken lines; Stream/ is a folder, not a region).
static void scan(void) {
    uint64_t n = romfs_file_count();
    unsigned bits = 0, all = (1u << VOICE_LANGS) - 1;
    for (uint64_t i = 0; i < n && bits != all; i++) {
        uint64_t off, size;
        const char *p = romfs_entry(i, &off, &size);
        if (!p || strncmp(p, "Voice/Resource/", 15) || strlen(p) < 20 || p[19] != '_') continue;
        for (int c = 0; c < VOICE_LANGS; c++)
            if (!memcmp(p + 15, choices[c].region, 4)) bits |= 1u << c;
    }
    atomic_store(&offered, bits);
    atomic_store(&scanned, 1);
    char list[VOICE_LANGS * 5 + 1] = "";
    for (int c = 0; c < VOICE_LANGS; c++)
        if (bits & 1u << c) strcat(list, " "), strcat(list, choices[c].region);
    rt_log("voice: the RomFS has voices for%s", bits ? list : " no language");
}

// The flag's entry in the database, 0 while the game has not built it.
static uint64_t find_entry(uint64_t mgr) {
    uint64_t c = mgr + ENUMS;
    uint64_t table = LD64(c + CN_TABLE), entries = LD64(c + CN_ENTRIES);
    uint32_t cap = LD32(c + CN_CAP), count = LD32(c + CN_COUNT);
    if (!cap || !table || !entries || !count) return 0;
    if (mgr != seen_mgr || table != seen_table || entries != seen_entries) {
        if (!mem_readable(table, 8ull * cap) || !mem_readable(entries, (uint64_t)ENUM_SIZE * count)) return 0;
        seen_mgr = mgr, seen_table = table, seen_entries = entries;
    }
    for (uint32_t i = flag_hash % cap, k = 0; k < cap; k++, i = i + 1 == cap ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == flag_hash) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            return idx < count ? entries + (uint64_t)idx * ENUM_SIZE : 0;
        }
        if (!h) return 0;
    }
    return 0;
}

// The guest's registers as they were at the present, after calls into the game.
static void restore(Cpu *c, const Cpu *keep) {
    memcpy(c->x, keep->x, sizeof c->x);
    memcpy(c->v, keep->v, sizeof c->v);
    c->nzcv = keep->nzcv, c->fpsr = keep->fpsr, c->sp = keep->sp;
}

// Sets the option through the game's own routines.
static int set_option(Cpu *c, uint64_t mgr, int i) {
    Cpu keep = *c;
    uint64_t ref = (c->sp - 0x20) & ~15ull;   // below the guest's stack pointer, which the calls then start from
    c->sp = ref;
    ST64(ref, 0);
    ST64(ref + 8, 0);
    int found = (rt_call(c, GET_STRUCT, 3, mgr, ref, (uint64_t)struct_hash) & 1) != 0;
    if (found) rt_call(c, SET_ENUM_MEMBER, 4, mgr, (uint64_t)value_hash[i], ref, (uint64_t)member_hash);
    restore(c, &keep);
    return found;
}

// Asks the game to write option.sav (see the top of the file); 0 when its AutoSaveMgr or the options entry is not there.
static int save_options(Cpu *c) {
    uint64_t holder = LD64(SYSTEM_HOLDER), sys = holder ? LD64(holder) : 0;
    if (!sys) return 0;
    Cpu keep = *c;
    uint64_t as = rt_call(c, COMPONENT, 2, sys, (uint64_t)AUTOSAVE_MGR);
    uint64_t list = as ? LD64(as + AS_REQUESTS) : 0;
    // With no entry 1 the game's routine marks entry 0 instead, a save of the game's progress.
    int ok = list && mem_readable(list, 4) && LD32(list) > AS_OPTIONS_ENTRY;
    if (ok) rt_call(c, SAVE_OPTIONS, 1, as);
    restore(c, &keep);
    return ok;
}

void voice_lang_present(Cpu *c) {
    if (!RT_V111) return;
    names_once();
    if (!atomic_load(&scanned)) {
        if (!romfs_file_count()) return;
        scan();
    }
    if (!atomic_load(&options_opened)) return;
    if (load_frames < LOAD_FRAMES) {
        load_frames++;
        return;
    }
    uint64_t holder = LD64(DB_HOLDER), mgr = holder ? LD64(holder) : 0, e = mgr ? find_entry(mgr) : 0;
    atomic_store(&entry_at, e);
    if (!e) return;
    int cur = index_of(LD32(e + E_CURRENT));
    if (cur != logged) {
        if (cur >= 0)
            rt_log("voice: the game's Voice Language option is %s (voices from %s)", choices[cur].value,
                   choices[cur].region);
        else
            rt_log("voice: the game's Voice Language option holds %#x, none of its nine values", LD32(e + E_CURRENT));
        logged = cur;
    }
    // An option whose voice files the RomFS lacks (a save from a fuller copy of the game) stays as it is: the runtime
    // does not change a player's saved choice by itself.
    if (cur >= 0 && !(atomic_load(&offered) & 1u << cur) && nofiles_logged != cur) {
        rt_log("voice: the RomFS has no voices for %s (%s); the option stays", choices[cur].value, choices[cur].region);
        nofiles_logged = cur;
    }
    int p = atomic_load(&pending);
    if (p >= 0 && (cur == p || ++pending_frames > SETTLE_FRAMES)) {
        if (cur != p)
            rt_log("voice: the game did not take %s within %d frames", choices[p].value, SETTLE_FRAMES);
        else if (save_options(c))
            rt_log("voice: asked the game to save its options (option.sav) with Voice Language %s", choices[p].value);
        else
            rt_log("voice: the game's save manager is not there; Voice Language %s is kept from the game's next save "
                   "of its options", choices[p].value);
        atomic_store(&pending, -1);
        p = -1;
    }
    int want = atomic_exchange(&requested, -1);
    // Against the value the flag will hold: going back to the current value while another change waits in the game's
    // queue is a change too.
    if (want < 0 || want == (p >= 0 ? p : cur)) return;
    if (!set_option(c, mgr, want)) {
        rt_log("voice: the game's flag database has no OptionData struct; the Voice Language option stays %s",
               cur >= 0 ? choices[cur].value : "as it is");
        return;
    }
    rt_log("voice: set the game's Voice Language option to %s", choices[want].value);
    atomic_store(&pending, want);
    pending_frames = 0;
}
