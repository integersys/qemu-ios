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

After the panel is switched off the guest still settles into a kernel wait, but
in different code: samples land at `0xc017b614`, `0xc0188cb0` and `0xc0066750`
with return addresses `0xc017b904` and `0xc01882c0`, and the sampled registers
point at kernel text rather than a device base. That is not the DSI poll, and it
is not an MMIO wait of the same shape. The host still reaches about 99 percent CPU
in that state.

So the display-off path now passes the DSI handshake but still does not return to
a usable guest. Sleep and wake remain unimplemented, as before; this is the same
underlying gap.

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
