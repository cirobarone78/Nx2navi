"""Run a build of the game unattended on a copy of the user's profile with a backed-up save and scripted input.

The profile is never written: each run copies it to <exe dir>/prof-<tag>/profile, imports the save into the copy as the
launcher's Import Saves imports one, and deletes <exe dir>/prof-<tag> afterwards, also when the import fails or the run
is interrupted. It is the game folder's own profile when there is one (<exe dir>/profile, a folder make_game.py built),
else build/profile. The log records how many other copies of the game and of Ninja ran, because timings taken next to
them are not comparable.

    python tools/run_game.py build/v143 --save <backup> --seconds 15 --tag idle --env NX2NAVI_TRACE=perf
    python tools/run_game.py build/v143 --save <backup> --script <input.txt> --seconds 18 --tag fight --quiet \
        --env NX2NAVI_TRACE=perf,sched
    python3 tools/run_game.py ~/Games/Ocarina --save <report.zip> --seconds 600 --wall 90 \
        --env NX2NAVI_SHOT_DIR=shots --env NX2NAVI_SHOT_AFTER=40 --env NX2NAVI_SHOT_EVERY=10

<backup> is a folder of build/save-backup or of the profile's save-backup (the game keeps its last three saves there),
or a save folder or .zip from elsewhere: a zip the launcher or the game exported (Export Save, also for a bug report)
or another zip, such as a crash report's zipped profile folder; --script defaults to
tools/input/load_save.txt (title screen to the first save slot), and a recording made with NX2NAVI_INPUT_RECORD replays
the same way. On 1.1.1 the world clock does not start yet (frame_rate.c player_pos), so a run ends at --wall and its
pictures are NX2NAVI_SHOT_EVERY's, taken by wall time.
"""
import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from ctypes import wintypes
from pathlib import Path

if os.name == 'nt':
    import msvcrt
else:
    import fcntl

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from launcher import saves   # noqa: E402  (Import Saves' search, for a save from elsewhere)
PROFILE = ROOT / 'build' / 'profile'
SAVES = ROOT / 'build' / 'save-backup'
SLOTS = ROOT / 'build' / 'run-slots'
GPU_LOCK = ROOT / 'build' / '.gpu-run.lock'
LOCK_OFFSET = 1 << 16   # Windows byte-range lock sits past the holder text, so other processes can still read it
MAX_RUNS = int(os.environ.get('NX2NAVI_MAX_RUNS', '3'))   # more parallel runs exhaust device memory (16 GB RX 9070)
# The game's executable as each tool names it: build_game.py, make_game.py on Windows, make_game.py on Linux,
# build_game.py on Linux
GAME_EXES = ('nx2navi.exe', 'ocarina.exe', 'ocarina', 'nx2navi')
NINJA = 'ninja.exe' if os.name == 'nt' else 'ninja'


def count(image):
    """Running processes of this executable name."""
    if os.name == 'nt':
        out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {image}', '/NH'], capture_output=True, text=True).stdout
        return sum(line.lower().startswith(image) for line in out.splitlines())
    n = 0
    for comm in Path('/proc').glob('[0-9]*/comm'):   # the process name, cut to 15 characters
        try:
            n += comm.read_text().strip() == image[:15]
        except OSError:   # the process ended while the list was read
            pass
    return n


def busy(game):
    return count(game) + count(NINJA)


def game_exe(exe_dir):
    for name in GAME_EXES:
        if (exe_dir / name).is_file():
            return exe_dir / name
    sys.exit(f'run_game: {exe_dir} holds none of {", ".join(GAME_EXES)}')


def keep_in_background(proc):
    """Push the game window behind the user's windows as soon as it appears, for builds older than
    NX2NAVI_BACKGROUND_WINDOW."""
    user32 = ctypes.windll.user32
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wintypes.UINT)
    found = []
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def match(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == proc.pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    callback = enum_proc(match)
    while proc.poll() is None and not found:
        user32.EnumWindows(callback, 0)
        time.sleep(0.05)
    for hwnd in found:
        user32.SetWindowPos(hwnd, 1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)   # HWND_BOTTOM; NOSIZE | NOMOVE | NOACTIVATE


def take_slot():
    """Block until one of MAX_RUNS run slots is free and hold it (a locked file) until this process exits."""
    SLOTS.mkdir(parents=True, exist_ok=True)
    while True:
        for i in range(MAX_RUNS):
            f = open(SLOTS / f'slot{i}.lock', 'a+b')
            if try_lock(f):
                return f
            f.close()
        time.sleep(5)


def try_lock(f):
    """Take an exclusive OS lock on the open file without blocking; False if another process holds it."""
    try:
        if os.name == 'nt':
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def holder_of(path):
    try:
        return path.read_text(encoding='utf-8', errors='replace').strip() or 'unknown'
    except OSError:
        return 'unknown'


def take_gpu_lock(exe_dir, poll=2.0):
    """Block until this process holds the machine-wide measured-run lock (build/.gpu-run.lock) and return the open
    file. The OS drops the lock when the process exits or dies, so a stale file never blocks. The file text names the
    holder (pid and build path) for the waiters."""
    GPU_LOCK.parent.mkdir(parents=True, exist_ok=True)
    f = open(GPU_LOCK, 'a+b')
    waiting = False
    while not try_lock(f):
        if not waiting:
            print(f'run_game: --quiet waits for the GPU run lock held by {holder_of(GPU_LOCK)}', flush=True)
            waiting = True
        time.sleep(poll)
    f.seek(0)
    f.truncate()
    f.write(f'pid {os.getpid()}, build {exe_dir}'.encode())
    f.flush()
    return f


def release_gpu_lock(f):
    if f is None:
        return
    try:
        if os.name == 'nt':
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    except OSError:
        pass
    f.close()


def save_source(save, profile, scratch):
    """The folder or zip --save names. A backup's name is looked up in build/save-backup and the profile's save-backup;
    '.' and '..' are paths. A zip the launcher or the game's menu exported (manifest.json, save/, bug-report/) is
    imported as it is (launcher/saves.py import_zip); any other zip (a crash report's zipped profile folder) is unpacked
    into scratch, emptied first, and searched as a folder."""
    if save not in ('.', '..') and Path(save).name == save:   # a backup's name; a path is searched below
        for d in (SAVES / save, profile / 'save-backup' / save):
            if d.is_dir():
                return d
    path = Path(save).expanduser()
    if path.is_file() and zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            if saves.MANIFEST in z.namelist():
                return path
            infos = z.infolist()
            if len(infos) > saves.MAX_FILES or sum(i.file_size for i in infos) > saves.MAX_BYTES:
                sys.exit(f'run_game: {path} unpacks to more than a save holds')
            shutil.rmtree(scratch, ignore_errors=True)   # an earlier zip's files would be searched with this one's
            z.extractall(scratch)   # zipfile leaves out absolute paths and '..' parts
        return scratch
    if path.is_dir():
        return path
    sys.exit(f'run_game: {save} is no backup in {SAVES} or {profile / "save-backup"}, and no save folder or .zip')


def check_folder(folder, run):
    """SaveError when the save the import takes from folder holds a link, whose target the copy would take whatever it
    is, or holds the run folder, which the copy would copy into itself. Its file count and size are the import's to
    check (launcher/saves.py MAX_FILES, MAX_BYTES)."""
    src, skip = saves.find_save(folder)
    if src is None:
        return   # the import says so
    top = Path(src).resolve()
    if Path(run).resolve().is_relative_to(top):
        raise saves.SaveError(f'{top} holds the folder of this run ({run})')
    for root, dirs, files in os.walk(top):
        for name in dirs + files:
            p = Path(root) / name
            if (Path(root) != top or name not in skip) and (p.is_symlink() or p.is_junction()):
                raise saves.SaveError(f'{p} is a link; copy the save without links and give that folder')


def prepare_run(run, profile, save=None, log=print):
    """Make run/profile a copy of profile for the game, with the save --save names imported into it (None: the
    profile's own save). The import is the launcher's Import Saves (launcher/saves.py import_save) with run as the game
    folder: the same search, the same file count and size limits, a single slot laid over the save the game would load
    now, the copy's save-tx cleared. What it keeps (the replaced save in run/saves, a zip's files in run/zip) stays in
    run."""
    if run.exists():
        shutil.rmtree(run)
    shutil.copytree(profile, run / 'profile')
    # A save the game's menu took from a zip waits there for the next start (runtime/fs_host.c import_pending), where it
    # would replace the save of this run and back the replaced one up into the game folder's saves/, outside run.
    for name in (saves.PENDING, saves.PENDING + '.part'):
        if (run / 'profile' / name).exists():
            log(f'the save import waiting in {profile / name} is left out of this run')
            shutil.rmtree(run / 'profile' / name)
    if save is None:
        return
    src = save_source(save, profile, run / 'zip')
    try:
        if src.is_dir():
            check_folder(src, run)
        saves.import_save(run, src, log=log)
    except saves.SaveError as e:
        sys.exit(f'run_game: --save {save}: {e}')
    # The search takes any folder with files for a save; one the game cannot load would run without the save asked for.
    account = saves.account_dir(run)
    if not any(p.is_file() and saves.SAVE_FILE.match(p.relative_to(account).as_posix()) for p in account.rglob('*')):
        sys.exit(f'run_game: --save {save} holds no save of the game (account.sav, option.sav or slot_NN/progress.sav)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('exe_dir', type=Path, help='build directory or game folder holding ' + ', '.join(GAME_EXES))
    ap.add_argument('--save', help="the save to run: a backup's name in build/save-backup or the profile's save-backup, "
                                   "or a save folder or .zip from elsewhere (a crash report's zipped profile folder); "
                                   'default: the profile as it is')
    ap.add_argument('--script', type=Path, default=ROOT / 'tools' / 'input' / 'load_save.txt', help='NX2NAVI_INPUT_SCRIPT')
    ap.add_argument('--seconds', type=float, required=True,
                    help='game time to run: the run ends once the world clock (game time since the save loaded) passed '
                         'this many seconds (shot is taken of that frame)')
    ap.add_argument('--wall', type=float, help='stop the game after this many seconds of wall time if it has not ended '
                                               '(a script that never reaches the world clock)')
    ap.add_argument('--tag', default='run', help='names the profile copy and the log')
    ap.add_argument('--shot', type=Path, help='PNG of the last frame; turns async pipeline builds off (boot.c)')
    ap.add_argument('--log', type=Path, help='default: <exe dir>/logs/<tag>.log')
    ap.add_argument('--env', action='append', default=[], metavar='KEY=VALUE', help='extra environment, repeatable')
    ap.add_argument('--quiet', action='store_true', help='wait until no other copy of the game or Ninja runs (benchmarks)')
    ap.add_argument('--show', action='store_true',
                    help='Windows: open the window on top; by default it opens behind other windows')
    a = ap.parse_args()

    exe_dir = a.exe_dir.resolve()
    exe = game_exe(exe_dir)
    profile = exe_dir / 'profile' if (exe_dir / 'profile').is_dir() else PROFILE
    log = (a.log or exe_dir / 'logs' / f'{a.tag}.log').resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    gpu = take_gpu_lock(exe_dir) if a.quiet else None
    try:
        slot = take_slot()
        if a.quiet:
            while busy(exe.name):
                time.sleep(10)
        run = exe_dir / f'prof-{a.tag}'
        try:   # the run folder goes also when the copy or the import stops, so the next run with this tag starts clean
            prepare_run(run, profile, a.save, log=lambda m: print('run_game:', m))
            env = dict(os.environ, NX2NAVI_ASSUME_FOCUS='1', NX2NAVI_INPUT='none',
                       NX2NAVI_INPUT_SCRIPT=str(a.script.resolve()))
            if not a.show and os.name == 'nt':   # SDL windows open in front either way
                env['NX2NAVI_BACKGROUND_WINDOW'] = '1'
            for kv in a.env:
                k, _, v = kv.partition('=')
                env[k] = v
            args = [str(exe), '--profile', str(run / 'profile'), '--seconds', str(a.seconds)]
            if a.shot:
                args += ['--shot', str(a.shot.resolve())]
            with open(log, 'w', encoding='utf-8', errors='replace') as f:
                f.write(f'run_game: others running before: {exe.name} {count(exe.name)}, {NINJA} {count(NINJA)}\n')
                f.flush()
                start = time.time()
                proc = subprocess.Popen(args, cwd=exe_dir, env=env, stdout=f, stderr=subprocess.STDOUT)
                if not a.show and os.name == 'nt':
                    threading.Thread(target=keep_in_background, args=(proc,), daemon=True).start()
                try:
                    rc = proc.wait(timeout=a.wall)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    rc = 124
                    f.write(f'run_game: stopped at --wall {a.wall:g} s\n')
                f.write(f'run_game: rc={rc} wall={time.time() - start:.0f}s; others running after: '
                        f'{exe.name} {count(exe.name)}, {NINJA} {count(NINJA)}\n')
        finally:
            shutil.rmtree(run, ignore_errors=True)
    finally:
        release_gpu_lock(gpu)
    print(f'{log}: rc={rc}')
    sys.exit(rc)


if __name__ == '__main__':
    main()
