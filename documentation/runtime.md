# Running the iPod touch 2G guest

## Guest files

The guest requires three artifacts from the upstream QEMU-iOS release
`n72ap_v1`:

| File | Size | Role |
| --- | --- | --- |
| `bootrom_240_4` | 64 KiB | S5L8720 boot ROM, executed first |
| `nor_n72ap.bin` | 1 MiB | NOR image holding iBoot and the auxiliary images |
| `nand/` | 128,060 page files | NAND contents, one file per page |

`bootrom_240_4` and `nor_n72ap.bin` are single files. The NAND is a directory
tree of `cs0` through `cs3`, each holding `N.page` files of 4160 bytes
(4096 bytes of page data plus 64 bytes of spare). The emulator opens one file
per page access and treats a missing file as an all-zero page.

Because of the file-per-page layout the NAND needs 128,060 inodes. It cannot
live on the 1060 MB root filesystem, which has roughly 54,000 inodes available.
It is therefore stored on the data partition:

```
/mnt/young2-data/ipod/bootrom_240_4
/mnt/young2-data/ipod/nor_n72ap.bin
/mnt/young2-data/ipod/nand/cs0..cs3/*.page
```

The NAND occupies about 1.0 GB of the 2.1 GB data filesystem.

Checksums of the two single-file artifacts, as deployed:

```
55f4d8ea2791ba51dd89934168f38f0fb21ce8762ff614c1e742407c0d3ca054  bootrom_240_4
c2284d53fd45ef430415921cf7017aef03e2352dcd2490cd52799204c252868f  nor_n72ap.bin
```

## Guest version

The NAND corresponds to an iPod touch 2G (board `N72AP`, SoC `S5L8720X`)
running iPhone OS with:

```
Darwin Kernel Version 9.4.1: Sun Aug 10 21:25:25 PDT 2008
root:xnu-1228.7.27~12/RELEASE_ARM_S5L8720X
iBoot-385.22
OS version 5F138
```

This is the image shipped in the upstream `n72ap_v1` release. It is the only
guest image available to the project; no other iOS version was tested here.

## Launch

The launcher is `/usr/local/bin/start-ipod.sh`:

```
-M iPod-Touch,bootrom=/mnt/young2-data/ipod/bootrom_240_4,\
nand=/mnt/young2-data/ipod/nand,nor=/mnt/young2-data/ipod/nor_n72ap.bin
-m 256
-display sdl,gl=off
-serial file:/tmp/ipod-serial.log
-monitor none
```

with `DISPLAY=:0`, `SDL_VIDEODRIVER=x11` and `SDL_AUDIODRIVER=dummy`.

The script also creates `/dev/fb0` and `/dev/tty1` when the kernel has not
populated them, mounts the data partition if needed, and calls
`/usr/local/bin/start-x-with-handover` so the panel follows framebuffer memory
before QEMU starts.

`-cpu` is not specified. The machine declares `arm1176` as its default CPU type,
which matches the S5L8720. The upstream instructions suggest `-cpu max`, which
would override that default; it is not used here.

`-display sdl,gl=off` keeps the SDL backend in software mode. The X screen and
the guest display are both 320x480, so no scaling or colour conversion beyond
the 32-bit copy in `lcd_refresh` takes place.

Guest serial output goes to `/tmp/ipod-serial.log`. Emulator `printf` output goes
to the file the launcher's stdout is redirected to, normally
`/tmp/start-ipod.out`.

## Boot sequence observed

The guest progresses through these stages, each confirmed in the serial log:

```
iBoot parses the NOR images (illb, ibot, dtre, logo, nsrv, bat0, bat1, recm, glyC, glyP)
iBoot loads the kernel from NAND
Darwin Kernel Version 9.4.1 starts
driver registration: multitouch firmware, AppleImage3NORAccess, AppleS5L8720XFMSS
FTL: VFL_Open / FTL Register
BSD root: disk0s1, major 14, minor 1
userspace: launchd, lockdownd, configd, notifyd, SpringBoard
```

Timing on the physical device is slow. Individual driver starts take tens of
seconds; `AppleS5L8900XAES::start` reports 25,494 ms. Reaching SpringBoard takes
roughly 13 minutes of wall clock time with the guest at 128 MB.

The known sleep and wake behaviour was not investigated in this task and remains
open.
