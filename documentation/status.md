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
| Emulator exit | root cause identified | `SIGSEGV` core shows `ipod_touch_multitouch_transfer` writing `in_buffer[in_buffer_ind]` past its allocation; `SIGILL` runs are consistent with the same corruption but were not captured |
| LDRD / TCG encoding | ruled out | QEMU 8.2.0 guard from upstream `76cff100be` is present in `tcg/arm/tcg-target.c.inc` |
| Multitouch input bound | fixed, under test | unbounded write into the input buffer and stale frame hand-over removed; upstream documents the same defect |
| Emulator runtime stability | 6037 s controlled run, no crash | 0 digitizer deaths, 14 taps, RSS 191 MB; previous worst case 2392 s |
| Frame poll command | fixed, under test | iOS 3.1.3 polls with `0xEB`, which the fork never answered |
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

## Remaining blockers

SpringBoard is reached and stays usable while input keeps arriving. Two
independent problems remain.

The first is the display-off transition. When the guest switches the panel off
it waits on a guest-internal completion flag, held in kernel data and polled
through a virtual call; the thread that would set it never runs, so the guest
does not return. This is not a device register that emulation can satisfy, and
the power-management register file that upstream keeps did not remove it.

The second is the emulator itself. It was killed by `SIGILL` (exit status 132)
in some runs and by `SIGSEGV` (exit status 139) in another, which already argues
against a single bad instruction. The `SIGSEGV` run left a core: the fault is
`strb` inside `ipod_touch_multitouch_transfer`, writing
`s->in_buffer[s->in_buffer_ind]` with an index of 9952 against a 256-byte
allocation, past the end of the mapping. The `SIGILL` runs are consistent with
the same corruption reaching code rather than data, but no core was captured for
them. `documentation/host-crashes.md` holds the full record.

The QEMU issue #896 `LDRD` defect does not apply here: this tree is QEMU 8.2.0
and already carries the guard from upstream `76cff100be`, and the remaining
`LDRD` emissions use fixed register pairs.

A run with no touch input at all never reached SpringBoard: it stayed on the
Apple logo for over 2000 seconds while `configd` ran, the USB OTG driver printed
register dumps periodically, and the digitizer killed itself with
`Too many errors performing control write`. Whether that stall is caused by the
digitizer failure or is independent of it is unresolved.

`documentation/display.md` records the MIPI DSI register model, the two one-bit
models that were tested and rejected, and the multitouch frame ownership defect.

## Historical comparison

`D:\qemu` was analysed as read-only evidence. The old source tree did not
survive, but its recorded state, assets, binary and logs did. The conclusion: the
old and current sources are equivalent, the guest assets are byte-identical, the
preserved old binary reproduces the same failure, and the difference that matters
is the LCD invalidation change described above.

## Next steps

1. Measure the multitouch frame ownership fix on the device: the digitizer should
   stay alive after its firmware download and SpringBoard should react to a
   single deliberate tap without the device being killed.
2. Capture the faulting address of the `SIGILL`, by running the emulator under a
   tracer that records `si_addr`, to confirm or reject heap corruption as the
   cause.
3. Identify the guest completion flag that the display-off path waits on, and
   what is supposed to set it.
4. Re-measure boot time and interaction latency, then revisit the per-frame
   display copy.
5. Sleep and wake remain unimplemented and are the same underlying gap as the
   display-off transition.
