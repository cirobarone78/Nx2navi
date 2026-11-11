"""The game's own settings, nx2navi.ini in the game folder, for the few the launcher's Settings switch.

Read the way runtime/cfg.c reads the file: a UTF-8 byte order mark is skipped, lines starting with #, ; or [ are
not settings, a key is matched without regard to case and with or without its NX2NAVI_ prefix, a value in double
quotes loses them, an empty value leaves the setting as it was (its default, or an earlier line's value), and the last
line with a value wins. Sections do not matter to the game; a key the file does not have yet goes under its section,
as the game writes it.
A change rewrites only the line it touches, through a temporary file: the other lines keep their bytes (the byte
order mark, line ends, text that is not UTF-8). A file that exists but cannot be read is never rewritten, as
runtime/cfg.c cfg_save_to does not: rebuilding it from nothing would lose its lines.
"""
import os
import shutil
import tempfile
from pathlib import Path

PREFIX = 'NX2NAVI_'
BOM = '﻿'


def _path(game_dir):
    return Path(game_dir) / 'nx2navi.ini'


def _key(text):
    k = text.strip().upper()
    return k[len(PREFIX):] if k.startswith(PREFIX) else k


def _text(game_dir):
    """The file's text with its own line ends (bytes that are not UTF-8 kept as surrogates), '' when it does not exist.
    Any other error is raised."""
    try:
        with open(_path(game_dir), encoding='utf-8', errors='surrogateescape', newline='') as f:
            return f.read()
    except FileNotFoundError:
        return ''


def _setting(line):
    """(key, value) of a settings line, or None."""
    s = line.lstrip(BOM).strip()
    if not s or s[0] in '#;[' or '=' not in s:
        return None
    k, v = s.split('=', 1)
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] == '"':
        v = v[1:-1]
    return _key(k), v


def get(game_dir, key):
    """The value the file gives key ('' when it gives none: the game's default applies). Raises OSError when the file
    exists but cannot be read."""
    want, value = _key(key), ''
    for line in _text(game_dir).splitlines():
        kv = _setting(line)
        if kv and kv[0] == want and kv[1]:
            value = kv[1]
    return value


def _edit(text, section, key, value):
    """Change the last valued line or insert under [section]. None removes all active lines for the key."""
    bom = BOM if text.startswith(BOM) else ''
    lines = text[len(bom):].splitlines(keepends=True)
    want = _key(key)
    entry = f'{key.lower()} = {value}'
    nl = '\r\n' if lines and lines[0].endswith('\r\n') else '\n'
    mine = [i for i, line in enumerate(lines) if (_setting(line) or ('',))[0] == want]
    if value is None:
        for i in reversed(mine):
            del lines[i]
        return bom + ''.join(lines)
    valued = [i for i in mine if _setting(lines[i])[1]]
    at = (valued or mine or [-1])[-1]
    if at >= 0:
        end = lines[at][len(lines[at].rstrip('\r\n')):] or nl
        lines[at] = entry + end
    else:
        head = next((i for i, line in enumerate(lines) if line.strip().lower() == f'[{section.lower()}]'), -1)
        if head >= 0:
            if not lines[head].endswith(('\n', '\r')):   # the header is the file's last line
                lines[head] += nl
            lines.insert(head + 1, entry + nl)
        else:
            if lines and not lines[-1].endswith(('\n', '\r')):
                lines[-1] += nl
            lines += [nl] if lines else []
            lines += [f'[{section}]{nl}', entry + nl]
    return bom + ''.join(lines)


def _write(game_dir, old, new):
    if old == new:
        return
    path = _path(game_dir)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix=path.name + '.', suffix='.tmp')
    tmp_path = Path(name)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
            f.write(new)
        if path.is_file():
            shutil.copymode(path, tmp_path)
        if _text(game_dir) != old:
            raise OSError('The game settings changed while saving. Reopen Game Settings and try again.')
        os.replace(tmp_path, path)
    finally:
        tmp_path.unlink(missing_ok=True)


def put(game_dir, section, key, value):
    """Change one game setting, preserving unrelated lines."""
    text = _text(game_dir)
    _write(game_dir, text, _edit(text, section, key, value))


# Tokens under "upscaling" denote the combinations written by runtime/ui/menu.cpp row_upscaling.
_MODES = {'dlaa': 'Native AA', 'quality': 'Quality', 'balanced': 'Balanced', 'performance': 'Performance',
          'ultra_performance': 'Ultra Performance'}
_DEFAULT = {'default': 'Game default'}
_TOGGLE = {**_DEFAULT, '0': 'Off', '1': 'On'}
CHOICES = {
    'fps_limit': {**_DEFAULT, **{str(n): f'{n} FPS' for n in (30, 40, 45, 60, 90, 120, 144, 165, 240)},
                  'display': 'Display refresh rate', 'unlimited': 'Unlimited'},
    'upscaling': {**_DEFAULT, 'bilinear': 'Bilinear', 'fsr': 'AMD FSR 1',
                  **{f'dlss-{k}': 'NVIDIA DLAA' if k == 'dlaa' else f'DLSS {v}' for k, v in _MODES.items()},
                  **{f'fsr3-{k}': f'FSR 3.1 {v}' for k, v in _MODES.items()}},
    'framegen': {**_DEFAULT, 'off': 'Off', 'fsr3': 'AMD FSR 3.1', 'dlss': 'NVIDIA DLSS'},
    'game_res': {**_DEFAULT, **{str(n): f'{n}p' for n in (1080, 1440, 1800, 2160, 2880)}, 'window': 'Window height'},
    'fullscreen': _TOGGLE, 'microphone': _TOGGLE,
    'perf_overlay': {**_DEFAULT, 'off': 'Off', 'fps': 'FPS', 'full': 'Full panel'},
}
_UPSCALING_KEYS = ('upscaler', 'dlaa', 'taa', 'dlss')


def _options(text):
    raw = {}
    for line in text.splitlines():
        item = _setting(line)
        if item and item[1]:
            raw[item[0].lower()] = item[1]
    options = {k: raw.get(k, 'default') for k in CHOICES}
    if 'fps_limit' not in raw and raw.get('fps60') == '0':
        options['fps_limit'] = '30'
    if raw.get('dlaa', '0') != '0':
        method = 'fsr3' if raw.get('taa') == 'fsr3' else 'dlss'
        options['upscaling'] = method + '-' + raw.get('dlss', 'dlaa')
    elif any(k in raw for k in _UPSCALING_KEYS):
        options['upscaling'] = raw.get('upscaler', 'fsr')
    return options


def read_options(game_dir):
    """File choices, with 'default' for unset options and custom values preserved for display."""
    return _options(_text(game_dir))


def save_options(game_dir, changes, original=None):
    """Save a selection together. Compare edited options with the ones first displayed, merge unrelated disk edits."""
    if not changes:
        return
    for option, value in changes.items():
        if option not in CHOICES or value not in CHOICES[option]:
            raise ValueError(f'Unsupported setting: {option} = {value}')
    text = _text(game_dir)
    current = _options(text)
    changes = dict(changes)
    upscaling = changes.get('upscaling', current['upscaling'])
    if not upscaling.startswith(('dlss-', 'fsr3-')):
        if changes.get('framegen') in ('dlss', 'fsr3'):
            raise ValueError('Choose DLSS or FSR 3.1 upscaling before enabling frame generation.')
        if 'upscaling' in changes and current['framegen'] not in ('off', 'default'):
            changes.setdefault('framegen', 'off')
    if original is not None and any(current[k] != original[k] for k in changes):
        raise OSError('The game settings changed since this screen opened. Reopen Game Settings and try again.')

    edits = {}
    for key, value in changes.items():
        if key == 'upscaling':
            if value == 'default':
                edits.update(dict.fromkeys(_UPSCALING_KEYS))
            elif value in ('fsr', 'bilinear'):
                edits.update(upscaler=value, dlaa='0')
            else:
                method, mode = value.split('-', 1)
                edits.update(upscaler='bilinear', dlaa='1', taa=method, dlss=mode)
        else:
            edits[key] = None if value == 'default' else value
            if key == 'fps_limit' and value == 'default':
                edits['fps60'] = None

    updated = text
    for key, value in edits.items():
        section = {'fps_limit': 'frame rate', 'fps60': 'frame rate', 'microphone': 'audio'}.get(key, 'display')
        updated = _edit(updated, section, key, value)
    _write(game_dir, text, updated)
