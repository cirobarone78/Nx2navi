"""Assemble the compiler, the linker and the headers and libraries a Linux game build reads into one folder, so the
AppImage of tools/make_appimage.py --toolchain builds the game on a PC without a compiler or development packages
(SteamOS, any system whose base is read-only).

    python3 tools/linux_toolchain.py --out <folder>     # alone; make_appimage.py --toolchain puts it in the kit's llvm/
    python3 tools/linux_toolchain.py --check <folder>   # only the end check, on a folder made before

The folder:

    bin/       clang, clang++ and lld of the official LLVM 23.1.4 release, with clang.cfg and clang++.cfg, which clang
               reads from beside itself: they point it at sysroot/ and lld, so tools/make_game.py runs it from PATH as
               it runs a system clang
    lib/clang/ clang's own headers
    sysroot/   the files of the Steam Runtime 3 "sniper" SDK (glibc 2.31) a game build reads, and in usr/local/ what the
               SDK lacks or carries unsuited, built here against it: Vulkan-Headers 1.4.363, zstd 1.5.7, dav1d 1.5.4
               and SDL 3.4.18 (with the client headers of wayland 1.24)
    licenses/  the licence texts of all of it, and SOURCES.txt, which says where the source of its LGPL and GPL parts is

SDL3, Opus, libvpx, dav1d, zstd, libatomic and libstdc++ are there as static archives only, so the game links them in
and needs nothing on the player's PC but glibc 2.31 or newer and the graphics driver's Vulkan loader. The toolchain
itself needs what the official LLVM binaries are built against: glibc 2.34, the libstdc++ of GCC 12 or newer
(GLIBCXX_3.4.30) and zlib (libz.so.1).

The LLVM release, the SDK, the Vulkan headers and zstd are the pins of tools/make_release_linux.py and share its
download cache (toolchain/release-dl) and its unpacked SDK. dav1d is built with meson and nasm from their pinned
sources, SDL with CMake's pinned release; none of the three is shipped. Building the folder needs network access the
first time and HOST_TOOLS; its end check (check) also runs a program, which needs the Vulkan loader (libvulkan.so.1).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_release as mr  # noqa: E402
import make_release_linux as mrl  # noqa: E402

DAV1D = '1.5.4'
PINS = {
    'dav1d-src': (f'https://downloads.videolan.org/pub/videolan/dav1d/{DAV1D}/dav1d-{DAV1D}.tar.xz',
                  '686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5'),
    'meson-src': ('https://github.com/mesonbuild/meson/releases/download/1.12.1/meson-1.12.1.tar.gz',
                  'ab0a6ca09f8ef70c564c8241fb5a23957886a0b53fb58412b5e07eaf07dba743'),
    'nasm-src': ('https://www.nasm.us/pub/nasm/releasebuilds/3.02/nasm-3.02.tar.xz',
                 '87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d'),
    'sdl3-src': (f'https://github.com/libsdl-org/SDL/releases/download/release-{mr.SDL_VERSION}/'
                 f'SDL3-{mr.SDL_VERSION}.tar.gz',
                 '9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3'),
    'wayland-src': ('https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.24.0/downloads/wayland-1.24.0.tar.xz',
                    '82892487a01ad67b334eca83b54317a7c86a03a89cfadacfef5211f11a5d0536'),
    'cmake-linux': ('https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-linux-x86_64.tar.gz',
                    'ed85135c5a5135b45e50423cb406dea04ca3338da0b8ef3c9ede963559680be0'),
}
mr.PINS.update(PINS)

# What tools/make_game.py runs; the release's archiver and objcopy build the libraries here and stay out (60 MB).
LLVM_KEEP = {f'clang-{mrl.LLVM_MAJOR}', 'clang', 'clang++', 'lld', 'ld.lld'}
# What building the folder runs from the system. SDL leaves out a back-end whose tool is missing without an error
# (Wayland without wayland-scanner), so they are looked for before anything is downloaded.
HOST_TOOLS = ('make', 'tar', 'xz', 'pkg-config', 'wayland-scanner', 'readelf')
# The SDK packages whose headers a game build reads: the C library's, the kernel's, the C++ library's, and those of
# the SDK libraries the runtime includes. Taken by the SDK's own file lists, so no header of another package (and of
# another licence) comes along.
HEADER_PACKAGES = ['libc6-dev', 'linux-libc-dev', 'libstdc++-10-dev', 'libopus-dev', 'libvpx-dev']
# The back-ends SDL must have compiled in, as SDL_build_config.h names them. SDL's options (CMakeCache.txt) stay ON
# when a dependency is missing, so they cannot tell.
SDL_BACKENDS = ('SDL_VIDEO_DRIVER_X11', 'SDL_VIDEO_DRIVER_WAYLAND', 'SDL_AUDIO_DRIVER_PIPEWIRE',
                'SDL_AUDIO_DRIVER_PULSEAUDIO', 'SDL_AUDIO_DRIVER_ALSA', 'SDL_UDEV_DYNAMIC', 'HAVE_DBUS_DBUS_H')
# The source packages of the object code under the LGPL and the GPL that sysroot/ carries (glibc's shared objects,
# static parts and start files; GCC's C++ and runtime libraries), by an SDK package built from each. The SDK snapshot
# publishes them in its sources/ folder; licenses/SOURCES.txt names them, and a new SDK pin with other versions stops
# the build until they are named here.
SOURCE_PACKAGES = {'libc6': ('glibc', '2.31-13+deb11u14'), 'libstdc++-10-dev': ('gcc-10', '10.3.0-3+steamrt3.1')}
SOURCES = '''Where the source of the LGPL and GPL parts of this folder is

sysroot/ holds object code of glibc (LGPL-2.1-or-later: its shared objects, libc_nonshared.a and the crt*.o start
files) and of GCC's runtime (GPL-3.0-or-later with the GCC Runtime Library Exception: libstdc++.a, libsupc++.a,
libgcc.a, libgcc_eh.a, libgcc_s.so.1, libatomic.a and crtbegin*.o and crtend*.o), as the Steam Runtime 3 "sniper"
SDK {sniper} carries them. Their complete corresponding source is published with that SDK:

{packages}

in {url}/{sniper}/sources/
Each .dsc file names the other files of its source package, which are in the same folder. The licence texts are
LGPL-2.1.txt and GPL-3.txt here, the exception and the copyright notices are in glibc-copyright.txt and
libstdc++-libgcc-libatomic-copyright.txt. The kernel's headers (linux-kernel-headers-copyright.txt, GPL-2.0 with the
Linux syscall note) are themselves source.
'''
# The SDK libraries the game links as archives. Without a .so of the same name beside them lld takes the archive, so
# make_game.py's -lopus -lvpx -latomic need no change, and the game does not depend on the player's versions (the
# SDK's libvpx.so.6 is on no current distribution).
STATIC = ['usr/lib/x86_64-linux-gnu/libopus.a', 'usr/lib/x86_64-linux-gnu/libvpx.a',
          'usr/lib/gcc/x86_64-linux-gnu/10/libatomic.a']
CONFIG = '''# clang reads this file from beside itself (and clang++ its copy, clang++.cfg): every compile and link uses the
# sysroot beside this folder instead of the system's headers and libraries, and lld instead of the system's linker.
--sysroot=<CFGDIR>/../sysroot
-fuse-ld=lld
-L<CFGDIR>/../sysroot/usr/local/lib
'''


def sysroot_files(sdk):
    """The SDK files a compile and link of the game read: the headers of HEADER_PACKAGES, the C runtime and the
    libraries."""
    files = set()
    for pkg in HEADER_PACKAGES:
        listing = (sdk / 'var' / 'lib' / 'dpkg' / 'info' / f'{pkg}:amd64.list').read_text(encoding='utf-8')
        for line in listing.splitlines():
            p = sdk / line.lstrip('/')   # links into the SDK's /etc/alternatives lead nowhere outside its container
            if line.startswith('/usr/include/') and p.exists() and not p.is_dir():
                files.add(p)
    lib, root_lib = sdk / 'usr' / 'lib' / 'x86_64-linux-gnu', sdk / 'lib' / 'x86_64-linux-gnu'
    for name in mrl.GLIBC_LIBS:   # the link scripts, their archives and the shared objects they name
        files.update(p for p in lib.glob(f'lib{name}[._]*') if p.suffix in ('.so', '.a') or '.so' in p.name)
        files.update(root_lib.glob(f'lib{name}.so*'))
    files.update(lib.glob('*crt*.o'))
    files.update((sdk / 'lib64').glob('ld-linux*'))
    files.update(root_lib.glob('ld-linux*'))
    files.update(root_lib.glob('libgcc_s.so*'))
    gcc = sdk / 'usr' / 'lib' / 'gcc' / 'x86_64-linux-gnu' / '10'
    files.update(p for p in gcc.iterdir() if p.name.startswith('crt') or p.name in mrl.GCC_LIBS)
    files.update(sdk / p for p in STATIC)
    files.update(lib.glob('libvulkan.so*'))   # what lld links against; the game loads the driver's loader
    return files


def build_zstd(dest, local):
    zs = mrl.tar_extract('zstd-src', mrl.WORK / 'zstd', ['zstd-1.5.7'], strip=1)
    lib = zs / 'lib'
    mr.run(['make', '-C', lib, 'clean'], stdout=subprocess.DEVNULL)   # objects of another compiler or sysroot
    mr.run(['make', '-C', lib, f'-j{os.cpu_count() or 4}', 'libzstd.a', f'CC={dest / "bin" / "clang"}',
            f'AR={dest / "bin" / "llvm-ar"}', 'ZSTD_LEGACY_SUPPORT=0', 'MOREFLAGS=-O2'], stdout=subprocess.DEVNULL)
    shutil.copy2(lib / 'libzstd.a', local / 'lib' / 'libzstd.a')
    for name in ('zstd.h', 'zstd_errors.h'):
        shutil.copy2(lib / name, local / 'include' / name)
    return zs / 'LICENSE'


def build_dav1d(dest, local):
    """dav1d as a static library with its assembly, into local. nasm is built first: few systems install it."""
    env = dict(os.environ, CC=str(dest / 'bin' / 'clang'), AR=str(dest / 'bin' / 'llvm-ar'),
               RANLIB=str(dest / 'bin' / 'llvm-ranlib'))
    nasm = mr.unpack('nasm-src') / 'nasm-3.02'
    if not (nasm / 'nasm').is_file():
        mr.run(['./configure'], cwd=nasm, env=env, stdout=subprocess.DEVNULL)
        mr.run(['make', f'-j{os.cpu_count() or 4}', 'nasm'], cwd=nasm, env=env, stdout=subprocess.DEVNULL)
    meson = mr.unpack('meson-src') / 'meson-1.12.1' / 'meson.py'
    ninja = mr.unpack('ninja-linux')
    (ninja / 'ninja').chmod(0o755)
    src = mr.unpack('dav1d-src') / f'dav1d-{DAV1D}'
    build = mrl.WORK / 'dav1d-build'
    shutil.rmtree(build, ignore_errors=True)
    env['PATH'] = os.pathsep.join([str(nasm), str(ninja), env.get('PATH', '')])
    mr.run([sys.executable, meson, 'setup', build, src, '--buildtype=release', '--default-library=static',
            f'--prefix={local}', '--libdir=lib', '-Denable_tools=false', '-Denable_tests=false',
            '-Denable_examples=false', '-Denable_asm=true'], env=env, stdout=subprocess.DEVNULL)
    mr.run([sys.executable, meson, 'install', '-C', build, '--quiet'], env=env, stdout=subprocess.DEVNULL)
    shutil.rmtree(local / 'lib' / 'pkgconfig', ignore_errors=True)
    return src / 'COPYING'


def wayland_headers():
    """The client headers of wayland 1.24 in a folder of their own. The SDK's are 1.18, older than the protocol code
    the build machine's wayland-scanner writes for SDL; SDL loads libwayland-client when the game starts, and a desktop
    with wayland older than 1.20 (2021) gets X11."""
    src = mr.unpack('wayland-src') / 'wayland-1.24.0'
    inc = mrl.WORK / 'wayland-headers'
    shutil.rmtree(inc, ignore_errors=True)
    inc.mkdir(parents=True)
    for name in ('src/wayland-client.h', 'src/wayland-client-core.h', 'src/wayland-util.h', 'cursor/wayland-cursor.h',
                 'egl/wayland-egl.h', 'egl/wayland-egl-core.h'):
        shutil.copy2(src / name, inc)
    text = (src / 'src' / 'wayland-version.h.in').read_text(encoding='utf-8')
    for key, value in (('MAJOR', '1'), ('MINOR', '24'), ('MICRO', '0'), ('', '1.24.0')):
        text = text.replace(f'@WAYLAND_VERSION_{key}@' if key else '@WAYLAND_VERSION@', value)
    (inc / 'wayland-version.h').write_text(text, encoding='utf-8')
    return inc, src / 'COPYING'


def build_sdl3(dest, local, sdk):
    """SDL3 as a static library that loads X11, Wayland, ALSA, PulseAudio, PipeWire, libdecor, D-Bus and libudev when
    the game starts, as SDL does by default, compiled against the whole SDK for their headers. The SDK's own SDL links
    those libraries directly, so a game built with it would not start where one of them is missing."""
    cmake = mr.unpack('cmake-linux') / 'cmake-4.4.5-linux-x86_64' / 'bin' / 'cmake'
    src = mr.unpack('sdl3-src') / f'SDL3-{mr.SDL_VERSION}'
    build = mrl.WORK / 'sdl3-build'
    shutil.rmtree(build, ignore_errors=True)
    wayland, wayland_license = wayland_headers()
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(sdk), PKG_CONFIG_PATH='',
               PKG_CONFIG_LIBDIR=os.pathsep.join(str(sdk / p) for p in ('usr/lib/x86_64-linux-gnu/pkgconfig',
                                                                          'usr/share/pkgconfig')))
    mr.run([cmake, '-S', src, '-B', build, '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={mr.unpack("ninja-linux") / "ninja"}',
            f'-DCMAKE_C_COMPILER={dest / "bin" / "clang"}',
            f'-DCMAKE_SYSROOT={sdk}', f'-DCMAKE_C_FLAGS=-isystem {wayland}', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={local}',
            '-DCMAKE_INSTALL_LIBDIR=lib', '-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_DEPS_SHARED=ON',
            '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', '-DSDL_INSTALL_DOCS=OFF',
            '-DSDL_LIBURING=OFF'], env=env, stdout=subprocess.DEVNULL)
    mr.run([cmake, '--build', build, '--target', 'install'], env=env, stdout=subprocess.DEVNULL)
    for d in ('cmake', 'pkgconfig'):
        shutil.rmtree(local / 'lib' / d, ignore_errors=True)
    shutil.rmtree(local / 'share', ignore_errors=True)
    config = (build / 'include-config-release' / 'build_config' / 'SDL_build_config.h').read_text(encoding='utf-8')
    # the space after the name keeps the *_DYNAMIC lines (the library a back-end loads) from counting as the back-end
    missing = [k for k in SDL_BACKENDS if f'#define {k} ' not in config]
    if missing:
        sys.exit(f'SDL3 was built without {", ".join(missing)} (its configure output in {build} says why)')
    return src / 'LICENSE.txt', wayland_license


def sources_text(sdk):
    """SOURCES for this SDK; stops when an SDK package of SOURCE_PACKAGES was built from another source version."""
    versions = {}
    for para in (sdk / 'var' / 'lib' / 'dpkg' / 'status').read_text(encoding='utf-8').split('\n\n'):
        f = dict(l.split(': ', 1) for l in para.splitlines() if ': ' in l and not l.startswith(' '))
        if f.get('Architecture') == 'amd64':
            versions[f.get('Package')] = f.get('Version', '')
    lines = []
    for pkg, (source, version) in SOURCE_PACKAGES.items():
        if not versions.get(pkg, '').startswith(version):
            sys.exit(f"the SDK's {pkg} is {versions.get(pkg)}, not built from {source} {version}: "
                     'name its source in SOURCE_PACKAGES')
        lines.append(f'  {source} {version}: {source}_{version}.dsc')
    return SOURCES.format(sniper=mrl.SNIPER, url=mrl.SNIPER_URL, packages='\n'.join(lines))


def collect_licenses(dest, sdk, zstd_license, dav1d_license, sdl_licenses):
    lic = dest / 'licenses'
    lic.mkdir(parents=True, exist_ok=True)
    doc = sdk / 'usr' / 'share' / 'doc'
    for src, name in [
        (mr.fetch('llvm-license'), 'llvm-LICENSE.txt'),
        (doc / 'libc6' / 'copyright', 'glibc-copyright.txt'),
        (doc / 'gcc-10-base' / 'copyright', 'libstdc++-libgcc-libatomic-copyright.txt'),
        (doc / 'linux-libc-dev' / 'copyright', 'linux-kernel-headers-copyright.txt'),
        (sdl_licenses[0], 'SDL3-LICENSE.txt'),
        (sdl_licenses[1], 'wayland-COPYING.txt'),   # SDL's Wayland code is compiled with its client headers
        (doc / 'libopus0' / 'copyright', 'opus-copyright.txt'),
        (doc / 'libvpx6' / 'copyright', 'libvpx-copyright.txt'),
        (doc / 'libvulkan1' / 'copyright', 'vulkan-loader-copyright.txt'),
        (mrl.WORK / 'vulkan-headers' / 'LICENSE.md', 'vulkan-headers-LICENSE.txt'),
        (zstd_license, 'zstd-LICENSE.txt'),
        (dav1d_license, 'dav1d-COPYING.txt'),
    ] + [   # the Debian copyright files above point at these full texts, which a player's system need not have
        (sdk / 'usr' / 'share' / 'common-licenses' / n, f'{n}.txt') for n in ('LGPL-2.1', 'GPL-2', 'GPL-3', 'Apache-2.0')
    ]:
        shutil.copy2(src, lic / name)
    import tarfile   # Debian's libvpx copyright carries no patent grant: the WebM PATENTS text from the pinned source tag
    with tarfile.open(mr.fetch('libvpx-src')) as t:
        (lic / 'libvpx-PATENTS.txt').write_bytes(t.extractfile('libvpx-1.17.0/PATENTS').read())
    (lic / 'SOURCES.txt').write_text(sources_text(sdk), encoding='utf-8')


def assemble(dest):
    """Write the folder of the module docstring to dest (replaced)."""
    missing = [t for t in HOST_TOOLS if not shutil.which(t)]
    if missing:
        sys.exit(f'missing: {" ".join(missing)} (Debian and Ubuntu: apt install make tar xz-utils pkg-config '
                 'libwayland-bin binutils)')
    dest = Path(dest).resolve()
    shutil.rmtree(dest, ignore_errors=True)
    dest.parent.mkdir(parents=True, exist_ok=True)
    mrl.build_llvm(dest)
    for name in ('clang.cfg', 'clang++.cfg'):
        (dest / 'bin' / name).write_text(CONFIG, encoding='utf-8')
    sdk = mrl.full_sysroot()
    mrl.build_sysroot(dest / 'sysroot', sdk, sysroot_files(sdk))
    local = dest / 'sysroot' / 'usr' / 'local'
    (local / 'include').mkdir(parents=True)
    (local / 'lib').mkdir(parents=True)
    vk = mrl.tar_extract('vulkan-headers-src', mrl.WORK / 'vulkan-headers',
                         ['Vulkan-Headers-1.4.363/include', 'Vulkan-Headers-1.4.363/LICENSE.md'], strip=1)
    for sub in ('vulkan', 'vk_video'):   # newer than the SDK's: clang searches usr/local/include before usr/include
        shutil.copytree(vk / 'include' / sub, local / 'include' / sub,   # the C++ bindings: 20 MB the C code never reads
                        ignore=shutil.ignore_patterns('*.hpp', '*.cppm'))
    zstd_license = build_zstd(dest, local)
    dav1d_license = build_dav1d(dest, local)
    sdl_licenses = build_sdl3(dest, local, sdk)
    collect_licenses(dest, sdk, zstd_license, dav1d_license, sdl_licenses)
    check(dest)
    for p in (dest / 'bin').iterdir():   # the archiver and objcopy built the libraries above; a game build needs neither
        if p.name not in LLVM_KEEP and p.suffix != '.cfg':
            p.unlink()
    print(f'toolchain: {dest}, {mr.mib(size(dest))} (sysroot {mr.mib(size(dest / "sysroot"))})')
    return dest


def size(folder):
    """Bytes of the files in folder, each once (clang, clang++ and ld.lld are links)."""
    return sum(p.stat().st_size for p in folder.rglob('*') if p.is_file() and not p.is_symlink())


def check(dest):
    """What a player's build reads, tried with the folder's tools alone (PATH holds only its bin/): make_game.py
    compiles every runtime file that does not depend on the game (--make-runtime-lib), so a header or library the
    runtime gains and the folder lacks fails here, not on a player's PC after the RomFS and the lift; then a C++ program
    that uses SDL3, Vulkan, Opus, libvpx, dav1d and zstd links with make_game.LINK_LIBS, must need no library of the SDK
    at run time, and must run. The archiver is the folder's while assemble has it, else NX2NAVI_AR's."""
    import build_shaders
    import make_game
    build_shaders.build(mr.ROOT / 'runtime')   # the runtime includes its shaders compiled; nothing when they are current
    env = dict(os.environ, PATH=str(dest / 'bin'), VMA_INCLUDE=str(mr.ROOT / 'toolchain' / 'vma'))
    if (dest / 'bin' / 'llvm-ar').is_file():
        env['NX2NAVI_AR'] = str(dest / 'bin' / 'llvm-ar')
    with tempfile.TemporaryDirectory(prefix='toolchain-check-') as tmp:
        work = Path(tmp)
        if subprocess.run([sys.executable, mr.ROOT / 'tools' / 'make_game.py', '--make-runtime-lib', work / 'rt'],
                          env=env).returncode:
            sys.exit('toolchain check: the runtime does not compile with the folder alone')
        src = work / 'check.cpp'
        src.write_text('#include <SDL3/SDL.h>\n#include <vulkan/vulkan.h>\n#include <opus/opus.h>\n'
                       '#include <vpx/vpx_decoder.h>\n#include <vpx/vp8dx.h>\n#include <dav1d/dav1d.h>\n#include <zstd.h>\n'
                       '#include <atomic>\n#include <string>\n#include <cstdio>\n'
                       'int main() { std::atomic<long long> a{1}; std::string s = dav1d_version();\n'
                       '  std::printf("SDL %d, dav1d %s, opus %s, vpx %s, zstd %u, Vulkan headers %d, %lld\\n",\n'
                       '              SDL_GetVersion(), s.c_str(), opus_get_version_string(), vpx_codec_version_str(),\n'
                       '              ZSTD_versionNumber(), VK_HEADER_VERSION, a.fetch_add(1));\n'
                       '  PFN_vkGetInstanceProcAddr volatile loader = vkGetInstanceProcAddr; return loader ? 0 : 1; }\n',
                       encoding='utf-8')
        exe = work / 'check'
        mr.run([dest / 'bin' / 'clang++', '-std=gnu++17', '-O1', '-no-pie', src, '-o', exe, *make_game.LINK_LIBS],
               env=env)
        needed = subprocess.run(['readelf', '-d', str(exe)], capture_output=True, text=True, check=True).stdout
        libs = sorted(set(l.split('[')[1].rstrip(']') for l in needed.splitlines() if '(NEEDED)' in l))
        allowed = {'libvulkan.so.1', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'libc.so.6', 'librt.so.1',
                   'libgcc_s.so.1', 'ld-linux-x86-64.so.2'}
        if set(libs) - allowed:
            sys.exit(f'toolchain check: the program needs {", ".join(sorted(set(libs) - allowed))} at run time')
        print(f'toolchain check: linked; needs {", ".join(libs)}')
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'toolchain check: the program ended with {r.returncode}: {r.stderr.strip()}')
        print(f'toolchain check: {r.stdout.strip()}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    what = ap.add_mutually_exclusive_group(required=True)
    what.add_argument('--out', type=Path, help='the toolchain folder (replaced)')
    what.add_argument('--check', type=Path, metavar='FOLDER',
                      help="only the end check, on a folder made before: it has no archiver then, so NX2NAVI_AR names "
                           "one (the LLVM release's llvm-ar)")
    a = ap.parse_args()
    if a.check:
        check(a.check.resolve())
    else:
        assemble(a.out)


if __name__ == '__main__':
    main()
