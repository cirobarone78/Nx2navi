"""launcher/ and tools/make_game.py's bookkeeping, without game files or Windows.

Run: python tests/launcher_test.py   (Python 3.14 for the zstd tests; the window test needs tkinter, numpy and a display)

- versions, settings
- toolchain: the package database, dependency resolution and an install from a local fake repository
- net: downloads that resume, check their sha256 and stop on cancel; the certificate checks against a local HTTPS
  server with certificates the openssl command makes (an expired one, an unknown one), the clock comparison
- feed: GitHub releases (stable, preview, private with a token) and a plain manifest URL, served locally
- updater: staging a release, switching, rollback, pruning, refusing unsafe archives
- background updates: a release staged beside the running version without switching to it, the ready marker, a
  staged release found and installed after a restart, no install while the game runs, the install once it exited,
  nothing by itself with Install Updates Automatically off, a failed staging tried again after the next check
- gamefiles: the .bfotf font wrapping and zstd dictionaries, on a font of this system made into a fake RomFS
- the button art of a release: the files tools/prompt_art.py copies, and the packages and tools the prompts need
- make_game.py: a synthetic .nup through the check, exefs and romfs steps with --events, skipped steps on a second
  run, --rebuild without the game files, the shaders key following the translator the build uses (the kit's bin/ or
  a source checkout's build/xlate/), the prompts step with a stand-in prompt tool, the object cache of the compile
  step with the system's C compiler, and its windres call under folders with spaces (a script in windres's place)
- jobs: an install driven by a fake make_game.py, with and without the prompts row
- the look: the colour schemes, the background picture's fitting and kept copy, the sound effects (low by default,
  only the game's own select and back sounds, decoded from a made-up .bwav)
- the window: --selftest draws every screen (skipped without a display)
"""
import contextlib
import hashlib
import http.server
import io
import json
import os
import shutil
import ssl
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))

from launcher import build, feed, gameini, gamefiles, jobs, net, saves, settings, toolchain, updater, versions, winsys  # noqa: E402
from launcher.context import Context  # noqa: E402
from launcher.paths import Layout  # noqa: E402

try:
    from compression import zstd as ZSTD
except ImportError:
    ZSTD = None


# ---------------------------------------------------------------- helpers

class Server:
    """A local HTTP server for a folder, with extra routes (path -> (status, headers, body)) and Range support; HTTPS
    with cert (a PEM file holding the certificate and its key). HEAD answers a redirect to https, as a server that
    serves only HTTPS does."""

    def __init__(self, folder, routes=None, cert=None):
        folder, routes = Path(folder), routes if routes is not None else {}
        self.hits = []
        server = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_HEAD(self):
                server.hits.append((self.path, dict(self.headers)))
                self.send_response(301)
                self.send_header('Location', 'https://127.0.0.1' + self.path)
                self.send_header('Content-Length', '0')
                self.end_headers()

            def do_GET(self):
                server.hits.append((self.path, dict(self.headers)))
                if self.path in routes:
                    status, headers, body = routes[self.path]
                    self.send_response(status)
                    for k, v in headers.items():
                        self.send_header(k, v)
                    if 'Content-Length' not in headers:
                        self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                p = folder / self.path.lstrip('/').split('?')[0]
                if not p.is_file():
                    self.send_error(404)
                    return
                data = p.read_bytes()
                rng = self.headers.get('Range')
                if rng and rng.startswith('bytes='):
                    start = int(rng[6:].split('-')[0])
                    if start >= len(data):
                        self.send_error(416)
                        return
                    self.send_response(206)
                    self.send_header('Content-Range', f'bytes {start}-{len(data) - 1}/{len(data)}')
                    data = data[start:]
                else:
                    self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except ConnectionError:   # the client stopped reading (the cancel test)
                    pass

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}/'
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert)
            self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
            self.url = 'https' + self.url[4:]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_cert(folder, start, end):
    """A self-signed certificate for 127.0.0.1 valid from start to end (epoch seconds), with its key, as one PEM file
    in the new folder; made by the openssl command (its ca command takes any dates, its req command none in the
    past)."""
    folder = Path(folder)
    folder.mkdir(parents=True)
    (folder / 'index.txt').write_text('')
    (folder / 'serial.txt').write_text('01\n')
    (folder / 'ca.cnf').write_text('[ca]\ndefault_ca = d\n[d]\ndatabase = index.txt\nserial = serial.txt\n'
                                   'new_certs_dir = .\ndefault_md = sha256\npolicy = p\ncopy_extensions = copy\n'
                                   'unique_subject = no\n[p]\ncommonName = supplied\n')
    def stamp(t):
        return time.strftime('%Y%m%d%H%M%SZ', time.gmtime(t))

    def run(*args):
        subprocess.run(['openssl', *args], cwd=folder, check=True, capture_output=True)

    run('req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', 'key.pem',
        '-out', 'req.csr', '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1')
    run('ca', '-batch', '-config', 'ca.cnf', '-selfsign', '-keyfile', 'key.pem', '-in', 'req.csr', '-out', 'cert.pem',
        '-startdate', stamp(start), '-enddate', stamp(end), '-notext')
    pem = folder / 'both.pem'
    pem.write_bytes((folder / 'cert.pem').read_bytes() + (folder / 'key.pem').read_bytes())
    return pem


class CountingContext(ssl.SSLContext):
    """A client SSLContext that counts its handshakes."""

    def wrap_socket(self, *args, **kwargs):
        self.handshakes = getattr(self, 'handshakes', 0) + 1
        return super().wrap_socket(*args, **kwargs)


def client_context(trust=None):
    ctx = CountingContext(ssl.PROTOCOL_TLS_CLIENT)
    if trust:
        ctx.load_verify_locations(cafile=trust)
    else:
        ctx.load_default_certs()
    return ctx


def tar_bytes(files, compress=True):
    """A tar (zstd-compressed when the stdlib has zstd, else gzip) of {name: bytes}."""
    buf = io.BytesIO()
    mode = 'w:gz' if not (compress and ZSTD) else 'w'
    with tarfile.open(fileobj=buf, mode=mode) as t:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    data = buf.getvalue()
    return ZSTD.compress(data) if compress and ZSTD else data


def desc(name, version, filename, data, depends=(), provides=()):
    lines = ['%FILENAME%', filename, '', '%NAME%', name, '', '%VERSION%', version, '', '%CSIZE%', str(len(data)), '',
             '%ISIZE%', str(len(data) * 3), '', '%SHA256SUM%', sha(data), '']
    if depends:
        lines += ['%DEPENDS%', *depends, '']
    if provides:
        lines += ['%PROVIDES%', *provides, '']
    return '\n'.join(lines).encode()


def fake_repo(folder, damage=None):
    """A pacman repository with the ten root packages (each holding one of make_game.py's required files), one
    dependency and one name provided by another package."""
    P = toolchain.PREFIX
    files = {'clang': {'ucrt64/bin/clang.exe': b'clang', 'ucrt64/bin/clang++.exe': b'clang++'},
             'sdl3': {'ucrt64/bin/SDL3.dll': b'sdl', 'ucrt64/lib/libSDL3.dll.a': b'implib'},
             'vulkan-memory-allocator': {'ucrt64/include/vma/vk_mem_alloc.h': b'// vma'},
             'vulkan-loader': {'ucrt64/lib/libvulkan-1.dll.a': b'implib'},
             'llvm-libs': {'ucrt64/bin/libLLVM.dll': b'llvm', 'ucrt64/share/doc/llvm/README': b'skipped'}}
    deps = {'clang': [P + 'llvm-libs=23.1', P + 'gcc'], 'sdl3': [P + 'vulkan']}
    entries = {}
    for root in list(toolchain.ROOTS) + ['llvm-libs']:
        name = P + root
        fn = f'{name}-1.0-1-any.pkg.tar.zst'
        data = tar_bytes({'.PKGINFO': b'pkg', **files.get(root, {f'ucrt64/share/{root}/x': b'x'})})
        if damage == root:
            (folder / fn).write_bytes(data[:-3] + b'bad')
        else:
            (folder / fn).write_bytes(data)
        entries[f'{name}-1.0-1/desc'] = desc(name, '1.0-1', fn, data, deps.get(root, ()),
                                             [P + 'vulkan'] if root == 'vulkan-loader' else ())
    (folder / 'ucrt64.db').write_bytes(tar_bytes(entries))


def fake_fast_repo(folder, clang_version='1.0-1'):
    """The CLANG64 packages toolchain.FAST_PACKAGES names, in folder/clang64."""
    folder = folder / 'clang64'
    folder.mkdir()
    P = toolchain.FAST_PREFIX
    files = {'clang': {'clang64/bin/clang.exe': b'clang64', 'clang64/bin/clang-repl.exe': b'not needed',
                       'clang64/lib/clang/23/include/immintrin.h': b'// x86'},
             'llvm-libs': {'clang64/bin/libLLVM-23.dll': b'llvm', 'clang64/share/doc/x': b'skipped'}}
    entries = {}
    for root in toolchain.FAST_PACKAGES:
        name, ver = P + root, clang_version if root == 'clang' else '1.0-1'
        fn = f'{name}-{ver}-any.pkg.tar.zst'
        data = tar_bytes({'.PKGINFO': b'pkg', **files.get(root, {f'clang64/bin/{root}.dll': b'dll'})})
        (folder / fn).write_bytes(data)
        entries[f'{name}-{ver}/desc'] = desc(name, ver, fn, data, (), ())
    (folder / 'clang64.db').write_bytes(tar_bytes(entries))


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-test-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.layout = Layout(root=self.tmp / 'launcher', app=ROOT).ensure()


# ---------------------------------------------------------------- versions, settings

class VersionTest(unittest.TestCase):
    def test_order(self):
        self.assertTrue(versions.newer('0.3.11', '0.3.10'))
        self.assertTrue(versions.newer('0.4', '0.3.99'))
        self.assertFalse(versions.newer('0.3.10+abc', '0.3.10'))
        self.assertTrue(versions.newer('1.0.0', '1.0.0-rc1'))
        self.assertEqual(versions.display('0.3.11+abc1234'), '0.3.11')
        with self.assertRaises(ValueError):
            versions.parse('banana')


class GameIniTest(Tmp):
    """The game settings the launcher switches (launcher/gameini.py), read as runtime/cfg.c reads nx2navi.ini."""

    def test_new_file_gets_the_section(self):
        gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertEqual((self.tmp / 'nx2navi.ini').read_bytes(), b'[discord]\ndiscord_rpc = 1\n')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '1')

    def test_only_the_key_line_changes(self):
        # the byte order mark, the line ends and a line that is not UTF-8 stay as they were
        ini = self.tmp / 'nx2navi.ini'
        ini.write_bytes(b'\xef\xbb\xbf[display]\r\nfullscreen = 1\r\n# caf\xe9\r\n# discord_rpc = 1\r\n[discord]\r\n'
                        b'NX2NAVI_DISCORD_RPC = "1"\r\ndiscord_app_id = 42')
        self.assertEqual(gameini.get(self.tmp, 'DISCORD_RPC'), '1')   # quotes, prefix and case as the game reads them
        gameini.put(self.tmp, 'discord', 'discord_rpc', '0')
        self.assertEqual(ini.read_bytes(), b'\xef\xbb\xbf[display]\r\nfullscreen = 1\r\n# caf\xe9\r\n# discord_rpc = 1\r\n'
                         b'[discord]\r\ndiscord_rpc = 0\r\ndiscord_app_id = 42')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '0')
        self.assertFalse((self.tmp / 'nx2navi.ini.tmp').exists())

    def test_missing_key_goes_under_its_section(self):
        ini = self.tmp / 'nx2navi.ini'
        ini.write_bytes(b'[discord]\ndiscord_app_id = 42\n[display]\nfullscreen = 1')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '')
        gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertEqual(ini.read_bytes(), b'[discord]\ndiscord_rpc = 1\ndiscord_app_id = 42\n[display]\nfullscreen = 1')

    def test_an_empty_line_leaves_the_earlier_value(self):
        # runtime/cfg.c: "key =" leaves the setting as it was, so the game keeps 0 here, and a change goes to that line
        ini = self.tmp / 'nx2navi.ini'
        ini.write_bytes(b'discord_rpc = 0\ndiscord_rpc =\n')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '0')
        gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertEqual(ini.read_bytes(), b'discord_rpc = 1\ndiscord_rpc =\n')

    def test_an_unreadable_file_is_not_rewritten(self):
        (self.tmp / 'nx2navi.ini').mkdir()   # there, but not a file that can be read
        with self.assertRaises(OSError):
            gameini.get(self.tmp, 'discord_rpc')
        with self.assertRaises(OSError):
            gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertTrue((self.tmp / 'nx2navi.ini').is_dir())


class RebuildArgsTest(Tmp):
    """Rebuild the Game compiles the game's code for this processor's level (--retune); an update keeps the level its
    objects were compiled for, so that it compiles only what changed (tools/make_game.py lifted_level)."""

    def test_retune_only_for_rebuild_the_game(self):
        self.assertIn('--retune', build.rebuild_args(self.layout, 'G:/game', True))
        self.assertNotIn('--retune', build.rebuild_args(self.layout, 'G:/game'))
        self.assertTrue(jobs.RebuildJob.retune)
        self.assertFalse(jobs.ContinueUpdateJob.retune)


class SettingsTest(Tmp):
    def test_roundtrip_and_reset(self):
        s = settings.Settings(self.layout.settings)
        self.assertTrue(s['auto_check'])
        s['auto_check'], s['game_dir'], s['channel'] = False, 'D:/g', 'preview'
        s.save()
        t = settings.Settings(self.layout.settings)
        self.assertEqual((t['auto_check'], t['game_dir'], t['channel']), (False, 'D:/g', 'preview'))
        t.reset()
        self.assertEqual((t['auto_check'], t['game_dir'], t['channel']), (True, 'D:/g', 'stable'))

    def test_defaults(self):
        # the Settings page of a new install, and what Default Settings goes back to: updates checked at start, the
        # launcher hidden while playing, the build cache kept (an update recompiles only what changed), Stable
        chosen = ('auto_check', 'hide_while_playing', 'keep_cache', 'channel')
        want = (True, True, True, 'stable')
        s = settings.Settings(self.layout.settings)
        self.assertEqual(tuple(s[k] for k in chosen), want)
        s.update(auto_check=False, hide_while_playing=False, keep_cache=False, channel='preview')
        s.save()
        t = settings.Settings(self.layout.settings)   # a player's own choices stay across starts and updates
        self.assertEqual(tuple(t[k] for k in chosen), (False, False, False, 'preview'))
        t.reset()
        self.assertEqual(tuple(t[k] for k in chosen), want)

    def test_bad_types_ignored(self):
        self.layout.settings.write_text(json.dumps({'auto_check': 'yes', 'channel': 'preview', 'junk': 1}))
        s = settings.Settings(self.layout.settings)
        self.assertEqual((s['auto_check'], s['channel']), (True, 'preview'))
        self.assertNotIn('junk', s)

    def test_bom_and_typo(self):
        # a file saved by an editor with a BOM reads as written; one with a typo keeps a copy before a save replaces it
        self.layout.settings.write_bytes(b'\xef\xbb\xbf' + json.dumps({'game_dir': 'D:/g'}).encode())
        s = settings.Settings(self.layout.settings)
        self.assertEqual(s['game_dir'], 'D:/g')
        self.assertFalse(s.unreadable)
        typo = '{"game_dir": "D:/g", "github_token": "t",}'
        self.layout.settings.write_text(typo, encoding='utf-8')
        s = settings.Settings(self.layout.settings)
        self.assertTrue(s.unreadable)
        self.assertEqual(s['game_dir'], '')
        s.save()
        self.assertEqual(self.layout.settings.with_name('launcher.json.bad').read_text(encoding='utf-8'), typo)


class UninstallTest(Tmp):
    def tree(self, root, rels):
        for rel in rels:
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('x')

    def test_plan_keeps_the_game_and_what_is_not_ours(self):
        from types import SimpleNamespace
        root = self.tmp / 'NX2navi'
        self.tree(root, ('NX2navi.exe', 'app/current.txt', 'python.old-3/x', 'game/profile/save/1/a.sav', 'mine.txt'))
        layout = SimpleNamespace(root=root, toolchain=root / 'toolchain')
        gone, empty = winsys.uninstall_plan(layout)
        self.assertNotIn(root / 'game', gone)
        self.assertNotIn(root, gone)
        self.assertIn(root / 'python.old-3', gone)
        self.assertEqual(empty, [root])
        gone, _ = winsys.uninstall_plan(layout, str(root / 'game'))
        self.assertEqual(gone[0], root / 'game')
        # the toolchain moved to C:\NX2navi: that folder goes only when nothing else is left in it
        other = self.tmp / 'C NX2navi' / 'toolchain'
        gone, empty = winsys.uninstall_plan(SimpleNamespace(root=root, toolchain=other))
        self.assertIn(other, gone)
        self.assertNotIn(other.parent, gone)
        self.assertEqual(empty, [root, other.parent])

    @unittest.skipUnless(os.name == 'nt', 'runs the cmd.exe script')
    def test_script_on_windows(self):
        from types import SimpleNamespace
        root = self.tmp / 'NX2navi ÿ x'   # spaces and a character outside ASCII
        self.tree(root, ('NX2navi.exe', 'app/1/VERSION', 'python/pythonw.exe', 'python.old-12/x', 'pydeps/z',
                         'data/launcher.json', 'toolchain/msys64/a', 'game/profile/save/1/a.sav', 'mine.txt'))
        layout = SimpleNamespace(root=root, toolchain=root / 'toolchain')
        script = winsys.uninstall_script(*winsys.uninstall_plan(layout)).replace('ping -n 3 127.0.0.1 >nul', 'echo')
        subprocess.run(f'cmd.exe /c "{script}"', capture_output=True)
        self.assertEqual(sorted(p.name for p in root.iterdir()), ['game', 'mine.txt'])
        self.assertTrue((root / 'game/profile/save/1/a.sav').is_file())
        (root / 'mine.txt').unlink()
        script = winsys.uninstall_script(*winsys.uninstall_plan(layout, str(root / 'game')))
        subprocess.run(f'cmd.exe /c "{script.replace("ping -n 3 127.0.0.1 >nul", "echo")}"', capture_output=True)
        self.assertFalse(root.exists())

    def test_script_leaves_out_percent_paths(self):
        script = winsys.uninstall_script([Path('C:/%HOMEPATH%'), Path('C:/x/a')], [Path('C:/%TEMP%/b')])
        self.assertNotIn('%', script)
        self.assertIn('a', script)


# ---------------------------------------------------------------- toolchain

class ToolchainTest(Tmp):
    def test_resolve_and_install(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url)
        names = [p['name'] for p in lock['packages']]
        self.assertEqual(names[0], toolchain.PREFIX + 'clang')
        self.assertIn(toolchain.PREFIX + 'llvm-libs', names)              # a dependency with a version constraint
        self.assertEqual(names.count(toolchain.PREFIX + 'vulkan-loader'), 1)   # 'vulkan' resolves to its provider
        self.assertFalse(toolchain.is_current(self.layout, lock))
        phases = []
        toolchain.install(self.layout, lock, progress=lambda ph, d, t: phases.append(ph), log=lambda m: None)
        self.assertTrue(toolchain.is_current(self.layout, lock))
        self.assertTrue((self.layout.msys / 'bin' / 'clang.exe').is_file())
        self.assertFalse((self.layout.msys / 'share' / 'doc').exists())    # documentation left out
        self.assertIn('download', phases)
        self.assertIn('unpack', phases)
        hits = len(srv.hits)
        toolchain.install(self.layout, lock, log=lambda m: None)           # cached package files: no download
        self.assertEqual(len(srv.hits), hits)
        self.assertEqual(toolchain.lock_digest(lock), toolchain.installed(self.layout)['digest'])

    def test_fast_clang_beside(self):
        """CLANG64's clang goes into msys64/clang64 when its version matches UCRT64's: clang.exe, the DLLs and its
        headers only."""
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        fake_fast_repo(repo)
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url, fast_repo=srv.url + 'clang64/')
        self.assertEqual(lock.get('fast_repo'), srv.url + 'clang64/')
        self.assertEqual(sum(p.get('env') == 'clang64' for p in lock['packages']), len(toolchain.FAST_PACKAGES))
        toolchain.install(self.layout, lock, log=lambda m: None)
        fast = self.layout.msys.parent / 'clang64'
        self.assertEqual((fast / 'bin' / 'clang.exe').read_bytes(), b'clang64')
        self.assertTrue((fast / 'bin' / 'libLLVM-23.dll').is_file())
        self.assertTrue((fast / 'lib' / 'clang' / '23' / 'include' / 'immintrin.h').is_file())
        self.assertFalse((fast / 'bin' / 'clang-repl.exe').exists())
        self.assertFalse((fast / 'share').exists())
        self.assertTrue(toolchain.is_current(self.layout, lock))

    def test_fast_clang_left_out_on_another_version(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        fake_fast_repo(repo, clang_version='2.0-1')
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url, fast_repo=srv.url + 'clang64/')
        self.assertNotIn('fast_repo', lock)
        self.assertIn('2.0-1', lock['fast_skipped'])
        self.assertFalse(any(p.get('env') for p in lock['packages']))

    def test_damaged_package_refused(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo, damage='opus')
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url)
        with self.assertRaises(toolchain.ToolchainError):
            toolchain.install(self.layout, lock, log=lambda m: None)
        self.assertFalse(toolchain.is_current(self.layout, lock))

    def test_wheel_tag(self):
        tag = toolchain.wheel_tag()[0]
        self.assertTrue(tag.startswith(f'cp{sys.version_info.major}{sys.version_info.minor}-'))


# ---------------------------------------------------------------- net

class NetTest(Tmp):
    def test_get_bytes_retries_a_cut_connection(self):
        """A connection reset (WinError 10054 during Gather Build Tools) or a server error is tried again; a 404 is not."""
        import urllib.error
        calls = []

        def flaky(url, headers=None, timeout=30, version='dev'):
            calls.append(url)
            if len(calls) == 1:
                raise ConnectionResetError(10054, 'An existing connection was forcibly closed by the remote host')
            if len(calls) == 2:
                raise urllib.error.HTTPError(url, 503, 'busy', {}, None)
            return io.BytesIO(b'{"ok": 1}')

        with mock.patch.object(net, 'request', flaky), mock.patch.object(net.time, 'sleep') as slept:
            self.assertEqual(net.get_json('https://example.invalid/x.json'), {'ok': 1})
        self.assertEqual(len(calls), 3)
        self.assertEqual(slept.call_count, 2)

        def missing(url, headers=None, timeout=30, version='dev'):
            calls.append(url)
            raise urllib.error.HTTPError(url, 404, 'not found', {}, None)

        calls.clear()
        with mock.patch.object(net, 'request', missing), mock.patch.object(net.time, 'sleep'):
            with self.assertRaises(urllib.error.HTTPError):
                net.get_bytes('https://example.invalid/missing')
        self.assertEqual(len(calls), 1)

        def down(url, headers=None, timeout=30, version='dev'):
            raise ConnectionResetError(10054, 'reset')

        with mock.patch.object(net, 'request', down), mock.patch.object(net.time, 'sleep'):
            with self.assertRaises(ConnectionResetError):   # every try failed: the last error goes up
                net.get_bytes('https://example.invalid/down')

    def test_early_eof_retries_metadata_and_resumes_file(self):
        data = b'{"build":123}'
        (self.tmp / 'asset').write_bytes(data)
        cut = (200, {'Content-Length': str(len(data))}, data[:7])
        routes = {'/asset': cut}
        srv = Server(self.tmp, routes)
        self.addCleanup(srv.close)
        url = srv.url + 'asset'
        with mock.patch.object(net.time, 'sleep', side_effect=lambda _: routes.clear()):
            self.assertEqual(net.get_json(url), {'build': 123})
            routes['/asset'] = cut
            dest = self.tmp / 'download'
            net.download(url, dest, sha256=sha(data), size=len(data))
        self.assertEqual(dest.read_bytes(), data)
        self.assertEqual([h.get('Range') for _, h in srv.hits], [None, None, None, 'bytes=7-'])

    def test_download_resume_and_checks(self):
        data = os.urandom(3 * net.CHUNK + 123)
        (self.tmp / 'f.bin').write_bytes(data)
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        dest = self.tmp / 'out' / 'f.bin'
        dest.parent.mkdir()
        (dest.parent / 'f.bin.part').write_bytes(data[:1000])             # a part file from a cut connection
        net.download(srv.url + 'f.bin', dest, sha256=sha(data), size=len(data))
        self.assertEqual(dest.read_bytes(), data)
        self.assertEqual(srv.hits[-1][1].get('Range'), 'bytes=1000-')
        with self.assertRaises(net.DownloadError):
            net.download(srv.url + 'f.bin', self.tmp / 'out' / 'g.bin', sha256='0' * 64)
        self.assertFalse((self.tmp / 'out' / 'g.bin').exists())
        with self.assertRaises(net.Cancelled):
            net.download(srv.url + 'f.bin', self.tmp / 'out' / 'h.bin', cancel=lambda: True)
        with self.assertRaises(net.DownloadError):
            net.download(srv.url + 'missing.bin', self.tmp / 'out' / 'm.bin')

    def test_plain_http_redirects_and_oversize(self):
        with self.assertRaises(net.DownloadError):                        # refused before any connection
            net.request('http://example.invalid/f.bin')
        (self.tmp / 'f.bin').write_bytes(b'0123456789')
        routes = {}
        srv = Server(self.tmp, routes)
        self.addCleanup(srv.close)
        other = srv.url.replace('127.0.0.1', 'localhost')                 # the same server under another host name
        routes['/same'] = (302, {'Location': srv.url + 'f.bin'}, b'')
        routes['/away'] = (302, {'Location': other + 'f.bin'}, b'')
        token = {'Authorization': 'Bearer secret'}
        self.assertEqual(net.get_bytes(srv.url + 'same', token), b'0123456789')
        self.assertEqual(srv.hits[-1][1].get('Authorization'), 'Bearer secret')
        self.assertEqual(net.get_bytes(srv.url + 'away', token), b'0123456789')
        self.assertEqual(srv.hits[-2][1].get('Authorization'), 'Bearer secret')
        self.assertIsNone(srv.hits[-1][1].get('Authorization'))           # not sent on to another host
        dest = self.tmp / 'out' / 'o.bin'
        with self.assertRaises(net.DownloadError):                        # more than the expected size
            net.download(srv.url + 'f.bin', dest, size=4)
        self.assertFalse(dest.exists() or dest.with_name('o.bin.part').exists())

    def test_server_clock(self):
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        t = net.server_clock(srv.url)          # the Date of the redirect to https, which is not followed
        self.assertIsNotNone(t)
        self.assertLess(abs(t - time.time()), 5)
        self.assertEqual([h[0] for h in srv.hits], ['/'])
        srv.close()
        self.assertIsNone(net.server_clock(srv.url, timeout=2))


@unittest.skipUnless(shutil.which('openssl'), 'needs the openssl command')
class CertificateTest(Tmp):
    def setUp(self):
        super().setUp()
        saved = net._CHECKS, dict(net._ACCEPTED), net.server_clock
        self.addCleanup(lambda: setattr(net, '_CHECKS', saved[0]) or setattr(net, '_ACCEPTED', saved[1]) or
                        setattr(net, 'server_clock', saved[2]))
        net._ACCEPTED = {}
        self.data = os.urandom(100_000)
        (self.tmp / 'f.bin').write_bytes(self.data)

    def serve(self, start, end):
        cert = make_cert(self.tmp / f'cert{start}', start, end)
        srv = Server(self.tmp, cert=cert)
        self.addCleanup(srv.close)
        return srv, cert

    def test_refused_certificate(self):
        """An expired certificate in the trust store: the player's 'certificate has expired', at once, not retried,
        with the advice the server's clock calls for."""
        now = time.time()
        srv, cert = self.serve(now - 400 * 86400, now - 300 * 86400)
        check = client_context(trust=cert)
        net._CHECKS = [("Python's check", check)]
        asked = []
        cases = ((now + 400 * 86400, "This PC's clock reads"), (now - 3 * 86400, 'set the date, time and time zone'),
                 (now + 60, "This PC's clock agrees with 127.0.0.1's. Security software"),
                 (None, "Check that this PC's date, time and time zone are right"))
        for i, (theirs, says) in enumerate(cases):
            net.server_clock = lambda url, timeout=10, version='dev', t=theirs: asked.append(url) or t
            t0 = time.time()
            with self.assertRaises(net.CertificateError) as cm:
                net.download(srv.url + 'f.bin', self.tmp / 'out' / 'f.bin', sha256=sha(self.data))
            self.assertLess(time.time() - t0, 5)                 # no retries: the answer would be the same
            msg = str(cm.exception)
            self.assertIn("the certificate of 127.0.0.1 was not accepted (Python's check: certificate has expired)",
                          msg)
            self.assertIn(says, msg)
            self.assertEqual(check.handshakes, i + 1)
        self.assertEqual(asked, ['http://127.0.0.1/'] * len(cases))   # the clock is asked over plain HTTP
        self.assertEqual(srv.hits, [])                            # no request got through
        self.assertFalse((self.tmp / 'out' / 'f.bin.part').exists())

    def test_second_check_accepts(self):
        """A certificate the first check refuses and the second accepts downloads, and the second goes first for that
        host from then on."""
        now = time.time()
        srv, cert = self.serve(now - 86400, now + 2 * 86400)
        refuses, accepts = client_context(), client_context(trust=cert)
        net._CHECKS = [("Python's check", refuses), ("Windows' check", accepts)]
        dest = net.download(srv.url + 'f.bin', self.tmp / 'out' / 'f.bin', sha256=sha(self.data))
        self.assertEqual(dest.read_bytes(), self.data)
        self.assertEqual((refuses.handshakes, accepts.handshakes), (1, 1))
        self.assertEqual(net._ACCEPTED, {'127.0.0.1': 1})
        self.assertEqual(net.get_bytes(srv.url + 'f.bin'), self.data)
        self.assertEqual((refuses.handshakes, accepts.handshakes), (1, 2))
        net._CHECKS = [("Python's check", refuses)]
        net.server_clock = lambda *a, **k: None
        with self.assertRaises(net.CertificateError) as cm:
            net.get_bytes(srv.url + 'f.bin')
        self.assertIn("(Python's check: self-signed certificate)", str(cm.exception))


# ---------------------------------------------------------------- feed and updater

def make_app_zip(folder, ver, extra=None):
    app = folder / f'app-{ver}'
    (app / 'launcher').mkdir(parents=True)
    (app / 'launcher' / 'start.pyw').write_text('# test\n')
    (app / 'VERSION').write_text(ver + '\n')
    z = folder / f'NX2navi-app-{ver}.zip'
    with zipfile.ZipFile(z, 'w') as zf:
        for p in app.rglob('*'):
            if p.is_file():
                zf.write(p, p.relative_to(app).as_posix())
        for name, data in (extra or {}).items():
            zf.writestr(name, data)
    return z


def manifest_for(z, ver):
    return {'format': 1, 'version': ver, 'notes': '- One\n- Two',
            'app': {'name': z.name, 'size': z.stat().st_size, 'sha256': sha(z.read_bytes())}}


class FeedTest(Tmp):
    def test_asset_needs_a_plain_name_size_and_sha256(self):
        good = {'name': 'a.zip', 'size': 10, 'sha256': '0' * 64}
        r = feed.Release({'format': 1, 'version': '1.0.0', 'app': good}, assets={'a.zip': 'https://x/a.zip'})
        self.assertEqual(r.asset('app')[2:], (10, '0' * 64))
        for bad in ({'name': 'a.zip'}, dict(good, sha256='x'), dict(good, size=0), dict(good, size='10'),
                    dict(good, name='../a.zip'), dict(good, name='C:a.zip'), dict(good, name='sub\\a.zip')):
            r = feed.Release({'format': 1, 'version': '1.0.0', 'app': bad}, assets={bad['name']: 'https://x/a'})
            with self.assertRaises(feed.FeedError, msg=repr(bad)):
                r.asset('app')

    def serve_github(self):
        self.routes = {}
        srv = Server(self.tmp, self.routes)
        self.addCleanup(srv.close)
        return srv

    def publish(self, releases):
        self.routes['/repos/o/r/releases?per_page=20'] = (200, {'Content-Type': 'application/json'},
                                                          json.dumps(releases).encode())

    def release(self, srv, tag, ver, pre=False, manifest=True):
        z = make_app_zip(self.tmp, ver)
        m = self.tmp / f'm-{ver}.json'
        m.write_text(json.dumps(manifest_for(z, ver)))
        assets = [{'name': z.name, 'browser_download_url': srv.url + z.name, 'url': srv.url + z.name}]
        if manifest:
            assets.append({'name': feed.MANIFEST, 'browser_download_url': srv.url + m.name, 'url': srv.url + m.name})
        return {'tag_name': tag, 'prerelease': pre, 'draft': False, 'body': 'notes', 'html_url': 'x', 'assets': assets}

    def test_github_channels(self):
        srv = self.serve_github()
        self.publish([self.release(srv, 'v0.4.0-rc1', '0.4.0-rc1', pre=True),
                      self.release(srv, 'v0.3.12', '0.3.12', manifest=False), self.release(srv, 'v0.3.11', '0.3.11')])
        f = {'type': 'github', 'repo': 'o/r', 'api': srv.url.rstrip('/')}
        self.assertEqual(feed.latest(f, 'stable').version, '0.3.11')    # 0.3.12 lacks a manifest
        self.assertEqual(feed.latest(f, 'preview').version, '0.4.0-rc1')
        r = feed.latest(f, 'stable', token='secret')
        self.assertEqual(r.headers.get('Authorization'), 'Bearer secret')
        self.assertTrue(any(h.get('Authorization') == 'Bearer secret' for _, h in srv.hits))
        self.assertEqual(feed.plain_notes('## Head\n- **One**\n* `Two`\n<!-- x -->'), ['Head', 'One', 'Two'])

    def test_url_feed_and_update(self):
        z = make_app_zip(self.tmp, '0.9.0')
        (self.tmp / feed.MANIFEST).write_text(json.dumps(manifest_for(z, '0.9.0')))
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        rel = feed.latest({'type': 'url', 'url': srv.url + feed.MANIFEST})
        self.assertEqual(rel.version, '0.9.0')
        self.layout.apps.mkdir(parents=True)
        (self.layout.apps / 'current.txt').write_text('0.8.0\n')
        old = self.layout.apps / '0.8.0' / 'launcher'
        old.mkdir(parents=True)
        (old / 'start.pyw').write_text('')
        for stale in ('0.7.0', '0.6.0'):
            (self.layout.apps / stale).mkdir()
        folder = updater.stage_app(self.layout, rel)
        self.assertTrue((folder / 'launcher' / 'start.pyw').is_file())
        updater.switch_to(self.layout, '0.9.0')
        self.assertEqual((updater.current(self.layout), updater.previous(self.layout)), ('0.9.0', '0.8.0'))
        updater.prune(self.layout)
        self.assertEqual(sorted(p.name for p in self.layout.apps.iterdir() if p.is_dir()), ['0.8.0', '0.9.0'])
        self.assertTrue(updater.rollback(self.layout))
        self.assertEqual(updater.current(self.layout), '0.8.0')
        self.assertFalse(updater.rollback(self.layout))                  # nothing further back

    def test_unsafe_or_wrong_archives(self):
        z = make_app_zip(self.tmp, '1.0.0', extra={'../evil.txt': 'x'})
        with self.assertRaises(updater.UpdateError):
            updater.safe_extract(z, self.tmp / 'x')
        self.assertFalse((self.tmp / 'evil.txt').exists())
        z2 = make_app_zip(self.tmp, '1.0.1')
        m = manifest_for(z2, '1.0.2')                                     # the archive says 1.0.1
        (self.tmp / feed.MANIFEST).write_text(json.dumps(m))
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        rel = feed.Release(m, assets={z2.name: srv.url + z2.name})
        with self.assertRaises(updater.UpdateError):
            updater.stage_app(self.layout, rel)
        self.assertFalse((self.layout.apps / '1.0.2').exists())


class BackgroundUpdateTest(Tmp):
    """An update fetched while the launcher runs (updater.stage, jobs.StageJob, jobs.BackgroundUpdate): staged beside
    the running version without switching to it, and installed only when the game is not running, by itself only
    with Install Updates Automatically on. The releases come from the local server through a URL feed."""

    def setUp(self):
        super().setUp()
        self.srv = Server(self.tmp)
        self.addCleanup(self.srv.close)
        self.layout = self.installed('0.4.0')
        self.game = str(self.tmp / 'game')
        self.running = False   # what build.game_running answers: a copy of the game the launcher did not start
        saved = build.game_running
        build.game_running = lambda game_dir, exe='ocarina.exe': self.running
        self.addCleanup(setattr, build, 'game_running', saved)
        self.restarts = []

    def installed(self, ver):
        """An installed launcher's folder (not a checkout) running version ver on the Python py-1."""
        root = self.tmp / 'nx'
        app = root / 'app' / ver
        (app / 'launcher').mkdir(parents=True)
        (app / 'launcher' / 'start.pyw').write_text('')
        (app / 'VERSION').write_text(ver + '\n')
        (root / 'app' / 'current.txt').write_text(ver + '\n')
        (root / 'python').mkdir()
        (root / 'python' / 'runtime-id.txt').write_text('py-1\n')
        layout = Layout(root=root, app=app).ensure()
        self.assertFalse(layout.dev)
        return layout

    def publish(self, ver, runtime='', pre=False, damaged=False):
        """A release of ver on the local server, read through a URL feed; runtime: the id of the Python it brings;
        damaged: its manifest gives the wrong sha256 for the app archive."""
        z = make_app_zip(self.tmp, ver)
        m = manifest_for(z, ver)
        if runtime:
            r = self.tmp / f'NX2navi-runtime-{runtime}.zip'
            with zipfile.ZipFile(r, 'w') as zf:
                zf.writestr('python.exe', 'MZ')
                zf.writestr('runtime-id.txt', runtime + '\n')
            m['runtime'] = {'id': runtime, 'name': r.name, 'size': r.stat().st_size, 'sha256': sha(r.read_bytes())}
        if damaged:
            m['app']['sha256'] = '0' * 64
        (self.tmp / f'm-{ver}.json').write_text(json.dumps(m))
        rel = feed.latest({'type': 'url', 'url': self.srv.url + f'm-{ver}.json'})
        rel.prerelease = pre
        return rel

    def context(self):
        """The launcher starting (again) in the installed folder: its settings, its log."""
        ctx = Context(self.layout)
        self.addCleanup(lambda: ctx._file and ctx._file.close())
        return ctx

    def background(self, ctx):
        return jobs.BackgroundUpdate(ctx, lambda args: self.restarts.append(args) or True)

    def stage(self, bg, rel):
        """poll() as the window's timer calls it: start the staging, wait for it, and return how it ended."""
        self.assertEqual(bg.poll(rel), 'started')
        deadline = time.time() + 60
        while bg.staging() and time.time() < deadline:
            time.sleep(0.02)
        return bg.poll(rel)

    def state(self):
        return updater.current(self.layout), updater.previous(self.layout), self.restarts

    def test_staging_leaves_the_running_version(self):
        """The release is unpacked beside the running version, its Python in its own folder rather than python.next
        (which the stub would swap in at the next start, under the running version), and nothing is switched or
        restarted."""
        bg = self.background(self.context())
        rel = self.publish('0.5.0', runtime='py-2')
        self.assertEqual(self.stage(bg, rel), 'staged')
        folder = self.layout.apps / '0.5.0'
        self.assertTrue((folder / 'launcher' / 'start.pyw').is_file())
        self.assertEqual(updater.read_text(folder / updater.STAGED_PYTHON / 'runtime-id.txt'), 'py-2')
        self.assertFalse((self.layout.root / 'python.next').exists())
        self.assertEqual(self.state(), ('0.4.0', '', []))
        self.assertEqual(bg.ready(rel)['version'], '0.5.0')
        self.assertIsNone(bg.poll(rel))                      # once per check
        bg.checked()
        self.assertIsNone(bg.poll(rel))                      # the next check finds it staged: nothing to download
        self.assertFalse(bg.staging())
        older = feed.Release({'format': 1, 'version': '0.4.5', 'app': {}})
        self.assertIsNone(bg.ready(older))                   # the feed no longer offers it: not installed
        updater.prune(self.layout)                           # an update's cleanup keeps a newer staged release
        self.assertTrue((folder / updater.STAGED).is_file())

    def test_ready_marker(self):
        """Only a release whose staging finished (staged.json, written last) and that is still whole installs."""
        rel = self.publish('0.5.0', runtime='py-2')
        updater.stage_app(self.layout, rel)                  # unpacked, as an interrupted staging leaves it
        self.assertIsNone(updater.staged(self.layout))
        with self.assertRaises(updater.UpdateError):
            updater.install_staged(self.layout, '0.5.0')
        self.assertEqual(updater.current(self.layout), '0.4.0')
        updater.stage(self.layout, rel)
        self.assertEqual(updater.staged(self.layout)['version'], '0.5.0')
        self.assertIsNone(updater.staged(self.layout, skip='0.5.0'))   # the version that failed to start
        py = self.layout.apps / '0.5.0' / updater.STAGED_PYTHON
        shutil.move(py, self.tmp / 'py')                     # the Python it needs is gone
        self.assertIsNone(updater.staged(self.layout))
        shutil.move(self.tmp / 'py', py)
        self.assertIsNotNone(updater.staged(self.layout))
        (self.layout.apps / '0.5.0' / updater.STAGED).write_text('{"version": "0.')   # cut short
        self.assertIsNone(updater.staged(self.layout))
        updater.stage(self.layout, rel)
        # a pre-release counts only on the Preview channel; staging it removes the staged release it supersedes
        updater.stage(self.layout, self.publish('0.6.0-rc1', pre=True))
        self.assertIsNone(updater.staged(self.layout, 'stable'))
        self.assertEqual(updater.staged(self.layout, 'preview')['version'], '0.6.0-rc1')
        self.assertFalse((self.layout.apps / '0.5.0').exists())
        self.assertTrue((self.layout.apps / '0.4.0').is_dir())

    def test_staged_version_installs_after_a_restart(self):
        """A staged release outlives the launcher: the next start finds it without a check and installs it: its
        Python to python.next for the stub, current.txt, and the restart that finishes in ContinueUpdateJob."""
        bg = self.background(self.context())
        rel = self.publish('0.5.0', runtime='py-2')
        self.assertEqual(self.stage(bg, rel), 'staged')
        bg = self.background(self.context())                 # the launcher started again, still 0.4.0
        self.assertEqual(bg.ready()['version'], '0.5.0')
        self.assertEqual(bg.install(self.game, why='the launcher started'), '0.5.0')
        self.assertEqual(self.state(), ('0.5.0', '0.4.0', [['--continue-update', '0.4.0']]))
        self.assertTrue(bg.restarting)
        self.assertEqual(updater.read_text(self.layout.root / 'python.next' / 'runtime-id.txt'), 'py-2')
        self.assertFalse((self.layout.apps / '0.5.0' / updater.STAGED).exists())
        # the stub rolled back from it: it is neither staged nor installed by itself again
        self.assertTrue(updater.rollback(self.layout))
        ctx = self.context()
        ctx.settings['rolled_back'] = '0.5.0'
        bg = self.background(ctx)
        self.assertIsNone(bg.poll(rel))
        self.assertEqual(bg.install(self.game, rel), '')
        self.assertEqual(len(self.restarts), 1)

    def test_no_install_while_the_game_runs(self):
        """A copy of the game started elsewhere, the copy the launcher started, and a download under way each hold
        the install back, also one the player asked for."""
        bg = self.background(self.context())
        rel = self.publish('0.5.0')
        self.assertEqual(self.stage(bg, rel), 'staged')
        self.running = True
        self.assertEqual(bg.install(self.game, rel, why='the launcher started'), '')
        self.assertTrue(bg.waiting)
        self.assertEqual(bg.install(self.game, rel, asked=True), '')
        self.running = False
        self.assertEqual(bg.install(self.game, rel, running=True), '')
        bg.job = jobs.StageJob(bg.ctx, feed.Release({'format': 1, 'version': '0.5.1', 'app': {}}))   # not finished
        self.assertEqual(bg.install(self.game, rel), '')
        self.assertEqual(self.state(), ('0.4.0', '', []))
        self.assertTrue((self.layout.apps / '0.5.0' / updater.STAGED).is_file())

    def test_install_after_the_game_exits(self):
        """Install Updates Automatically on (the default): a release staged during play installs once the game has
        exited."""
        ctx = self.context()
        self.assertTrue(ctx.settings['auto_install'])
        bg = self.background(ctx)
        self.running = True                                  # the check during play finds the release
        rel = self.publish('0.5.0')
        self.assertEqual(self.stage(bg, rel), 'staged')
        self.assertEqual(bg.install(self.game, rel, why='the download finished'), '')
        self.assertEqual(self.state(), ('0.4.0', '', []))
        self.running = False                                 # the game exits
        self.assertEqual(bg.install(self.game, rel, why='the game exited'), '0.5.0')
        self.assertEqual(self.state(), ('0.5.0', '0.4.0', [['--continue-update', '0.4.0']]))
        self.assertFalse(bg.waiting)

    def test_nothing_automatic_when_the_setting_is_off(self):
        """Install Updates Automatically off: the release is still staged in the background, and only selecting
        Updates installs it (also once selected during the download)."""
        ctx = self.context()
        ctx.settings['auto_install'] = False
        bg = self.background(ctx)
        rel = self.publish('0.5.0')
        self.assertEqual(self.stage(bg, rel), 'staged')
        for why in ('the launcher started', 'the game exited', 'the game closed', 'the download finished'):
            self.assertEqual(bg.install(self.game, rel, why=why), '')
        self.assertFalse(bg.waiting)
        self.assertEqual(self.state(), ('0.4.0', '', []))
        bg.asked = True                                      # selected while it downloaded
        self.assertEqual(bg.install(self.game, rel, why='the download finished'), '0.5.0')
        self.assertEqual(self.state(), ('0.5.0', '0.4.0', [['--continue-update', '0.4.0']]))

    def test_failed_download_waits_for_the_next_check(self):
        """A staging that fails is logged, never raised, leaves nothing to install, and is tried again only after the
        next update check."""
        ctx = self.context()
        bg = self.background(ctx)
        rel = self.publish('0.5.0', damaged=True)
        self.assertEqual(self.stage(bg, rel), 'failed')
        self.assertIn('0.5.0 was not staged', (self.layout.logs / 'launcher.log').read_text(encoding='utf-8'))
        self.assertIsNone(bg.poll(rel))
        self.assertIsNone(bg.ready(rel))
        self.assertEqual(bg.install(self.game, rel), '')
        bg.checked()
        self.assertEqual(self.stage(bg, rel), 'failed')
        self.assertEqual(self.state(), ('0.4.0', '', []))


# ---------------------------------------------------------------- gamefiles

def sarc(files):
    names = b''
    offs = []
    for n in files:
        offs.append(len(names))
        b = n.encode() + b'\0'
        names += b + b'\0' * (-len(b) % 4)
    data, ents = b'', b''
    for (n, d), no in zip(files.items(), offs):
        start = len(data)
        data += d
        ents += struct.pack('<IIII', 0, 0x01000000 | (no // 4), start, len(data))
        data += b'\0' * (-len(data) % 8)
    sfat = b'SFAT' + struct.pack('<HHI', 0xC, len(files), 0x65) + ents
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
    base = 0x14 + len(sfat) + len(sfnt)
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, base + len(data), base, 0x100, 0)
    return head + sfat + sfnt + data


def bfotf(otf, key=0x1234ABCD):
    body = otf + b'\0' * (-len(otf) % 4)
    words = struct.unpack(f'>{len(body) // 4}I', body)
    return struct.pack('>II', 0x7F9A0218 ^ key, len(otf) ^ key) + struct.pack(f'>{len(words)}I',
                                                                             *(w ^ key for w in words))


@unittest.skipUnless(ZSTD, 'needs compression.zstd (Python 3.14)')
class FontTest(Tmp):
    FONT = Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')

    @unittest.skipUnless(FONT.is_file(), 'needs DejaVu Sans')
    def test_font_from_romfs_image(self):
        otf = self.FONT.read_bytes()
        self.assertEqual(gamefiles.bfotf_to_otf(bfotf(otf)), otf)
        self.assertIsNone(gamefiles.bfotf_to_otf(b'\0' * 64))
        samples = [otf[i:i + 512] for i in range(0, min(len(otf), 400000), 997)]
        d = ZSTD.train_dict(samples, 16384)
        packed = ZSTD.compress(bfotf(otf), zstd_dict=d)
        self.assertNotEqual(ZSTD.get_frame_info(packed).dictionary_id, 0)
        files = {'Pack/ZsDic.pack.zs': ZSTD.compress(sarc({'zs.zsdic': d.dict_content})),
                 gamefiles.FONTS['title']: packed}
        romfs = self.tmp / 'game' / 'romfs'
        romfs.mkdir(parents=True)
        entries, blob = [], b''
        for path, data in sorted(files.items()):
            blob += b'\0' * (-len(blob) % 16)
            entries.append({'path': path, 'offset': len(blob), 'size': len(data)})
            blob += data
        (romfs / 'romfs.bin').write_bytes(blob)
        (romfs / 'index.json').write_text(json.dumps({'files': entries}))
        got = gamefiles.extract_fonts(self.layout, self.tmp / 'game')
        self.assertEqual(got['title'][1], 'DejaVu Sans')
        self.assertEqual(Path(got['title'][0]).read_bytes(), otf)
        self.assertEqual(gamefiles.cached_fonts(self.layout)['title'][1], 'DejaVu Sans')


# ---------------------------------------------------------------- the button art of a release

class PromptArtTest(Tmp):
    def make_art(self):
        """Every picture tools/prompt_art.py names, the pads in one pack's folder and the keys in another's, with two
        keys and two pictures the tool never reads. Returns (prompt_art, pads, keys, the files a release takes)."""
        import prompt_art as pa
        pads, keys = self.tmp / 'pack' / 'Source', self.tmp / 'keys' / 'Source'
        wanted = {rel for style in pa.STYLES.values() for part in style.values() for rel in part.values()
                  if rel != pa.OPTIONS}
        wanted |= {pa.SHARE, *pa.BLANKS.values(), f'{pa.KM}Dark/T_E_Key_Dark.png', f'{pa.KM}White/T_E_Key_White.png',
                   f'{pa.KM}Dark/T_F12_Key_Dark.png'}
        for rel in wanted | {'XGamepad/Default/T_X_Unused.png', f'{pa.KM}Retro/T_E_Key_Retro.png'}:
            path = (keys if rel.startswith(pa.KM) else pads) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(rel.encode())
        return pa, pads, keys, wanted

    def test_art_files(self):
        """A release takes the pictures pad_prompts.py reads and every key, from folders that hold the art's sets or
        are those sets; a set named twice, a set missing or a picture missing stops it."""
        pa, pads, keys, wanted = self.make_art()
        for folders in ([pads, keys], [pads, keys / 'Keyboard_Mouse'], [pads / 'XGamepad', pads / 'P5Gamepad', keys]):
            self.assertEqual(set(pa.art_files(folders)), wanted)
        dest = self.tmp / 'release' / pa.FOLDER
        n, size = pa.copy_art([pads, keys], dest)
        copied = {p.relative_to(dest).as_posix(): p.read_bytes() for p in dest.rglob('*') if p.is_file()}
        self.assertEqual(copied, {rel: rel.encode() for rel in wanted})
        self.assertEqual((n, size), (len(wanted), sum(len(rel) for rel in wanted)))
        # a set named itself wins over one inside a folder named; the same set twice at one rank is refused
        other = self.tmp / 'other' / 'Keyboard_Mouse'
        other.mkdir(parents=True)
        self.assertEqual(pa.art_roots([pads, keys, other])['Keyboard_Mouse'], other.parent)
        self.assertEqual(pa.art_roots([pads, keys])['Keyboard_Mouse'], keys)
        for folders, says in (([pads, keys, other.parent], 'in both'),
                              ([pads, keys / 'Keyboard_Mouse', other], 'in both'),
                              ([pads], 'no Keyboard_Mouse folder'), ([self.tmp / 'release'], 'holds none of')):
            with self.assertRaises(SystemExit) as e:
                pa.art_files(folders)
            self.assertIn(says, str(e.exception.code))
        (pads / pa.SHARE).unlink()
        with self.assertRaises(SystemExit) as e:
            pa.art_files([pads, keys])
        self.assertIn('T_X_Share-1.png', str(e.exception.code))

    def test_release_carries_what_the_prompts_need(self):
        """The release's Python has every package tools/pad_prompts.py imports, and the launcher release and the build
        kit carry every tool it imports."""
        import ast
        import make_kit
        import make_launcher
        pip = {'PIL': 'pillow', 'fontTools': 'fonttools'}
        local, packages = set(), set()
        for tool in ('pad_prompts.py', 'button_icons.py', 'prompt_art.py'):
            for node in ast.walk(ast.parse((ROOT / 'tools' / tool).read_text(encoding='utf-8'))):
                names = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                    [node.module] if isinstance(node, ast.ImportFrom) and node.module else []
                for root in (n.split('.')[0] for n in names):
                    if (ROOT / 'tools' / f'{root}.py').is_file():
                        local.add(f'{root}.py')
                    elif root not in sys.stdlib_module_names:
                        packages.add(pip.get(root, root))
        self.assertEqual(packages, {'zstandard', 'pillow', 'fonttools'})
        reqs = (ROOT / 'tools' / 'requirements-launcher.txt').read_text(encoding='utf-8')
        pinned = {ln.split('==')[0].strip().lower() for ln in reqs.splitlines()
                  if '==' in ln and not ln.startswith('#')}
        self.assertLessEqual(packages, pinned)
        for tools in (make_launcher.TOOLS, make_kit.TOOLS):
            self.assertLessEqual(local | {'pad_prompts.py'}, set(tools))


# ---------------------------------------------------------------- make_game.py

def nso(build_id=b'\0' * 16, size=0x300):
    head = bytearray(0x100)
    head[0:4] = b'NSO0'
    segs = [(0x100 + i * size, i * 0x10000, size) for i in range(3)]
    for i, (fo, mo, sz) in enumerate(segs):
        struct.pack_into('<III', head, 0x10 + i * 0x10, fo, mo, sz)
        struct.pack_into('<I', head, 0x60 + i * 4, sz)
    head[0x40:0x50] = build_id
    return bytes(head) + b''.join(bytes([i + 1]) * size for i in range(3))


def write_nup(path, exefs, romfs, title_id=0x0400486024DEC000):
    """A .nup (tools/nup.py's format) with one base package: exefs {name: bytes}, romfs {path: bytes}."""
    import nup as nupmod
    pool = bytearray()

    def name(s):
        off = len(pool)
        pool.extend(s.encode())
        return off, len(s.encode())

    dirs, files = [], []

    def add_tree(tree):
        """tree: {name: bytes | dict}; returns the directory index."""
        idx = len(dirs)
        dirs.append(None)
        kids = []
        for n, v in sorted(tree.items()):
            if isinstance(v, dict):
                kids.append(('d', add_tree(v), n))
            else:
                files.append([n, v])
                kids.append(('f', len(files) - 1, n))
        dirs[idx] = kids
        return idx

    root = add_tree({'exefs': exefs, 'romfs': _nest(romfs)})
    nd = len(dirs)
    dir_rows, child_list = [], []
    for kids in dirs:
        first = len(child_list)
        for kind, i, _n in kids:
            child_list.append(i if kind == 'd' else nd + i)
        dir_rows.append((first, len(kids)))
    dir_names = {root: ''}
    for kids in dirs:
        for kind, i, n in kids:
            if kind == 'd':
                dir_names[i] = n
    exefs_dir = next(i for k, i, n in dirs[root] if k == 'd' and n == 'exefs')
    romfs_dir = next(i for k, i, n in dirs[root] if k == 'd' and n == 'romfs')
    toc_head = 0x30 + nupmod.PACKAGE.size + nd * nupmod.DIRECTORY.size + len(files) * nupmod.FILE.size + \
        4 * len(child_list)
    blob_off = nupmod.SUPER.size
    data = bytearray()
    file_rows = []
    for n, v in files:
        no, nl = name(n)
        file_rows.append((no, nl, len(v), blob_off + len(data), len(v), hashlib.sha256(v).digest(), 0))
        data.extend(v)
    dir_name_rows = [name(dir_names[i]) for i in range(nd)]
    toc_off = blob_off + len(data)
    toc = bytearray(struct.pack('<IIQQQQQ', 0x01434F54, 1, nd, len(files), toc_head, len(pool), 0))
    toc += nupmod.PACKAGE.pack(title_id, root, exefs_dir, romfs_dir, 0, 0, 0, 0, 0, 0, 0, b'\0' * 20)
    for i in range(nd):
        toc += nupmod.DIRECTORY.pack(*dir_name_rows[i], *dir_rows[i], 0)
    for no, nl, size, off, stored, digest, codec in file_rows:
        toc += nupmod.FILE.pack(no, nl, size, off, stored, digest, codec, 0, 0, b'\0' * 12)
    toc += struct.pack(f'<{len(child_list)}I', *child_list)
    toc += pool
    total = toc_off + len(toc)
    sup = nupmod.SUPER.pack(b'NUP\x01', 0x80, 1, toc_off, len(toc), hashlib.sha256(bytes(toc)).digest(), blob_off,
                            total, 1, 0, b'\0' * 16, b'\0' * 32)
    Path(path).write_bytes(sup + bytes(data) + bytes(toc))


def _nest(flat):
    tree = {}
    for p, v in flat.items():
        parts = p.split('/')
        node = tree
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = v
    return tree


# Stands in for windres.exe: records its arguments, working directory and temporary folder, opens the .rc and the
# files it names from the working directory as windres does, and leaves two temporary files behind as windres does.
FAKE_WINDRES = r'''
import json, os, re, sys
from pathlib import Path
a = sys.argv[1:]
tmp = os.environ['TMP']
for n in ('cc1', 'cc2'):
    open(os.path.join(tmp, n), 'w').close()
Path(__file__).with_suffix('.json').write_text(json.dumps(
    {'argv': a, 'cwd': os.getcwd(), 'tmp': tmp, 'temp': os.environ['TEMP']}))
if os.environ.get('FAKE_WINDRES_FAIL'):
    sys.exit('windres: preprocessing failed.')
text = open(a[a.index('-i') + 1]).read()
for name in re.findall(r'^\s*\w+\s+\w+\s+"([^"]+)"', text, re.M):
    open(name, 'rb').close()
open(a[a.index('-o') + 1], 'wb').write(b'coff')
'''

# Stands in for tools/pad_prompts.py: counts its runs in the game folder and writes every overlay folder's manifest,
# printing the line the real tool prints for each and its closing report.
FAKE_PAD_PROMPTS = r'''
import json, sys
from pathlib import Path
import prompt_art
a = sys.argv[1:]
art, game = Path(a[a.index('--art') + 1]), Path(a[a.index('--game') + 1])
assert (art / 'XGamepad').is_dir(), art
with open(game / 'prompt-runs.txt', 'a') as f:
    f.write('run\n')
for name in prompt_art.OVERLAYS:
    (game / name).mkdir(exist_ok=True)
    (game / name / 'manifest.tsv').write_text('# path\n')
    print(f'{game / name}: 3 files, the font grew by 12 bytes', flush=True)
print(json.dumps({'status': 'pass', 'folders': []}))
'''


class MakeGameTest(Tmp):
    def setUp(self):
        super().setUp()
        import make_game
        self.mg = make_game
        exefs = {'main': nso(bytes.fromhex(make_game.MAIN_BUILD_ID)), 'sdk': nso(), 'subsdk0': nso(), 'rtld': nso(),
                 'main.npdm': b'META' + b'\0' * 0x100}
        self.romfs = {'a.txt': b'hello', 'dir/b.bin': os.urandom(5000), 'dir/sub/c': b'c' * 33}
        self.nup = self.tmp / 'game.nup'
        write_nup(self.nup, exefs, self.romfs)
        self.msys = self.tmp / 'msys'
        for need in ('bin/clang.exe', 'bin/clang++.exe', 'bin/SDL3.dll', 'lib/libSDL3.dll.a'):
            (self.msys / need).parent.mkdir(parents=True, exist_ok=True)
            (self.msys / need).write_bytes(b'')
        (self.msys / 'include' / 'vma').mkdir(parents=True)
        self.layout.pydeps.mkdir(parents=True, exist_ok=True)
        (self.layout.pydeps / 'zbic.py').write_text('def decompress(b):\n    return b\n')
        self.out = self.tmp / 'game out'

    def run_mg(self, *args):
        events, lines = [], []
        run = build.Run(build.command(self.layout, list(args)), events.append, lines.append)
        code = run.wait()
        return code, events, lines

    def test_pipeline_rebuild_and_keys(self):
        code, ev, lines = self.run_mg('--nup', self.nup, '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual(code, 0, '\n'.join(lines))
        states = [(e['step'], e['state']) for e in ev if e['ev'] == 'step']
        self.assertEqual(states, [('check', 'start'), ('check', 'done'), ('exefs', 'start'), ('exefs', 'done'),
                                  ('romfs', 'start'), ('romfs', 'done')])
        self.assertTrue(any(e['ev'] == 'progress' and e['step'] == 'romfs' and e['done'] == e['total'] for e in ev))
        index = json.loads((self.out / 'romfs' / 'index.json').read_text())
        blob = (self.out / 'romfs' / 'romfs.bin').read_bytes()
        for e in index['files']:
            self.assertEqual(blob[e['offset']:e['offset'] + e['size']], self.romfs[e['path']])
            self.assertEqual(e['offset'] % 16, 0)
        flat = (self.out / 'exefs' / 'main').read_bytes()
        self.assertEqual(flat[0x40:0x50].hex(), self.mg.MAIN_BUILD_ID)
        # a second run with the same input skips both steps
        code, ev, _ = self.run_mg('--nup', self.nup, '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual([e['state'] for e in ev if e['ev'] == 'step' and e['step'] in ('exefs', 'romfs')],
                         ['skip', 'skip'])
        # --rebuild needs no game files: it checks the unpacked ExeFS and keeps both steps
        self.nup.rename(self.tmp / 'moved.nup')
        code, ev, lines = self.run_mg('--rebuild', '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual(code, 0, '\n'.join(lines))
        self.assertEqual([e['state'] for e in ev if e['ev'] == 'step' and e['step'] in ('exefs', 'romfs')],
                         ['skip', 'skip'])
        # the exefs and romfs keys do not depend on the kit; the lift key does
        st = json.loads((self.out / 'build-cache' / 'state.json').read_text())
        self.assertEqual(st['steps']['exefs'], self.mg.step_key('exefs', st['input']))
        self.assertNotIn(self.mg.kit_version(), st['steps']['romfs'])

    def test_rebuild_without_build(self):
        code, ev, lines = self.run_mg('--rebuild', '--out', self.tmp / 'empty', '--msys', self.msys)
        self.assertEqual(code, 1)
        self.assertTrue(any(e['ev'] == 'error' and 'rebuild' in e['message'] for e in ev))

    def test_wrong_version_refused(self):
        bad = self.tmp / 'bad.nup'
        write_nup(bad, {'main': nso(b'\x11' * 16), 'sdk': nso(), 'subsdk0': nso(), 'rtld': nso(),
                        'main.npdm': b'META' + b'\0' * 0x100}, {'a': b'a'})
        code, ev, _ = self.run_mg('--nup', bad, '--check')
        self.assertEqual(code, 1)
        self.assertTrue(any(e['ev'] == 'error' and '1.1.1' in e['message'] for e in ev))

    @unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
    def test_object_cache_survives_a_moved_kit(self):
        """The launcher keeps each version in its own folder: a kit at a new place reuses every object."""
        kits = []
        for name in ('0.3.10', '0.3.11'):
            kit = self.tmp / 'app' / name / 'runtime'
            kit.mkdir(parents=True)
            (kit / 'h.h').write_text('#define V 1\n')
            (kit / 'a.c').write_text('#include "h.h"\nint a(void) { return V; }\n')
            kits.append(kit.parent)
        obj = self.tmp / 'obj'
        obj.mkdir()

        def go(kit):
            rt = kit / 'runtime'
            srcs = [(rt / 'a.c', obj / 'rt_a.o')]
            cache = self.mg.ObjectCache(obj / 'objects.json', {'<kit>': kit})
            return self.mg.compile_all(srcs, lambda s_, d: ['cc', '-c', '-I', str(rt), '-o', str(d), str(s_)], cache,
                                       1, dict(os.environ), salt='cc')
        self.assertEqual(go(kits[0])[:2], (1, 0))
        self.assertEqual(go(kits[1])[:2], (0, 1))
        (kits[1] / 'runtime' / 'h.h').write_text('#define V 2\n')        # the new version changed the header
        self.assertEqual(go(kits[1])[:2], (1, 0))

    def main_with(self, romfs, lift, compile_):
        """make_game.main() in this process for a first build, with the given step functions in place of the real
        lift and compile (they need real modules and clang). Returns the SystemExit code, or 0."""
        sys.path.insert(0, str(self.layout.pydeps))   # the fake zbic
        saved = (self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile, sys.argv)
        self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile = romfs, lift, compile_
        sys.argv = ['make_game.py', '--nup', str(self.nup), '--out', str(self.out), '--msys', str(self.msys),
                    '--until', 'compile']   # the shaders step needs the shader translator
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.mg.main()
            return 0
        except SystemExit as e:
            return e.code
        finally:
            self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile, sys.argv = saved
            sys.path.remove(str(self.layout.pydeps))

    def test_romfs_beside_lift_and_compile(self):
        """A first build packs the RomFS image while the lift and the compile run; the compile gets the running step
        and waits for it only for the RomFS index."""
        real_romfs, seen = self.mg.step_romfs, {}

        def romfs(src, out, jobs):
            time.sleep(0.5)
            real_romfs(src, out, jobs)

        def lift(out, cache):
            seen['lift_beside_romfs'] = not (out / 'romfs' / 'index.json').exists()

        def compile_(out, cache, msys, jobs, romfs_job=None, update=False):
            seen['job'] = romfs_job is not None
            romfs_job.join()
            seen['image_after_join'] = (out / 'romfs' / 'romfs.bin').is_file()
            (out / self.mg.EXE_NAME).write_bytes(b'MZ')
        self.assertEqual(self.main_with(romfs, lift, compile_), 0)
        self.assertEqual(seen, {'lift_beside_romfs': True, 'job': True, 'image_after_join': True})
        steps = json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps']
        self.assertEqual(set(steps), {'exefs', 'romfs', 'lift', 'compile'})

    def test_shader_progress_reaches_the_launcher(self):
        """The shaders step turns nx2navi.exe's counts into progress (translation the first half of the bar, the
        driver build the second), sends one every few seconds while nothing is printed, says when the counts stop
        moving, and passes its other lines on to the log."""
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'bin').mkdir()
        self.out.mkdir()
        for name in (self.mg.XLATE_NAME, 'nx2navi_xlate.id'):
            (kit / 'bin' / name).write_bytes(b'current translator')
            (self.out / name).write_bytes(b'old translator')
        (kit / 'tools' / 'prepare_shaders.py').write_text(
            'import time\n'
            'print("reading the inventory", flush=True)\n'
            'time.sleep(0.6)\n'
            'for d in (0, 50, 100):\n'
            '    print(f"prepare-shaders: progress translate {d} 100", flush=True)\n'
            '    time.sleep(0.05)\n'
            'print("prepare-shaders: progress build 10 100", flush=True)\n'
            'time.sleep(0.6)\n'
            'print("prepare-shaders: progress build 100 100", flush=True)\n')
        for name, value in (('KIT', kit), ('EVENTS', True), ('SHADER_BEAT_S', 0.1), ('SHADER_STALL_S', 0.3)):
            self.addCleanup(setattr, self.mg, name, getattr(self.mg, name))
            setattr(self.mg, name, value)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out)
        for name in (self.mg.XLATE_NAME, 'nx2navi_xlate.id'):
            self.assertEqual((self.out / name).read_bytes(), b'current translator')
        lines = buf.getvalue().splitlines()
        events = [json.loads(ln[2:]) for ln in lines if ln.startswith('@@')]
        self.assertIn('reading the inventory', lines)
        self.assertFalse([ln for ln in lines if ln.startswith('prepare-shaders: progress')])
        self.assertTrue(all(e['ev'] == 'progress' and e['step'] == 'shaders' for e in events))
        fractions = [e['fraction'] for e in events]
        self.assertEqual(fractions, sorted(fractions))
        self.assertEqual(fractions[-1], 1.0)
        details = [e['detail'] for e in events]
        self.assertTrue(any(d.startswith("Reading the game's shaders") for d in details))
        self.assertIn('Translated 50 of 100 shaders', details)
        self.assertGreaterEqual(sum(1 for e in events if e['fraction'] == 0.55), 2)   # heartbeats while the build waits
        self.assertTrue(any(d.startswith('Built 10 of 100') and 'still running' in d for d in details))
        self.assertEqual([e['fraction'] for e in events if e['detail'] == 'Translated 50 of 100 shaders'][0], 0.25)

    def test_shaders_step_packs_the_cache(self):
        """After the preparation the shaders step folds the loose files of the shader cache into shader-cache.pack
        (tools/pack_shader_cache.py), as the installer did; a kit without the tool leaves the cache as it is."""
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'tools' / 'prepare_shaders.py').write_text('print("prepare-shaders: progress build 1 1", flush=True)\n')
        cache = self.out / 'shader-cache'
        (cache / 'in').mkdir(parents=True)
        (cache / ('ab' * 32 + '.spv')).write_bytes(b'\x03\x02\x23\x07' * 8)
        (cache / 'in' / ('cd' * 32 + '.in')).write_bytes(b'TKSI' * 4)
        self.addCleanup(setattr, self.mg, 'KIT', self.mg.KIT)
        self.mg.KIT = kit
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out)
        self.assertFalse((cache / 'shader-cache.pack').exists())   # no tool in the kit
        shutil.copy(ROOT / 'tools' / 'pack_shader_cache.py', kit / 'tools')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out)
        self.assertTrue((cache / 'shader-cache.pack').is_file())
        self.assertEqual(sorted(p.name for p in cache.iterdir()), ['shader-cache.pack'])
        self.assertIn('shader cache packed', buf.getvalue())

    def test_carried_update_packs_many_loose_shaders(self):
        """An update that carries the shaders over packs the cache once it holds PACK_LOOSE_MIN loose translations,
        and leaves fewer loose."""
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        shutil.copy(ROOT / 'tools' / 'pack_shader_cache.py', kit / 'tools')
        cache = self.out / 'shader-cache'
        cache.mkdir(parents=True)
        for i in range(3):
            (cache / (f'{i:02x}' * 32 + '.spv')).write_bytes(b'\x03\x02\x23\x07' * 8)
        for name, value in (('KIT', kit), ('PACK_LOOSE_MIN', 4), ('shader_carry', lambda out: True)):
            self.addCleanup(setattr, self.mg, name, getattr(self.mg, name))
            setattr(self.mg, name, value)
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out, update=True)
        self.assertFalse((cache / 'shader-cache.pack').exists())   # 3 loose, under the minimum
        (cache / ('03' * 32 + '.spv')).write_bytes(b'\x03\x02\x23\x07' * 8)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(sorted(p.name for p in cache.iterdir()), ['shader-cache.pack'])
        self.assertIn('stay in use', buf.getvalue())
        self.assertIn('shader cache packed', buf.getvalue())

    def test_carried_update_warms_the_drivers_cache(self):
        """An update that carries the shaders over runs the game's --warm-shaders, in the game folder; a run that
        fails is said and does not stop the build, and without a built game or a preparation nothing runs."""
        calls = []
        for name, value in (('shader_carry', lambda out: True),
                            ('shader_run', lambda cmd, cwd=None: calls.append((cmd, cwd)) or self.rc)):
            self.addCleanup(setattr, self.mg, name, getattr(self.mg, name))
            setattr(self.mg, name, value)
        self.rc = 0
        self.out.mkdir(parents=True, exist_ok=True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(calls, [])                           # no exe, no prepared list
        (self.out / self.mg.EXE_NAME).write_bytes(b'MZ')
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(calls, [])                           # never prepared: nothing to build again
        (self.out / 'shader-prepared.bin').write_bytes(bytes(16))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(calls, [([str(self.out / self.mg.EXE_NAME), '--warm-shaders'], str(self.out))])
        self.assertIn("driver's cache warm", buf.getvalue())
        self.rc = 3
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out, update=True)       # no exception: the game warms it itself
        self.assertIn('did not finish (3)', buf.getvalue())
        self.assertEqual(len(calls), 2)

    def test_romfs_failure_stops_the_build(self):
        ran, lifting, failed = [], threading.Event(), threading.Event()

        def romfs(src, out, jobs):
            lifting.wait(5)
            failed.set()
            self.mg.fail('the RomFS image could not be written')

        def lift(out, cache):
            lifting.set()
            failed.wait(5)    # the RomFS step fails while the lift runs
            time.sleep(0.1)
            ran.append('lift')

        def compile_(out, cache, msys, jobs, romfs_job=None, update=False):
            ran.append('compile')
        self.assertEqual(self.main_with(romfs, lift, compile_), 1)
        self.assertEqual(ran, ['lift'])
        steps = json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps']
        self.assertNotIn('romfs', steps)

    def test_shaders_key_names_the_translator_the_build_uses(self):
        """A source checkout has its translator in build/xlate/ (tools/build_xlate.py), not in the kit's bin/: the
        shaders key follows its identity, so a rebuilt translator prepares the shaders again. A kit's bin/ comes first
        when both exist, and a kit with no translator is refused with the way to build one."""
        from unittest import mock
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'tools' / 'prepare_shaders.py').write_text('')
        xlate = kit / 'build' / 'xlate'
        xlate.mkdir(parents=True)
        (xlate / self.mg.XLATE_NAME).write_bytes(b'')
        (xlate / 'nx2navi_xlate.id').write_text('xlate 1\n')

        def key():
            return self.mg.step_key('shaders', 'game', self.out)
        with mock.patch.object(self.mg, 'KIT', kit):
            first = key()
            (xlate / 'nx2navi_xlate.id').write_text('xlate 2\n')
            self.assertNotEqual(key(), first)
            (kit / 'bin').mkdir()
            (kit / 'bin' / self.mg.XLATE_NAME).write_bytes(b'')
            (kit / 'bin' / 'nx2navi_xlate.id').write_text('xlate 3\n')
            from_bin = key()
            (xlate / 'nx2navi_xlate.id').write_text('xlate 4\n')
            self.assertEqual(key(), from_bin)
            shutil.rmtree(kit / 'bin')
            shutil.rmtree(xlate)
            with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
                key()
            self.assertIn('tools/build_xlate.py', err.getvalue())

    def test_prompts_step(self):
        """With button art in the kit, the last step draws the button prompts (a fake tools/pad_prompts.py), one
        progress event per folder. It runs again only when the art, the bind_ lines or a folder changed, and then no
        other step runs; a step before it that runs does not make it run; a kit without art skips it."""
        from unittest import mock
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'bin').mkdir()
        (kit / 'bin' / self.mg.XLATE_NAME).write_bytes(b'')   # the translator the shaders key names (xlate_source)
        (kit / 'bin' / 'nx2navi_xlate.id').write_text('xlate 1\n')
        (kit / 'tools' / 'prepare_shaders.py').write_text('')
        for name in ('prompt_art.py', 'button_icons.py'):
            shutil.copy2(ROOT / 'tools' / name, kit / 'tools' / name)
        (kit / 'tools' / 'pad_prompts.py').write_text(FAKE_PAD_PROMPTS)
        art = kit / 'prompt-art' / 'XGamepad' / 'Default'
        art.mkdir(parents=True)
        (art / 'T_X_A_White.png').write_bytes(b'picture one')
        ran = []

        def fake(name):
            def step(*args, **kw):
                ran.append(name)
            return step

        def build():
            """One build; (the other steps that ran, the prompts step's events, the prompt tool's runs so far)."""
            ran.clear()
            buf = io.StringIO()
            argv = ['make_game.py', '--nup', str(self.nup), '--out', str(self.out), '--msys', str(self.msys),
                    '--events']
            with mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(buf), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.mg.main()
            events = [json.loads(ln[2:]) for ln in buf.getvalue().splitlines() if ln.startswith('@@')]
            log = self.out / 'prompt-runs.txt'
            runs = log.read_text().count('run') if log.exists() else 0
            return sorted(ran), [e for e in events if e.get('step') == 'prompts'], runs

        def states(events):
            return [e['state'] for e in events if e['ev'] == 'step']

        sys.path.insert(0, str(self.layout.pydeps))   # the fake zbic
        self.addCleanup(sys.path.remove, str(self.layout.pydeps))
        with mock.patch.multiple(self.mg, KIT=kit, PROMPT_PACKAGES={}, EVENTS=False, step_romfs=fake('romfs'),
                                 step_lift=fake('lift'), step_compile=fake('compile'), step_scan=fake('scan'), step_shaders=fake('shaders')):
            others, ev, runs = build()
            self.assertEqual((others, runs), (['compile', 'lift', 'romfs', 'scan', 'shaders'], 1))
            self.assertEqual(states(ev), ['start', 'done'])
            fractions = [e['fraction'] for e in ev if e['ev'] == 'progress']
            self.assertEqual(fractions, [0.0, 0.2, 0.4, 0.6, 0.8, 1.0])
            details = [e['detail'] for e in ev if e['ev'] == 'progress']
            self.assertEqual(details[-1], 'Drew 5 of 5 sets of button prompts')
            self.assertIn('prompts', json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps'])
            self.assertEqual(build(), ([], [{'ev': 'step', 'step': 'prompts', 'state': 'skip'}], 1))
            # a setting that is no key binding changes nothing; a binding draws the prompts again, and only them
            (self.out / 'nx2navi.ini').write_text('vsync = on\n')
            self.assertEqual(build()[2], 1)
            (self.out / 'nx2navi.ini').write_text('vsync = on\nbind_a = key:f   # jump\n')
            others, ev, runs = build()
            self.assertEqual((others, states(ev), runs), ([], ['start', 'done'], 2))
            # a folder gone, or new art: drawn again
            shutil.rmtree(self.out / 'romfs-patches-pc')
            self.assertEqual(build()[2], 3)
            (art / 'T_X_B_White.png').write_bytes(b'picture two')
            self.assertEqual(build()[2], 4)
            # the lift running again makes the compile and the shaders run, not the prompts
            st = json.loads((self.out / 'build-cache' / 'state.json').read_text())
            del st['steps']['lift']
            (self.out / 'build-cache' / 'state.json').write_text(json.dumps(st))
            others, ev, runs = build()
            self.assertEqual((others, states(ev), runs), (['compile', 'lift', 'shaders'], ['skip'], 4))
            # a kit without art: skipped, the folders an earlier build drew stay
            shutil.rmtree(kit / 'prompt-art')
            self.assertEqual(build(), ([], [{'ev': 'step', 'step': 'prompts', 'state': 'skip'}], 4))
            self.assertTrue((self.out / 'romfs-patches-xbox' / 'manifest.tsv').is_file())

    @unittest.skipIf(os.name == 'nt', 'uses a POSIX script as the compiler')
    def test_largest_source_compiles_first(self):
        src, obj = self.tmp / 'src', self.tmp / 'obj'
        src.mkdir()
        obj.mkdir()
        for name, size in (('small.c', 10), ('big.c', 5000), ('mid.c', 800)):
            (src / name).write_text('/*' + 'x' * size + '*/\n')
        order = self.tmp / 'order.txt'
        cc = self.tmp / 'fakecc'
        cc.write_text(f'#!{sys.executable}\nimport sys\nargs = sys.argv[1:]\n'
                      'dep, out, s = args[args.index("-MF") + 1], args[args.index("-o") + 1], args[-1]\n'
                      f'open({str(order)!r}, "a").write(s.rsplit("/", 1)[-1] + "\\n")\n'
                      'open(out, "w").close(); open(dep, "w").write(out + ": " + s + "\\n")\n')
        cc.chmod(0o755)
        srcs = [(src / n, obj / (n[:-2] + '.o')) for n in ('small.c', 'big.c', 'mid.c')]
        cache = self.mg.ObjectCache(obj / 'objects.json')
        compiled, reused, bad = self.mg.compile_all(srcs, lambda s, d: [str(cc), '-c', '-o', str(d), str(s)], cache,
                                                    1, dict(os.environ))
        self.assertEqual((compiled, bad), (3, []))
        self.assertEqual(order.read_text().split(), ['big.c', 'mid.c', 'small.c'])

    def test_runtime_split(self):
        tainted = self.mg.per_install_names()
        self.assertTrue({'layout.h', 'patches.h', 'romfs_config.h'} <= tainted)
        names = {p.name: self.mg.per_install(p, tainted) for p, _ in self.mg.runtime_sources()}
        for f in ('loader.c', 'memory.c', 'romfs.c', 'patches.c'):
            self.assertTrue(names[f], f)
        self.assertFalse(names['vk_device.c'])
        self.assertLess(sum(names.values()), len(names) // 4)          # most of the runtime can come prebuilt

    def test_parse_deps(self):
        text = 'C:/o/x.o: C:/src/x.c C:/inc/a\\ b.h \\\n  C:/inc/c.h\n'
        self.assertEqual(self.mg.parse_deps(text), ['C:/src/x.c', 'C:/inc/a b.h', 'C:/inc/c.h'])

    def test_copy_if_changed_equal_metadata(self):
        # Windows has no POSIX executable mode and cannot replace read-only destinations.
        modes = (0o666,) if os.name == 'nt' else (0o444, 0o555)
        for mode in modes:
            with self.subTest(mode=oct(mode)):
                src, dst = self.tmp / f'source-{mode}.id', self.tmp / f'installed-{mode}.id'
                for path, data in ((src, b'7' * 64 + b'\n'), (dst, b'1' * 64 + b'\n')):
                    path.write_bytes(data)
                    os.utime(path, (1, 1))  # Build artifacts can share a normalized modification time.
                    path.chmod(mode)
                self.assertEqual(src.stat().st_size, dst.stat().st_size)
                self.assertEqual(src.stat().st_mtime_ns, dst.stat().st_mtime_ns)
                self.assertTrue(self.mg.copy_if_changed(src, dst))
                self.assertEqual(dst.read_bytes(), src.read_bytes())
                self.assertEqual(dst.stat().st_mode & 0o777, src.stat().st_mode & 0o777)
                self.assertFalse(self.mg.copy_if_changed(src, dst))

    @unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
    def test_object_cache(self):
        src = self.tmp / 'src'
        src.mkdir()
        (src / 'h.h').write_text('#define V 1\n')
        (src / 'a.c').write_text('#include "h.h"\nint a(void) { return V; }\n')
        (src / 'b.c').write_text('int b(void) { return 2; }\n')
        obj = self.tmp / 'obj'
        obj.mkdir()
        srcs = [(src / 'a.c', obj / 'a.o'), (src / 'b.c', obj / 'b.o')]

        def cmd_for(s, d):
            return ['cc', '-c', '-I', str(src), '-o', str(d), str(s)]

        def go():
            cache = self.mg.ObjectCache(obj / 'objects.json')
            return self.mg.compile_all(srcs, cmd_for, cache, 2, dict(os.environ), salt='cc')
        self.assertEqual(go()[:2], (2, 0))
        self.assertEqual(go()[:2], (0, 2))
        (src / 'h.h').write_text('#define V 3\n')                         # a header: only a.c includes it
        self.assertEqual(go()[:2], (1, 1))
        (src / 'b.c').write_text('int b(void) { return syntax error }\n')
        compiled, reused, bad = go()
        self.assertEqual((compiled, len(bad)), (1, 1))
        self.assertNotIn('b.o', json.loads((obj / 'objects.json').read_text()))

    def test_dlss_beside_the_game(self):
        """The kit's nvngx_dlss.dll and its notice go beside the game; a newer kit replaces the copy an earlier build
        placed, and a DLSS the player put there is kept."""
        import dlss_pin
        from unittest import mock
        kit, out = self.tmp / 'kit', self.tmp / 'game'
        (kit / 'bin').mkdir(parents=True)
        out.mkdir()

        def kit_dll(data, when):
            (kit / 'bin' / 'nvngx_dlss.dll').write_bytes(data)
            os.utime(kit / 'bin' / 'nvngx_dlss.dll', (when, when))
            (kit / 'bin' / 'nvngx_dlss-NOTICE.txt').write_text(f'NVIDIA DLSS\n\nsha256 {hashlib.sha256(data).hexdigest()}\n')

        dll, note = out / 'nvngx_dlss.dll', out / 'nvngx_dlss-NOTICE.txt'
        with mock.patch.object(self.mg, 'KIT', kit):
            self.assertIsNone(self.mg.place_dlss(out))   # a kit without DLSS: nothing
            self.assertFalse(dll.exists())
            kit_dll(b'dlss one', 1_700_000_000)
            self.assertIsNone(self.mg.place_dlss(out))
            self.assertEqual(dll.read_bytes(), b'dlss one')
            self.assertTrue(note.is_file())
            self.assertIsNone(self.mg.place_dlss(out))   # unchanged
            kit_dll(b'dlss two', 1_700_100_000)          # a newer kit: the copy the last build placed goes
            self.assertIsNone(self.mg.place_dlss(out))
            self.assertEqual(dll.read_bytes(), b'dlss two')
            self.assertIn(hashlib.sha256(b'dlss two').hexdigest(), note.read_text())
            dll.write_bytes(b"the player's own DLSS")    # another version the player put there: kept
            self.assertIn('kept', self.mg.place_dlss(out))
            self.assertEqual(dll.read_bytes(), b"the player's own DLSS")
        # the notice the release scripts write is the one make_game reads
        (self.tmp / 'n.txt').write_text(dlss_pin.notice_text(), encoding='utf-8')
        self.assertEqual(self.mg.notice_sha256(self.tmp / 'n.txt'), dlss_pin.SHA256)
        self.assertEqual((dlss_pin.NAME, dlss_pin.NOTICE), (self.mg.DLSS_DLL, self.mg.DLSS_NOTICE))

    def test_compile_key_ignores_file_time_and_sysroot(self):
        """The compiler's id leaves its file time out (a tool update that puts the same compiler back keeps every
        object), and the key leaves --sysroot out (the fast compiler's objects are the UCRT64 compiler's)."""
        exe = self.tmp / 'cc.exe'
        shutil.copy2(sys.executable, exe)
        a = self.mg.compiler_ids(exe, dict(os.environ))
        os.utime(exe, (1_700_000_000, 1_700_000_000))
        b = self.mg.compiler_ids(exe, dict(os.environ))
        self.assertEqual(a[0], b[0])
        self.assertNotEqual(a[1], b[1])   # the form up to 0.6.0 had it
        src, dst = self.tmp / 'a.c', self.tmp / 'a.o'
        src.write_text('int a;')
        cache = self.mg.ObjectCache(self.tmp / 'objects.json', {'<kit>': self.tmp / 'kit'})
        plain = ['clang.exe', '-c', '-O1', '-o', str(dst), str(src)]
        fast = ['clang.exe', '--sysroot=C:/msys64/ucrt64', '-c', '-O1', '-o', str(dst), str(src)]
        self.assertEqual(self.mg.object_key(plain, src, dst, cache, 'id'), self.mg.object_key(fast, src, dst, cache, 'id'))
        self.assertNotEqual(self.mg.object_key(plain, src, dst, cache, 'id', sysroot=True),
                            self.mg.object_key(fast, src, dst, cache, 'id', sysroot=True))

    def test_objects_of_an_earlier_version_are_taken_over(self):
        """An entry under a key an earlier version gave the same compile is re-recorded under the new key when its
        files read as they did; a header in the earlier kit's folder is read in this kit's (that folder may be gone)."""
        old, new, lift, obj = self.tmp / 'app' / '0.5.0', self.tmp / 'app' / '0.6.1', self.tmp / 'lift', self.tmp / 'obj'
        for d in (new / 'runtime', lift, obj):
            d.mkdir(parents=True)
        (new / 'runtime' / 'cpu.h').write_text('#define CPU 1\n')
        src, dst = lift / 'main_0000.c', obj / 'main_main_0000.o'
        src.write_text('int f(void) { return 1; }\n')
        dst.write_bytes(b'object')
        sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
        legacy = {'key': 'old-key', 'deps': {str(src).replace('\\', '/'): sha(src),
                                             str(old / 'runtime' / 'cpu.h').replace('\\', '/'): sha(new / 'runtime' / 'cpu.h')}}
        (obj / 'objects.json').write_text(json.dumps({dst.name: legacy}))
        cache = self.mg.ObjectCache(obj / 'objects.json', {'<kit>': new})
        self.assertEqual(cache.adopt(dst, 'new-key', ['other-key'], [old]), 0)   # not a key it knows
        self.assertEqual(cache.adopt(dst, 'new-key', ['old-key'], [old]), 1)
        e = cache.entries[dst.name]
        self.assertEqual(e['key'], 'new-key')
        self.assertIn('<kit>/runtime/cpu.h', e['deps'])
        self.assertTrue(cache.current(dst, 'new-key'))
        # a header that changed since: compiled again
        (obj / 'objects.json').write_text(json.dumps({dst.name: legacy}))
        (new / 'runtime' / 'cpu.h').write_text('#define CPU 2\n')
        cache = self.mg.ObjectCache(obj / 'objects.json', {'<kit>': new})
        self.assertEqual(cache.adopt(dst, 'new-key', ['old-key'], [old]), 0)

    def test_lifted_level_kept_on_updates(self):
        obj = self.tmp / 'obj'
        obj.mkdir()
        self.assertEqual(self.mg.lifted_level(obj, 3, update=False), 3)   # a build from the game files
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 3)    # nothing recorded, no old cache
        (obj / 'objects.json').write_text(json.dumps({'main_main_0000.o': {'key': 'k', 'deps': {'a.c': 'ab'}}}))
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 0)    # 0.5.0's cache: compiled for no level
        # 0.5.0's runtime objects left in a cache whose lifted code a later version compiled: not an old cache
        (obj / 'objects.json').write_text(json.dumps({
            'main_main_0000.o': {'key': 'k', 'deps': {'a.c': ['ab', 1, 2]}}, 'rt_amiibo.o': {'key': 'k', 'deps': {'b.c': 'cd'}}}))
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 3)
        cache = self.mg.ObjectCache(obj / 'objects.json')
        self.assertEqual(cache.keep_only({'main_main_0000.o'}), 1)        # what this build no longer makes
        self.assertEqual(list(cache.entries), ['main_main_0000.o'])
        (obj / 'objects.json').write_text(json.dumps({'main_main_0000.o': {'key': 'k', 'deps': {'a.c': 'ab'}}}))
        (obj / self.mg.LIFTED_BUILD).write_text(json.dumps({'level': 2}))
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 2)    # as recorded
        self.assertEqual(self.mg.lifted_level(obj, 1, update=True), 1)    # never above this processor's
        self.assertEqual(self.mg.lifted_level(obj, 3, update=False), 3)

    def test_retune_due_when_the_level_differs(self):
        obj = self.tmp / 'obj'
        obj.mkdir()
        with mock.patch.object(self.mg, 'cpu_level', lambda clang, env: 3),                 mock.patch.object(self.mg, 'toolchain', lambda msys: (None, None, {}, None)):
            self.assertTrue(self.mg.retune_due(obj, None))                     # nothing recorded
            (obj / self.mg.LIFTED_BUILD).write_text(json.dumps({'level': 0}))
            self.assertTrue(self.mg.retune_due(obj, None))                     # generic objects on a v3 processor
            (obj / self.mg.LIFTED_BUILD).write_text(json.dumps({'level': 3}))
            self.assertFalse(self.mg.retune_due(obj, None))

    def test_updates_skip_shader_preparation(self):
        """An update leaves the shaders to the game (shader_carry) only when they were prepared and the cache names the
        translator they were made with: without that the game has nothing to carry them over by."""
        out = self.tmp / 'game'
        (out / 'shader-cache').mkdir(parents=True)
        self.assertFalse(self.mg.shader_carry(out))   # nothing prepared yet: the update prepares
        (out / 'shader-prepared.bin').write_bytes(b'x')
        self.assertFalse(self.mg.shader_carry(out))   # prepared, but the cache names no translator
        (out / 'shader-cache' / 'identities.txt').write_text('# translators\n' + 'ab' * 32 + '\n')
        self.assertTrue(self.mg.shader_carry(out))
        (out / 'nx2navi.ini').write_text('[performance]\nNX2NAVI_SHADER_CARRY = "0"\n')
        self.assertFalse(self.mg.shader_carry(out))

    def test_cache_from_before_carrying_names_its_translator(self):
        """Before a new translator goes beside the game, a shader cache with no identities.txt gets one naming the
        translator still there, in the form runtime/gfx_shader.c reads (ids_parse): '#' lines, then 64 hex digits."""
        out = self.tmp / 'game'
        out.mkdir()
        old = 'B72C60033CE90FC475EF5B4D397555DDA9A52AD0FDF480B8A4E292ED925A6DE7'
        (out / 'nx2navi_xlate.id').write_text(old + '\r\n')
        self.mg.note_cache_translator(out)   # no cache: nothing to name
        self.assertFalse((out / 'shader-cache' / 'identities.txt').exists())
        (out / 'shader-cache').mkdir()
        self.mg.note_cache_translator(out)
        ids = out / 'shader-cache' / 'identities.txt'
        lines = ids.read_bytes().decode('ascii').split('\n')
        self.assertTrue(lines[0].startswith('#'))
        self.assertEqual(lines[1], old.lower())
        self.assertEqual(self.mg.cache_identities(out), [old.lower()])
        (out / 'nx2navi_xlate.id').write_text('cd' * 32)   # a file the game keeps is never written over
        self.mg.note_cache_translator(out)
        self.assertEqual(self.mg.cache_identities(out), [old.lower()])
        ids.unlink()
        (out / 'nx2navi_xlate.id').write_text('not an identity')
        self.mg.note_cache_translator(out)
        self.assertFalse(ids.exists())

    def test_dlssg_beside_the_game(self):
        """DLSS Frame Generation's files go beside the game only as a whole set, with the licence texts; the release
        script (tools/make_launcher.py) and the build agree on the set."""
        import streamline_pin
        from unittest import mock
        kit, out = self.tmp / 'kit', self.tmp / 'game'
        src = kit / 'bin' / 'dlssg'
        src.mkdir(parents=True)
        out.mkdir()
        self.assertEqual(streamline_pin.FILES, self.mg.DLSSG_FILES)
        with mock.patch.object(self.mg, 'KIT', kit):
            for n in self.mg.DLSSG_FILES[:-1]:
                (src / n).write_bytes(n.encode())
            self.assertEqual(self.mg.place_dlssg(out), 0)   # nvngx_dlssg.dll missing: nothing is placed
            self.assertFalse((out / 'nx2navi_dlssg.dll').exists())
            (src / 'nvngx_dlssg.dll').write_bytes(b'ngx')
            (src / 'streamline-LICENSE.txt').write_text('MIT')
            self.assertEqual(self.mg.place_dlssg(out), len(self.mg.DLSSG_FILES) + 1)
            for n in self.mg.DLSSG_FILES:
                self.assertEqual((out / n).read_bytes(), (src / n).read_bytes())
            self.assertEqual((out / 'streamline-LICENSE.txt').read_text(), 'MIT')

    def test_streamline_pin(self):
        """The Streamline files come out of the SDK archive checked one by one; the archive goes once they are out and
        a filled cache needs no download; an SDK folder on disk must hold the pinned files; bin/dlssg/ gets the set
        make_game.py places, the two licence texts and a notice with every file's sha256."""
        import streamline_pin
        from unittest import mock
        files = {n: f'{n} of the SDK'.encode() for n in streamline_pin.SDK_FILES}
        pins = {n: hashlib.sha256(d).hexdigest() for n, d in files.items()}
        cache = self.tmp / 'dl'
        cache.mkdir()
        archive = cache / f'streamline-sdk-v{streamline_pin.VERSION}.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            for n, d in files.items():
                z.writestr(n, d)
            z.writestr('bin/x64/sl.nis.dll', b'a plugin the game does not load')
        fetched = []

        def download(url, dest, sha256=None, size=None):   # the archive is already where net.download writes it
            fetched.append(url)
            self.assertEqual(Path(dest), archive)
            return dest

        with mock.patch.dict(streamline_pin.SDK_FILES, pins), mock.patch('launcher.net.download', download):
            sdk = streamline_pin.resolve(None, cache)
            self.assertEqual(fetched, [streamline_pin.URL])
            self.assertFalse(archive.exists())
            self.assertEqual(streamline_pin.wrong_files(sdk), [])
            self.assertFalse((sdk / 'bin' / 'x64' / 'sl.nis.dll').exists())
            self.assertEqual(streamline_pin.resolve(None, cache), sdk)   # filled: no second download
            self.assertEqual(len(fetched), 1)
            self.assertEqual(streamline_pin.resolve(sdk, cache), sdk)    # a folder on disk with the pinned files
            (sdk / 'bin' / 'x64' / 'sl.common.dll').write_bytes(b'another Streamline')
            with self.assertRaises(SystemExit):
                streamline_pin.resolve(sdk, cache)
            (sdk / 'bin' / 'x64' / 'sl.common.dll').write_bytes(files['bin/x64/sl.common.dll'])
            with zipfile.ZipFile(archive, 'w') as z:                     # an archive whose file differs is refused
                for n, d in files.items():
                    z.writestr(n, b'changed' if n == 'license.txt' else d)
            with self.assertRaises(SystemExit):
                streamline_pin.extract(archive, self.tmp / 'x')
            dll = self.tmp / 'nx2navi_dlssg.dll'
            dll.write_bytes(b'our frame generation shim')
            dest = self.tmp / 'bin' / 'dlssg'
            streamline_pin.place(dll, sdk, dest)
        self.assertEqual(sorted(p.name for p in dest.iterdir()),
                         sorted((*streamline_pin.FILES, *streamline_pin.TEXTS, streamline_pin.NOTICE)))
        self.assertEqual((dest / 'streamline-LICENSE.txt').read_bytes(), files['license.txt'])
        note = (dest / streamline_pin.NOTICE).read_text(encoding='utf-8')
        self.assertIn(f'Streamline {streamline_pin.VERSION} SDK', note)
        self.assertIn(f'nx2navi_dlssg.dll sha256 {hashlib.sha256(dll.read_bytes()).hexdigest()}', note)
        self.assertIn(f'nvngx_dlssg.dll sha256 {pins["bin/x64/nvngx_dlssg.dll"]}', note)

    @unittest.skipIf(os.name == 'nt', 'a script stands in for windres.exe')
    def test_compile_resources(self):
        # The toolchain, the kit and the game under folders with spaces, as under C:\Users\Ali Sayed. windres must
        # start gcc itself (--use-temp-file) on a line that names no path but gcc's: the .rc by its bare name from
        # runtime/, no -I, -D or -U.
        msys, obj = self.tmp / 'tool chain' / 'ucrt64', self.out / 'build-cache' / 'obj'
        windres = msys / 'bin' / 'windres.exe'
        windres.parent.mkdir(parents=True)
        windres.write_text(f'#!{sys.executable}\n' + FAKE_WINDRES)
        windres.chmod(0o755)
        obj.mkdir(parents=True)
        rt = self.tmp / 'the kit' / 'runtime'
        rt.mkdir(parents=True)
        for name in ('nx2navi.rc', 'nx2navi.manifest', 'nx2navi.ico'):
            shutil.copy2(ROOT / 'runtime' / name, rt / name)
        rco = self.mg.compile_resources(msys, rt, obj, dict(os.environ))
        self.assertEqual((rco, rco.read_bytes()), (obj / 'nx2navi_rc.o', b'coff'))
        seen = json.loads(windres.with_suffix('.json').read_text())
        args = seen['argv']
        self.assertEqual(seen['cwd'], str(rt))
        self.assertIn('--use-temp-file', args)
        self.assertIn(f'--preprocessor={msys / "bin" / "gcc.exe"}', args)
        self.assertEqual([a for a in args if a.startswith('--preprocessor-arg=')],
                         ['--preprocessor-arg=-E', '--preprocessor-arg=-xc', '--preprocessor-arg=-DRC_INVOKED'])
        self.assertEqual(args[args.index('-i') + 1], 'nx2navi.rc')
        self.assertFalse([a for a in args if a.startswith(('-I', '--include-dir', '-D', '-U'))])
        # the temporary files went to a folder of the build cache, which is gone with the two windres left there
        self.assertEqual(seen['tmp'], seen['temp'])
        self.assertEqual(Path(seen['tmp']).parent, obj)
        self.assertEqual(sorted(p.name for p in obj.iterdir()), ['nx2navi_rc.o'])
        # the same resource again: the object keeps its time, so the link key does not change and nothing relinks
        before = rco.stat().st_mtime_ns
        time.sleep(0.02)
        self.mg.compile_resources(msys, rt, obj, dict(os.environ))
        self.assertEqual(rco.stat().st_mtime_ns, before)
        # a failing windres stops the build with its message
        err = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(err):
            self.mg.compile_resources(msys, rt, obj, dict(os.environ, FAKE_WINDRES_FAIL='1'))
        self.assertIn('windres failed on runtime/nx2navi.rc: windres: preprocessing failed.', err.getvalue())
        self.assertEqual(sorted(p.name for p in obj.iterdir()), ['nx2navi_rc.o'])


# ---------------------------------------------------------------- jobs

FAKE_MAKE_GAME = r'''
import json, sys, time
def ev(**k): print('@@' + json.dumps(k), flush=True)
if '--fail' in open(sys.argv[0] + '.mode').read():
    ev(ev='step', step='check', state='start'); ev(ev='error', message='this is not game version 1.1.1'); sys.exit(1)
for s in ('check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts'):
    ev(ev='step', step=s, state='start')
    if s == 'compile':
        ev(ev='progress', step=s, done=5, total=10, unit='files'); ev(ev='phase', step=s, phase='link')
    print('working on', s, flush=True)
    ev(ev='step', step=s, state='done', seconds=0.1)
ev(ev='end', exe='x')
'''


class JobTest(Tmp):
    def make_ctx(self, mode):
        app = self.tmp / 'app'
        (app / 'tools').mkdir(parents=True)
        (app / 'VERSION').write_text('0.3.10\n')
        script = app / 'tools' / 'make_game.py'
        script.write_text(FAKE_MAKE_GAME)
        (app / 'tools' / 'make_game.py.mode').write_text(mode)
        ctx = Context(Layout(root=self.tmp / 'root', app=app))
        return ctx

    def patch_tools(self):
        saved = toolchain.load_lock, toolchain.is_current, toolchain.ensure_zbic
        toolchain.load_lock = lambda layout, version='dev': {'packages': []}
        toolchain.is_current = lambda layout, lock: True
        toolchain.ensure_zbic = lambda *a, **k: None
        self.addCleanup(lambda: setattr(toolchain, 'load_lock', saved[0]) or
                        setattr(toolchain, 'is_current', saved[1]) or setattr(toolchain, 'ensure_zbic', saved[2]))

    def test_install_job(self):
        self.patch_tools()
        ctx = self.make_ctx('ok')
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'ok', j.error)
        self.assertEqual([s.state for s in j.steps], [jobs.SKIPPED] + [jobs.DONE] * 6 + [jobs.SKIPPED, jobs.DONE])   # tools ... shaders, scan (the fake build has no such step), ready
        self.assertEqual(j.step('check').value, 'Version 1.1.1')
        self.assertTrue(any('working on lift' in ln for ln in j.lines))
        self.assertEqual(j.overall(), 1.0)
        self.assertIsNone(j.step('prompts'))   # a version without button art draws no prompts: no row

    def test_install_job_with_button_art(self):
        """A version with button art shows the prompts step as a row of its own, after the shaders."""
        self.patch_tools()
        ctx = self.make_ctx('ok')
        (ctx.layout.app / 'prompt-art').mkdir()
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'ok', j.error)
        self.assertEqual([s.id for s in j.steps][-4:], ['shaders', 'prompts', 'scan', 'ready'])
        self.assertEqual(j.step('prompts').state, jobs.DONE)
        self.assertEqual([s.id for s in jobs.RebuildJob(ctx, self.tmp / 'game').steps][-3:], ['shaders', 'prompts', 'scan'])

    def test_failed_job(self):
        self.patch_tools()
        ctx = self.make_ctx('--fail')
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'failed')
        self.assertIn('1.1.1', j.error)
        self.assertEqual(j.step('check').state, jobs.FAILED)


# ---------------------------------------------------------------- the icon

class SavesTest(unittest.TestCase):
    """launcher/saves.py: where the game keeps its save, the save folders it recognises, and the import."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-saves-'))
        self.game = self.tmp / 'game'
        self.game.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def files(self, folder, names):
        for n, data in names.items():
            (folder / n).parent.mkdir(parents=True, exist_ok=True)
            (folder / n).write_bytes(data)
        return folder

    def test_account_folder_as_the_game_names_it(self):
        (self.game / 'profile').mkdir()
        lo, hi = 0x1122334455667788, 0x99aabbccddeeff00
        (self.game / 'profile' / 'account.uid').write_bytes(lo.to_bytes(8, 'little') + hi.to_bytes(8, 'little'))
        self.assertEqual(saves.account_dir(self.game).name, f'{hi:016x}{lo:016x}')   # fs_host.c storage_init
        fresh = self.tmp / 'fresh'
        acct = saves.account_dir(fresh)                       # a game that never ran: the account is made
        self.assertEqual(len((fresh / 'profile' / 'account.uid').read_bytes()), 16)
        self.assertEqual(saves.account_dir(fresh), acct)

    def test_save_folders_found(self):
        plain = self.files(self.tmp / 'backup', {'save.dat': b'1', 'option.dat': b'2'})
        self.assertEqual(saves.find_save(plain)[0], plain)
        emu = self.files(self.tmp / 'emu', {'0000000000000000/' + 'a' * 32 + '/0100000000010000/save.dat': b'1'})
        self.assertEqual(saves.find_save(emu)[0].name, '0100000000010000')
        other = self.files(self.tmp / 'other', {'profile/save/' + 'b' * 32 + '/save.dat': b'1', 'ocarina.exe': b''})
        self.assertEqual(saves.find_save(other)[0].name, 'b' * 32)
        self.assertEqual(saves.find_save(other / 'profile')[0].name, 'b' * 32)
        (self.tmp / 'empty').mkdir()
        self.assertIsNone(saves.find_save(self.tmp / 'empty')[0])

    def test_import_backs_up_and_clears_the_journal(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-3/save.dat': b'newer', 'work/save.dat': b'w'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported', 'sub/extra.bin': b'x'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertEqual(r['files'], 2)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'imported')
        self.assertEqual((acct / 'sub' / 'extra.bin').read_bytes(), b'x')
        self.assertEqual(list(tx.iterdir()), [])                       # the game loads the imported save
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'newer')   # the save the game would have loaded
        self.assertTrue((self.game / 'saves' / 'README.txt').is_file())
        self.assertTrue((src / 'save.dat').is_file())                  # a picked folder is left as it was

    def test_one_slot_replaces_only_that_slot(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'account.sav': b'a', 'option.sav': b'o', 'slot_00/progress.sav': b'old0',
                          'slot_01/progress.sav': b'old1', 'slot_01/extra.sav': b'e'})
        src = self.files(self.tmp / 'slot_01', {'slot_01/progress.sav': b'new1', 'slot_01/caption.sav': b'c1'})   # nested
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertEqual(r['slot'], 'slot_01')
        self.assertEqual((acct / 'slot_01' / 'progress.sav').read_bytes(), b'new1')
        self.assertEqual((acct / 'slot_01' / 'caption.sav').read_bytes(), b'c1')
        self.assertFalse((acct / 'slot_01' / 'extra.sav').exists())    # the slot is replaced, not mixed
        self.assertFalse((acct / 'progress.sav').exists())             # the game reads it only inside a slot
        for name, data in (('slot_00/progress.sav', b'old0'), ('account.sav', b'a'), ('option.sav', b'o')):
            self.assertEqual((acct / name).read_bytes(), data)
        self.assertEqual((r['backup'] / 'slot_01' / 'progress.sav').read_bytes(), b'old1')

    def test_a_slot_under_another_name_is_the_first_slot(self):
        src = self.files(self.tmp / 'my save', {'progress.sav': b'p', 'caption.sav': b'c'})
        r = saves.import_save(self.game, src, log=lambda m: None)      # a game that never ran has no save to keep
        acct = saves.account_dir(self.game)
        self.assertEqual(r['slot'], 'slot_00')
        self.assertIsNone(r['backup'])
        self.assertEqual((acct / 'slot_00' / 'progress.sav').read_bytes(), b'p')

    def test_a_slot_clears_a_slot_left_at_the_top_of_the_save(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'progress.sav': b'stray', 'caption.sav': b'stray', 'option.sav': b'o'})
        src = self.files(self.tmp / 'slot_02', {'progress.sav': b'p'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertFalse((acct / 'progress.sav').exists())
        self.assertFalse((acct / 'caption.sav').exists())
        self.assertEqual((acct / 'option.sav').read_bytes(), b'o')
        self.assertEqual((acct / 'slot_02' / 'progress.sav').read_bytes(), b'p')
        self.assertEqual((r['backup'] / 'progress.sav').read_bytes(), b'stray')

    def test_a_whole_save_replaces_the_save(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'slot_00/progress.sav': b'old', 'slot_03/progress.sav': b'old3', 'option.sav': b'o'})
        src = self.files(self.tmp / 'whole', {'account.sav': b'a', 'option.sav': b'n', 'slot_00/progress.sav': b'new'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertIsNone(r['slot'])
        self.assertEqual((acct / 'slot_00' / 'progress.sav').read_bytes(), b'new')
        self.assertEqual((acct / 'option.sav').read_bytes(), b'n')
        self.assertFalse((acct / 'slot_03').exists())

    def test_a_backup_comes_back_without_its_journal(self):
        src = self.files(self.tmp / 'backup 2026-11-09 05.42.06', {'option.sav': b'o', 'slot_00/progress.sav': b'p',
                                                                   'save-tx/work/option.sav': b'w'})
        saves.import_save(self.game, src, log=lambda m: None)
        acct = saves.account_dir(self.game)
        self.assertEqual((acct / 'slot_00' / 'progress.sav').read_bytes(), b'p')
        self.assertFalse((acct / 'save-tx').exists())

    def test_import_from_the_saves_folder(self):
        d = saves.ensure_saves_dir(self.game)
        self.assertIsNone(saves.waiting(self.game)[0])                 # README.txt alone is no save
        self.files(d, {'emu/0000000000000000/' + 'c' * 32 + '/0100000000010000/save.dat': b'emu'})
        self.assertIsNotNone(saves.waiting(self.game)[0])
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertIsNone(r['backup'])                                 # there was no save yet
        self.assertEqual((saves.account_dir(self.game) / 'save.dat').read_bytes(), b'emu')
        left = sorted(e.name for e in d.iterdir())
        self.assertEqual(left[0], 'README.txt')
        self.assertTrue(left[1].startswith('imported '))
        self.assertIsNone(saves.waiting(self.game)[0])                 # not imported twice
        self.files(d, {'save.dat': b'loose'})                          # files put straight into saves/
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertEqual((saves.account_dir(self.game) / 'save.dat').read_bytes(), b'loose')
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'emu')
        self.assertIsNone(saves.waiting(self.game)[0])

    def test_picking_the_saves_folder_takes_what_waits_there(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'mine'})
        d = saves.ensure_saves_dir(self.game)
        with self.assertRaises(saves.SaveError):                      # README.txt alone is no save
            saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'mine')
        self.files(d, {'save.dat': b'put there'})
        r = saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual(sorted(p.name for p in acct.iterdir()), ['save.dat'])   # not README.txt or a backup
        self.assertEqual((acct / 'save.dat').read_bytes(), b'put there')
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'mine')

    def test_an_interrupted_commit_is_backed_up(self):
        # runtime/fs_host.c save_recover: with no account folder and no finished commit, save-tx/new is the save, and
        # without it save-tx/old; the import keeps that only copy
        for half in ('new', 'old'):
            with self.subTest(half=half):
                shutil.rmtree(self.game, ignore_errors=True)
                self.game.mkdir()
                acct = saves.account_dir(self.game)
                tx = self.game / 'profile' / 'save-tx'
                self.files(tx, {f'{half}/save.dat': b'only copy'})
                src = self.files(self.tmp / f'mine-{half}', {'save.dat': b'imported'})
                r = saves.import_save(self.game, src, log=lambda m: None)
                self.assertEqual((acct / 'save.dat').read_bytes(), b'imported')
                self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'only copy')
                self.assertEqual((r['backup'] / 'save-tx' / half / 'save.dat').read_bytes(), b'only copy')

    def test_a_failed_swap_keeps_the_old_save(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        real = saves.os.replace

        def replace(a, b):
            if Path(a).name == 'import':   # the imported save going into place: antivirus holds a file
                raise PermissionError(5, 'Access is denied')
            return real(a, b)
        saves.os.replace = replace
        try:
            with self.assertRaises(PermissionError):
                saves.import_save(self.game, src, log=lambda m: None)
        finally:
            saves.os.replace = real
        self.assertEqual((acct / 'save.dat').read_bytes(), b'old')

    def test_a_failed_swap_keeps_the_finished_commit(self):
        # the account folder cannot be moved aside (a file in it held open): the newer finished commit stays in
        # save-tx, where the game loads it first, instead of going into the backup
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'older'})
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-4/save.dat': b'newest'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        real = saves.os.replace

        def replace(a, b):
            if Path(a) == acct:
                raise PermissionError(32, 'The process cannot access the file')
            return real(a, b)
        saves.os.replace = replace
        try:
            with self.assertRaises(PermissionError):
                saves.import_save(self.game, src, log=lambda m: None)
        finally:
            saves.os.replace = real
        self.assertEqual((tx / 'ready-4' / 'save.dat').read_bytes(), b'newest')
        self.assertEqual((acct / 'save.dat').read_bytes(), b'older')
        self.assertFalse((tx / 'import').exists())

    @unittest.skipUnless(os.name == 'nt', 'the running check is Windows only')
    def test_a_read_only_exe_is_not_a_running_game(self):
        exe = self.game / 'ocarina.exe'
        exe.write_bytes(b'MZ')
        os.chmod(exe, stat.S_IREAD)
        self.assertFalse(build.game_running(self.game))
        self.assertFalse(saves.game_running(self.game))
        self.assertTrue(os.access(exe, os.W_OK))

    def test_read_only_files_import_writable(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        os.chmod(src / 'save.dat', stat.S_IREAD)
        os.chmod(acct / 'save.dat', stat.S_IREAD)   # the save it replaces: deleted after the swap all the same
        try:
            saves.import_save(self.game, src, log=lambda m: None)
            self.assertTrue(os.access(acct / 'save.dat', os.W_OK))
            self.assertEqual([p.name for p in (self.game / 'profile' / 'save-tx').iterdir()], [])
        finally:
            os.chmod(src / 'save.dat', stat.S_IREAD | stat.S_IWRITE)

    def test_another_installs_finished_commit_is_its_save(self):
        other = self.files(self.tmp / 'other', {'profile/save/' + 'b' * 32 + '/save.dat': b'older',
                                                'profile/save-tx/ready-2/save.dat': b'newest'})
        self.assertEqual(saves.find_save(other)[0].name, 'ready-2')

    def test_refusals(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'1'})
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, self.game / 'profile', log=lambda m: None)   # its own save
        big = self.tmp / 'big'
        big.mkdir()
        for i in range(saves.MAX_FILES + 1):
            (big / f'{i}.txt').write_bytes(b'')
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, big, log=lambda m: None)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'1')      # nothing changed


class SaveShareTest(unittest.TestCase):
    """launcher/saves.py export_save and import_zip: one zip with the save and a manifest, for a bug report or another
    player, and the checks an import makes before it replaces the save."""

    SAVE = {'account.sav': b'acct', 'option.sav': b'opt', 'slot_00/progress.sav': b'p0', 'slot_00/caption.sav': b'c0',
            'slot_02/progress.sav': b'p2'}

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-share-'))
        self.game = self.tmp / 'game'
        self.out = self.tmp / 'out'
        self.out.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def files(self, folder, names):
        for n, data in names.items():
            (folder / n).parent.mkdir(parents=True, exist_ok=True)
            (folder / n).write_bytes(data)
        return folder

    def game_with_save(self, game, names=None):
        self.files(saves.account_dir(game), names or self.SAVE)
        return game

    def zip_of(self, members, manifest=None):
        """A zip with members {name: bytes} and a manifest (export_save's keys unless given; None: none)."""
        path = self.tmp / f'made-{len(list(self.tmp.glob("made-*")))}.zip'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            if manifest is not False:
                m = manifest or {'format': saves.FORMAT, 'format_version': 1, 'title_id': '0400486024dec000',
                                 'game_version': '1.1.1', 'nx2navi': '0.6.1', 'exported': '2026-11-09T17:30:05Z'}
                z.writestr(saves.MANIFEST, json.dumps(m))
            for n, data in members.items():
                z.writestr(n, data)
        return path

    def test_export_writes_one_zip_of_the_newest_save(self):
        self.game_with_save(self.game)
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-7/option.sav': b'newer', 'ready-7/slot_00/progress.sav': b'p0 newer',
                        'work/option.sav': b'not committed'})
        self.files(self.game, {'logs/nx2navi.log': b'log', 'nx2navi.ini': b'[graphics]'})
        r = saves.export_save(self.game, self.out, version='0.6.1', log=lambda m: None)
        self.assertRegex(r['path'].name, r'^NX2navi-save-\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.zip$')
        self.assertEqual(r['extras'], [])
        self.assertEqual([p.name for p in self.out.iterdir()], [r['path'].name])   # no .part left behind
        with zipfile.ZipFile(r['path']) as z:
            names = sorted(z.namelist())
            self.assertEqual(names, ['manifest.json', 'save/option.sav', 'save/slot_00/progress.sav'])
            self.assertEqual(z.read('save/option.sav'), b'newer')   # the finished commit the game would load
            m = json.loads(z.read('manifest.json'))
        self.assertEqual((m['format'], m['format_version'], m['title_id'], m['game_version'], m['nx2navi'],
                          m['files'], m['bug_report']), (saves.FORMAT, 1, '0400486024dec000', '1.1.1', '0.6.1', 2,
                                                         False))
        self.assertRegex(m['exported'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
        r2 = saves.export_save(self.game, self.out, log=lambda m: None)   # the same second: a name of its own
        self.assertNotEqual(r2['path'], r['path'])

    def test_a_bug_report_adds_the_log_and_settings_and_never_keys(self):
        self.game_with_save(self.game, dict(self.SAVE, **{'prod.keys': b'secret', 'sub/title.keys': b'secret'}))
        self.files(self.game, {'logs/nx2navi.log': b'the log', 'logs/nx2navi.1.log': b'older', 'nx2navi.ini': b'[x]',
                               'prod.keys': b'secret'})
        r = saves.export_save(self.game, self.out, bug_report=True, log=lambda m: None)
        self.assertTrue(r['path'].name.endswith('-bug-report.zip'))
        self.assertEqual(r['extras'], ['nx2navi.log', 'nx2navi.ini'])
        with zipfile.ZipFile(r['path']) as z:
            names = z.namelist()
            self.assertEqual(z.read('bug-report/nx2navi.log'), b'the log')
            self.assertEqual(z.read('bug-report/nx2navi.ini'), b'[x]')
            self.assertTrue(json.loads(z.read('manifest.json'))['bug_report'])
            self.assertFalse(any(b'secret' in z.read(n) for n in names))
        self.assertFalse([n for n in names if 'keys' in n])
        self.assertEqual(len([n for n in names if n.startswith('save/')]), len(self.SAVE))

    def test_no_save_no_export(self):
        with self.assertRaises(saves.SaveError):
            saves.export_save(self.game, self.out, log=lambda m: None)   # a game that never ran
        self.assertFalse((self.game / 'profile' / 'account.uid').exists())   # and the export made no account
        saves.account_dir(self.game).mkdir(parents=True)
        with self.assertRaises(saves.SaveError):
            saves.export_save(self.game, self.out, log=lambda m: None)   # an account without files
        self.assertEqual(list(self.out.iterdir()), [])

    def test_a_shared_save_replaces_the_save_and_keeps_a_backup(self):
        other = self.game_with_save(self.tmp / 'other')
        shared = saves.export_save(other, self.out, bug_report=True, version='0.6.1', log=lambda m: None)['path']
        self.game_with_save(self.game, {'option.sav': b'mine', 'slot_05/progress.sav': b'mine5'})
        acct = saves.account_dir(self.game)
        self.files(self.game / 'profile' / 'save-tx', {'ready-2/option.sav': b'mine newer'})
        r = saves.import_save(self.game, shared, log=lambda m: None)
        self.assertEqual(r['manifest']['nx2navi'], '0.6.1')
        self.assertIsNone(r['slot'])
        got = {p.relative_to(acct).as_posix(): p.read_bytes() for p in acct.rglob('*') if p.is_file()}
        self.assertEqual(got, self.SAVE)                                # every slot: slot_05 is gone too
        self.assertFalse((acct / 'nx2navi.log').exists())               # the bug report files stay out
        self.assertEqual((r['backup'] / 'option.sav').read_bytes(), b'mine newer')
        self.assertEqual(r['backup'].parent, self.game / 'saves')
        self.assertEqual(list((self.game / 'profile' / 'save-tx').iterdir()), [])
        self.assertTrue(shared.is_file())                              # a picked zip is left where it was

    def test_a_zip_in_the_saves_folder_is_taken_first(self):
        self.game_with_save(self.game)
        shared = saves.export_save(self.game_with_save(self.tmp / 'other', {'option.sav': b'theirs'}), self.out,
                                   log=lambda m: None)['path']
        d = saves.ensure_saves_dir(self.game)
        self.files(d, {'loose.sav': b'x'})
        shutil.copy2(shared, d / shared.name)
        src, _ = saves.waiting(self.game)
        self.assertEqual(src, d / shared.name)
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertEqual((saves.account_dir(self.game) / 'option.sav').read_bytes(), b'theirs')
        self.assertFalse((saves.account_dir(self.game) / shared.name).exists())   # not copied in as a file
        done = [e for e in d.iterdir() if e.name.startswith('imported ')]
        self.assertEqual([p.name for p in done[0].iterdir()], [shared.name])   # moved out: not imported twice
        self.assertEqual(saves.waiting(self.game)[0], d)              # the loose file waits on its own
        self.assertIsNotNone(r['backup'])
        # picking the saves folder itself takes what waits there, as the row does
        shutil.copy2(shared, d / 'again.zip')
        r = saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual(r['source'].name, 'again.zip')

    def test_an_import_in_the_launcher_drops_the_game_menus_waiting_import(self):
        self.game_with_save(self.game)
        self.files(self.game / 'profile', {'save-import/option.sav': b'from the menu',
                                           'save-import.part/option.sav': b'half'})
        self.assertIsNotNone(saves.pending_import(self.game))
        shared = saves.export_save(self.game_with_save(self.tmp / 'other'), self.out, log=lambda m: None)['path']
        saves.import_save(self.game, shared, log=lambda m: None)
        self.assertIsNone(saves.pending_import(self.game))
        self.assertFalse((self.game / 'profile' / 'save-import.part').exists())

    def test_zips_that_are_refused(self):
        acct = saves.account_dir(self.game_with_save(self.game))
        good = {'save/option.sav': b'o', 'save/slot_00/progress.sav': b'p'}
        manifest = {'format': saves.FORMAT, 'format_version': 1, 'title_id': '0400486024dec000'}
        notzip = self.tmp / 'not.zip'
        notzip.write_bytes(b'PK not really')
        cases = {
            'not a zip': notzip,
            'no manifest': self.zip_of(good, manifest=False),
            'another format': self.zip_of(good, dict(manifest, format='something-else')),
            'a newer format': self.zip_of(good, dict(manifest, format_version=2)),
            'no format version': self.zip_of(good, {'format': saves.FORMAT, 'title_id': '0400486024dec000'}),
            'another game': self.zip_of(good, dict(manifest, title_id='0100000000010000')),
            'a path outside the save': self.zip_of(dict(good, **{'save/../../evil.sav': b'x'}), manifest),
            'an absolute path': self.zip_of(dict(good, **{'save//abs.sav': b'x'}), manifest),
            'a drive': self.zip_of(dict(good, **{'save/C:/evil.sav': b'x'}), manifest),
            'a backslash': self.zip_of(dict(good, **{'save/..\\evil.sav': b'x'}), manifest),
            'not this game': self.zip_of({'save/save.dat': b'x', 'save/user/0001.bin': b'y'}, manifest),
            'no save': self.zip_of({'bug-report/nx2navi.log': b'l'}, manifest),
        }
        for why, path in cases.items():
            with self.subTest(why=why):
                with self.assertRaises(saves.SaveError):
                    saves.import_save(self.game, path, log=lambda m: None)
                self.assertEqual((acct / 'option.sav').read_bytes(), self.SAVE['option.sav'])
        self.assertFalse((self.tmp / 'evil.sav').exists())
        self.assertFalse((self.game / 'saves').exists() and
                         [e for e in (self.game / 'saves').iterdir() if e.name.startswith('backup ')])

    def test_a_link_in_the_zip_is_refused(self):
        acct = saves.account_dir(self.game_with_save(self.game))
        path = self.zip_of({'save/option.sav': b'o'})
        with zipfile.ZipFile(path, 'a') as z:
            info = zipfile.ZipInfo('save/slot_00/progress.sav')
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(info, '../../../outside')
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, path, log=lambda m: None)
        self.assertEqual((acct / 'option.sav').read_bytes(), self.SAVE['option.sav'])

    def test_a_damaged_zip_changes_nothing(self):
        acct = saves.account_dir(self.game_with_save(self.game))
        path = self.zip_of({'save/option.sav': b'theirs' * 200, 'save/slot_00/progress.sav': b'p' * 300})
        data = bytearray(path.read_bytes())
        at = data.index(b'save/option.sav') + len('save/option.sav') + 4   # inside the first member's data
        data[at] ^= 0xff
        path.write_bytes(bytes(data))
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, path, log=lambda m: None)
        self.assertEqual((acct / 'option.sav').read_bytes(), self.SAVE['option.sav'])
        self.assertFalse((self.game / 'profile' / 'save-tx' / 'import').exists())

    def test_the_zip_of_the_games_menu_imports(self):
        # runtime/save_share.c writes the same layout with stored and deflated members; Python's reader takes both
        path = self.tmp / 'from-the-game.zip'
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr(saves.MANIFEST, json.dumps({'format': saves.FORMAT, 'format_version': 1, 'exported_by': 'game',
                                                   'title_id': '0400486024DEC000', 'game_version': '1.1.1'}))
            z.writestr('save/option.sav', b'stored', zipfile.ZIP_STORED)
            z.writestr('save/slot_01/progress.sav', b'deflated' * 50, zipfile.ZIP_DEFLATED)
        r = saves.import_save(self.game, path, log=lambda m: None)   # a game that never ran: no backup
        self.assertIsNone(r['backup'])
        acct = saves.account_dir(self.game)
        self.assertEqual((acct / 'slot_01' / 'progress.sav').read_bytes(), b'deflated' * 50)

    @unittest.skipUnless(os.name == 'nt', 'the shell\'s known folders are Windows only')
    def test_the_default_export_folder(self):
        d = saves.default_export_dir()
        self.assertTrue(d.is_dir())
        self.assertIn(d, (winsys.known_folder('downloads'), winsys.known_folder('desktop'), Path.home()))


class IconTest(unittest.TestCase):
    def test_navi(self):
        try:
            from launcher.ui import art
        except ImportError:
            self.skipTest('needs numpy')
        self.assertEqual(art.navi(2).shape, (42, 44, 4))
        data = art.navi_ico()
        count = struct.unpack_from('<H', data, 4)[0]
        sizes = [data[6 + 16 * i] or 256 for i in range(count)]
        self.assertEqual(sizes[0], 16)
        self.assertIn(256, sizes)
        off = struct.unpack_from('<I', data, 6 + 12)[0]
        self.assertEqual(struct.unpack_from('<Iii', data, off), (40, 16, 32))   # a bitmap entry, not PNG
        big = struct.unpack_from('<I', data, 6 + 16 * (count - 1) + 12)[0]
        self.assertEqual(data[big:big + 4], b'\x89PNG')


class LookTest(Tmp):
    """Customize Launcher: the colour schemes, the background picture and the sounds."""

    def setUp(self):
        super().setUp()
        try:
            from launcher.ui import art, picture, sound
        except ImportError:
            self.skipTest('needs numpy')
        self.art, self.picture, self.sound = art, picture, sound

    def test_every_colour_scheme_has_every_colour(self):
        art = self.art
        self.assertEqual(art.theme_palette('forest'), art.BASE)   # the first scheme is the colours as written
        for name, t in art.THEMES.items():
            pal = art.theme_palette(name)
            self.assertEqual(set(pal), set(art.BASE), name)
            for key, value in pal.items():
                self.assertRegex(value, r'^#[0-9a-f]{6}$', f'{name} {key}')
            self.assertTrue(t['label'])
        grouped = [k for keys in art.GROUPS.values() for k in keys]
        self.assertEqual(len(grouped), len(set(grouped)), 'a colour in two groups')
        self.assertLessEqual(set(grouped), set(art.BASE))

    def test_set_theme_changes_the_palette_in_place(self):
        art = self.art
        held = art.PALETTE
        try:
            self.assertEqual(art.set_theme('lake'), 'lake')
            self.assertIs(art.PALETTE, held)
            self.assertNotEqual(held['panel'], art.BASE['panel'])
            self.assertEqual(held['salmon'], art.BASE['salmon'])        # a warning stays a warning in every scheme
            self.assertEqual(art.set_theme('no such scheme'), 'forest')
            self.assertEqual(dict(held), art.BASE)
            self.assertEqual(art.set_theme(None), 'forest')             # launcher.json without the key
        finally:
            art.set_theme('forest')

    def test_recolor_keeps_light_and_dark(self):
        import colorsys
        art = self.art

        def hls(c):
            return colorsys.rgb_to_hls(*(int(c[i:i + 2], 16) / 255 for i in (1, 3, 5)))
        for c in ('#7a5c32', '#2a2019', '#d69127'):
            h, l, _s = hls(art.recolor(c, 200))
            self.assertAlmostEqual(h * 360, 200, delta=4)
            self.assertAlmostEqual(l, hls(c)[1], delta=0.01)
        self.assertEqual(art.recolor('#7a5c32', None), '#7a5c32')

    def test_a_picture_is_cut_to_the_windows_shape(self):
        import numpy as np
        pic = self.picture
        wide = np.zeros((100, 400, 3), dtype=np.uint8)
        wide[:, 150:250] = 255                                # the middle of a wide picture
        out = pic.cover(wide, 160, 100)
        self.assertEqual(out.shape, (100, 160, 3))
        self.assertGreater(out[:, 40:120].mean(), 200)        # its middle fills most of the result
        tall = np.zeros((400, 100, 3), dtype=np.uint8)
        tall[150:250] = 255
        self.assertEqual(pic.cover(tall, 160, 100).shape, (100, 160, 3))
        self.assertGreater(pic.cover(tall, 160, 100).mean(), 120)
        big = np.full((2048, 3200, 3), 77, dtype=np.uint8)   # shrunk to half: averaged, the grey stays
        self.assertTrue((pic.cover(big, 1600, 1024) == 77).all())

    def test_the_kept_background(self):
        import numpy as np
        art, pic = self.art, self.picture
        src = self.tmp / 'mine.png'
        img = np.zeros((90, 300, 4), dtype=np.float32)
        img[..., 0], img[..., 3] = 1.0, 1.0                  # red
        src.write_bytes(art.png_bytes(img))
        store = self.tmp / 'background.npy'
        if pic.Image is None and os.name != 'nt':
            self.skipTest('needs Pillow, or Tk with a display')
        fitted = pic.keep(src, store)
        self.assertEqual(fitted.shape, (pic.KEEP_H, pic.KEEP_W, 3))
        back = pic.kept(store, 1000, 640)
        self.assertEqual(back.shape, (640, 1000, 3))
        self.assertEqual(str(back.dtype), 'float32')
        self.assertAlmostEqual(float(back[..., 0].min()), 1.0, places=3)
        self.assertAlmostEqual(float(back[..., 1:].max()), 0.0, places=3)
        # the backdrop with it: the picture darkened by the dimming, solid everywhere
        plain = art.backdrop(200, 128, 1.0, pic.kept(store, 200, 128), 0.0)
        dimmed = art.backdrop(200, 128, 1.0, pic.kept(store, 200, 128), 0.8)
        self.assertEqual(plain.shape, (128, 200, 4))
        self.assertGreater(plain[64, 100, 0], 0.8)                   # the corners' shade reaches in a little
        self.assertLess(dimmed[64, 100, 0], 0.35)
        self.assertEqual(float(dimmed[..., 3].min()), 1.0)
        # what is not a kept background, and what is no picture
        self.assertIsNone(pic.kept(self.tmp / 'none.npy', 100, 64))
        store.write_bytes(b'not numpy')
        self.assertIsNone(pic.kept(store, 100, 64))
        text = self.tmp / 'notes.txt'
        text.write_text('no picture')
        with self.assertRaises(pic.PictureError):
            pic.keep(text, store)
        with self.assertRaises(pic.PictureError):
            pic.load(self.tmp / 'gone.png')

    def test_ppm(self):
        import numpy as np
        px = bytes(range(18))
        a = self.picture.read_ppm(b'P6\n# a comment\n3 2\n255\n' + px)
        self.assertEqual(a.shape, (2, 3, 3))
        self.assertEqual(a.tobytes(), px)
        self.assertEqual(a.dtype, np.uint8)
        with self.assertRaises(self.picture.PictureError):
            self.picture.read_ppm(b'P5\n1 1\n255\n\0')

    @staticmethod
    def bwav(channels, rate=48000):
        """A .bwav of channels given as (codec, pan, samples, data): the header, 0x4c bytes a channel, the data."""
        out = bytearray(b'BWAV' + struct.pack('<HHIHH', 0xfeff, 1, 0, 0, len(channels)))
        at = 0x10 + 0x4c * len(channels)
        for codec, pan, count, data in channels:
            info = bytearray(0x4c)
            struct.pack_into('<HHIII', info, 0, codec, pan, rate, count, count)
            struct.pack_into('<I', info, 0x30, at)
            out += info
            at += len(data)
        for _codec, _pan, _count, data in channels:
            out += data
        return bytes(out)

    def game_sounds(self, names=('Do', 'Cancel')):
        """A dump folder whose RomFS holds the given interface sounds; decoded into this layout's cache."""
        dump = self.tmp / 'dump'
        (dump / 'exefs').mkdir(parents=True, exist_ok=True)
        (dump / 'exefs' / 'main').write_bytes(b'')
        stream = dump / 'romfs' / 'Sound' / 'Resource' / 'Stream'
        stream.mkdir(parents=True, exist_ok=True)
        loud = struct.pack('<4h', 0, 16000, -16000, 8000)
        for name in names:
            (stream / f'SE_Sys_{name}00.bwav').write_bytes(self.bwav([(0, 0, 4, loud), (0, 1, 4, loud)]))
        return gamefiles.extract_sounds(self.layout, '', str(dump))

    def test_the_games_interface_sounds(self):
        sound = self.sound
        # one PCM16 channel in the middle: both sides, at 0.707
        mono = self.bwav([(0, 2, 3, struct.pack('<3h', 1000, -2000, 3000))])
        self.assertEqual(gamefiles.bwav_to_pcm(mono), (48000, [(707, 707), (-1414, -1414), (2121, 2121)]))
        # two DSP-ADPCM channels, left and right: a frame's header byte (coefficient pair 0, all zero here, shift 8)
        # and its four-bit samples 1, -1, 7 make 256, -256, 1792
        frame = bytes([0x08, 0x1f, 0x70]) + bytes(5)
        both = self.bwav([(1, 0, 3, frame), (1, 1, 2, frame)])
        self.assertEqual(gamefiles.bwav_to_pcm(both), (48000, [(256, 256), (-256, -256), (1792, 0)]))
        for bad in (b'', b'RIFF' + bytes(60), mono[:0x30], self.bwav([(7, 2, 3, bytes(6))]), mono[:-2]):
            self.assertIsNone(gamefiles.bwav_to_pcm(bad))
        # from a dump folder into data/cache/sounds, and from there into the effects: the decide sound only here
        got = self.game_sounds(('Do',))
        (self.tmp / 'dump' / 'romfs' / 'Sound' / 'Resource' / 'Stream' / 'SE_Sys_Cancel00.bwav').write_bytes(b'no sound')
        self.assertEqual(set(got), {'select'})
        self.assertEqual(gamefiles.cached_sounds(self.layout), got)
        with mock.patch.object(sound.os, 'name', 'posix'), mock.patch.object(sound.shutil, 'which', return_value=None):
            s = sound.Sounds(self.layout.cache, 'high')
        self.assertEqual(set(s.game), {'select'})
        wav = s.wav('select')                                  # the game's sound: stereo at its rate, at the level
        self.assertEqual(wav[:4] + wav[8:16], b'RIFFWAVEfmt ')
        self.assertEqual(struct.unpack_from('<I', wav, 4)[0], len(wav) - 8)
        self.assertEqual(struct.unpack_from('<HHI', wav, 20), (1, 2, 48000))
        peak = max(abs(v) for v in struct.unpack_from('<8h', wav, 44))
        self.assertAlmostEqual(peak / 32767, sound.LEVELS['high'], delta=0.01)
        self.assertFalse(sound.Sounds(self.tmp / 'empty', 'high').game)

    def test_only_the_games_sounds_play_at_low_unless_set(self):
        sound = self.sound
        self.assertEqual(settings.DEFAULTS['sounds'], 'low')
        self.assertEqual(sound.DEFAULT_LEVEL, 'low')
        self.assertEqual(set(gamefiles.SOUNDS), set(sound.EFFECTS))
        empty = self.tmp / 'empty'
        self.game_sounds()
        with mock.patch.object(sound.os, 'name', 'posix'), mock.patch.object(sound.shutil, 'which',
                                                                             return_value='/usr/bin/aplay'), \
                mock.patch.object(sound.subprocess, 'Popen') as popen:
            quiet = sound.Sounds(empty, 'high')                # before the game has been read: nothing to play
            quiet.play('select')
            quiet.play('back')
            popen.assert_not_called()
            quiet.cache = self.layout.cache
            self.assertTrue(quiet.load_game())                 # the game's sounds, once decoded
            quiet.play('select')
            self.assertEqual(popen.call_count, 1)
            s = sound.Sounds(self.layout.cache)                # no level given, as a new launcher.json has it: low
            self.assertEqual(s.level, 'low')
            s.play('select')
            self.assertEqual(popen.call_count, 2)
            s.set_level('off')                                 # the player turned them off
            s.play('select')
            self.assertEqual(popen.call_count, 2)
            s.set_level('nonsense')                            # a level launcher.json cannot hold: the default
            self.assertEqual(s.level, 'low')
            s.play('back')
            s.play('move')                                     # the cursor has no sound
            s.play('no such effect')
            self.assertEqual(popen.call_count, 3)
            with open(popen.call_args[0][0][1], 'rb') as f:
                self.assertEqual(f.read(4), b'RIFF')
            peak = max(abs(v) for v in struct.unpack_from('<8h', s.wav('back'), 44))
            self.assertAlmostEqual(peak / 32767, sound.LEVELS['low'], delta=0.01)

    def test_the_look_settings_survive_and_fall_back(self):
        path = self.tmp / 'launcher.json'
        s = settings.Settings(path)
        self.assertEqual((s['theme'], s['background'], s['background_dim'], s['animations'], s['sounds']),
                         ('forest', '', 40, True, 'low'))
        s['theme'], s['sounds'], s['background_dim'] = 'shadow', 'high', 60
        s.save()
        t = settings.Settings(path)
        self.assertEqual((t['theme'], t['sounds'], t['background_dim']), ('shadow', 'high', 60))
        path.write_text(json.dumps({'theme': 5, 'animations': 'yes', 'background_dim': '40'}))
        u = settings.Settings(path)                        # values of the wrong kind: the defaults
        self.assertEqual((u['theme'], u['animations'], u['background_dim']), ('forest', True, 40))


# ---------------------------------------------------------------- the window

def have_display():
    if os.name == 'nt':
        return True
    if not os.environ.get('DISPLAY'):
        return False
    try:
        import tkinter  # noqa: F401
        import numpy  # noqa: F401
        return True
    except ImportError:
        return False


@unittest.skipUnless(have_display(), 'needs tkinter, numpy and a display')
class WhilePlayingTest(Tmp):
    def test_the_games_restart_does_not_bring_the_launcher_back(self):
        try:
            from launcher.ui.app import App
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        a = mock.MagicMock()
        a.settings = {'game_dir': str(self.tmp)}
        a.game_proc = mock.MagicMock(returncode=0)
        a.game_proc.poll.return_value = 0                              # the process started here has ended
        with mock.patch('launcher.ui.app.saves.game_running', return_value=True):
            App.watch_game(a)                                          # but the restarted game runs from the same exe
        a.root.deiconify.assert_not_called()
        a.root.after.assert_called_once()
        with mock.patch('launcher.ui.app.saves.game_running', return_value=False):
            App.watch_game(a)
        a.root.deiconify.assert_called_once()


class WindowTest(Tmp):
    def test_selftest(self):
        r = subprocess.run([sys.executable, '-m', 'launcher', '--root', str(self.tmp / 'w'), '--selftest'],
                           cwd=ROOT, capture_output=True, text=True, timeout=180)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('15 screens drawn', r.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
