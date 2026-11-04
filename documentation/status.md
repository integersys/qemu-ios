# Project status

## Scope of the current task

Establish a clean, lightweight postmarketOS host on the Young 2, rebuild
QEMU-iOS for ARMv7, and confirm the previously working iPod touch 2G guest still
boots. Sleep/wake, performance tuning, display redesign, audio, networking and
OpenGL are out of scope for this task.

## Verified results

| Item | State | Evidence |
| --- | --- | --- |
| postmarketOS boots on the device | verified | boots to a shell, `sshd` reachable over RNDIS at 172.16.42.1:2222 |
| Host slimmed | verified | 444 MB used / 550 MB free, 131 MB RAM used, 105 processes |
| Data partition usable | verified | 2.1 GB ext4 with 300,048 inodes, mounted at `/mnt/young2-data` |
| X display at 320x480 | verified | `xrandr` reports `current 320 x 480`, depth 24, fb 32 bpp |
| QEMU-iOS builds for ARMv7 | verified | `qemu-system-arm` 8.2.0, 15.5 MB stripped, runs on the device |
| iPod touch 2G machine present | verified | `-M help` lists `iPod-Touch` |
| Guest reaches iBoot | verified | NOR image table in the serial log |
| Guest reaches the XNU kernel | verified | `Darwin Kernel Version 9.4.1 ... RELEASE_ARM_S5L8720X` |
| Guest reaches the NAND filesystem | verified | `FTL Register [OK]`, `BSD root: disk0s1` |
| Guest reaches userspace | verified | `launchd`, `lockdownd`, `configd`, `notifyd`, `SpringBoard` in the task list |
| Guest displays iOS | **not reached** | framebuffer stays black; guest panics at SpringBoard start |
| Sleep/wake behaviour | not investigated | out of scope for this task |

## Current blocker

The guest reaches SpringBoard and then takes a kernel data abort:

```
kernel abort type 4: fault_type=0x3, fault_addr=0xec3fd01c
pc: 0xc04fef28  lr: 0xc04ff6b0  r11: 0xc0b60240
Debugger message: Fatal Exception
```

`0xc0b60240` is the same object that appears earlier in the log as
`AppleMPVDDriver[0xc0b60240]`, and the backtrace contains
`AppleMPVDDriver::setPowerStateGated()`. The fault therefore occurs in the video
decoder driver during a power-state transition, immediately after SpringBoard
starts and before the LCD is programmed.

The display is never initialised, which is why the framebuffer stays black. The
guest does probe LCD registers that the emulator does not implement
(`0x300`, `0x304`, `0x410`, `0x1b18`, `0x1b1c`, `0x1b20`, `0x1b24`) and receives
zero for all of them.

This is a guest-side crash reached through an incomplete emulation path. It is
not a host configuration error: the same binary boots the same guest to the same
point reproducibly.

## Guest memory size

`-m 128` matches the real iPod touch 2G and boots to SpringBoard.

`-m 256` was tried because the upstream instructions use a much larger `-m`. It
exceeds what the host can support: with 461 MB of RAM and a 256 MB swapfile on
eMMC, the host stops responding entirely, the RNDIS gadget stops enumerating and
the device requires a power cycle. Guest RAM must stay at or near 128 MB on this
host.

## Source defects found

`hw/arm/ipod_touch_lcd.c` does not compile as committed: two `fprintf` calls
contain a literal newline inside the format string instead of `\n`. Both were
repaired. Details are in `documentation/build.md`.

## Unresolved questions

- Whether the `AppleMPVDDriver` abort is specific to this NAND image
  (OS 5F138 / XNU 1228.7.27) or would occur with an iOS 3.1.3 image. Only the
  upstream `n72ap_v1` image is available to the project.
- Whether the unimplemented LCD registers are required for the panel to be
  programmed at all, or whether the abort happens first regardless.
- How much of the abort is the known sleep/power issue that this task was asked
  not to investigate.

## Next steps

1. Confirm whether the same abort occurs with an iOS 3.1.3 NAND, which is the
   documented preferred guest target.
2. Instrument the LCD register reads to return plausible values instead of zero,
   and check whether the display initialises before the power-state transition.
3. Only after the guest reaches the interface, revisit the sleep/wake problem.
