// The save files on the Saves tab (saves_page.h). The list and every operation run on a thread of their own (a copy
// reads and flushes a whole save, the list reads each file's captions); the rows show the last list read and wait greyed
// while one runs, and the line under them says what the last operation did. Playing another file is a restart: the
// game mounts its save once, at boot, and holds its progress in memory (fs_host.c section "save files").
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "menu_sound.h"
#include "ui/menu_skin.h"
#include "ui/option_rows.h"
#include "ui/saves_page.h"

extern "C" {
#include "cfg.h"
#include "fs.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "ui_text.h"
}

enum { MAX_FILES = 64 };
enum Job { J_LIST, J_NEW, J_COPY, J_RENAME, J_DELETE };
enum Page { P_ACTIONS, P_PLAY, P_NAME, P_DELETE };

static PlatMutex g_lock = PLAT_MUTEX_INIT;   // g_files, g_n, g_msg*
static FsSaveFile g_files[MAX_FILES];
static int g_n;
static int g_next_id;                        // the number New gives a file, read with the list
static int g_msg = -1;                       // the last operation's UiTextId, -1 none; a format with g_msg_arg
static char g_msg_arg[FS_SAVE_NAME_MAX + 16];
static bool g_msg_warn;
static std::atomic<int> g_busy{0};           // a list or an operation runs
static std::atomic<int> g_stale{1};          // the list wants reading when the page shows next
static std::atomic<int> g_focus{0};          // a file the cursor goes to once the list holds it

static bool g_open, g_window_shown, g_page_new, g_by_pad;
static int g_page, g_id, g_name_job;         // the window's step, its file, and what its name step makes
static char g_name[FS_SAVE_NAME_MAX];

static void say(int text, const char *arg, bool warn) {
    plat_mutex_lock(&g_lock);
    g_msg = text;
    snprintf(g_msg_arg, sizeof g_msg_arg, "%s", arg ? arg : "");
    g_msg_warn = warn;
    plat_mutex_unlock(&g_lock);
}

// The name the menu shows: the player's, else "File <n>".
static const char *label_of(const FsSaveFile &f, char *buf, size_t cap) {
    if (*f.name) return f.name;
    snprintf(buf, cap, menu_text(UI_MENU_SAVE_FILE_DEFAULT), f.id);
    return buf;
}

struct JobArgs {
    int job, id;
    char name[FS_SAVE_NAME_MAX];
};

static void job_main(void *arg) {
    JobArgs *a = (JobArgs *)arg;
    int r = 0;
    switch (a->job) {
    case J_NEW: r = fs_save_file_new(a->name, 0); break;
    case J_COPY: r = fs_save_file_new(a->name, a->id); break;
    case J_RENAME: r = fs_save_file_rename(a->id, a->name); break;
    case J_DELETE: r = fs_save_file_delete(a->id); break;
    default: break;
    }
    if (a->job != J_LIST) {
        if (r == FS_FILE_NAME_SET) say(UI_MENU_SAVE_FILE_E_NAME_SET, fs_save_file_setting(), true);
        else if (r < 0) say(UI_MENU_SAVE_FILE_E_FAILED, "", true);
        else if (a->job == J_RENAME) say(UI_MENU_SAVE_FILE_RENAMED, "", false);
        else if (a->job == J_DELETE) say(UI_MENU_SAVE_FILE_DELETED, a->name, false);
        else say(UI_MENU_SAVE_FILE_MADE, a->name, false);
        if (r > 0) g_focus.store(r);
    }
    static FsSaveFile list[MAX_FILES];   // one job at a time (g_busy)
    int n = fs_save_files(list, MAX_FILES), next_id = fs_save_file_next_id();
    plat_mutex_lock(&g_lock);
    memcpy(g_files, list, sizeof(FsSaveFile) * (size_t)n);
    g_n = n;
    g_next_id = next_id;
    plat_mutex_unlock(&g_lock);
    free(a);
    g_busy.store(0);
}

static void start(int job, int id, const char *name) {
    if (g_busy.exchange(1)) return;
    JobArgs *a = (JobArgs *)calloc(1, sizeof *a);
    if (!a) {
        g_busy.store(0);
        return;
    }
    a->job = job, a->id = id;
    snprintf(a->name, sizeof a->name, "%s", name ? name : "");
    if (!plat_thread_spawn(job_main, a, "save files", 0)) job_main(a);
}

// The file this run plays, from the last list; NULL before it is read.
static const FsSaveFile *find(const FsSaveFile *files, int n, int id) {
    for (int i = 0; i < n; i++)
        if (files[i].id == id) return &files[i];
    return nullptr;
}

// "Adult, 8 hearts, Forest Temple", or "not saved yet".
static void summary_of(const FsSaveFile &f, char *out, size_t cap) {
    if (!f.sum.slots) {
        snprintf(out, cap, "%s", menu_text(UI_MENU_SAVE_FILE_EMPTY));
        return;
    }
    size_t k = 0;
    out[0] = 0;
    if (f.sum.adult >= 0)
        k += (size_t)snprintf(out + k, cap - k, "%s", menu_text(f.sum.adult ? UI_MENU_SAVE_FILE_ADULT : UI_MENU_SAVE_FILE_CHILD));
    if (f.sum.max_life > 0 && k < cap) {
        k += (size_t)snprintf(out + k, cap - k, "%s", k ? ", " : "");
        if (k < cap) k += (size_t)snprintf(out + k, cap - k, menu_text(UI_MENU_SAVE_FILE_HEARTS), f.sum.max_life / 4);
    }
    if (f.sum.area[0] && k < cap) snprintf(out + k, cap - k, "%s%s", k ? ", " : "", f.sum.area);
}

// "saved 2026-11-09 21:13", with "in use" or "loads at next start" after it.
static void note_of(const FsSaveFile &f, char *out, size_t cap) {
    char when[40] = "";
    if (f.sum.year)
        snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d", f.sum.year, f.sum.month, f.sum.day, f.sum.hour, f.sum.minute);
    size_t k = 0;
    out[0] = 0;
    if (*when) k += (size_t)snprintf(out, cap, menu_text(UI_MENU_SAVE_FILE_SAVED), when);
    int mark = f.running ? UI_MENU_SAVE_FILE_IN_USE : f.next ? UI_MENU_SAVE_FILE_NEXT : -1;
    if (mark >= 0 && k < cap) snprintf(out + k, cap - k, "%s%s", k ? "  ·  " : "", menu_text((UiTextId)mark));
}

static void open_window(int id) {
    g_open = true, g_page = P_ACTIONS, g_id = id, g_page_new = true;
}

static void name_step(int job, const char *initial) {
    g_page = P_NAME, g_name_job = job, g_page_new = true;
    g_by_pad = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
    snprintf(g_name, sizeof g_name, "%s", initial);
}

void saves_rows(float scale) {
    (void)scale;
    if (g_stale.load() && !g_busy.load()) {
        g_stale.store(0);
        start(J_LIST, 0, nullptr);
    }
    static FsSaveFile files[MAX_FILES];
    plat_mutex_lock(&g_lock);
    int n = g_n, next_id = g_next_id;
    memcpy(files, g_files, sizeof(FsSaveFile) * (size_t)n);
    char line[512] = "";
    bool warn = g_msg_warn;
    if (g_msg >= 0) snprintf(line, sizeof line, menu_text((UiTextId)g_msg), g_msg_arg);
    plat_mutex_unlock(&g_lock);

    const bool busy = g_busy.load() != 0;
    opt_group(UI_MENU_SAVE_FILES);
    for (int i = 0; i < n; i++) {
        const FsSaveFile &f = files[i];
        char id[24], buf[FS_SAVE_NAME_MAX], value[160], note[160], desc[768];
        snprintf(id, sizeof id, "##file%d", f.id);
        summary_of(f, value, sizeof value);
        note_of(f, note, sizeof note);
        snprintf(desc, sizeof desc, "%s\n%s%s%s\n\n%s%s%s", label_of(f, buf, sizeof buf), value, *note ? "\n" : "", note,
                 menu_text(UI_MENU_SAVE_FILE_DESC), f.running ? " " : "",
                 f.running ? menu_text(UI_MENU_SAVE_FILE_DESC_IN_USE) : "");
        if (g_focus.load() == f.id) {
            opt_focus_key(id);
            g_focus.store(0);
        }
        if (busy) opt_disable_next(UI_MENU_SAVE_BUSY);
        if (opt_text_row(id, label_of(f, buf, sizeof buf), value, note, desc, false) == 2 && !busy) open_window(f.id);
    }
    if (busy) opt_disable_next(UI_MENU_SAVE_BUSY);
    if (opt_button("##save_file_new", UI_MENU_SAVE_FILE_NEW, UI_MENU_SAVE_FILE_NEW_DESC) && !busy) {
        // the number the file will get, not the highest listed plus one: a deleted file keeps its number while it is
        // kept, and an unnamed file shows as "File <its number>"; New is greyed until a list (and this) was read
        char name[FS_SAVE_NAME_MAX];
        snprintf(name, sizeof name, menu_text(UI_MENU_SAVE_FILE_DEFAULT), next_id);
        g_open = true, g_id = 0;
        name_step(J_NEW, name);
    }
    if (const char *set = fs_save_file_setting()) {
        char t[256];
        snprintf(t, sizeof t, menu_text(UI_MENU_SAVE_FILE_SET_BY), set);
        opt_text_str(t, false);
    }
    if (line[0]) opt_text_str(line, warn);
}

// What Play this file's Restart does: the next start mounts the file, and the game restarts as the menu's Restart does.
// nx2navi.ini is written first: when it cannot be (a read-only game folder) the game does not restart, and the next
// start must then load the file the player still plays, so the choice is written only after it. The one place a
// switch of files restarts the game. Refused while the game writes a save (the restart would cut that save off) and
// for the file in use.
static void play(int id) {
    // a menu import waiting would be the next start's file instead (fs_host.c import_pending)
    if (fs_save_busy() || id == fs_save_file_running() || fs_save_import_waiting()) return;
    if (!cfg_save()) {
        say(UI_MENU_SAVE_FAILED, "", true);
        return;
    }
    if (fs_save_file_choose(id)) say(UI_MENU_SAVE_FILE_E_FAILED, "", true);
    else gfx_window_request_restart();
}

// The cursor on the item just drawn, shown (as option_rows.cpp puts it on a row). SetItemDefaultFocus as well: on the
// window's first frame ImGui's own pick of a first item would replace the cursor set here.
static void take_cursor(void) {
    ImGui::SetItemDefaultFocus();
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
}

// Two buttons side by side, the safe one first: 1 the first, 2 the second. The cursor goes to the safe one whenever a
// step opens (fresh), not only when the window appears: the window changes steps inside one popup, and ImGui's
// SetItemDefaultFocus works only on a window's first frame, so the cursor stayed on the item of the step before. The
// second button has an ID of its own ("##yes"), as the step before has a button with the same text (Delete, Copy,
// Rename) whose ID the cursor would otherwise keep. ImGui activates a button on a press, not on a held key's repeats.
// Only Right reaches the second button (and Left goes back): ImGui loops a popup's navigation up and down and Tab goes
// to the next button, so one Up, Down or Tab from the safe one landed on it. While another move than Right is scored,
// the second button is no place the cursor can go.
static int two_buttons(const char *no, const char *yes, bool yes_enabled, bool fresh, float scale) {
    const ImGuiStyle &st = ImGui::GetStyle();
    float bw = (ImGui::CalcTextSize(no).x > ImGui::CalcTextSize(yes).x ? ImGui::CalcTextSize(no).x : ImGui::CalcTextSize(yes).x) +
               st.FramePadding.x * 4;
    char id[160];
    snprintf(id, sizeof id, "%s##no", no);
    int r = skin_widget_button(id, ImVec2(bw, 0), scale) ? 1 : 0;
    if (fresh && !(g_page == P_NAME && !g_by_pad)) take_cursor();
    ImGui::SameLine(0, st.ItemSpacing.x + skin_cursor_reach(scale));
    ImGui::BeginDisabled(!yes_enabled);
    snprintf(id, sizeof id, "%s##yes", yes);
    const ImGuiContext &g = *GImGui;
    const bool right = g.NavMoveDir == ImGuiDir_Right && !(g.NavMoveFlags & ImGuiNavMoveFlags_IsTabbing);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, g.NavMoveScoringItems && !right && g.NavId != ImGui::GetID(id));
    if (skin_widget_button(id, ImVec2(bw, 0), scale)) r = 2;
    ImGui::PopItemFlag();
    ImGui::EndDisabled();
    return r;
}

void saves_after_page(float scale) {
    g_window_shown = false;
    if (g_open && !ImGui::IsPopupOpen("##savefile")) ImGui::OpenPopup("##savefile");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool shown = ImGui::BeginPopupModal("##savefile", nullptr,
                                              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!shown) {
        g_open = false;
        return;
    }
    g_window_shown = true;
    // A step opened since the last frame. Latched and cleared here, before the step is drawn: a button of the step
    // that opens another sets g_page_new again, and that must reach the next frame, where the new step is drawn.
    const bool fresh = g_page_new;
    g_page_new = false;
    const ImVec2 box = ImGui::GetWindowPos(), box_size = ImGui::GetWindowSize();
    skin_panel(ImGui::GetWindowDrawList(), box, ImVec2(box.x + box_size.x, box.y + box_size.y), scale);
    static FsSaveFile files[MAX_FILES];
    plat_mutex_lock(&g_lock);
    int n = g_n;
    memcpy(files, g_files, sizeof(FsSaveFile) * (size_t)n);
    plat_mutex_unlock(&g_lock);
    const FsSaveFile *f = find(files, n, g_id), *running = nullptr;
    for (int i = 0; i < n; i++)
        if (files[i].running) running = &files[i];
    char buf[FS_SAVE_NAME_MAX], buf2[FS_SAVE_NAME_MAX], text[512];
    const char *name = f ? label_of(*f, buf, sizeof buf) : "";
    const bool busy = g_busy.load() != 0;
    bool close = false, back = false;
    const float wrap = ImGui::GetFontSize() * 28;
    if (g_page != P_NAME || g_name_job != J_NEW) {
        if (!f) close = true;   // gone since the window opened
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(SKIN_GOLD));
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    if (g_page == P_ACTIONS && f) {
        char value[160], note[160];
        summary_of(*f, value, sizeof value);
        note_of(*f, note, sizeof note);
        ImGui::TextUnformatted(value);
        if (*note) ImGui::TextDisabled("%s", note);
        ImGui::Spacing();
        const char *set = fs_save_file_setting();
        // Delete: not the file in use, the next start's, or the one the menu last picked (save_file may pick another
        // file meanwhile); Rename: not the file save_file names by its name (the next start would not find it)
        const bool can_play = !f->running && !set && !busy,
                   can_delete = !f->running && !f->next && !f->current && !busy;
        float bw = 0;
        static const UiTextId acts[] = {UI_MENU_SAVE_FILE_PLAY, UI_MENU_SAVE_FILE_COPY, UI_MENU_SAVE_FILE_RENAME,
                                        UI_MENU_SAVE_FILE_DELETE, UI_MENU_SAVE_FILE_BACK};
        for (UiTextId a : acts) bw = ImGui::CalcTextSize(menu_text(a)).x > bw ? ImGui::CalcTextSize(menu_text(a)).x : bw;
        bw += ImGui::GetStyle().FramePadding.x * 4;
        ImGui::BeginDisabled(!can_play);
        if (skin_widget_button(menu_text(UI_MENU_SAVE_FILE_PLAY), ImVec2(bw, 0), scale)) g_page = P_PLAY, g_page_new = true;
        ImGui::EndDisabled();
        if (set) {
            snprintf(text, sizeof text, menu_text(UI_MENU_SAVE_FILE_SET_BY), set);
            ImGui::TextDisabled("%s", text);
        }
        ImGui::BeginDisabled(busy);
        if (skin_widget_button(menu_text(UI_MENU_SAVE_FILE_COPY), ImVec2(bw, 0), scale)) {
            snprintf(text, sizeof text, menu_text(UI_MENU_SAVE_FILE_COPY_OF), name);
            name_step(J_COPY, text);
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(busy || f->named);
        if (skin_widget_button(menu_text(UI_MENU_SAVE_FILE_RENAME), ImVec2(bw, 0), scale)) name_step(J_RENAME, f->name);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!can_delete);
        if (skin_widget_button(menu_text(UI_MENU_SAVE_FILE_DELETE), ImVec2(bw, 0), scale)) g_page = P_DELETE, g_page_new = true;
        ImGui::EndDisabled();
        if (!busy && (f->running || f->next || f->current))
            ImGui::TextDisabled("%s", menu_text(UI_MENU_SAVE_FILE_DELETE_IN_USE));
        if (skin_widget_button(menu_text(UI_MENU_SAVE_FILE_BACK), ImVec2(bw, 0), scale)) close = true;
        if (fresh) take_cursor();   // on opening and on every return from a step: never on Delete
    } else if (g_page == P_PLAY && f) {
        snprintf(text, sizeof text, menu_text(UI_MENU_SAVE_FILE_PLAY_CONFIRM), name,
                 running ? label_of(*running, buf2, sizeof buf2) : "");
        ImGui::TextUnformatted(text);
        const bool saving = fs_save_busy() != 0, importing = fs_save_import_waiting() != 0;
        if (saving) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", menu_text(UI_MENU_RESTART_SAVING));
        if (importing) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", menu_text(UI_MENU_SAVE_FILE_IMPORT_WAITS));
        ImGui::Spacing();
        int r = two_buttons(menu_text(UI_MENU_RESET_NO), menu_text(UI_MENU_RESTART_YES), !saving && !importing && !busy,
                            fresh, scale);
        if (r == 1) back = true;
        if (r == 2) play(f->id);
    } else if (g_page == P_NAME) {
        if (g_name_job == J_COPY) ImGui::TextDisabled("%s", menu_text(UI_MENU_SAVE_FILE_COPY_NOTE));
        ImGui::TextUnformatted(menu_text(UI_MENU_SAVE_FILE_NAME));
        if (fresh && !g_by_pad) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(wrap);
        // the whole name selected whenever the field is entered, so typing replaces it: ImGui keeps a field's cursor
        // when it is entered again holding the same text (Rename after an Esc, or after a Copy that typed this name),
        // and the typing went after the name
        bool enter = ImGui::InputText("##save_file_name", g_name, sizeof g_name,
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const char *p = g_name;
        while (*p == ' ') p++;
        const bool named = *p != 0;
        ImGui::Spacing();
        UiTextId act = g_name_job == J_NEW ? UI_MENU_SAVE_FILE_NEW : g_name_job == J_COPY ? UI_MENU_SAVE_FILE_COPY
                                                                                         : UI_MENU_SAVE_FILE_RENAME;
        int r = two_buttons(menu_text(UI_MENU_RESET_NO), menu_text(act), named && !busy, fresh, scale);
        if (r == 1) back = true;
        if ((r == 2 || enter) && named && !busy) {
            start(g_name_job, g_id, g_name);
            close = true;
        }
    } else if (g_page == P_DELETE && f) {
        snprintf(text, sizeof text, menu_text(UI_MENU_SAVE_FILE_DELETE_CONFIRM), name);
        ImGui::TextUnformatted(text);
        ImGui::Spacing();
        int r = two_buttons(menu_text(UI_MENU_RESET_NO), menu_text(UI_MENU_SAVE_FILE_DELETE), !busy, fresh, scale);
        if (r == 1) back = true;
        if (r == 2 && !busy) {
            start(J_DELETE, f->id, name);
            close = true;
        }
    }
    ImGui::PopTextWrapPos();
    if (!fresh && !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        back = true;
        menu_sound_play(MENU_SOUND_CANCEL);
    }
    if (back && (g_page == P_ACTIONS || (g_page == P_NAME && g_name_job == J_NEW))) close = true;
    else if (back) g_page = P_ACTIONS, g_page_new = true;
    if (close) {
        g_open = false;
        ImGui::CloseCurrentPopup();
        if (g_id) {
            char key[24];
            snprintf(key, sizeof key, "##file%d", g_id);
            opt_focus_key(key);   // back on the file's row
        }
    }
    ImGui::EndPopup();
}

bool saves_modal(void) { return g_open || g_window_shown; }

void saves_cancel(void) {
    g_open = false;
    g_stale.store(1);
    // Opening, the list is read at once, not when the tab first shows: the page's first row takes the cursor as it
    // shows, and with no list read yet that row was New save file (a press of A or Enter right after made a file).
    if (menu_is_open() && !g_busy.load()) {
        g_stale.store(0);
        start(J_LIST, 0, nullptr);
    }
}

int saves_test_set(const char *key, const char *value) {
    if (!strcmp(key, "NX2NAVI_SAVE_FILE_OPEN")) {
        open_window(atoi(value));
    } else if (!strcmp(key, "NX2NAVI_SAVE_FILE_PAGE")) {
        g_page_new = true;
        if (!strcmp(value, "play")) g_page = P_PLAY;
        else if (!strcmp(value, "delete")) g_page = P_DELETE;
        else if (!strcmp(value, "copy")) g_page = P_NAME, g_name_job = J_COPY, g_by_pad = true;
        else if (!strcmp(value, "rename")) g_page = P_NAME, g_name_job = J_RENAME, g_by_pad = true;
        else if (!strcmp(value, "new")) g_open = true, g_id = 0, g_page = P_NAME, g_name_job = J_NEW, g_by_pad = true;
    } else if (!strcmp(key, "NX2NAVI_SAVE_FILE_PLAY")) {
        play(atoi(value));
    } else {
        return 0;
    }
    return 1;
}
