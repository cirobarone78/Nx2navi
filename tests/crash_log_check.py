"""Exercise the capped game log, crash report and rotation without game files. Run with clang on PATH."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    logs = Path(tmp) / 'logs'
    logs.mkdir()
    exe = Path(tmp) / ('check.exe' if os.name == 'nt' else 'check')
    gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O2', '-ffunction-sections', '-fdata-sections',
                    '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/crash_log_check.c'),
                    str(ROOT / 'runtime/dispatch.c'), str(ROOT / 'runtime/logq.c'), gc, '-o', str(exe)], check=True)
    # A legacy log has no companion. An orphaned companion in its destination must be cleared on rotation.
    (logs / 'nx2navi.log').write_text('legacy run')
    (logs / 'nx2navi.1.crash.log').write_text('older crash')
    result = subprocess.run([exe, 'fail'], cwd=tmp, capture_output=True)
    assert result.returncode == 70, result.stderr
    first = (logs / 'nx2navi.log').read_bytes()
    crash = (logs / 'nx2navi.crash.log').read_bytes()
    assert b'64 MiB' in first and b'synthetic crash' not in first
    assert b'FATAL: synthetic crash' in crash and b'fatal: memory commit' in crash
    assert b'GPU fault breadcrumb' in crash and b'ordinary trace' not in crash
    assert b'ordinary trace after the breadcrumb' in result.stderr
    assert not (logs / 'nx2navi.1.crash.log').exists()
    for n in range(1, 5):
        subprocess.run([exe, 'clean'], cwd=tmp, check=True)
        assert (logs / f'nx2navi.{n}.log').read_bytes() == first
        assert (logs / f'nx2navi.{n}.crash.log').read_bytes() == crash
        assert not (logs / 'nx2navi.crash.log').read_bytes()
    subprocess.run([exe, 'clean'], cwd=tmp, check=True)
    assert all(not f.read_bytes() for f in logs.glob('*.crash.log'))
    result = subprocess.run([exe, 'exception'], cwd=tmp, capture_output=True)
    assert result.returncode == 71, result.stderr
    crash = (logs / 'nx2navi.crash.log').read_text()
    assert 'FATAL: synthetic exception' in crash and 'driver+0x2a' in crash and 'not on a guest thread' in crash
    result = subprocess.run([exe, 'fallback'], cwd=tmp, capture_output=True)
    assert result.returncode == 70, result.stderr
    fallback = (logs / 'nx2navi.log').read_bytes()
    assert fallback.count(b'GPU fault breadcrumb') == 1 and fallback.count(b'FATAL: synthetic crash') == 1
    assert b'ordinary trace' not in fallback
    print('Capped fatal and exception reports and five-run log rotation passed.')
