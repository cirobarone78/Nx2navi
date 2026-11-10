// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
// The nn::fs host layer: the read-only RomFS (romfs.c) and writable save, temporary and
// cache storage under the profile directory (fs_host.c). Both register their nn::fs overrides with HOST_OVERRIDE;
// this header is what the two files and the probe share.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "save_summary.h"

// ---- nn::fs results
#define FS_R_PATH_NOT_FOUND 0x202u
#define FS_R_ALREADY_EXISTS 0x402u
#define FS_R_NOT_EMPTY 0x1002u
#define FS_R_UNSUPPORTED_OPEN 0x6402u    // RomFS: any open mode but read
#define FS_R_INVALID 0xe402u             // RomFS: bad handle, offset or argument
#define FS_R_TARGET_NOT_FOUND 0x7d402u
#define FS_R_INVALID_ARGUMENT 0x2ee202u
#define FS_R_INVALID_PATH 0x2ee602u
#define FS_R_OPEN_MODE 0x307402u
#define FS_R_APPEND_REQUIRED 0x2f5e02u

enum { FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_APPEND = 4 };   // nn::fs::OpenMode
enum { FS_ENTRY_DIRECTORY = 0, FS_ENTRY_FILE = 1 };                 // nn::fs::DirectoryEntryType

// ---- romfs.c
// Handles 0x1_0000_0000 + j; host storage uses 0x2_0000_0000 + j (files) and 0x3_0000_0000 + j (directories).
#define ROMFS_HANDLE_BASE 0x100000000ull
int romfs_owns(const char *path);                          // path is on the mounted RomFS ("<mount>:...")
int romfs_handle(uint64_t h);                              // h is a RomFS file handle
uint32_t romfs_mount(const char *name);                    // nn::fs::MountRom; a second name is fatal
uint32_t romfs_open(uint64_t *out, const char *path, int mode);
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
uint32_t romfs_size(int64_t *out, uint64_t h);
void romfs_close(uint64_t h);
uint8_t *romfs_load(const char *rel, uint64_t *size_out);   // whole file by RomFS-relative path, malloc'd (or NULL)
uint32_t romfs_entry_type(int *out, const char *path);
// The children of RomFS directory path: fn(name, is_dir, size, ctx) once per file and once per subdirectory, in
// index order; fn returns 0 to stop. FS_R_PATH_NOT_FOUND when path is not a directory.
uint32_t romfs_list(const char *path, int (*fn)(const char *name, int is_dir, uint64_t size, void *ctx), void *ctx);
uint64_t romfs_file_count(void);                           // files in the index (fs_init first)
const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size);   // path, image offset and size of index row i
const char *romfs_image_path(void);                        // the romfs.bin path fs_init opened
size_t romfs_overlay_count(void);
int romfs_patches_loaded(void);                            // a romfs-patches manifest gave at least one file
// The button prompts (input_prompts_folder): the folder loaded at start, NULL for the game's own prompts; whether a
// folder had a manifest at start; and why the folder the settings asked for at start was not loaded (ROMFS_PROMPTS_OK
// for any other folder: it was not asked for). romfs-patches-pc goes by that name also where romfs-patches-pc-<w>x<h>
// stands in for it (romfs.c overlay_init).
enum { ROMFS_PROMPTS_OK, ROMFS_PROMPTS_MISSING, ROMFS_PROMPTS_FAILED, ROMFS_PROMPTS_SHAPE, ROMFS_PROMPTS_MODS,
       ROMFS_PROMPTS_PATCHES };
const char *romfs_prompts(void);
int romfs_prompts_ready(const char *folder);
int romfs_prompts_off(const char *folder);
const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added);   // RomFS path of overlay k
void fs_trace(const char *what, const char *path);         // NX2NAVI_TRACE=fs: one line per open

// ---- fs_host.c
void fs_host_set_profile(const char *dir);                 // profile directory; before the first storage mount only
                                                           // (default rt_config()->profile)
void fs_host_lock_profile(void);                           // the one-game-per-profile lock; fatal when another copy
                                                           // holds it (rt_boot, before the prewarm; storage_init)
int fs_save_busy(void);                                    // a save is being written: a save file open for writing,
                                                           // or save data changed in the last 3 s and not committed
void fs_save_flush(void);                                  // wait for a save Commit still running in the background;
                                                           // every exit path calls it before the process ends
// A shared save (save_share.c). fs_save_export: visit(ctx, path below the save with '/', bytes, size) for every file of
// the save the game would load next (the running save file's newest), under the commit lock; visit returns 0 to fail.
// The number of files (0: no save yet), -1 when one could not be read or visit failed.
typedef int (*FsSaveVisit)(void *ctx, const char *rel, const uint8_t *data, size_t n);
int fs_save_export(FsSaveVisit visit, void *ctx);
// The menu's import: count files (relative '/' paths, bytes) become <profile>/save-import, which the next start makes a
// new save file and mounts; one waiting already is replaced. 0 on success.
int fs_save_import_stage(int count, const char *const *rel, const uint8_t *const *data, const size_t *size);
// 1 while an import waits for the next start: save-import, or a save-files/.import-<n> folder a start could not make a
// file yet (0 before the first storage use). Cancel drops both; 0 on success.
int fs_save_import_waiting(void);
int fs_save_import_cancel(void);

// Save files (fs_host.c section "save files"): any number of playthroughs, each a save with its own journal and
// backups. File 1 is the profile's own save; the game mounts one file per run, picked at start. Any thread; the
// operations do disk work (a copy reads and flushes a whole save) and belong off the present thread.
#define FS_SAVE_NAME_MAX 64
enum { FS_FILE_FAILED = -1, FS_FILE_GONE = -2, FS_FILE_IN_USE = -3, FS_FILE_NAME_SET = -4 };
typedef struct {
    int id;                         // 1: the profile's own save; 2 and up: <profile>/save-files/<id>
    char name[FS_SAVE_NAME_MAX];    // UTF-8, as the player named it; "" unnamed
    int running;                    // the file this run mounted
    int next;                       // the file the next start mounts
    int current;                    // the file save-files/current names (the menu's last choice; save_file may pick
                                    // another): not deleted either
    int named;                      // save_file names it by its name: not renamed, or the next start would stop
    SaveSummary sum;                // its newest save at a glance
} FsSaveFile;
// Every file, by id; the count. More than max: the lowest, with the running file and the next start's kept in the list.
int fs_save_files(FsSaveFile *out, int max);
int fs_save_file_running(void);                            // the file this run mounted
// A new file named name (control characters dropped, cut to FS_SAVE_NAME_MAX - 1 bytes), empty (the game starts a
// new game in it) or with a copy of file from's newest save (from 0: empty); its id, else FS_FILE_*.
int fs_save_file_new(const char *name, int from);
// The number New would give a file now: one past the highest of every file, file in the making and deleted file kept,
// so not always the highest listed plus one.
int fs_save_file_next_id(void);
// Gives file id the name name, cleaned as New's; 0, else FS_FILE_*: FS_FILE_IN_USE for the file save_file names by
// its name (the next start would stop), FS_FILE_NAME_SET for a name save_file picks a file by, given to another file
// (the start takes the lowest file of a name, so this one could start instead).
int fs_save_file_rename(int id, const char *name);
// The next start mounts file id (unless save_file names another); 0, else FS_FILE_*.
int fs_save_file_choose(int id);
// Moves file id to <profile>/save-files/deleted, where it is kept for a while; refused (FS_FILE_IN_USE) for the
// running file, the next start's and the one save-files/current names. FS_FILE_FAILED means the file stays as it was:
// what a failing Delete of file 1 cannot put back at once, the next start puts back. 0, else FS_FILE_*.
int fs_save_file_delete(int id);
const char *fs_save_file_setting(void);                    // NX2NAVI_SAVE_FILE when set (it picks the file at
                                                           // start), else NULL
// The file this start made from an import, when this start also mounts it (save_file may pick another); 0 otherwise.
int fs_save_file_import(void);
