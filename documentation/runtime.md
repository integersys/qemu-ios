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
seconds; `AppleS5L8900XAES::start` reports 25,494 ms. Reaching userspace takes
between roughly 10 and 20 minutes of wall clock time, and the figure varies
between boots.

The kernel data abort that used to follow SpringBoard start is fixed; see
`documentation/display.md`. What remains is a wait after the panel is switched
off, also described there.

## Runtime supervisor

Long runs must not be started by hand. `ipod-supervisor.sh` runs on the device
and is the only supported way to launch the guest during development.

It prepares the host, starts the touch translator and the X server, then launches
the emulator with a monitor socket, a QMP socket, `-no-shutdown` and the guest
serial captured to a file. It records the binary path and its hash, the QEMU PID,
the start time, the governor and free memory before the run, and on exit the exit
code, end time, memory and swap after the run, plus the serial, emulator and QMP
tails and the host `dmesg` tail. Everything lands in `/tmp/ipod-run/`.

```
supervisor.log   run metadata, exit code, captured tails
qmp.log          timestamped QMP greeting, replies and events
serial.log       guest serial output
qemu.out         emulator stdout and stderr
monitor.sock     HMP monitor, used for screendump and register sampling
qmp.sock         QMP endpoint
```

Two details matter when driving it:

- the monitor and QMP sockets live inside the run directory, not at `/tmp`
- the supervisor refuses to start the emulator if the X server did not come up,
  and reports `FAIL_STAGE=xorg_not_running` instead of launching a doomed run

A stale `/tmp/.X0-lock` left behind by a killed X server prevents X from
starting. The supervisor removes it before starting X.

## Known-good and test binaries

Two emulator binaries are kept on the device:

```
/usr/local/bin/qemu-known-good    last build that physically rendered SpringBoard
/usr/local/bin/qemu-test          current experimental build
```

`qemu-known-good` is never overwritten during experiments. A build is promoted
only after it passes the same physical check. The supervisor defaults to
`qemu-known-good` and accepts `QEMU_BIN` to select another.

`qemu-mtfix` is the current test build: `qemu-known-good` plus the multitouch
input-bound and frame-ownership fixes described in `documentation/display.md`.
Its stripped hash is `9396ba2ed0c34f4a867d20001812413a5f3042c64a4ca2fe78a875cfc4f0fffb`.

Later test builds carry the frame-length and poll-command fixes as well:

```
qemu-mtfix2   eb5f32bd...   SPI idle byte consumed
qemu-mtfix3   7f383b70...   no-finger frame keeps one slot
qemu-mtfix4   edd48dc0...   0xEB frame poll answered
```

The matching unstripped twin of the current test build is kept on the
development machine as `/root/qemu-test-unstripped`; it strips to the same hash
as the deployed binary, so a faulting address from a core resolves exactly.

The deployed binary is not reproducible from the current tree. Rebuilding the
checked-out source with the same configure options produced 15,689,508 bytes
against the deployed 15,681,316, and a different hash. The cause is not
understood; a rebuild is therefore a new binary, not a copy of
`qemu-known-good`, and must be verified on the device like any other.

## Fatal signal capture

A run can be started under `strace` so that a fatal signal records its address.
`/usr/local/bin/qemu-strace` raises `RLIMIT_CORE` and wraps the known-good
binary in `strace -f -tt -e trace=none`: syscall tracing stays off, so only
signal-delivery stops are logged.

```
STRACE_LOG=/tmp/ipod-run-crash/strace.log \
QEMU_BIN=/usr/local/bin/qemu-strace \
RUN_DIR=/tmp/ipod-run-crash \
ipod-supervisor.sh
```

A fatal signal then appears in that log as

```
--- SIGILL {si_signo=SIGILL, si_code=ILL_ILLOPC, si_addr=0x...} ---
```

`si_addr` is enough to tell generated code (the anonymous `rwxp` mapping, about
32 MB) from QEMU's own text (`/usr/local/bin/qemu-known-good`, based at
`0x10000` because the build is `-no-pie`). Core dumps go to
`/mnt/young2-data/core.%p`; the root filesystem has only about 350 MB free and a
core of the emulator is larger than that. `prlimit64` can raise `RLIMIT_CORE` on
an already running process when the run was started without it.

No `SIGILL` has been captured this way yet: the runs started under the wrapper
did not die.

## Touch input

Touch reaches the guest through `ts-xlate.py`, which converts the type-B
multitouch device into a synthetic device that X's evdev driver accepts. The
translator must be running before X starts, because evdev only attaches a device
that exists at initialisation time.

The translator originally forwarded every `ABS_MT_TRACKING_ID` from the source
device, so a repeated identifier or a repeated release produced a second tracking
identifier for a slot that was still active, which X reported as
`libevdev: BUG: Device "young2-ts-xlate" received a double tracking ID`. It now
emits a press only on a real transition and a release only while a contact is
active. This is statically verified; confirming it needs a physical touch,
because synthetic pointer events from `xdotool` bypass the translator entirely.

Do not run continuous tap loops. A keep-alive that tapped the centre of the
screen every 45 seconds was left running across sessions and drove the guest's
multitouch driver into `Too many errors reading a frame of data` and
`Killing device`. Any keep-alive must be sparse, deliberate and stopped before
interaction testing.

