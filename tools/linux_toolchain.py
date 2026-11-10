"""Assemble the compiler, the linker and the headers and libraries a Linux game build reads into one folder, so the
AppImage of tools/make_appimage.py builds the game on a PC without a compiler or development packages (a Steam Deck,
any system whose base is read-only).

    python3 tools/linux_toolchain.py --out <folder>     # alone; tools/make_appimage.py puts it in the kit's llvm/

The folder:

    bin/       clang, clang++ and lld of the official LLVM 23.1.4 release, with clang.cfg and clang++.cfg, which clang
               reads from beside itself: they point it at sysroot/ and lld, so tools/make_game.py runs it from PATH as
               it runs a system clang
    lib/clang/ clang's own headers
    sysroot/   the files of the Steam Runtime 3 "sniper" SDK (glibc 2.31) a game build reads, and in usr/local/ what the
               SDK lacks or carries unsuited, built here against it: Vulkan-Headers 1.4.363, zstd 1.5.7, dav1d 1.5.4
               and SDL 3.4.18 (with the client headers of wayland 1.24)
    licenses/  the licence texts of all of it

SDL3, Opus, libvpx, dav1d, zstd, libatomic and libstdc++ are there as static archives only, so the game links them in
and needs nothing on the player's PC but glibc 2.31 or newer and the graphics driver's Vulkan loader. The toolchain
itself needs glibc 2.34 (the official LLVM binaries are built against it): SteamOS 3, Ubuntu 22.04, Debian 12.

The LLVM release, the SDK, the Vulkan headers and zstd are the pins of tools/make_release_linux.py and share its
download cache (toolchain/release-dl) and its unpacked SDK. dav1d is built with meson and nasm from their pinned
sources, SDL with CMake's pinned release; none of the three is shipped. Building the folder needs network access the
first time, make, tar, pkg-config and wayland-scanner.
"""
import argparse
import os
import shutil
import subprocess
import sys
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
# The C library's and the kernel's header folders beside usr/include/*.h and the multiarch folder, the C++ library's,
# and those of the SDK libraries the runtime includes.
HEADER_DIRS = ['x86_64-linux-gnu', 'arpa', 'net', 'netash', 'netatalk', 'netax25', 'neteconet', 'netinet', 'netipx',
               'netiucv', 'netpacket', 'netrom', 'netrose', 'nfs', 'protocols', 'rpc', 'scsi', 'linux', 'asm-generic',
               'misc', 'mtd', 'rdma', 'sound', 'video', 'xen', 'c++', 'opus', 'vpx']
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
    """The SDK files a compile and link of the game read: headers by folder, the C runtime and the libraries."""
    inc = sdk / 'usr' / 'include'
    files = set(inc.glob('*.h'))
    for d in HEADER_DIRS:   # links into the SDK's /etc/alternatives lead nowhere outside its container
        files.update(p for p in (inc / d).rglob('*') if p.exists() and not p.is_dir())
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
    files.update(lib.glob('libvulkan.so*'))   # the loader is the driver's: linked by name, never copied
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
    summary = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    missing = [k for k in ('SDL_X11:BOOL=ON', 'SDL_WAYLAND:BOOL=ON', 'SDL_PIPEWIRE:BOOL=ON', 'SDL_PULSEAUDIO:BOOL=ON',
                           'SDL_ALSA:BOOL=ON', 'SDL_LIBUDEV:BOOL=ON', 'SDL_DBUS:BOOL=ON') if k not in summary]
    if missing:
        sys.exit(f'SDL3 was configured without {", ".join(missing)}')
    return src / 'LICENSE.txt', wayland_license


def collect_licenses(dest, sdk, zstd_license, dav1d_license, sdl_licenses):
    lic = dest / 'licenses'
    lic.mkdir(parents=True, exist_ok=True)
    doc = sdk / 'usr' / 'share' / 'doc'
    for src, name in [
        (mr.fetch('llvm-license'), 'llvm-LICENSE.txt'),
        (doc / 'libc6' / 'copyright', 'glibc-copyright.txt'),
        (doc / 'gcc-10-base' / 'copyright', 'libstdc++-libgcc-libatomic-copyright.txt'),
        (sdl_licenses[0], 'SDL3-LICENSE.txt'),
        (sdl_licenses[1], 'wayland-COPYING.txt'),   # SDL's Wayland code is compiled with its client headers
        (doc / 'libopus0' / 'copyright', 'opus-copyright.txt'),
        (doc / 'libvpx6' / 'copyright', 'libvpx-copyright.txt'),
        (doc / 'libvulkan1' / 'copyright', 'vulkan-loader-copyright.txt'),
        (mrl.WORK / 'vulkan-headers' / 'LICENSE.md', 'vulkan-headers-LICENSE.txt'),
        (zstd_license, 'zstd-LICENSE.txt'),
        (dav1d_license, 'dav1d-COPYING.txt'),
    ]:
        shutil.copy2(src, lic / name)
    import tarfile   # Debian's libvpx copyright carries no patent grant: the WebM PATENTS text from the pinned source tag
    with tarfile.open(mr.fetch('libvpx-src')) as t:
        (lic / 'libvpx-PATENTS.txt').write_bytes(t.extractfile('libvpx-1.17.0/PATENTS').read())


def assemble(dest):
    """Write the folder of the module docstring to dest (replaced)."""
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
    for p in (dest / 'bin').iterdir():   # the archiver and objcopy built the libraries above; a game build needs neither
        if p.name not in LLVM_KEEP and p.suffix != '.cfg':
            p.unlink()
    check(dest)
    print(f'toolchain: {dest}, {mr.mib(size(dest))} (sysroot {mr.mib(size(dest / "sysroot"))})')
    return dest


def size(folder):
    """Bytes of the files in folder, each once (clang, clang++ and ld.lld are links)."""
    return sum(p.stat().st_size for p in folder.rglob('*') if p.is_file() and not p.is_symlink())


def check(dest):
    """The folder's clang compiles and links a C++ program against SDL3, Vulkan, Opus, libvpx, dav1d and zstd with no
    flag of its own, as make_game.py calls it, and the program needs no library of the SDK at run time."""
    work = mrl.WORK / 'toolchain-check'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    src = work / 'check.cpp'
    src.write_text('#include <SDL3/SDL.h>\n#include <vulkan/vulkan.h>\n#include <opus/opus.h>\n'
                   '#include <vpx/vpx_decoder.h>\n#include <vpx/vp8dx.h>\n#include <dav1d/dav1d.h>\n#include <zstd.h>\n'
                   '#include <atomic>\n#include <string>\n#include <cstdio>\n'
                   'int main() { std::atomic<long long> a{1}; std::string s = dav1d_version();\n'
                   '  std::printf("SDL %d, dav1d %s, opus %s, vpx %s, zstd %u, Vulkan headers %d, %lld\\n",\n'
                   '              SDL_GetVersion(), s.c_str(), opus_get_version_string(), vpx_codec_version_str(),\n'
                   '              ZSTD_versionNumber(), VK_HEADER_VERSION, a.fetch_add(1));\n'
                   '  PFN_vkGetInstanceProcAddr volatile loader = vkGetInstanceProcAddr; return loader ? 0 : 1; }\n', encoding='utf-8')
    exe = work / 'check'
    mr.run([dest / 'bin' / 'clang++', '-std=gnu++17', '-O1', '-no-pie', src, '-o', exe, *[
        '-lvulkan', '-lSDL3', '-lopus', '-lvpx', '-ldav1d', '-lzstd', '-l:libstdc++.a', '-latomic', '-lm', '-lpthread',
        '-ldl']], env=dict(os.environ, PATH='/usr/bin:/bin'))
    needed = subprocess.run(['readelf', '-d', str(exe)], capture_output=True, text=True, check=True).stdout
    libs = sorted(set(l.split('[')[1].rstrip(']') for l in needed.splitlines() if '(NEEDED)' in l))
    allowed = {'libvulkan.so.1', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'libc.so.6', 'librt.so.1',
               'libgcc_s.so.1', 'ld-linux-x86-64.so.2'}
    if set(libs) - allowed:
        sys.exit(f'toolchain check: the program needs {", ".join(sorted(set(libs) - allowed))} at run time')
    print(f'toolchain check: linked; needs {", ".join(libs)}')
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(f'toolchain check: {r.stdout.strip() or r.stderr.strip()}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, required=True, help='the toolchain folder (replaced)')
    assemble(ap.parse_args().out)


if __name__ == '__main__':
    main()
