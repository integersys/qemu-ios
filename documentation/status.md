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
| SpringBoard interface visible | **not yet verified** | the guest stops after `SCHelper`; no SpringBoard UI observed |
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

The guest reaches userspace and then stops:

```
configd[9]   ... SCPreferences(Embedded AirPort plug-in:com.apple.wifi.plist)
SCHelper[15] <Debug>: processing command "EXIT"
```

After that point the serial log fills with USB OTG register dumps, the guest
performs no further NAND reads, and no SpringBoard interface appears. Host CPU
stays around 45 percent, so the guest is polling rather than blocked on I/O.

The same USB polling appears throughout the historical logs in `D:\qemu` and was
investigated there without a conclusion; `pre-usb-freeze.txt` and
`guest-freeze-serial.txt` contain only raw dumps.

## Historical comparison

`D:\qemu` was analysed as read-only evidence. The old source tree did not
survive, but its recorded state, assets, binary and logs did. The conclusion: the
old and current sources are equivalent, the guest assets are byte-identical, the
preserved old binary reproduces the same failure, and the difference that matters
is the LCD invalidation change described above.

## Next steps

1. Determine what the guest waits for at the `SCHelper` stage. The USB OTG
   register polling is the only activity and is the obvious lead.
2. Confirm whether the same stall occurs with the VNC backend and without the X
   server, which is closer to the historical configuration.
3. Revisit the per-frame display copy once the interface is confirmed.
4. Sleep and wake remain unimplemented.
