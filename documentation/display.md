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

The device also fails without any touch input at all. A run started with no
keep-alive produced, after the digitizer firmware had been downloaded:

```
AppleMultitouchZ2SPI: *** Killing device (#1): Too many errors performing control write ***
AppleMultitouchZ2SPI: kIOReturnError: 5 times
AppleMultitouchZ2SPI:  Could not detect HBPP. Response: 0x00 0x00 ...
```

so the failure is in the command path, not only in frame delivery.

### Multitouch frame ownership and input bound

The frame read path handed the pending frame to `out_buffer` and left
`next_frame` pointing at it:

```
s->out_buffer = (uint8_t *) s->next_frame;
```

Nothing owned the frame afterwards. The next command allocated a fresh scratch
buffer and dropped the pointer, so every touch frame leaked, and every read in
between re-delivered the same stale frame.

The input path is the more dangerous one. A command the guest abandons
mid-transfer leaves `cur_cmd` set, and every following byte keeps incrementing
`in_buffer_ind` with no bound; past 256 bytes that is a linear write off the end
of the heap allocation. The maintained upstream tree records this class of
defect and its consequence - corruption that surfaced far from the device, as a
fault inside QEMU's own translation structures. That is the leading candidate
for the `SIGILL` described above, and it is not yet confirmed.

The same file also aborted the whole emulator on guest-supplied data:
`hw_error()` was called for an unknown report ID and for a bad HBPP header
checksum. A guest probe could therefore kill the process.

Both are fixed in this tree: the frame read clears `next_frame`, a read with no
pending frame returns a well-formed no-finger report instead of zeros, the
response length comes from the frame's own length field, both buffers are
released per command, the input store is bounded by the allocation, and the two
`hw_error()` calls became logged guest errors. The HBPP data length expression
is parenthesised, and a new data packet resets the attention acknowledgement to
the firmware-download status.

### The SPI idle byte

The first build with those changes ran for 1208 seconds, reached the display-off
transition, and logged no `Killing device` and no `Could not detect HBPP` at all;
the same guest on the previous binary killed the digitizer within that window.
The emulator output showed 130 occurrences of

```
ipod_touch_multitouch_transfer Unknown command 0x00!
```

`0x00` is the SPI idle byte, not a command. Treating it as one left `buf_size`
at its previous value, so every idle byte shifted the framing of the following
bytes; the same log held five each of `0xec`, `0xed`, `0xeb`, `0x01` and `0x02`,
which are the desynchronised payload bytes the upstream tree describes as the
alarm that touch is dead or about to be. The idle byte is now consumed without
starting a command.

### The no-finger frame length

With the idle byte handled, the byte stream stayed in sync and the driver got
further: it then reported

```
AppleMultitouchZ2SPI: *** Killing device (#1): Too many errors reading a frame of data ***
```

The no-finger report was being built with no finger slot at all - data length 26,
75 bytes of response reduced to 47. The driver sizes its read from the frame, and
the common frame on the wire has always carried one slot: data length 54, 75
bytes in total. A shorter report leaves the driver reading past the response,
which it counts as a frame error.

The no-finger report now keeps one zeroed slot with `numFingers = 0` and
`fingerDataLen = sizeof(FingerData)`, which is what the maintained upstream tree
does and why it does it.

### The poll command is 0xEB, not 0xEA

A hundred-minute run on the build with the fixes above stayed alive with the
digitizer intact, but the emulator logged 3582 unknown commands in a perfectly
repeating six-byte cycle:

```
eb 01 ec eb 02 ed eb 01 ec eb 02 ed ...
```

That cycle is the guest polling for frames. iOS 3.1.3's `AppleMultitouchZ2SPI`
never issues `0xEA`; it polls with `0xEB`, and the observed sequence is `0xEB`,
then `0xEC` and `0x01`, repeating. The maintained upstream tree records exactly
this and treats `0xEB` as a second frame-read command.

The fork only answered `0xEA`, so every poll was an unknown command, the
response stream shifted by a byte each time, and the driver never received a
frame it could accept. `0xEB` is now handled in the same path, and the response
carries the command byte it was asked with: both checksums cover that byte, so
they are adjusted by the difference rather than recomputed.

This is the difference between "taps produce `INPUTTRACE` lines in the emulator"
and "iOS receives touches", which is why earlier sessions could see SpringBoard
react while later polling produced nothing usable.

### Guest panic at the display-off transition

With the poll command answered, the guest stopped desynchronising (zero unknown
commands over the run) and the digitizer stayed alive, but one boot then took a
kernel abort instead of settling into the wait:

```
ApplePinotLCD::_lcdEnable: enable: 0
+ AppleMPVDDriver[0xc0b60240]::setPowerStateGated()
kernel abort type 4: fault_type=0x3, fault_addr=0xec3fd01c
r0: 0xc0c68540  r1: 0x00000000  r2: 0xec3fd000  r3: 0xffffffff
pc: 0xc04fef28  lr: 0xc04ff6b0  far: 0xec3fd01c
Debugger message: Fatal Exception
```

The fault is in the guest's video driver while powering the panel down, not in
the emulator: the host process kept running and the guest halted in its panic
handler waiting for a debugger. A second run of the same build reached the same
transition without the abort and without ever executing the panel disable, so
this is not deterministic and is not yet attributed to a specific device
register.

This is the next guest-side blocker. It belongs to the power path that the PMU
register file experiment targets, and it should not be mixed with host crash
work.

### Verification state

The changes above are statically verified against the upstream implementation.
On the device, the digitizer now survives its firmware download and stays alive
across taps, which the previous binary did not manage. A screenshot of
SpringBoard reacting to a tap under this build has not been captured yet, so the
input path is not yet physically verified end to end.

### Emulator exits

The emulator does not exit cleanly. The supervisor records the exit status of
every run, and the last captured run reports:

```
Illegal instruction
EXIT_CODE=132
```

132 is 128 + 4, so the process is killed by `SIGILL`, not by a shutdown request
or an allocation failure. There is no OOM record, no segfault, no core file and
no shutdown request in the guest serial log, which matches a signal that the
emulator neither handles nor logs. The exit timing stays unpredictable,
observed between roughly 560 and 1580 seconds after launch.

Two runs bracket the behaviour:

- with a keep-alive delivering touches, the guest reached SpringBoard, switched
  the panel off at 1364 s and the emulator died at 1595 s;
- with no touch input at all, the same binary ran for over 2000 s without dying,
  but never drew SpringBoard.

A `SIGILL` in generated code or in a corrupted data structure is consistent with
the multitouch device's frame-buffer handling, which frees and re-hands the same
allocation; see the frame ownership note below. That is a hypothesis, not a
verified cause, and the faulting address has not been captured yet.


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
