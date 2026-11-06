# Display

## MIPI DSI

The iPod touch 2G drives its panel through a Synopsys DesignWare MIPI DSI host at
`MIPI_DSI_MEM_BASE` (`0x3d800000`). The emulated device is
`hw/arm/ipod_touch_mipi_dsi.c`.

### Register map as implemented

| Offset | Name | Read | Write |
| --- | --- | --- | --- |
| `0x00` | `STATUS` | lane stop bits, clock-lane state, software-reset release | ignored |
| `0x04` | `SWRST` | not implemented | bit 0 records that a reset was requested |
| `0x08` | `CLKCTRL` | not implemented | bit 31 records the high-speed clock request |
| `0x14` | `ESCMODE` | last written value | stored |
| `0x2C` | `INTSRC` | `RxDatDone` | ignored |
| `0x34` | `PKTHDR` | not implemented | packet header, drives the panel-ID reply |
| `0x3C` | `RXFIFO` | panel-ID reply, alternating words | ignored |
| `0x44` | `FIFOCTRL` | `EmptyHSfr` | ignored |

`STATUS` composes its value from state rather than returning a constant:

```
status  = 0x3                       lanes 0 and 1 stopped
status |= clkctrl bit 31 ? TxReadyHsClk (0x400) : StopStateClk (0x100)
status |= swrst_released ? SwRstRelease (0x00100000) : 0
```

### The polling loop

Guest program counter sampling during a stuck boot put every sample in four
consecutive addresses, with return address `0xc05c7df8`:

```
c05c7de4  ldr r3, [r4]
c05c7de8  mov r0, r4
c05c7dec  mov r1, #0
c05c7df0  mov lr, pc
c05c7df4  ldr pc, [r3, #0x36c]
c05c7df8  tst r0, #0x400
c05c7dfc  bne 0xc05c7de4
```

This is `while (readReg(0) & 0x400)`. The accessor loads the device base from
`[dev+0x58]`; at sample time the base was `0xea5e5000`, and `gva2gpa` resolved it
to `0x3d800000`, the DSI host. The read value was `0x00000503`.

The previous implementation returned `0x103 | rDSIM_STATUS_TxReadyHsClk` on every
read, so bit `0x400` was asserted permanently and the guest never left the loop.
The host burned 99 percent of one core with no guest progress, no NAND reads, and
a black panel.

### Upstream comparison

The maintained `samhenrigold/qemu-ios` tree carries a stateful implementation of
this device (about 10 KB against this fork's 2.5 KB). Its `STATUS` read derives
`TxReadyHsClk` from the high-speed clock request in `CLKCTRL` bit 31, and its
comments record this exact failure: reporting the bit permanently set satisfies
panel bring-up but makes shutdown spin forever, wedging the kernel mid
power-down.

The behaviour was backported rather than reimplemented. What was taken:

- the register offsets `STATUS`, `SWRST`, `CLKCTRL`, `ESCMODE` and their bit
  definitions
- `CLKCTRL` bit 31 as the clock request, and `STATUS` mirroring it with
  `StopStateClk` reported in its place when the clock is not requested
- the software-reset release bit, set when the guest writes `SWRST` bit 0
- a device reset that clears the recorded clock request

What was not taken, because this fork does not need it and it could not be
verified here: the ULPS lane modelling, the migration state, the tracing
plumbing, the configurable lane and panel-ID properties, and the DCS power
tracking used by the iPad machine.

The panel-ID reply on `RXFIFO` is unchanged.

### Result

With the backport the DSI polling loop no longer occurs. Sampled program counters
during the same boot stage moved into userspace at `0x3144c0d4` and `0x3145b3dc`,
and the guest rendered the full SpringBoard: status bar, Safari, Calendar, Mail,
Contacts, App Store, and the dock.

The transition that used to spin forever now completes far enough for the guest
to switch the panel off (`ApplePinotLCD::_lcdEnable: enable: 0` is reached).

### What remains

After the panel is switched off the guest still settles into a wait, but in
different code: samples land at `0xc017b614`, `0xc0188cb0` and `0xc0066750` with
return addresses `0xc017b904` and `0xc01882c0`, and the sampled registers point
at kernel text rather than a device base.

Disassembling the caller shows a virtual-call poll, not a register read:

```
c017b8f4  ldr r4, [pc, #0x24]     r4 = address of the global at 0xc02065a8
c017b8f8  ldr r3, [r4]            r3 = *(0xc02065a8)
c017b8fc  mov lr, pc
c017b900  ldr pc, [r3, #0x10]     call the object's vtable entry at +0x10
c017b904  cmp r0, #0
c017b908  beq c017b8f4            retry while the call returns zero
```

The callee is the accessor at `0xc017b60c`:

```
c017b60c  ldr r3, [pc, #12]       r3 = address of the global at 0xc02065b0
c017b610  ldr r3, [r3]            r3 = *(0xc02065b0)
c017b614  ldr r0, [r3, #0x10]     r0 = *(r3 + 0x10)
c017b618  and r0, r0, #1
c017b61c  bx lr
```

So the guest polls bit 0 of a software flag at `*(0xc02065b0) + 0x10`, through
an object held in a second global. Both addresses are kernel data, not device
registers, so this is a guest-internal completion flag and not something device
emulation can satisfy directly.

## Power management

### PMU register file

The PMU at `hw/arm/ipod_touch_pcf50633_pmu.c` implements no register file at all:
every I2C write is discarded and `pcf50633_recv()` returns zero for any register
that is not one of the RTC, charger or identity cases.

The maintained upstream tree keeps a register file and returns the last value the
guest wrote, and its comment records the consequence of the stub behaviour:

> A stateless stub that always returned 0 here caused iOS's sleep sequence to
> never observe the power-state transition it had just requested, making it fall
> through into a reset instead of suspending.

That is a plausible match for the stall above, so the register file was
backported and measured.

Result: the guest booted to userspace, but the stall moved rather than
disappeared. Program counters that previously sat in the flag poll at
`0xc017b8f4` with 99 percent host CPU instead landed in a kernel delay loop at
`0xc005ffac` (`subs`/`bne` around cache-maintenance operations) with 64 percent
host CPU, and the guest survived longer before the emulator exited. The panel
still did not return to SpringBoard.

The change was therefore reverted: it is defensible on upstream evidence, but it
did not produce a verified improvement here, and the last configuration that
demonstrably rendered SpringBoard is the one to keep. It remains the first
candidate to retry once the exit behaviour below is understood.

### Multitouch frame corruption from repeated taps

A keep-alive loop left running across several sessions was tapping the centre of
the screen every 45 seconds. With that loop active the guest logged:

```
AppleMultitouchZ2SPI:  Could not detect HBPP. Response: ...
AppleMultitouchZ2SPI: *** Killing device (#3): Too many errors reading a frame of data ***
AppleMultitouchZ2SPI: kIOReturnError: 6 times
```

The emulated multitouch device does not produce frames the guest accepts when
touches arrive in quick succession, and the driver eventually kills the device.
Repeated blind tapping is therefore harmful, not merely imprecise. Interaction
must be deliberate and sparse, and the emulated frame format is a real defect
worth fixing separately.

### Emulator exits

The emulator exits cleanly at unpredictable points, observed between roughly 560
and 1580 seconds after launch, with no OOM record, no segfault, no core file and
no shutdown request in the guest serial log. Host memory is not exhausted when it
happens. Whether the guest is requesting a power-off that the machine does not
model, or whether the exit comes from the display backend, is unresolved. The
exit status has not yet been captured.


### Debug output

The device previously printed one line per register write and one per
non-status read, both unconditionally. Writes now print nothing, and the invalid
read path uses `qemu_log_mask(LOG_UNIMP, ...)` so it stays silent unless tracing
is enabled.

## Framebuffer

The framebuffer binding fix is unchanged and still required. `lcd_refresh()`
rebinds the console memory section from `w1_framebuffer_base` and sets
`invalidate`, and the base register write also sets `invalidate` when the guest
programs a new address. The single-bind-in-`realize()` behaviour that produced a
black panel must not return.
