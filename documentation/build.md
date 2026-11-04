# Building QEMU-iOS for the Young 2

The emulator is cross-compiled on the Windows host inside WSL and the resulting
binary is copied to the device. Building natively on the Young 2 was abandoned:
the device has 461 MB of RAM, a 1 GHz single core and no working package mirror,
so a full QEMU build is impractical there.

## Source base

The repository is QEMU 8.2.0 (`VERSION`) with the QEMU-iOS fork on top. The iPod
touch 2G machine is provided by `hw/arm/ipod_touch_2g.c` and the surrounding
`ipod_touch_*` devices. No upstream migration was performed.

Two properties of the working tree affect the build.

### Line endings

The repository is committed with CRLF line endings. Shell scripts, Meson build
files and the `configure` script therefore do not run under Linux as checked out.
The build uses a copy of the tree with CRLF converted to LF:

```
tar -C <repo> --exclude=./.git --exclude=./.local -cf - . | tar -C /root/qemu-src -xf -
cd /root/qemu-src
grep -rlI $'\r' . | xargs sed -i 's/\r$//'
```

The conversion has to cover files without an extension as well. `scripts/hxtool`
and the other helper scripts in `scripts/` are extension-less and are executed
directly, so a shebang ending in CR fails with a missing-interpreter error.

### Git symlinks

Windows checked out the seven symlinked paths in `subprojects/` as text files
containing the link target. `subprojects/libvduse/include/atomic.h` was therefore
a one-line text file, which fails to compile. The symlinks are recreated in the
build copy from `git ls-files -s`:

```
subprojects/libvduse/include/atomic.h          -> ../../../include/qemu/atomic.h
subprojects/libvduse/include/compiler.h        -> ../../../include/qemu/compiler.h
subprojects/libvduse/linux-headers/linux       -> ../../../linux-headers/linux/
subprojects/libvduse/standard-headers/linux    -> ../../../include/standard-headers/linux/
subprojects/libvhost-user/include/atomic.h     -> ../../../include/qemu/atomic.h
subprojects/libvhost-user/include/compiler.h   -> ../../../include/qemu/compiler.h
subprojects/libvhost-user/standard-headers/linux -> ../../../include/standard-headers/linux
```

Both `libvduse` and `libvhost-user` are disabled in the configuration below, so
the symlinks are not strictly required for this build. They are restored anyway
so the tree behaves like a normal checkout.

### Source defect in the fork

`hw/arm/ipod_touch_lcd.c` contains two `fprintf` calls whose format string holds
a literal newline instead of the `\n` escape:

```
fprintf(stderr, "INPUTTRACE DOWN raw_x=%d raw_y=%d fx=%.6f fy=%.6f
", x, y, fx, fy);
```

A string literal cannot span lines, so this file does not compile. The committed
blob has the same defect, so it is not a checkout artifact. Both occurrences were
repaired by restoring the escape sequence. This is the only source change
required to build the current tree.

## Toolchain

| Component | Value |
| --- | --- |
| Compiler | `arm-linux-musleabihf-gcc` 11.2.1 (musl.cc build, installed at `/opt/musl-armhf`) |
| Sysroot | Alpine edge armv7 root filesystem at `/opt/armv7` |
| Target | `arm-softmmu` only |
| Build host | WSL Ubuntu, 4 cores |

The sysroot is populated from Alpine edge armv7 packages. Beyond the base
libraries it needs the development files for glib, pixman, zlib, SDL2, OpenSSL
and pcre2:

```
/opt/armv7/usr/lib/pkgconfig/   glib-2.0, gobject-2.0, pixman-1, zlib,
                                sdl2, openssl, libcrypto, libpcre2-8, libffi
```

Three packages are needed that are not part of a minimal root filesystem:
`sdl2-dev`, `openssl-dev` and `pcre2` plus `pcre2-dev`. `pcre2-8` in particular
must be present as a library, not only as a `.pc` file, because `libglib-2.0.so`
has a `DT_NEEDED` entry for it and the linker resolves that at link time.

`pkg-config` is taken from the WSL distribution and pointed at the sysroot:

```
export PKG_CONFIG=/usr/bin/pkg-config
export PKG_CONFIG_SYSROOT_DIR=/opt/armv7
export PKG_CONFIG_LIBDIR=/opt/armv7/usr/lib/pkgconfig
```

QEMU's `configure` derives the pkg-config program name from `--cross-prefix` and
looks for `arm-linux-musleabihf-pkg-config`, which does not exist. Setting
`PKG_CONFIG` in the environment is what makes the glib detection succeed.

## Configure

```
export PATH=/opt/musl-armhf/bin:$PATH
export PKG_CONFIG=/usr/bin/pkg-config
export PKG_CONFIG_SYSROOT_DIR=/opt/armv7
export PKG_CONFIG_LIBDIR=/opt/armv7/usr/lib/pkgconfig

mkdir -p /root/qemu-build && cd /root/qemu-build

/root/qemu-src/configure \
  --cross-prefix=arm-linux-musleabihf- \
  --target-list=arm-softmmu \
  --enable-sdl \
  --disable-gtk \
  --disable-cocoa \
  --disable-vnc \
  --disable-opengl \
  --disable-virglrenderer \
  --disable-capstone \
  --disable-slirp \
  --disable-docs \
  --disable-tools \
  --disable-guest-agent \
  --disable-plugins \
  --disable-curl \
  --disable-libssh \
  --disable-libnfs \
  --disable-bzip2 \
  --disable-seccomp \
  --disable-libiscsi \
  --disable-rbd \
  --disable-glusterfs \
  --disable-usb-redir \
  --disable-smartcard \
  --disable-tpm \
  --disable-xen \
  --disable-kvm \
  --disable-vhost-user \
  --disable-vhost-net \
  --disable-vhost-kernel \
  --disable-vhost-vdpa \
  --disable-vhost-crypto \
  --disable-libvduse \
  --disable-vduse-blk-export \
  --disable-alsa \
  --disable-sndio \
  --disable-pa \
  --disable-werror \
  --disable-pie \
  --extra-cflags="-march=armv7-a -mfpu=neon -mfloat-abi=hard" \
  --extra-ldflags="-march=armv7-a -mfpu=neon -mfloat-abi=hard -no-pie -Wl,-rpath-link,/opt/armv7/usr/lib -L/opt/armv7/usr/lib -lcrypto"
```

### Why these flags

`--disable-vhost-user` and `--disable-libvduse` remove the two subprojects that
are not needed for an ARM system emulator without virtio backends. `libvduse`
also pulls in `linux-headers` include paths that the standalone subproject build
does not set up on this configuration.

`-no-pie` is required. The musl.cc toolchain defaults to producing position
independent executables, so the final link is a shared object. Objects compiled
with `-fno-pie` then fail with

```
relocation R_ARM_MOVW_ABS_NC against `a local symbol' can not be used
when making a shared object; recompile with -fPIC
```

QEMU's `--disable-pie` does not propagate a `-no-pie` link flag here, so it is
passed explicitly.

`-lcrypto` is required. `hw/arm/ipod_touch_pke.c` calls the OpenSSL bignum API
(`BN_mod_exp`, `BN_bn2hex`, `BN_new`, ...) directly rather than through QEMU's
crypto abstraction, so `libcrypto` must be on the link line. This matches the
upstream build instructions, which also link `-lcrypto`.

`-Wl,-rpath-link,/opt/armv7/usr/lib` lets the linker resolve the `DT_NEEDED`
entries of the sysroot shared libraries (`libpcre2-8.so.0`, `libffi.so.8`)
without adding them to the link explicitly.

## Build

```
cd /root/qemu-build
ninja -j4 qemu-system-arm
```

The resulting binary is `/root/qemu-build/qemu-system-arm`. It is statically
linked against QEMU's own code and dynamically linked against the target system
libraries:

```
libz.so.1  libpixman-1.so.0  libSDL2-2.0.so.0  libglib-2.0.so.0
libintl.so.8  libcrypto.so.3  libc.musl-armv7.so.1
```

All of these except SDL2 are present in the flashed host image. SDL2 is copied
from the sysroot to `/usr/lib` on the device; see `documentation/young2-host.md`.

## Deployment

```
scp qemu-system-arm root@172.16.42.1:/usr/local/bin/
```

The binary is placed in `/usr/local/bin` so it survives alongside the launch
script and does not collide with any package-managed path.
