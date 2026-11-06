# Host crashes

The emulator has been observed to die on its own since the first long runs. This
file records what the failure actually is, how it was captured, and what it was
traced to. It is the authoritative record for host-side crashes; the display and
input behaviour around them is in `documentation/display.md`.

## Symptoms before capture

The first captured exit status came from `ipod-supervisor.sh`, which records the
status of every run:

```
Illegal instruction
EXIT_CODE=132
```

132 is 128 + 4, so the process was killed by `SIGILL`. Runs died between roughly
560 and 1580 seconds after launch, with no OOM record, no segfault message, no
core file and no shutdown request in the guest serial log. Dense synthetic touch
input made the failure more likely; a run with no touch input at all survived
over 2000 seconds without dying.

A later run under `strace` died differently:

```
EXIT_CODE=139
```

139 is 128 + 11, so that run was killed by `SIGSEGV`, and it left a core.

Two different fatal signals from the same binary is itself evidence: a fixed,
deterministic bad instruction would always produce the same signal. Varying
signals point at corrupted host state rather than at one bad encoding.

## Capture pipeline

Core dumps are written to the data partition, not to the root filesystem, which
has only about 320 MB free:

```
echo /mnt/young2-data/core.%p > /proc/sys/kernel/core_pattern
ulimit -c unlimited            # in the shell that starts the supervisor
```

`ulimit -c` must be raised in the launching shell; the supervisor inherits it and
the emulator inherits it from the supervisor. Setting `core_pattern` alone is not
enough, and a test process was used to confirm that a core is actually written
before relying on it.

The core is read with a small ARM32 ELF reader, because the device has no GDB:
it reads `NT_PRSTATUS` (registers), `NT_FILE` (mapping table) and `NT_SIGINFO`
(signal and fault address) directly. For a 32-bit ARM core the register block
starts at offset 72 of `NT_PRSTATUS` and holds 18 words: `r0`-`r15`, `cpsr`,
`orig_r0`.

Cores must never be deleted by a run script. The 205 MB core behind the record
below was analysed first and then destroyed by a cleanup line in the run
launcher, before it could be copied off the device; only the fingerprint below
survives. The launcher now archives cores instead of removing them.

## Crash record: SIGSEGV in the multitouch input path

| Field | Value |
| --- | --- |
| Binary | `qemu-known-good`, stripped hash `1f2d7c2a...` |
| Signal | `SIGSEGV`, `si_code` 1 (`SEGV_MAPERR`), `si_addr` `0xb62c2000` |
| Faulting thread | thread 0, the vCPU thread |
| Faulting PC | `0x003b56b4`, inside `qemu-known-good` text |
| Faulting instruction | `strb r6, [r0]` |
| Faulting function | `ipod_touch_multitouch_transfer + 0x5c` |
| Runtime | about 2390 seconds, with one synthetic tap every 180 seconds |
| Host state | no OOM, no swap, emulator RSS about 190 MB |

Register state at the fault:

```
r0=0xb62c2000 r1=0x00d20bdc r2=0x00000000 r3=0x000026e0
r4=0xb3f8b0d0 r5=0x0000000f r6=0x0000000f r7=0x000000eb
pc=0x003b56b4 lr=0x00d20bdc sp=0xb3e661a8
```

The code around the fault, from the deployed binary:

```
3b56a8  ldr r0, [r4, #0x70]     r0 = s->in_buffer
3b56ac  ldr r3, [r4, #0x7c]     r3 = s->in_buffer_ind
3b56b0  add r0, r0, r3
3b56b4  strb r6, [r0]           s->in_buffer[s->in_buffer_ind] = value
```

The member offsets are confirmed from the debug information of a build of the
same source: `cur_cmd` at 104, `out_buffer` at 108, `in_buffer` at 112,
`buf_size` at 116, `buf_ind` at 120, `in_buffer_ind` at 124. The store is
therefore exactly

```c
s->in_buffer[s->in_buffer_ind] = value;
```

with `in_buffer` = `0xb62bf920` and `in_buffer_ind` = `0x26e0` (9952). The target
address `0xb62c2000` is the first byte past an anonymous 20 KB mapping and is not
mapped, which is what produced `SEGV_MAPERR`.

## Classification

The PC is inside the emulator's own text, not inside the generated code buffer
(which is a separate 32 MB `rwxp` mapping, `0xb42b7000`-`0xb62b6000` in this
run). This is the case the project treats as host memory corruption: a
guest-reachable index was used as a host array index without a bound.

The mechanism is visible in the source of that build. A command is started by
allocating a 256-byte input buffer; if the guest abandons a command mid-transfer,
`cur_cmd` stays set and every following SPI byte keeps incrementing
`in_buffer_ind`. Nothing checks that index against the allocation, so the store
walks off the end of the heap buffer and eventually past the mapping.

The maintained upstream tree documents the same defect and the same consequence:
corruption that surfaces far from the device, as a fault inside QEMU's own
structures. The `SIGILL` seen in the other runs is consistent with the same
corruption reaching code or a function pointer instead of a data pointer, but
that has not been captured and remains probable, not verified.

## What the fix is

The experimental binary bounds the store by the allocation size, abandons a
packet whose header checksum fails instead of aborting, and consumes the SPI idle
byte without starting a command, so the byte stream resynchronises instead of
running on. It also removes the two `hw_error()` calls that a guest could reach.

## After the fix: controlled runs

The experimental binary that bounds the input store was run on the device with
core dumps enabled, the performance governor, and one synthetic tap every
180 seconds. It survived

```
6037 s   0 "Killing device"   0 "Could not detect HBPP"   14 taps
RSS 191,904 kB   display switched off by the guest once
```

against a previous worst case of 2392 seconds, and against crash windows that
started at 560 seconds. The digitizer also stayed alive for the whole run, which
the pre-fix binary never managed.

That run still logged 3582 unknown commands, because the guest polls for frames
with `0xEB` and the device only answered `0xEA`; see `documentation/display.md`.
The desync stopped once the guest settled into its post-display-off wait, and it
did not crash the host.

A single long run is not proof. The requirement for calling this closed is
several controlled runs past the previous crash window with the core capture
armed, and a capture of the faulting instruction if one still occurs.


## The LDRD lead

QEMU issue #896 describes `SIGILL` on a Cortex-A7 host while executing generated
code, caused by an `LDRD` whose address register overlapped one of the
destination registers. Upstream fixed it in `76cff100be`, and QEMU 8.2.0
contains that fix.

This tree is QEMU 8.2.0 and contains the guard. In
`tcg/arm/tcg-target.c.inc`, the 64-bit load path refuses to emit the
register-form `LDRD` when the index or base register overlaps the destination
pair:

```c
if ((h.index & ~1) != datalo) {
    tcg_out_ldrd_r(s, h.cond, datalo, h.base, h.index);
    break;
}
if ((h.base & ~1) != datalo) {
    tcg_out_ldrd_r(s, h.cond, datalo, h.index, h.base);
    break;
}
```

The remaining `LDRD` emissions in that file use fixed registers (`r0`/`r1`,
`r2`/`r3`), so they cannot hit the constraint. The 64-bit store path emits
`STRD` without an equivalent guard, but QEMU 10.0.2 does the same, so that is
not treated as a defect here.

The LDRD hypothesis is therefore rejected for this tree on static evidence, and
the captured core places the fault in the device model rather than in generated
code.
