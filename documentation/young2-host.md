# Young 2 host environment

The host is a Samsung Galaxy Young 2 (SM-G130HN) running postmarketOS from the
internal eMMC. The host kernel is the vendor 3.10.17CAKLKERNELY2D+ build, reports
`armv7l`, and boots with `mem=512M`. The kernel exposes 461 MB of usable RAM to
Linux.

## Partition layout

The layout below is the state after flashing. Sizes are read from
`/sys/class/block/*/size`.

| Partition | Size | Role |
| --- | --- | --- |
| mmcblk0p11 | 8 MB | boot image (pmOS kernel + initramfs) |
| mmcblk0p17 | 200 MB | untouched vendor partition |
| mmcblk0p18 | 1060 MB | postmarketOS root filesystem |
| mmcblk0p19 | 30 MB | untouched vendor partition |
| mmcblk0p20 | 2344 MB | project data partition (`/mnt/young2-data`) |

The PIT, bootloader, modem, EFS and calibration partitions are not touched by any
step described here. TWRP remains installed as the recovery path.

## Flashed host image

Both artifacts are taken from the archived release in the reference workspace
`C:\Users\user\Desktop\postmarketOS Automatization`.

| Artifact | SHA256 | Target |
| --- | --- | --- |
| `pmos-xfce-boot-SM-G130HN.img` | `b2b252874f7fdb47419e2072ef3135e80ed2fcb3010b7da9fc6db50feb620d5b` | mmcblk0p11, 1800 blocks of 2048 bytes |
| `system_pmos_xfce-SM-G130HN.img.xz` | `de506f6d3b520a79f95c54c58db83ed01370cee246908a9b271f2f9b6ec04006` | mmcblk0p18, decompressed, 1060 MiB |

The uncompressed root filesystem is byte-identical to the pre-wipe working image
in the same archive (`release-work/p18-working.img`), which is why it was chosen
over any other candidate.

Both writes were verified by reading the target region back and comparing
SHA256 against the source.

## Data partition

`mmcblk0p20` held an orphaned Android userdata filesystem. The Android
installation cannot boot on this device because `mmcblk0p11` now contains the
postmarketOS kernel. The partition contents were archived to the host before it
was reused:

```
.local/backups/android-p20-data-2026-11-04.tar.gz   85 MB, 811 entries
```

The partition was then formatted as ext4 for the project:

```
mkfs.ext4 -F -L young2-data -N 300000 -O ^orphan_file,^metadata_csum_seed /dev/mmcblk0p20
```

The two disabled features are mandatory. `mke2fs` 1.47 enables `orphan_file` and
`metadata_csum_seed` by default, and the 3.10 kernel refuses to mount a
filesystem that carries either of them. The resulting filesystem provides
300,048 inodes, which is required because the guest NAND is stored as one file
per page and needs 128,060 inodes.

Mounting is handled at boot by `/etc/local.d/20-young2-data.start`, which is
idempotent and exits successfully when the partition is absent.

## Package and payload removal

The flashed image is the Xfce variant. The following removals were applied.

Removed with `apk del`:

```
xfce4-session xfce4-panel xfce4-settings xfce4-power-manager xfce4-notifyd
xfce4-terminal xfce4-appfinder thunar xfwm4 xfdesktop matchbox-keyboard
```

This also pulled out `polkit`, `polkit-openrc`, `polkit-common` and
`duktape-libs`.

Removed directly, because `apk` treats them as hard dependencies of
`xorg-server`, `postmarketos-base` and `device-samsung-young23g`:

```
/usr/lib/libLLVM.so.23.1        139 MB
/usr/lib/libgallium-26.2.3.so    29 MB
/usr/lib/dri/
```

These files belong to the Mesa software renderer. The host X server runs the
`fbdev` driver with `ShadowFB` and never loads Mesa, so the payload is unused.
The `llvm23-libs`, `mesa*` and `postmarketos-base-mesa` packages therefore remain
registered in the apk database while their renderer payload is absent. The space
is only reclaimed after the processes that still map the deleted files exit.

The apk index cache under `/var/cache/apk` was cleared as well. The host has no
working network route, so the index cannot be regenerated; `apk del` operates
from the installed database and continues to work.

## Measured host state

Measured over SSH on the physical device.

| Metric | Before slimming | After slimming |
| --- | --- | --- |
| Root filesystem used | 761 MB | 444 MB |
| Root filesystem free | 232 MB | 550 MB |
| RAM used | 204 MB | 131 MB |
| RAM available | 256 MB | 329 MB |
| Processes | 125 | 105 |

The root filesystem no longer needs to hold guest data, so the 550 MB of free
space is headroom rather than a hard requirement.

CPU frequency governor is `ondemand` and was left unchanged.

## Swap

`postmarketos-zram-swap` fails on this kernel, so a 256 MB swapfile is used
instead:

```
/mnt/young2-data/swapfile   256 MB
```

It is created and enabled by hand; it is not yet wired into boot. Swapping to
eMMC is slow, so the swapfile is a safety margin for QEMU, not a performance
feature.

## Display and input path

Xorg uses the vendor framebuffer through the `fbdev` driver:

```
Driver "fbdev"
Option "fbdev" "/dev/fb0"
Option "ShadowFB" "true"
Virtual 320 960
```

`/usr/local/bin/start-x-with-handover` starts Xorg on `:0`/`vt1` and then writes
one `pan` value to `/sys/class/graphics/fb0/pan`. Without that write the display
controller keeps scanning a stale buffer after Xorg's mode set and the panel
stays white. The helper waits for the DISPC resume marker in `dmesg` instead of
sleeping a fixed interval.

The resulting X screen is 320x480 at depth 24, framebuffer 32 bpp, which matches
the guest display exactly. No scaling is required between the two.

## Runtime library added

The image does not ship SDL2. The runtime library is copied from the Alpine edge
armv7 sysroot used for the cross build:

```
/usr/lib/libSDL2-2.0.so.0.3200.10
/usr/lib/libSDL2-2.0.so.0 -> libSDL2-2.0.so.0.3200.10
/usr/lib/libSDL2-2.0.so   -> libSDL2-2.0.so.0
```

`ldd` resolves it against `libc.musl-armv7.so.1` only. Every other runtime
dependency of the emulator (glib, gobject, pixman, zlib, libcrypto) is already
present in the image.
