// The menu's Co-op tab (runtime/ui/coop_page.cpp) without the game: the page's own code, the menu's rows and shapes, the
// real room-secret parser and rules (coop_proto.c, coop_state.c) and Dear ImGui, driven frame by frame with the key
// events the menu hands ImGui (menu.cpp drain_events and pad_events: a key's down and up, a held key's repeats as more
// downs, a typed letter as its key and its character), the co-op layer and the settings answered here. Checks typing a
// name, a relay and a secret with the letters that switch tabs elsewhere, Make, Join with a wrong secret and a pasted
// one, the offer accepted and declined, a held or repeated confirm never accepting, removing or leaving, the maker's
// rules and the others' read-only ones, Remove and Leave; with the keyboard and with the pad.
// tests/coop_page_check.py builds and runs it; the exit status is the number of failed checks.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/ui/coop_page.cpp"   // its statics (the window's step, the field) are checked below
#include "imgui_internal.h"
#include "ui/menu_art.h"

// ---- the settings: the co-op rows of cfg.c's table, in memory

static CfgMeta kMetas[] = {
    {"COOP", "0", "0|1", 1},
    {"COOP_NAME", "", NULL, 1},
    {"COOP_SERVER", "", NULL, 1},
    {"COOP_PROGRESS", "shared", "shared|own", 1},
    {"COOP_ITEMS", "shared", "shared|own", 1},
    {"COOP_SONGS", "shared", "shared|own", 1},
    {"COOP_QUEST", "shared", "shared|own", 1},
    {"COOP_KEYS", "pool", "pool|own", 1},
    {"COOP_RUPEES", "own", "own|pool", 1},
    {"COOP_AMMO", "own", "own|pool", 1},
};
enum { N_METAS = sizeof kMetas / sizeof kMetas[0] };
static char g_set[N_METAS][160];
static bool g_is_set[N_METAS];
static unsigned g_cfg_version = 1;

static int meta_index(const char *key) {
    if (!strncmp(key, "NX2NAVI_", 8)) key += 8;
    for (int i = 0; i < N_METAS; i++)
        if (!strcmp(kMetas[i].key, key)) return i;
    return -1;
}

static void settings_clear(void) {
    memset(g_is_set, 0, sizeof g_is_set);
    g_cfg_version++;
}

// ---- the co-op layer, as the test sets it

static CoopView g_fake;
static CoopOffer g_fake_offer;
static int n_create, n_join, n_leave, n_remove, last_remove, n_accept;
static uint32_t last_accept;
static char last_join[64], g_clipboard[256], g_copied_text[64];
static bool g_paste_asked;

extern "C" {
const CfgMeta *cfg_meta(const char *key) {
    int i = meta_index(key);
    return i >= 0 ? &kMetas[i] : NULL;
}
const CfgMeta *cfg_meta_at(int i) { return i >= 0 && i < N_METAS ? &kMetas[i] : NULL; }
const char *cfg_get(const char *key) {
    int i = meta_index(key);
    return i >= 0 && g_is_set[i] ? g_set[i] : NULL;
}
const char *cfg_value(const char *key) {
    int i = meta_index(key);
    if (i < 0) return "";
    return g_is_set[i] && g_set[i][0] ? g_set[i] : kMetas[i].def;
}
const char *cfg_default(const char *key) {
    int i = meta_index(key);
    return i >= 0 ? kMetas[i].def : NULL;
}
int cfg_int(const char *key) { return atoi(cfg_value(key)); }
double cfg_double(const char *key) { return atof(cfg_value(key)); }
int cfg_bool(const char *key) { return strcmp(cfg_value(key), "0") != 0 && cfg_value(key)[0]; }
int cfg_from_env(const char *) { return 0; }
void cfg_set(const char *key, const char *value) {
    int i = meta_index(key);
    if (i < 0) return;
    snprintf(g_set[i], sizeof g_set[i], "%s", value);
    g_is_set[i] = true;
    g_cfg_version++;
}
int cfg_save(void) { return 1; }
int cfg_reset(const char *const *keys, int n) {
    for (int k = 0; k < n; k++)
        if (int i = meta_index(keys[k]); i >= 0) g_is_set[i] = false;
    g_cfg_version++;
    return n;
}
unsigned cfg_version(void) { return g_cfg_version; }
void menu_sound_play(MenuSound) {}
const char *ov_language(void) { return "en-US"; }
const char *plat_ui_language(void) { return "en"; }
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void rt_log(const char *, ...) {}
int menu_art(MenuArt, ImTextureID *, float *, float *) { return 0; }
void plat_mutex_lock(PlatMutex *) {}
void plat_mutex_unlock(PlatMutex *) {}
int menu_is_open(void) { return 1; }

void coop_view_get(CoopView *out) { *out = g_fake; }
int coop_command(CoopCmd cmd, const char *secret) {
    if (g_fake.state == COOP_OFF) return 0;   // as the layer: nothing while co-op is off
    if (cmd == COOP_CMD_CREATE) n_create++;
    if (cmd == COOP_CMD_JOIN) n_join++, snprintf(last_join, sizeof last_join, "%s", secret ? secret : "");
    if (cmd == COOP_CMD_LEAVE) n_leave++;
    return 1;
}
int coop_remove_member(int member) { return n_remove++, last_remove = member, 1; }
int coop_game_offer(CoopOffer *out) {
    *out = g_fake_offer;
    return out->token != 0;
}
int coop_game_accept(uint32_t token) {
    if (!token || token != g_fake_offer.token) return 0;
    return n_accept++, last_accept = token, 1;
}
int plat_clipboard_set(const char *utf8) { return snprintf(g_copied_text, sizeof g_copied_text, "%s", utf8), 1; }
void gfx_window_paste(void) { g_paste_asked = true; }
int menu_paste_take(char *out, size_t cap) {
    if (!g_paste_asked) return 0;
    g_paste_asked = false;
    snprintf(out, cap, "%s", g_clipboard);
    return 1;
}
}

// ---- frames and keys

static int failures, n_tab_keys;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static ImGuiTextBuffer g_text;   // the text the last frame drew with ImGui's text calls (lines, windows)

static void frame(void) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1 / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1200, 640), ImGuiCond_Always);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::LogToBuffer();
    opt_begin("##coop", 1);   // as menu.cpp tab_coop
    coop_rows(1);
    opt_end();
    coop_after_page(1);
    g_text.clear();
    g_text.append(GImGui->LogBuffer.c_str());
    ImGui::LogFinish();
    // menu.cpp layout_menu's tab keys, which the page's window and a text field keep for themselves
    const bool keeps_keys = coop_modal() || io.WantTextInput;
    if (!keeps_keys && (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))) n_tab_keys++;
    ImGui::End();
    ImGui::Render();
}

static void frames(int n) {
    for (int i = 0; i < n; i++) frame();
}

static void key(ImGuiKey k, bool down) { ImGui::GetIO().AddKeyEvent(k, down); }

// A press and its release, a few frames apart as a player's are.
static void tap(ImGuiKey k) {
    key(k, true);
    frames(3);
    key(k, false);
    frames(3);
}

// Held for n frames with the system's repeats (another down every frame), then released.
static void hold(ImGuiKey k, int n) {
    for (int i = 0; i < n; i++) {
        key(k, true);
        frame();
    }
    key(k, false);
    frames(3);
}

// Typed on a keyboard: a letter's key goes down, its character arrives, the key comes up (SDL's order).
static void type(const char *s) {
    for (; *s; s++) {
        char c = *s;
        ImGuiKey k = c >= 'a' && c <= 'z' ? (ImGuiKey)(ImGuiKey_A + (c - 'a'))
                   : c >= 'A' && c <= 'Z' ? (ImGuiKey)(ImGuiKey_A + (c - 'A'))
                                          : ImGuiKey_None;
        if (k != ImGuiKey_None) key(k, true);
        ImGui::GetIO().AddInputCharacter((unsigned)c);
        frame();
        if (k != ImGuiKey_None) key(k, false);
        frame();
    }
}

struct Input {
    const char *name;
    ImGuiKey act, back, up, down, right, left;
};
static const Input kKeyboard = {"keyboard", ImGuiKey_Enter, ImGuiKey_Escape, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                                ImGuiKey_RightArrow, ImGuiKey_LeftArrow};
static const Input kPad = {"gamepad", ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadDpadUp,
                           ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadLeft};

static ImGuiID window_id(const char *label) {
    ImGuiWindow *w = ImGui::FindWindowByName("##coopwin");
    return w ? w->GetID(label) : 0;
}

static const char *nav_name(void) {
    static char out[64];
    static const char *const labels[] = {"Not now##no", "Accept##yes", "Cancel##no", "Remove##yes", "Stay##no",
                                         "Leave the room##yes", "OK##yes", "Join a room##yes", "Paste", "##coop_field"};
    for (const char *l : labels)
        if (GImGui->NavId && GImGui->NavId == window_id(l)) return l;
    snprintf(out, sizeof out, "%08x", GImGui->NavId);
    return out;
}

static bool shows(UiTextId id) { return strstr(g_text.c_str(), menu_text(id)) != nullptr; }

// The cursor on a row of the page, as a move of the D-pad would put it there; then act on it.
static void row(const char *key_of_row) {
    opt_focus_key(key_of_row);
    frames(3);
}

static void press_row(const Input &in, const char *key_of_row) {
    row(key_of_row);
    tap(in.act);
}

static void close_window(void) {
    coop_cancel();
    frames(4);
}

static void counters_clear(void) {
    n_create = n_join = n_leave = n_remove = n_accept = 0;
    last_remove = 0, last_accept = 0, last_join[0] = 0, n_tab_keys = 0;
}

// ---- the views the layer hands out

static void member(CoopView &v, int id, const char *name, bool maker, uint32_t heard_ms) {
    CoopPeer &m = v.member[v.n++];
    memset(&m, 0, sizeof m);
    m.id = (uint8_t)id, m.maker = maker, m.heard_ms = heard_ms;
    snprintf(m.name, sizeof m.name, "%s", name);
}

static void out_of_room(CoopState st, const char *status) {
    memset(&g_fake, 0, sizeof g_fake);
    memset(&g_fake_offer, 0, sizeof g_fake_offer);
    g_fake.state = st;
    g_fake.version++;
    snprintf(g_fake.status, sizeof g_fake.status, "%s", status);
}

// Bob in Alice's room (Alice made it), or Alice in her own room with Bob; the offer waiting or not.
static void in_room_as(bool maker, uint32_t token, bool accepted) {
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.state = COOP_JOINED;
    snprintf(g_fake.code, sizeof g_fake.code, "K7M3PR-BCDFGH-234567");
    member(g_fake, 1, "Alice", true, maker ? 0 : 1200);
    member(g_fake, 2, "Bob", false, maker ? 2300 : 0);
    g_fake.self = maker ? 1 : 2, g_fake.maker = 1, g_fake.made = maker;
    g_fake.version++;
    memset(&g_fake_offer, 0, sizeof g_fake_offer);
    g_fake_offer.token = token, g_fake_offer.accepted = accepted, g_fake_offer.copy = accepted;
    g_fake_offer.n = 1;
    g_fake_offer.member[0].id = maker ? 2 : 1, g_fake_offer.member[0].maker = !maker, g_fake_offer.member[0].heard = 1;
    snprintf(g_fake_offer.member[0].name, sizeof g_fake_offer.member[0].name, "%s", maker ? "Bob" : "Alice");
    coop_rules_default(&g_fake_offer.rules);
    g_fake_offer.rules.epoch = 1, g_fake_offer.rules.maker = 1;
}

// ---- the checks

static void check_fields_by_keyboard(void) {
    printf("keyboard text:\n");
    settings_clear();
    out_of_room(COOP_OFF, "");
    counters_clear();
    frames(4);
    // with no relay, Make and Join are greyed: a press makes nothing
    press_row(kKeyboard, "##coop_make");
    press_row(kKeyboard, "##coop_join");
    CHECK(n_create == 0 && !g_open, "Make and Join with no relay: %d rooms, window %d", n_create, g_open);

    // the name: the field takes the cursor, Q and E are letters (no tab switch), Enter keeps it
    press_row(kKeyboard, "NX2NAVI_COOP_NAME");
    CHECK(g_open && g_page == P_NAME && GImGui->ActiveId == window_id("##coop_field"), "Name: window %d step %d, field %s",
          g_open, g_page, GImGui->ActiveId == window_id("##coop_field") ? "active" : "not active");
    type("Zelda Qe");
    CHECK(n_tab_keys == 0 && coop_modal(), "typing Q and E in the name switched tabs %d times", n_tab_keys);
    tap(ImGuiKey_Enter);
    CHECK(!g_open && !strcmp(cfg_value("NX2NAVI_COOP_NAME"), "Zelda Qe"), "Name: '%s' kept, window %d",
          cfg_value("NX2NAVI_COOP_NAME"), g_open);
    printf("  name typed with Q and E: '%s', %d tab switches\n", cfg_value("NX2NAVI_COOP_NAME"), n_tab_keys);
    // the same letters with no field: the tab keys work (the gate above is not always shut)
    tap(ImGuiKey_E);
    CHECK(n_tab_keys == 1, "E on the page itself: %d tab switches", n_tab_keys);
    n_tab_keys = 0;

    // Esc in a field drops the edit and closes the window
    press_row(kKeyboard, "NX2NAVI_COOP_NAME");
    type("x");
    tap(ImGuiKey_Escape);
    CHECK(!g_open && !strcmp(cfg_value("NX2NAVI_COOP_NAME"), "Zelda Qe"), "Esc in the name: '%s', window %d",
          cfg_value("NX2NAVI_COOP_NAME"), g_open);

    // the relay
    press_row(kKeyboard, "NX2NAVI_COOP_SERVER");
    type("127.0.0.1:24680");
    tap(ImGuiKey_Enter);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_SERVER"), "127.0.0.1:24680"), "Relay: '%s'", cfg_value("NX2NAVI_COOP_SERVER"));
    printf("  relay typed: '%s'\n", cfg_value("NX2NAVI_COOP_SERVER"));

    // Make while co-op is off: the switch goes on and the room is made once the coop thread is up
    press_row(kKeyboard, "##coop_make");
    CHECK(cfg_bool("NX2NAVI_COOP") && n_create == 0, "Make with co-op off: switch %d, %d rooms before it is up",
          cfg_bool("NX2NAVI_COOP"), n_create);
    out_of_room(COOP_LEFT, "");
    frames(2);
    CHECK(n_create == 1, "Make once co-op is up: %d rooms", n_create);
    printf("  Make with co-op off: switch on, then %d room made\n", n_create);

    // Join with a wrong secret: refused in the window, nothing sent; then one in lower case with dashes
    press_row(kKeyboard, "##coop_join");
    CHECK(g_open && g_page == P_JOIN && GImGui->ActiveId == window_id("##coop_field"), "Join: the field is not active");
    type("k7m3pq-bcdfgh-23456e");   // E and Q are not in the alphabet of secrets
    tap(ImGuiKey_Enter);
    frames(2);
    CHECK(n_join == 0 && g_open && g_field_bad && shows(UI_MENU_COOP_SECRET_BAD) && n_tab_keys == 0,
          "a wrong secret: %d joins, window %d, said %d, %d tab switches", n_join, g_open, shows(UI_MENU_COOP_SECRET_BAD),
          n_tab_keys);
    printf("  wrong secret: %d joins, the window says it is not one\n", n_join);
    ImGui::ClearActiveID();
    frames(2);
    tap(ImGuiKey_Escape);
    press_row(kKeyboard, "##coop_join");
    type("k7m3pr-bcdfgh-234567");
    tap(ImGuiKey_Enter);
    CHECK(n_join == 1 && !strcmp(last_join, "K7M3PRBCDFGH234567") && !g_open, "the secret in lower case: %d joins of '%s'",
          n_join, last_join);
    printf("  secret in lower case with dashes: %d join of %s\n", n_join, last_join);
    // the relay knows no such room: the page says the secret is wrong
    out_of_room(COOP_LEFT, "no such room");
    frames(3);
    CHECK(shows(UI_MENU_COOP_NO_ROOM), "no such room: the page does not say the secret may be wrong");
    printf("  the relay has no such room: the page says so\n");
}

static void check_join_by_pad(void) {
    printf("gamepad join:\n");
    out_of_room(COOP_LEFT, "");
    counters_clear();
    frames(3);
    // the window opens with the cursor on Cancel: A, A goes back
    press_row(kPad, "##coop_join");
    CHECK(g_open && g_page == P_JOIN && !strcmp(nav_name(), "Cancel##no"), "Join by pad: cursor on %s", nav_name());
    tap(kPad.act);
    CHECK(!g_open && n_join == 0, "Join, A, A: window %d, %d joins", g_open, n_join);
    // Paste: the clipboard's secret with a line end and spaces, then Join
    snprintf(g_clipboard, sizeof g_clipboard, " k7m3pr-bcdfgh-234567\n");
    press_row(kPad, "##coop_join");
    tap(kPad.up);
    CHECK(!strcmp(nav_name(), "Paste"), "up from Cancel: %s", nav_name());
    tap(kPad.act);
    frames(2);
    CHECK(!strcmp(g_field, "K7M3PR-BCDFGH-234567"), "pasted: '%s'", g_field);
    tap(kPad.down);
    tap(kPad.right);
    CHECK(!strcmp(nav_name(), "Join a room##yes"), "down, right: %s", nav_name());
    tap(kPad.act);
    CHECK(n_join == 1 && !strcmp(last_join, "K7M3PRBCDFGH234567"), "pasted secret: %d joins of '%s'", n_join, last_join);
    // an empty clipboard says so
    snprintf(g_clipboard, sizeof g_clipboard, "\n");
    press_row(kPad, "##coop_join");
    tap(kPad.up);
    tap(kPad.act);
    frames(2);
    CHECK(g_paste_empty && shows(UI_MENU_COOP_PASTE_EMPTY), "an empty clipboard is not said");
    close_window();
    printf("  pasted secret joined by pad: %d join; an empty clipboard is said\n", n_join);
}

static void check_offer(const Input &in) {
    printf("%s offer:\n", in.name);
    in_room_as(false, 0x1234567u, false);
    counters_clear();
    frames(4);
    CHECK(shows(UI_MENU_COOP_OFFER_TRUST) == false && strstr(g_text.c_str(), "Alice (made the room)"),
          "the page does not name the room's members");
    // the row's press opens the window with the cursor on Not now; the same button again goes back
    press_row(in, "##coop_answer");
    CHECK(g_open && g_page == P_ACCEPT && !strcmp(nav_name(), "Not now##no"), "Accept window: cursor on %s", nav_name());
    CHECK(shows(UI_MENU_COOP_OFFER_COPY) && shows(UI_MENU_COOP_OFFER_TRUST), "the window does not say what accepting does");
    tap(in.act);
    CHECK(n_accept == 0 && !g_open, "Accept or not, act, act: %d accepts, window %d", n_accept, g_open);
    // held on the row through the window's opening, then held again in it
    row("##coop_answer");
    hold(in.act, 120);
    CHECK(n_accept == 0, "act held 2 s on the row: %d accepts", n_accept);
    if (!g_open) press_row(in, "##coop_answer");
    hold(in.act, 120);
    CHECK(n_accept == 0, "act held again in the window: %d accepts", n_accept);
    // declined: Not now, and B
    close_window();
    press_row(in, "##coop_answer");
    tap(in.back);
    CHECK(n_accept == 0 && !g_open, "back: %d accepts", n_accept);
    // a new player while the cursor is on Accept: the window shows the new offer and the cursor goes back to Not now
    press_row(in, "##coop_answer");
    tap(in.right);
    CHECK(!strcmp(nav_name(), "Accept##yes"), "right from Not now: %s", nav_name());
    g_fake_offer.token = 0x7654321u, g_fake_offer.n = 2;
    snprintf(g_fake_offer.member[1].name, sizeof g_fake_offer.member[1].name, "Carol");
    frames(3);
    CHECK(!strcmp(nav_name(), "Not now##no") && strstr(g_text.c_str(), "Carol"), "a new offer under the cursor: %s",
          nav_name());
    tap(in.act);
    CHECK(n_accept == 0, "a new offer, act: %d accepts", n_accept);
    printf("  act, act / held / back / a new offer: %d accepts\n", n_accept);
    // accepted on purpose: right, act
    press_row(in, "##coop_answer");
    tap(in.right);
    tap(in.act);
    CHECK(n_accept == 1 && last_accept == 0x7654321u && !g_open, "Accept chosen: %d accepts of %08x", n_accept, last_accept);
    printf("  right, act: %d accept of the offer shown\n", n_accept);
    g_fake_offer.accepted = 1, g_fake_offer.copy = 1;
    frames(3);
    CHECK(shows(UI_MENU_COOP_ACCEPTED), "accepted: the page does not say so");

    // the rules of someone else's room: read only
    const char *before = cfg_value("NX2NAVI_COOP_RUPEES");
    press_row(in, "##coop_room_rule5");
    row("##coop_room_rule5");
    tap(in.right);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_RUPEES"), before), "a member changed the room's rule: rupees %s",
          cfg_value("NX2NAVI_COOP_RUPEES"));
    printf("  a member's press and right on a rule: rupees still %s\n", cfg_value("NX2NAVI_COOP_RUPEES"));
}

static void check_maker(const Input &in) {
    printf("%s maker:\n", in.name);
    settings_clear();
    in_room_as(true, 0x2222223u, true);
    counters_clear();
    frames(4);
    // a rule the maker changes: rupees into a pool; items cannot leave a shared world (greyed)
    row("NX2NAVI_COOP_RUPEES");
    tap(in.right);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_RUPEES"), "pool"), "the maker's rupees: %s", cfg_value("NX2NAVI_COOP_RUPEES"));
    row("NX2NAVI_COOP_ITEMS");
    tap(in.right);
    tap(in.act);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_ITEMS"), "shared"), "items with progress shared: %s", cfg_value("NX2NAVI_COOP_ITEMS"));
    printf("  rupees -> %s; items stay %s while progress is shared\n", cfg_value("NX2NAVI_COOP_RUPEES"),
           cfg_value("NX2NAVI_COOP_ITEMS"));

    // Remove: the cursor on Cancel; act again, held, back: nothing; right, act: removed
    press_row(in, "##coop_member2");
    CHECK(g_open && g_page == P_REMOVE && !strcmp(nav_name(), "Cancel##no"), "Remove window: cursor on %s", nav_name());
    tap(in.act);
    CHECK(n_remove == 0 && !g_open, "Remove, act, act: %d removals", n_remove);
    row("##coop_member2");
    hold(in.act, 120);
    if (!g_open) press_row(in, "##coop_member2");
    hold(in.act, 120);
    CHECK(n_remove == 0, "act held on a player and in the window: %d removals", n_remove);
    close_window();
    press_row(in, "##coop_member1");   // the maker's own row opens nothing
    CHECK(!g_open, "the maker's own row opened a window");
    press_row(in, "##coop_member2");
    tap(in.right);
    tap(in.act);
    CHECK(n_remove == 1 && last_remove == 2, "Remove chosen: %d removals of %d", n_remove, last_remove);
    printf("  Remove: act, act / held: none; right, act: %d removal of member %d\n", n_remove, last_remove);

    // the secret: hidden until asked (its row's press shows it, a second hides it), copied on its row
    CHECK(!g_show_secret, "the secret shows unasked");
    press_row(in, "##coop_secret");
    CHECK(g_show_secret, "the secret's row did not show it");
    press_row(in, "##coop_secret");
    CHECK(!g_show_secret, "the secret's row did not hide it again");
    press_row(in, "##coop_copy");
    CHECK(!strcmp(g_copied_text, "K7M3PR-BCDFGH-234567"), "copied: '%s'", g_copied_text);

    // Leave: the cursor on Stay, the maker told what leaving costs; act again, held: stays; right, act: leaves
    press_row(in, "##coop_leave");
    CHECK(g_open && g_page == P_LEAVE && !strcmp(nav_name(), "Stay##no") && shows(UI_MENU_COOP_LEAVE_MAKER),
          "Leave window: cursor on %s", nav_name());
    tap(in.act);
    CHECK(n_leave == 0 && !g_open, "Leave, act, act: %d leaves", n_leave);
    row("##coop_leave");
    hold(in.act, 120);
    if (!g_open) press_row(in, "##coop_leave");
    hold(in.act, 120);
    CHECK(n_leave == 0, "act held on Leave and in the window: %d leaves", n_leave);
    close_window();
    press_row(in, "##coop_leave");
    tap(in.right);
    tap(in.act);
    CHECK(n_leave == 1, "Leave chosen: %d leaves", n_leave);
    printf("  Leave: act, act / held: stays; right, act: %d leave\n", n_leave);
}

// The conversions of a printf format in order ("%s%u" for "heard %u s ago, %s"), "%%" left out.
static void conversions(const char *f, char *out, size_t cap) {
    size_t k = 0;
    for (; *f && k + 3 < cap; f++) {
        if (*f != '%') continue;
        if (f[1] == '%') {
            f++;
            continue;
        }
        while (f[1] && !strchr("sdiuxXcfgp", f[1])) f++;
        out[k++] = '%', out[k++] = f[1];
    }
    out[k] = 0;
}

// The checks --probe-services makes of the catalog (ov_probe.c probe_menu_text), for the Co-op tab's texts: English,
// Russian, Brazilian Portuguese and Simplified Chinese, none empty, each short enough for the description pane, and
// the same printf conversions in every language as in English. The other menu texts that lack a language are counted
// and named, not failed: they are not this page's.
static void check_catalog(void) {
    static const char *const langs[] = {"en", "ru", "pt-BR", "zh-Hans"};
    for (int id = UI_MENU_TAB_COOP; id <= UI_MENU_COOP_TOAST_OFFER; id++) {
        char en[32], other[32];
        conversions(ui_text_in("en", (UiTextId)id), en, sizeof en);
        for (const char *l : langs) {
            CHECK(ui_text_defined(l, (UiTextId)id), "co-op text %d ('%s') lacks %s", id, ui_text_in("en", (UiTextId)id), l);
            CHECK(strlen(ui_text_in(l, (UiTextId)id)) + 256 < UI_DESC_BYTES, "co-op text %d in %s is too long", id, l);
            conversions(ui_text_in(l, (UiTextId)id), other, sizeof other);
            CHECK(!strcmp(en, other), "co-op text %d: %s has '%s', English '%s'", id, l, other, en);
        }
    }
    printf("catalog: the %d co-op texts in en, ru, pt-BR and zh-Hans, with English's conversions\n",
           UI_MENU_COOP_TOAST_OFFER - UI_MENU_TAB_COOP + 1);
    int gaps = 0;
    for (int id = UI_MENU_FIRST; id < UI_MENU_TAB_COOP; id++)
        for (const char *l : langs)
            if (!ui_text_defined(l, (UiTextId)id)) {
                if (!gaps++) printf("  not this page's, lacking a language:");
                printf(" %d (%s, '%s')", id, l, ui_text_in("en", (UiTextId)id));
            }
    if (gaps) printf("\n");
}

int main(void) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;   // as menu.cpp
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    unsigned char *px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    frames(2);

    check_fields_by_keyboard();
    check_join_by_pad();
    check_offer(kKeyboard);
    check_offer(kPad);
    check_maker(kKeyboard);
    check_maker(kPad);
    check_catalog();

    ImGui::DestroyContext();
    printf("coop page: %s\n", failures ? "see FAIL lines" : "ok");
    return failures;
}
