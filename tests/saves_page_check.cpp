// The Saves tab's file window (runtime/ui/saves_page.cpp) without the game: the page's own code, the menu's rows and
// shapes and Dear ImGui, driven frame by frame with the key events the menu hands ImGui (menu.cpp drain_events and
// pad_events: a key's down and up, a held key's repeats as more downs), the save files answered here. Checks that no
// confirmation opens with the cursor on the button that acts, that pressing A or Enter again, a B and A again, or a
// held key never deletes, restarts, copies or renames, and that the button that acts still works when chosen.
// tests/saves_page_check.py builds and runs it; the exit status is the number of failed checks.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/ui/saves_page.cpp"   // its statics (the step, the window's file) are checked below
#include "imgui_internal.h"
#include "ui/menu_art.h"

// ---- what the page, the rows and the catalog ask of the runtime

static int n_delete, n_new, n_rename, n_choose, n_restart, n_cfg_save, cfg_save_ok = 1, import_waiting;
static int last_delete = -1;
static int text_on, rename_refused;    // the window's text input as the menu last set it; Rename answers FS_FILE_NAME_SET
static const char *setting;             // save_file
static bool menu_open = true;           // false: frames as menu_overlay_frame runs them while the menu is closed

extern "C" {
const CfgMeta *cfg_meta(const char *) { return NULL; }
const CfgMeta *cfg_meta_at(int) { return NULL; }
const char *cfg_get(const char *) { return NULL; }
const char *cfg_value(const char *) { return ""; }
const char *cfg_default(const char *) { return NULL; }
int cfg_int(const char *) { return 0; }
double cfg_double(const char *) { return 0; }
int cfg_bool(const char *) { return 0; }
int cfg_from_env(const char *) { return 0; }
int cfg_pending(const char *) { return 0; }   // the rows' "after restart" note asks it; no setting waits here
void cfg_set(const char *, const char *) {}
int cfg_save(void) { return n_cfg_save++, cfg_save_ok; }
int cfg_reset(const char *const *, int) { return 0; }
unsigned cfg_version(void) { return 1; }
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
int plat_thread_spawn(PlatThreadFn, void *, const char *, size_t) { return 0; }   // the job runs on the caller
int menu_is_open(void) { return 1; }
void gfx_window_request_restart(void) { n_restart++; }
void gfx_window_text_input(int on) { text_on = on; }

// three files: 1 in use and the next start's, named by save_file by its name; 2 neither; 3 the one the menu picked
// last (save-files/current) while save_file picks file 1
int fs_save_files(FsSaveFile *out, int max) {
    int n = 0;
    for (int id = 1; id <= 3 && n < max; id++, n++) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].id = id;
        snprintf(out[n].name, sizeof out[n].name, id == 1 ? "Field run" : id == 2 ? "Forest run" : "Water run");
        out[n].running = out[n].next = out[n].named = id == 1;
        out[n].current = id == 3;
    }
    return n;
}
int fs_save_file_running(void) { return 1; }
int fs_save_file_new(const char *, int) { return n_new++, 7; }
int fs_save_file_next_id(void) { return 7; }   // file 6 was deleted: past the highest listed (3) plus one
static char last_name[FS_SAVE_NAME_MAX];
int fs_save_file_rename(int, const char *name) {
    return snprintf(last_name, sizeof last_name, "%s", name), n_rename++, rename_refused ? FS_FILE_NAME_SET : 0;
}
int fs_save_file_choose(int) { return n_choose++, 0; }
int fs_save_file_delete(int id) { return n_delete++, last_delete = id, 0; }
const char *fs_save_file_setting(void) { return setting; }
int fs_save_busy(void) { return 0; }
int fs_save_import_waiting(void) { return import_waiting; }
}

// ---- frames and keys

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

// One frame as menu_overlay_frame runs it: the menu laid out while it is open (the Saves tab), then the window's text
// input follows the menu, on the frames of a closed menu too.
static void frame(void) {
    ImGuiIO &io = ImGui::GetIO();
    if (menu_open) {
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1 / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1200, 640), ImGuiCond_Always);
        ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        opt_begin("##saves", 1);
        saves_rows(1);   // as menu.cpp tab_saves
        opt_end();
        saves_after_page(1);
        ImGui::End();
        ImGui::Render();
    }
    menu_text_input_follow(menu_open && io.WantTextInput);
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

struct Input {
    const char *name;
    ImGuiKey act, back, up, right, left;
};
static const Input kKeyboard = {"keyboard", ImGuiKey_Enter, ImGuiKey_Escape, ImGuiKey_UpArrow, ImGuiKey_RightArrow,
                                ImGuiKey_LeftArrow};
static const Input kPad = {"gamepad", ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadDpadUp,
                           ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadLeft};

// The ID of a button of the file window, as two_buttons and the actions step label them.
static ImGuiID window_id(const char *label) {
    ImGuiWindow *w = ImGui::FindWindowByName("##savefile");
    return w ? w->GetID(label) : 0;
}

static const char *nav_name(void) {
    static char out[64];
    static const char *const labels[] = {"Cancel##no", "Delete##yes", "Restart##yes", "Copy##yes", "Rename##yes",
                                         "Play this file", "Copy", "Rename", "Delete", "Back", "##save_file_name"};
    for (const char *l : labels)
        if (GImGui->NavId && GImGui->NavId == window_id(l)) return l;
    snprintf(out, sizeof out, "%08x", GImGui->NavId);
    return out;
}

// The window of a file opened by its row, as a press on the row does (open_window); the cursor on Back.
static void open_file(const char *id) {
    saves_cancel();
    frames(4);
    n_delete = n_new = n_rename = n_choose = n_restart = n_cfg_save = 0;
    last_delete = -1;
    saves_test_set("NX2NAVI_SAVE_FILE_OPEN", id);
    frames(4);
    CHECK(g_open && g_page == P_ACTIONS && !strcmp(nav_name(), "Back"), "the window opens with the cursor on Back, not %s",
          nav_name());
}

static void open_file2(void) { open_file("2"); }

// From Back up n buttons (Delete 1, Rename 2, Copy 3, Play this file 4), then act.
static void choose(const Input &in, int up) {
    for (int i = 0; i < up; i++) tap(in.up);
    tap(in.act);
}

static void check_input(const Input &in) {
    printf("%s:\n", in.name);
    // Delete, then the act key again: the confirmation's cursor is on Cancel, so the second press goes back
    open_file2();
    choose(in, 1);
    CHECK(g_page == P_DELETE && !strcmp(nav_name(), "Cancel##no"), "%s: the delete confirmation opens with the cursor "
          "on %s", in.name, nav_name());
    tap(in.act);
    CHECK(n_delete == 0 && g_open && g_page == P_ACTIONS && !strcmp(nav_name(), "Back"),
          "%s: Delete, act, act: %d deletes, step %d, cursor on %s", in.name, n_delete, g_page, nav_name());
    printf("  Delete then %s again: %d deletes, back on the file's step with the cursor on %s\n", in.name, n_delete,
           nav_name());

    // A, B, A, A: back from the confirmation the cursor is on Back, whose press closes the window
    open_file2();
    choose(in, 1);
    tap(in.back);
    CHECK(g_open && g_page == P_ACTIONS && !strcmp(nav_name(), "Back"), "%s: back from the confirmation: cursor on %s",
          in.name, nav_name());
    tap(in.act);
    tap(in.act);
    CHECK(n_delete == 0, "%s: Delete, back, act, act: %d deletes", in.name, n_delete);
    printf("  Delete, back, act, act: %d deletes\n", n_delete);

    // the act key held through the confirmation, with the system's repeats, for two seconds
    open_file2();
    for (int i = 0; i < 1; i++) tap(in.up);
    hold(in.act, 120);
    CHECK(n_delete == 0 && g_page == P_DELETE, "%s: act held on Delete for 2 s: %d deletes, step %d", in.name, n_delete,
          g_page);
    hold(in.act, 120);
    CHECK(n_delete == 0, "%s: act held again in the confirmation: %d deletes", in.name, n_delete);
    printf("  act held 2 s on Delete, then again in the confirmation: %d deletes\n", n_delete);

    // chosen on purpose, Delete still deletes
    open_file2();
    choose(in, 1);
    tap(in.right);
    CHECK(!strcmp(nav_name(), "Delete##yes"), "%s: right from Cancel: %s", in.name, nav_name());
    tap(in.left);
    CHECK(!strcmp(nav_name(), "Cancel##no"), "%s: left from Delete back to Cancel: %s", in.name, nav_name());
    tap(in.right);
    tap(in.act);
    CHECK(n_delete == 1 && last_delete == 2 && !g_open, "%s: Delete chosen in the confirmation: %d deletes of file %d",
          in.name, n_delete, last_delete);
    printf("  Delete, right, act: %d delete of file %d\n", n_delete, last_delete);

    // Play this file: the confirmation's cursor on Cancel; Restart only when chosen
    open_file2();
    choose(in, 4);
    CHECK(g_page == P_PLAY && !strcmp(nav_name(), "Cancel##no"), "%s: play confirmation cursor on %s", in.name, nav_name());
    tap(in.act);
    CHECK(n_choose == 0 && n_restart == 0 && g_page == P_ACTIONS, "%s: Play, act, act: %d choices, %d restarts", in.name,
          n_choose, n_restart);
    choose(in, 4);
    tap(in.right);
    tap(in.act);
    CHECK(n_choose == 1 && n_restart == 1 && n_cfg_save == 1, "%s: Restart chosen: %d choices, %d restarts", in.name,
          n_choose, n_restart);
    printf("  Play this file then %s again: no restart; Restart chosen: %d restart\n", in.name, n_restart);
}

// From Cancel, every way to move but Right: Up and Down (by pad the d-pad and the left stick too), Tab and Shift+Tab,
// and Left, each pressed one to six times, then held for 2 s with its repeats. The cursor never reaches the button
// that acts, and the act key afterwards presses Cancel. ImGui loops a popup's navigation up and down and Tab goes to
// the next button, so one Up, Down or Tab from Cancel used to land on it.
static void check_only_right(const Input &in) {
    struct Way {
        const char *name;
        ImGuiKey key;
        bool shift;
    };
    static const Way keys[] = {{"Up", ImGuiKey_UpArrow, false},  {"Down", ImGuiKey_DownArrow, false},
                               {"Tab", ImGuiKey_Tab, false},     {"Shift+Tab", ImGuiKey_Tab, true},
                               {"Left", ImGuiKey_LeftArrow, false}};
    static const Way pad[] = {{"Up", ImGuiKey_GamepadDpadUp, false},          {"Down", ImGuiKey_GamepadDpadDown, false},
                              {"Left", ImGuiKey_GamepadDpadLeft, false},      {"stick up", ImGuiKey_GamepadLStickUp, false},
                              {"stick down", ImGuiKey_GamepadLStickDown, false}};
    struct Step {
        const char *name;
        int up;
        const char *yes;
        int *acted;
    };
    // the name steps by pad only: by keyboard the name field has the cursor, and Enter in it makes the file
    const Step steps[] = {{"Delete", 1, "Delete##yes", &n_delete}, {"Play this file", 4, "Restart##yes", &n_restart},
                          {"Copy", 3, "Copy##yes", &n_new}, {"Rename", 2, "Rename##yes", &n_rename}};
    const bool pad_in = in.act == kPad.act;
    const Way *ways = pad_in ? pad : keys;
    int reached = 0, acted = 0, runs = 0;
    for (const Step &s : steps) {
        if (!pad_in && s.up != 1 && s.up != 4) continue;
        for (int w = 0; w < 5; w++) {
            open_file2();
            choose(in, s.up);
            CHECK(!strcmp(nav_name(), "Cancel##no"), "%s: %s opens on %s", in.name, s.name, nav_name());
            if (ways[w].shift) key(ImGuiMod_Shift, true);
            int here = 0;   // presses and held frames that left the cursor on the button that acts
            for (int i = 0; i < 6; i++) {
                tap(ways[w].key);
                here += !strcmp(nav_name(), s.yes);
            }
            for (int i = 0; i < 120; i++) {   // held 2 s, with the system's repeats
                key(ways[w].key, true);
                frame();
                here += !strcmp(nav_name(), s.yes);
            }
            key(ways[w].key, false);
            frames(3);
            if (ways[w].shift) key(ImGuiMod_Shift, false);
            reached += here;
            CHECK(!here, "%s: %s, then %s: the cursor reached %s %d times", in.name, s.name, ways[w].name, s.yes, here);
            tap(in.act);
            acted += *s.acted;
            CHECK(*s.acted == 0, "%s: %s, then %s and act: it acted %d times", in.name, s.name, ways[w].name, *s.acted);
            runs++;
        }
    }
    printf("  from Cancel, %s once to six times and held: the button that acts reached %d times, acted %d times, in "
           "%d runs\n", pad_in ? "Up, Down, Left and the stick" : "Up, Down, Tab, Shift+Tab and Left", reached, acted,
           runs);
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

    check_input(kKeyboard);
    check_only_right(kKeyboard);
    check_input(kPad);
    check_only_right(kPad);

    // Up as a compositor delivers a tapped key (down and up within one frame), 0.3 s apart: every press moves the
    // cursor one button, and past the top it comes round to the bottom, as in every ImGui popup (EndPopup makes a
    // popup's navigation loop), so six presses from Back end on Delete
    open_file2();
    char seen[160] = "";
    for (int i = 0; i < 6; i++) {
        key(ImGuiKey_UpArrow, true);
        key(ImGuiKey_UpArrow, false);
        frames(18);
        snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "%s%s", i ? ", " : "", nav_name());
    }
    CHECK(!strcmp(seen, "Delete, Rename, Copy, Play this file, Back, Delete"), "six Ups from Back: %s", seen);
    printf("keyboard: six Ups from Back, each down and up in one frame: %s\n", seen);
    tap(ImGuiKey_Escape);

    // New's default name is the number New gives (the stub's 7), not the highest listed plus one (4)
    saves_cancel();
    frames(4);
    opt_focus_key("##save_file_new");
    frames(2);
    tap(ImGuiKey_Enter);
    CHECK(g_open && g_page == P_NAME && g_name_job == J_NEW && !strcmp(g_name, "File 7"), "New's default name: \"%s\"",
          g_name);
    printf("keyboard: New's default name is \"%s\"\n", g_name);
    tap(ImGuiKey_Escape);

    // the name steps by pad: the cursor on Cancel, so A, A neither copies nor renames
    open_file2();
    choose(kPad, 3);
    CHECK(g_page == P_NAME && g_name_job == J_COPY && !strcmp(nav_name(), "Cancel##no"), "Copy by pad: cursor on %s",
          nav_name());
    tap(kPad.act);
    CHECK(n_new == 0 && g_page == P_ACTIONS, "Copy, A, A: %d copies", n_new);
    choose(kPad, 2);
    CHECK(g_page == P_NAME && g_name_job == J_RENAME && !strcmp(nav_name(), "Cancel##no"), "Rename by pad: cursor on %s",
          nav_name());
    tap(kPad.act);
    CHECK(n_rename == 0 && g_page == P_ACTIONS, "Rename, A, A: %d renames", n_rename);
    printf("gamepad: Copy, A and Rename, A go back: %d copies, %d renames\n", n_new, n_rename);

    // the name steps by keyboard: the text field has the cursor; typed text and Enter make it
    open_file2();
    choose(kKeyboard, 2);
    CHECK(g_page == P_NAME && GImGui->ActiveId == window_id("##save_file_name"), "Rename by keyboard: the field is %s",
          GImGui->ActiveId ? "active" : "not active");
    io.AddInputCharacter('X');
    frames(2);
    tap(ImGuiKey_Enter);
    CHECK(n_rename == 1 && !g_open, "Rename by keyboard, typed X and Enter: %d renames", n_rename);
    printf("keyboard: Rename takes the typed name on Enter: %d rename\n", n_rename);

    // Esc in the name field: the edit is dropped and the same press leaves the step (the step's own Cancel), nothing
    // is renamed
    open_file2();
    choose(kKeyboard, 2);
    io.AddInputCharacter('Y');
    frames(2);
    tap(ImGuiKey_Escape);
    CHECK(n_rename == 0 && g_open && g_page == P_ACTIONS, "Esc in the name field: %d renames, step %d", n_rename, g_page);
    printf("keyboard: Esc in the name field: %d renames, back on the file's step\n", n_rename);

    // Rename opened again after that Esc: ImGui keeps a field's cursor when it is entered again holding the same text,
    // so the name was no longer selected and the typing went after it ("Forest runZ"); it must replace the name
    open_file2();
    choose(kKeyboard, 2);
    tap(ImGuiKey_Escape);
    choose(kKeyboard, 2);
    io.AddInputCharacter('Z');
    frames(2);
    tap(ImGuiKey_Enter);
    CHECK(n_rename == 1 && !strcmp(last_name, "Z"), "Rename again after an Esc, typed Z and Enter: %d renames to \"%s\"",
          n_rename, last_name);
    printf("keyboard: Rename again after an Esc, typed Z and Enter: renamed to \"%s\"\n", last_name);

    // nx2navi.ini cannot be written: no restart, and the next start's file is not switched either
    open_file2();
    cfg_save_ok = 0;
    choose(kPad, 4);
    tap(kPad.right);
    tap(kPad.act);
    cfg_save_ok = 1;
    CHECK(n_cfg_save == 1 && n_choose == 0 && n_restart == 0, "Restart with nx2navi.ini not written: %d choices, %d "
          "restarts", n_choose, n_restart);
    printf("Restart with nx2navi.ini not written: %d choices of the next file, %d restarts\n", n_choose, n_restart);

    // file 3, the one the menu picked last while save_file picks another: Delete is greyed, so the cursor skips it
    open_file("3");
    choose(kPad, 1);
    CHECK(g_page == P_NAME && g_name_job == J_RENAME && n_delete == 0, "file 3 (current): up from Back reached step %d",
          g_page);
    // file 1, which save_file names by its name: Rename (and Delete, Play: it is in use) greyed, up from Back is Copy
    open_file("1");
    choose(kPad, 1);
    CHECK(g_page == P_NAME && g_name_job == J_COPY, "file 1 (named by save_file): up from Back reached step %d job %d",
          g_page, g_name_job);
    printf("the file the menu picked last cannot be deleted, the file save_file names by name cannot be renamed\n");

    // a menu import waits: the next start would load it, not the file picked, so Restart is greyed and play refuses
    open_file2();
    import_waiting = 1;
    choose(kPad, 4);
    tap(kPad.right);
    tap(kPad.act);
    saves_test_set("NX2NAVI_SAVE_FILE_PLAY", "2");
    import_waiting = 0;
    CHECK(n_cfg_save == 0 && n_choose == 0 && n_restart == 0, "Play this file while an import waits: %d choices, %d "
          "restarts", n_choose, n_restart);
    printf("Play this file while a menu import waits: %d choices of the next file, %d restarts\n", n_choose, n_restart);

    // the menu closes (F1, or Minus and Plus) while Rename's name field takes typing: the window's text input goes off
    // on the closed menu's next frame, not only when the menu opens again (the Windows input method would stay on the
    // game window, SDL's text input on, meanwhile)
    open_file2();
    choose(kKeyboard, 2);
    const int on_in_field = text_on;
    menu_open = false;
    frames(2);
    const int on_closed = text_on;
    menu_open = true;
    if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);   // opening again, as menu.cpp's reset
    saves_cancel();
    frames(4);
    CHECK(on_in_field == 1 && on_closed == 0 && text_on == 0, "text input with the name field active %d, after the "
          "menu closed %d, opened again %d", on_in_field, on_closed, text_on);
    printf("keyboard: text input with Rename's field active: %s; after the menu closed with it active: %s\n",
           on_in_field ? "on" : "off", on_closed ? "on" : "off");

    // a Rename the files refuse because save_file picks another file by that name: the line under the list says
    // that, with save_file's value, not only that it did not work
    setting = "Hero", rename_refused = 1;
    open_file2();
    choose(kKeyboard, 2);
    for (const char *c = "hero"; *c; c++) io.AddInputCharacter(*c);
    frames(2);
    tap(ImGuiKey_Enter);
    char said[256];
    snprintf(said, sizeof said, g_msg >= 0 ? menu_text((UiTextId)g_msg) : "", g_msg_arg);
    CHECK(n_rename == 1 && g_msg == UI_MENU_SAVE_FILE_E_NAME_SET && !strcmp(g_msg_arg, "Hero") && g_msg_warn,
          "Rename refused for save_file's name: the line says \"%s\"", said);
    printf("keyboard: Rename to the name save_file = Hero picks another file by: \"%s\"\n", said);
    setting = nullptr, rename_refused = 0;

    ImGui::DestroyContext();
    printf("saves page: %s\n", failures ? "see FAIL lines" : "ok");
    return failures;
}
