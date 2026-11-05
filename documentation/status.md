# Project status

## Verified results

| Item | State | Evidence |
| --- | --- | --- |
| postmarketOS boots on the device | verified | `sshd` reachable over RNDIS at 172.16.42.1:2222 |
| Host slimmed | verified | 444 MB used / 550 MB free, 131 MB RAM used |
| Data partition usable | verified | 2.1 GB ext4 with 300,048 inodes at `/mnt/young2-data` |
| X display at 320x480 | verified | `xrandr` reports `current 320 x 480` |
| QEMU-iOS builds for ARMv7 | verified | `qemu-system-arm` 8.2.0 runs on the device |
| Guest reaches iBoot | verified | NOR image table in the serial log |
| Guest reaches the XNU kernel | verified | `Darwin Kernel Version 9.4.1 ... RELEASE_ARM_S5L8720X` |
| Guest reaches the NAND filesystem | verified | `FTL Register [OK]`, `BSD root: disk0s1` |
| Guest reaches userspace | verified | `configd` and `SCHelper` running in the serial log |
| Guest draws to the framebuffer | verified | monitor screendump shows the Apple logo at 320x480 |
| Kernel abort on SpringBoard start | resolved | `PM notification timeout` and the `AppleMPVDDriver` abort no longer occur |
| SpringBoard interface visible | verified | home screen with icons and dock on screen; guest raised its own `Edit Home Screen` dialog in response to taps |
| Touch input reaches the guest | verified | `INPUTTRACE` events in the emulator output; SpringBoard reacted to them |
| Display-off transition | **open** | guest waits on a DSI transmit-clock bit that is never released; host burns a full core |
| Sleep/wake | not investigated | out of scope for this task |

## The display defect that blocked boot

The AppleMPVDDriver abort was a symptom, not the cause. Commit `bdec002822`
moved the framebuffer invalidation out of `lcd_refresh()` and into
`ipod_touch_lcd_realize()`:

```
-    lcd->invalidate = 1;            in lcd_refresh()
+    s->invalidate = 1;              in ipod_touch_lcd_realize()
```

`lcd_refresh()` only calls `framebuffer_update_memory_section()` when
`lcd->invalidate` is set, and that call binds the console memory section to the
guest framebuffer address in `w1_framebuffer_base`. Binding once in `realize()`
binds it before the guest has programmed the base register, so the section stays
at address 0 and is never rebound. The console reads guest physical address 0
for the rest of the run and the display stays black.

SpringBoard then blocks in its first draw and never acknowledges the
power-management notification, which produced
`PM notification timeout (pid 10, SpringBoard)` followed by the kernel data
abort in `AppleMPVDDriver`.

The invalidation is restored in `lcd_refresh()`, and the memory section is also
rebound whenever the guest writes a new framebuffer base:

```
case 0x24:
    if (s->w1_framebuffer_base != val) {
        s->w1_framebuffer_base = val;
        s->invalidate = 1;
    }
    break;
```

With this change the guest no longer aborts and the framebuffer carries real
guest output. A monitor screendump shows the Apple logo on a 320x480 surface,
which no earlier build produced.

The per-frame invalidation costs a full 320x480 copy per refresh. Rebinding only
when the base changes, without the per-frame flag, was also tested: it removes
the abort but the console stops updating after the first frame. The per-frame
invalidation is therefore kept, and the display cost is a known trade-off to
revisit during performance work.

## Remaining blocker

SpringBoard is reached and stays usable while input keeps arriving. The blocker
is the display-off transition. When the guest switches the panel off it waits on
the MIPI DSI transmit-clock status bit, which the emulation asserts permanently,
and never returns. Host CPU then sits at about 99 percent with no guest progress.

`documentation/performance.md` records the measurement, the program counter
sampling that located the loop, the address translation that identified the
device, and the two one-bit models that were tested and rejected.

## Historical comparison

`D:\qemu` was analysed as read-only evidence. The old source tree did not
survive, but its recorded state, assets, binary and logs did. The conclusion: the
old and current sources are equivalent, the guest assets are byte-identical, the
preserved old binary reproduces the same failure, and the difference that matters
is the LCD invalidation change described above.

## Next steps

1. Implement the MIPI DSI panel state machine so the display-off transition
   completes. A single status bit is not sufficient, as the two rejected
   experiments in `documentation/performance.md` show.
2. Re-measure boot time and interaction latency once that transition works.
3. Revisit the per-frame display copy once the transition is stable.
4. Sleep and wake remain unimplemented and are the same underlying gap as the
   display-off transition.
