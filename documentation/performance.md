# Performance baseline

## Test configuration

| Item | Value |
| --- | --- |
| Emulator source | commit `e22c2cb751`, binary `qemu-system-arm-baseline`, 15,681,316 bytes stripped |
| Guest | iPod touch 2G `n72ap_v1` NAND, Darwin 9.4.1 (XNU 1228.7.27), OS 5F138 |
| Guest RAM | 128 MB |
| Host RAM | 461 MB total |
| Host swap | 256 MB swapfile on the data partition, unused during the run |
| CPU governor | `performance`, 1.0 to 1.2 GHz |
| Emulator priority | `nice -20` |
| Display path | SDL over X11, X screen 320x480, no window manager |
| Touch path | `ts-xlate.py` to `/dev/input/touchscreen-xlate`, X evdev, SDL, emulated multitouch |

The host also runs `Xorg` and `ts-xlate.py`. The translator must be started before
Xorg, because evdev only attaches a device that exists when X initialises.

## Boot timeline

Measured from process launch on the physical device.

| Marker | Time |
| --- | --- |
| Darwin kernel banner | under 287 s |
| `BSD root: disk0s1` | under 287 s |
| `configd` running | under 287 s |
| `configd` sets hostname | 625 s |
| display switched off by the guest (`ApplePinotLCD::_lcdEnable: enable: 0`) | 1364 s |

## Steady state

After userspace starts, the guest stops making progress and settles into a
kernel spin loop.

| Metric | Value |
| --- | --- |
| Host CPU used by the emulator | 98.7 percent of one core |
| Emulator resident memory | 192,552 kB |
| Host memory available | 159 MB with the emulator running |
| Swap in use | 0 |
| NAND reads during the spin | none |
| Framebuffer content | black |

## Where the time goes

Guest program counter sampled 12 times through the QEMU monitor during the
spin. All samples fall in four consecutive addresses:

```
c05c7f90  c05c7f94  c05c7f98  c05c7df8
```

The return address `c05c7df8` identifies the caller. The instructions at that
address are:

```
c05c7de4  ldr r3, [r4]
c05c7de8  mov r0, r4
c05c7dec  mov r1, #0
c05c7df0  mov lr, pc
c05c7df4  ldr pc, [r3, #0x36c]
c05c7df8  tst r0, #0x400
c05c7dfc  bne 0xc05c7de4
```

This is a virtual call to `readReg(0)` in a loop that repeats while bit 10 of the
returned value is set. The accessor at `c05c7f90` is

```
c05c7f90  ldr r3, [r0, #0x58]
c05c7f94  ldr r0, [r3, r1]
c05c7f98  bx lr
```

so the polled address is the device base held at offset 0x58 of the driver
object. At the time of sampling the base was `0xea5e5000` and the value read was
`0x00000503`, which has bit 10 set.

`gva2gpa 0xea5e5000` resolves the base to physical `0x3d800000`, which is
`MIPI_DSI_MEM_BASE`. The guest is therefore waiting for the MIPI DSI controller
to report that its high-speed transmit clock is no longer running.

`hw/arm/ipod_touch_mipi_dsi.c` returns a constant for that register:

```
case 0x0:
    return 0x103 | rDSIM_STATUS_TxReadyHsClk;
```

The bit is asserted on every read and never clears, so the guest waits forever.
This accounts for the sustained 98.7 percent host CPU and for the guest never
reaching SpringBoard.

## SpringBoard reached and responsive

The guest does reach SpringBoard, and it responds to touch, provided input keeps
arriving. Booting with a pointer event every 45 seconds produced a screenshot of
the home screen with the Safari, Calendar, Mail and Contacts icons, the dock, and
an `Edit Home Screen` dialog. The dialog is the guest's own reaction to the
repeated taps, so the full input chain reaches iOS and SpringBoard acts on it.

Without that input the guest switches the display off
(`ApplePinotLCD::_lcdEnable: enable: 0`) and enters the DSI wait described below,
from which it does not return. The visible difference between a working and a
stuck guest is therefore the display-off transition, not touch delivery and not
frame rate.

## Attempted changes and results

Two models of the DSI transmit-clock state were tested on the device and both
were rejected.

Clearing the status bit unconditionally, so the register reads `0x103`, let the
guest leave the spin loop, but iBoot then read DSI registers at invalid offsets
(`0x00de5ed4`, `0x14`, `0x08`) and stopped early in boot, well before the kernel.
The bit is load-bearing during the panel initialisation that iBoot performs.

Clearing the bit only after the guest writes the screen-off value to register
`0x8` removed the spin. Host CPU fell from 99 percent to 26 percent and sampled
program counters moved into userspace at `0x3144ed40` and `0x31454b3c`, so the
guest was running real code instead of polling. However the panel then stayed on
the Apple logo for the whole observation window and never reached SpringBoard, so
the change traded a spin for a display regression and was also rejected.

Neither change was kept. The only DSI change retained is the removal of the
per-write `fprintf`, which emitted one line per register write and serves no
purpose during normal operation.

Both experiments were attempts to derive the bit from a single hardcoded event.
The maintained upstream tree already models it properly, by deriving
`TxReadyHsClk` from the high-speed clock request that the guest itself writes to
`CLKCTRL` bit 31. That behaviour was backported and does remove the loop; see
`documentation/display.md`.

## Touch path

Touch delivery was verified independently of rendering. A synthetic pointer
event at the centre of the 320x480 window produced, in the emulator's own
output:

```
INPUTTRACE DOWN raw_x=16383 raw_y=16383 fx=0.499969 fy=0.500031
INPUTTRACE UP   raw_x=16383 raw_y=16383 fx=0.499969 fy=0.500031
```

The full chain works: X pointer, SDL, the emulated touch device and the guest
multitouch driver. The absence of a visible reaction while the guest is spinning
is therefore a guest-side effect, not a missing input path.

## Current bottleneck

The DSI transmit-clock wait described above is fixed and no longer occurs. See
`documentation/display.md` for the register model, the upstream comparison and
the backport.

After the panel is switched off the guest still settles into a kernel wait, but
in different code: samples land at `0xc017b614`, `0xc0188cb0` and `0xc0066750`
with return addresses `0xc017b904` and `0xc01882c0`. That is not an MMIO poll of
the DSI shape. Host CPU still reaches about 99 percent in that state, so the
display-off path passes the DSI handshake but does not yet return a usable guest.

While the guest is running rather than stuck, host CPU sits well below a full
core, so the emulation speed itself is not the limiting factor for interaction.

## Next steps

1. Determine what the guest waits for after the panel is switched off. The
   sampled addresses are in kernel text, not a device base, so the next step is
   to identify the function at `0xc017b614` and `0xc0188cb0` rather than to add
   more device state.
2. Re-measure the boot timeline and interaction latency once that wait is
   resolved.
3. Revisit the per-frame framebuffer invalidation, which copies 320x480 pixels on
   every refresh.

2. Re-measure the boot timeline once the wait terminates.
3. Revisit the per-frame framebuffer invalidation, which copies 320x480 pixels on
   every refresh, after the guest reaches SpringBoard.
