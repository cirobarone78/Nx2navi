"""Export the launcher's logs and the game's retained runs, including runs made before the first save."""
import os
import tempfile
import time
import zipfile
from pathlib import Path

from .saves import default_export_dir

# A game log stops at 64 MiB, with the cap notice and the launcher's exit code appended afterwards.
GAME_LOG_LIMIT = 65 << 20
LAUNCHER_LOG_LIMIT = 8 << 20
TRUNCATED = b'[diagnostics export: earlier log lines omitted]\n'


def _add_log(archive, path, name, limit):
    if path.is_symlink() or not path.is_file():
        return
    try:
        source = path.open('rb')
    except FileNotFoundError:   # a log rotated after the directory check
        return
    with source:
        stat = os.fstat(source.fileno())
        size = stat.st_size
        if not size:
            return
        # Copy the size seen on open, even if a running launcher or game continues writing.
        # For an oversized file, keep the end where the failure and exit code are reported.
        start = max(0, size - limit)
        source.seek(start)
        left = size - start
        stamp = time.localtime(stat.st_mtime)[:6]
        stamp = min(max(stamp, (1980, 1, 1, 0, 0, 0)), (2107, 12, 31, 23, 59, 59))
        info = zipfile.ZipInfo(name, stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        with archive.open(info, 'w') as target:
            if start:
                target.write(TRUNCATED)
            while left:
                chunk = source.read(min(left, 1 << 20))
                if not chunk:
                    break
                target.write(chunk)
                left -= len(chunk)


def export_diagnostics(game_dir, logs_dir, dest=None):
    """Return the ZIP in dest (Downloads by default). A missing game directory gives a launcher-only report."""
    dest = Path(dest) if dest else default_export_dir()
    dest.mkdir(parents=True, exist_ok=True)
    prefix = time.strftime('NX2navi-diagnostics-%Y-%m-%d_%H-%M-%S-')
    with tempfile.NamedTemporaryFile(dir=dest, prefix=prefix, suffix='.zip.part', delete=False) as part:
        tmp = Path(part.name)
    try:
        with zipfile.ZipFile(tmp, 'w') as archive:
            for name in ('launcher.log', 'launcher.1.log', 'crash.log'):
                _add_log(archive, Path(logs_dir) / name, f'launcher/{name}', LAUNCHER_LOG_LIMIT)
            if game_dir:
                logs = Path(game_dir) / 'logs'
                # The failed run may have rotated after a restart, even if it died before writing a fatal line.
                for stem in ('nx2navi', 'nx2navi.1', 'nx2navi.2', 'nx2navi.3', 'nx2navi.4'):
                    for name in (f'{stem}.log', f'{stem}.crash.log'):
                        _add_log(archive, logs / name, f'game/{name}', GAME_LOG_LIMIT)
            if not archive.namelist():
                raise OSError('No launcher or game logs are available to export.')
        final = tmp.with_suffix('')
        os.replace(tmp, final)
        return final
    finally:
        tmp.unlink(missing_ok=True)
