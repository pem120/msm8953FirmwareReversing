# MSM8953 TrustZone EL3 research

~research #android #msm8953 #trustzone #reverse-engineering #arm64

> [!abstract]
> Working notes toward code execution in the ARM **EL3 secure world** on an MSM8953
> (Snapdragon 425/625) phone, and an evaluation of EL2.
> All analysis is on the requester's own device.
> No exploit yet — see [[Central finding: TZ dereferences EL1-controlled pointers]]
> for the live lead and [[Two independent problems]] for why it may not pay off.

> [!info]
> Everything below is evidence-backed from the binary unless marked as inference.

## Where this lives

> [!info]
> **Repository:** <https://github.com/pem120/msm8953FirmwareReversing>
>
> `master` carries the full history of this analysis, from the initial `SCR_EL3` finding
> through the `x20` audit and the Firehose `peek`/`poke` discovery. Everything here is
> reproducible: each claim cites a file offset or a virtual address in a named image, and the
> images are either tracked here or recoverable from the stock ROM.

Referenced artifacts, with the binary each address belongs to:

| Address / offset | Binary | Tracked? | Recover from |
| --- | --- | --- | --- |
| `0x86500000` entry, `0x86508350` jump table, `0x86501c20` dispatcher | `tz.mbn` | no | stock sakura ROM, SHA-256 `7f218713…` |
| `0x86500e30` context restore, `0x86501b4c` mailbox consumer | `tz.mbn` | no | same |
| `0x8650247c` mailbox producer (`ec_21_handler`) | `tz.mbn` | no | same |
| `0x3c100` Firehose command table, `peek`/`poke` | `prog_emmc_firehose_8953_ddr.mbn` | **yes** | in this repo, SHA-256 `fc3df4df…` |

> [!tip]
> **Working on this?** The `tz.mbn` addresses are the load-bearing ones. The image is not
> tracked here, but `sakura-stock/images/tz.mbn` from the stock ROM is byte-identical, so
> every address in these notes resolves without pulling anything from an untrusted source.
> Ghidra is already configured for it: 2954 functions, 48 handler bodies created at the
> exception jump-table targets.

## Documents

> [!summary]
> These are the other notes in this set.

- **[[Static attack surface map for tz.mbn]]** — `attack_surface_map.md` — dispatch tables,
  string inventory, bug candidates.
- **[[TrustZone vulnerability inventory for MSM8953]]** — `trustzone_vulnerability_inventory.md`
  — public CVE/QPSA triage for this chip family.
- **[[Flash and delivery paths for MSM8953]]** — `flash_delivery_paths.md` — EDL / Firehose /
  unlock research. Answers whether a modified image can be written at all.

## Target and boot chain

| Image | Arch | Entry | Role |
| --- | --- | --- | --- |
| `sbl1.mbn` | ELF32 ARM | `0x08008010` | Stage-2 bootloader; authenticates and jumps to TZ |
| `tz.mbn` | ELF64 AArch64 | `0x86500000` | **EL3 Secure Monitor (QSEE)** |
| `emmc_appsboot.mbn` | ELF32 ARM | `0x8f600000` | App bootloader (high-level `fastboot`) |

Chain: `PBL -> sbl1 -> tz -> emmc_appsboot -> boot.img`

`tz.mbn` self-identifies as `TZ.BF.4.0.5-202044`, built **2019-05-17**.

> [!warning]
> Raw firmware images are OEM copyrighted and are gitignored. Keep them local.

## EL2 is architecturally unreachable

> [!summary]
> This was the original question ("EL2 or EL3 for KVM/hyp"). Answer: **EL2 does not exist on
> this device** and cannot be reached from Linux without changing EL3 code.

There are exactly 9 `SCR_EL3` accesses in `tz.mbn`. Three write the constant
`SCR_EL3 = 0xE00` at `0x865001a0`, `0x865010f4`, `0x8650128c`. Decoding the two bits that
decide the question:

- **bit 0 = `NS` = 0** — secure state
- **bit 1 = `HCE` = 0** — HVC traps to EL3; EL2 is bypassed

The EL3 exception handler toggles only those two bits:

```
; returning to the non-secure world (set NS):
86501c58  bl  0x86507fd0      ; read SCR_EL3
86501c5c  orr x0, x0, #0x1    ; NS = 1
86501c60  bl  0x86507fc8      ; write SCR_EL3

; common tail before ERET (clear HCE):
865026a0  bl  0x86507fd0      ; read SCR_EL3
865026a4  and x0, x0, #-0x2   ; HCE = 0
865026a8  bl  0x86507fc8      ; write SCR_EL3
```

**`HCE` is never set to 1 anywhere in the image.** TZ explicitly clears it before every
`ERET` to a lower EL. A lower-EL HVC therefore traps to EL3, never to EL2.

Corroborated independently by the kernel tree:
`arch/arm64/boot/dts/qcom/msm8953.dtsi` declares `compatible = "arm,psci-1.0"` with
`method = "smc"` — PSCI owned by an EL3 monitor, with no EL2 stage in the chain.

> [!warning]
> Bits 9–11 of `0xE00` are also set. These are the hypervisor-restriction bits
> (`HVD`/`HVI` on my reading) and would lock EL2 down further, but I could **not** confirm
> the bit names from tooling — `search_data_types` for `SCR_EL3` returns nothing, so Ghidra
> is not carrying named bit-fields. Bits 0 and 1 are the ones that matter and are certain.

A research subagent pushed back that an HVC handler alone does not prove a hypervisor is or
isn't present. That is fair in general, but the `SCR_EL3.HCE` evidence above is direct and
does not depend on which files ship. Cheap cross-check if desired: see whether the device has
a `hyp` partition.

> [!tip]
> **Consequence:** to get EL2 you must modify TZ (set `HCE=1` and boot the kernel at EL2) or
> exploit TZ. There is no Linux-side or kernel-config route.

## Exception model

- `vbar_el3 = 0x86504000` (literal at `0x86500378`).
- 16 vector stubs, `0x80` bytes apart. `VBAR_EL3` points at **code**, not a pointer table.
- SMC from AArch64 EL1 enters at `+0x400` → `0x86504400`.
- Generic entry stub (`0x86504400`) saves all GP registers plus `SP_EL0`/`SP_EL1` to stack,
  then `br`s via a literal at `0x86504468` → **`0x865012d0`** (the common handler).
- `EC = (ESR_EL3 >> 26) & 0x3f`.
- Jump table at VA `0x86508350` (file offset `0xb350`), entries `0x00`–`0x38`, entry `0x39`
  is a `0xdeaddead` sentinel. **49 unique handler targets.**

## EC decoding correction

> [!warning]
> The static-analysis report labelled index `0x17` as "SMC from AArch64 lower EL" and `0x2a`
> as "HVC". **That is wrong.** Using the AArch64 ESR encoding:

| EC | Actual meaning | Jump-table target |
| --- | --- | --- |
| `0x11` | SMC from AArch32 lower EL | `0x86502310` |
| `0x12` | **SMC from AArch64 lower EL** | `0x86502324` |
| `0x17` | Permission fault from lower EL, AArch64 | `0x86502368` |
| `0x18` | Instruction Abort from lower EL, other (translation) | `0x86502388` |
| `0x2a` | System register access from EL2, AArch64 | `0x8650257c` |
| `0x2c` | Data Abort from EL1, AArch64 (translation) | `0x86501c50` |

This also corrects my own earlier assumption: I had read `0x2c` as the SMC path because it
sets `SCR_EL3.NS` before returning. It is actually a **data abort from EL1** — and that is
exactly consistent with the observed behaviour, since a fault taken from EL1 must return to
EL1 in the non-secure state. The EL2 conclusion is unaffected.

## Central finding: TZ dereferences EL1-controlled pointers

> [!success]
> This is the most important result so far.

Every handler in the dispatch table indexes through **`x20`**:

```
86501c94  ldr x8, [x20]
86501c6c  ldr x10, [x20, #0x8]
86501d0c  ldr w0, [x20, #0x10]
86501d10  ldr w1, [x20, #0x18]
86501d1c  str x13, [x20]
```

**No handler body ever assigns `x20`.** Across all 19 writes to `x20` in the image, the only
one inside the handler range `0x86501c20`–`0x86502680` is a stack restore at `0x86502060`.
The relevant restores are:

```
8650138c  ldp x20, x21, [sp, #0xa0]
865013f4  ldp x20, x21, [sp, #0xa0]
86501478  ldp x20, x21, [sp, #0xa0]
```

Offset `0xa0` is exactly where the generic entry stub stored `x20`/`x21`:

```
86504438  stp x20, x21, [sp, #0xa0]
```

**Therefore `x20` holds the EL1 value of register `x20` at the moment of the trap** — i.e. a
fully attacker-chosen pointer — and EL3 dereferences it with no ownership or range check.

Concrete example, `ec_19_handler` (`0x86502398`), decompiled:

```c
uVar1 = *(uint *)(x20 + 1) & 0x3fffffff;   // read from [x20+8] — attacker address
uVar3 = 1;
if (uVar1 == 0x2000102 || uVar1 == 0x2000109 || uVar1 == 0x2000305) goto ok;
if (uVar1 + 0xfdfffaff < 2) goto ok;
if (uVar1 == 0x2000902) goto ok;
uVar3 = 0;
ok:
  *x20 = uVar3;                            // write 0 or 1 to [x20] — attacker address
```

> [!caution]
> **This particular path is not an arbitrary write of a controlled value.** It writes only
> `0` or `1`, and that is the documented SMC return-value ABI — Linux passes a result-buffer
> pointer in a register and TZ fills it in.

The masked FID values sit in the `0x02000xxx` SMCCC range; the mask `0x3fffffff` strips
bits 30/31, so the original SMC32/SMC64 and standard/vendor encoding is not recoverable from
the comparison alone.

> [!tip]
> Consequence: searching the binary for a literal `0xC200xxxx` finds nothing *by
> construction*. Do not read that as "there is no vendor SMC ABI".

> [!question]
> **The actual hunt is therefore:** find any handler that does *more* with an `x20`-derived
> pointer than compare-then-write-a-constant — a copy, an index, a length, a nested
> dereference. The FID whitelist is currently the only thing standing between an
> attacker-controlled address and an EL3 read/write.

## False positive killed: FUN_86503304

The attack-surface report ranked this first, at "High/medium" — handler `0x02000502` loads
`x0=[x20+0x10]`, `w1=[x20+0x18]` and writes through the caller-derived pointer. Ghidra
rendered it as:

```c
cVar1 = FUN_8650335c();          // no argument visible  -> looks unchecked
if (cVar1 == 1) { lock(); *param_1 = param_2; unlock(); }
```

The instructions show otherwise:

```
86503314  mov x20, x0      ; pointer kept, NOT dereferenced
86503318  mov w19, w1      ; the value to write
8650331c  bl  0x8650335c   ; x0 = pointer value passed straight through
86503340  str w19, [x20]   ; write only if the gate allowed it
```

And the gate compares that pointer against 18 entries at `0x86508520` which are **physical
IPC handle addresses**, not magic numbers:

```
0x0b1880b0  0x0b1880b8   0x0b1980b0  0x0b1980b8
0x0b1a80b0  0x0b1a80b8   0x0b1b80b0  0x0b1b80b8
0x0b0880b0  0x0b0880b8   0x0b0980b0  0x0b0980b8
0x0b0a80b0  0x0b0a80b8   0x0b0b80b0  0x0b0b80b8
0x0193d100                  0x01c46000
```

> [!success]
> That is a correct allowlist of known-safe write targets. **Candidate #1 is a false
> positive** — killed early rather than three days in.

## EL3 context save and restore

`FUN_86500d70` (save) / `FUN_86500e30` (restore) handle the full EL1+EL3 context:

```c
spsr_el1  = p[0];    elr_el1   = p[1];
ttbr0_el1 = p[6];    ttbr1_el1 = p[5];
tcr_el1   = p[7];    sctlr_el1 = p[0xb];
elr_el3   = p[0x17]; scr_el3   = p[0x18];
spsr_el3  = p[0x19]; cptr_el3  = p[0x1a];
```

Taken at face value this would hand an attacker the `ERET` target and `SCR_EL3`. I checked
the caller chain rather than assuming — it is **not** directly EL1-reachable:

```
FUN_86502f50:
  lVar1 = FUN_86502c2c(0);        // per-CPU saved-context block (TZ-owned)
  FUN_86502cac(lVar1 + 0x108);    // restore from TZ's OWN save area
  FUN_86502cac() also null-guards: param==0 -> infinite loop
```

The struct comes from TZ's own per-CPU save area, written at exception entry by the save side.
So this is the normal exception-return path.

> [!tip]
> The bug hunt target is anything that lets EL1 influence that save block, or reach
> `FUN_86500e30` with attacker data.

## Target confirmed: sakura (Redmi 6 Pro)

The firmware being analysed is now positively identified. `sakura-stock/images` is a stock
Xiaomi fastboot ROM, build `d1s-sakura-india-p-stable-symbols-20200508`, and all three images
hash **byte-identical** to the ones in this repo:

| Image | Size | SHA-256 (first 16) | Match |
| --- | --- | --- | --- |
| `tz.mbn` | 1,531,776 | `7f21871366071836` | identical |
| `sbl1.mbn` | 401,492 | `e463227e8c345e47` | identical |
| `emmc_appsboot.mbn` | 689,564 | `fc57d7097087e0c1` | identical |

> [!success]
> This validates the whole static analysis: the `tz.mbn` under Ghidra is genuine, unmodified,
> stock sakura firmware, not a dump from some other device.

## The Firehose programmer exposes peek and poke

> [!danger]
> **This overturns the earlier conclusion that delivery was blocked.**

`sakura-stock/images/prog_emmc_firehose_8953_ddr.mbn` (399,552 bytes, ELF32 ARM, entry
`0x8009330`) is a device-specific, factory-signed programmer for this exact handset. Its
Firehose command table sits at file offset `0x3c100` and contains:

```
configure, program, firmwarewrite, patch, setbootablestoragedrive, emmc, power,
benchmark, read, getstorageinfo, getcrc16digest, getsha256digest, erase, peek, poke
```

`peek` and `poke` are **both recognised commands**. In a Firehose programmer these are
arbitrary physical memory read and write, driven from the host over the XML interface, with no
fuse or signing check behind them. The dispatcher has a `WARNING: Ignoring unreco[gnised
command]` fallback, so the table is the full accepted set.

Build identity strings confirm the lineage: `OEM_IMAGE_VERSION_STRING=c3-miui-ota-bd116.bj`,
`OEM_IMAGE_UUID_STRING=Q_SENTINEL_{7702A6B6-2304-4FA9-96CB-2485B8B12FA8}_20200508_1626`, build
id `8953A-JAADANAZA-40000000`, with DDR init paths for `pm8953_pmi8950`, `pm8953_pmi8940` and
`pm8953_pmi8937`.

### Why this matters

Public research by Aleph Security on Qualcomm EDL programmers covers **this exact SoC
generation**: they obtained and reverse-engineered PBLs for MSM8917, MSM8937 and MSM8953, and
tested Xiaomi MSM8953 programmers from the Note 4, Mi 5X, Mi A1 and Mi Max 2. Their results
differed by device — on MSM8937 (Nokia 6) they demonstrated a **complete secure-boot bypass**,
and on MSM8953 they demonstrated programmer-level access and a working memory-execution path.

The technique is not an unauthenticated Sahara flaw. It is a **post-authentication compromise
of an accepted programmer**: obtain privileged execution inside the Firehose image, then remap
the PBL's page tables, copy the PBL into writable memory, remap the PBL virtual address onto
the clone, patch the verification, and continue booting. No physical ROM is touched and no
unsigned image is ever presented to the real PBL.

> [!success]
> **Verified in this programmer: both are unrestricted, and neither is behind the
> authentication gate.** See [[The sakura Firehose programmer is an arbitrary memory primitive]].

## The sakura Firehose programmer is an arbitrary memory primitive

> [!danger]
> `peek` and `poke` in `prog_emmc_firehose_8953_ddr.mbn` perform **unauthenticated arbitrary
> physical memory read and write at any 32-bit address**, with no range check. This is the
> precondition for the Aleph PBL-remapping secure-boot bypass, and it is present in the loader
> that ships with the device.

### The command dispatch has an auth gate, and peek/poke are outside it

`FUN_0803ee58` is the Firehose command dispatcher. It has an authentication concept — there is
a literal string *"ERROR: Only nop and sig tag can be recevied before authentication."* (sic)
and an auth flag at `DAT_08060618`. That flag gates an early, specific command set only. After
that, commands fall through a flat sequence of `FUN_0804eae2(&DAT_08071778, <name>)` string
comparisons with **no flag check at all**:

| Command | Handler | Auth-gated? |
| --- | --- | --- |
| `configure` | `FUN_0802c990` | no |
| `program` | `FUN_0802e720` | no |
| `firmwarewrite` | `FUN_0802d140` | no |
| `patch` | `FUN_0802db50` | no |
| `setbootablestoragedrive` | `FUN_0802f2e8` | no |
| `power` | `FUN_0802e5e4` | no |
| `benchmark` | `FUN_0802c59c` | no |
| `getstorageinfo` | `FUN_0802da18` | no |
| `getcrc16digest` / `getsha256digest` | `FUN_0802d4f4(1/0)` | no |
| `erase` | `FUN_0802ce98` | no |
| **`peek`** | **`FUN_0802e194`** | **no** |
| **`poke`** | **`FUN_0802e38c`** | **no** |
| (unrecognised) | warn + `FUN_0804c564(1)` | — |

### peek — arbitrary read

`FUN_0802e194` parses `SizeInBytes` and `address64` from the Firehose XML, then:

```c
FUN_0803239c("Using address %p", local_440);
...
FUN_080064b0(auStack_428, 0x200, "0x%02X ", *(undefined1 *)(local_440 + uVar5));
```

The only rejections are a string-parse failure (`"Failed to get address"`) and
`local_440 == 0 || size == 0` (`"Invalid parameters"`). **There is no range, alignment or
permission check on the address whatsoever** — any non-zero 32-bit value is dereferenced.

### poke — arbitrary write

`FUN_0802e38c` parses `address64`, `SizeInBytes` and `value`, then:

```c
else if (size <= 8) {
    FUN_0803239c("Using address %p", puVar8);
    do {
        *puVar8 = (char)uVar11;     // write at an arbitrary address
        puVar8 = puVar8 + 1;
        uVar11 = uVar11 >> 8;       // little-endian, up to 8 bytes
    } while (...);
}
else {
    FUN_0803239c("Cannot handle size in bytes greater than %llu", 8);
}
```

Same story: the only guards are a parse failure, a null/zero parameter, and the 8-byte size
cap. **No address validation.**

### What follows from this

If this programmer is accepted by the device's PBL — which is untested — then the chain is:

1. Enter EDL 9008.
2. Upload this factory-signed programmer (it is signed for exactly this SoC and OEM).
3. `peek`/`poke` give arbitrary physical memory read/write from the host.
4. Per the Aleph technique, use that to copy the PBL into writable memory, remap the PBL
   virtual address onto the copy, and patch its verification.
5. Boot an attacker-supplied `tz.mbn`, which the patched PBL now accepts.

No unsigned image is ever presented to the unmodified PBL, and no physical fuse is touched.

> [!question]
> **What is still unverified.** Three things, and only the first needs hardware:
>
> 1. Whether this device's PBL actually accepts this programmer, and whether it applies any
>    post-Sahara check of its own. This is the gating question for the whole approach.
> 2. Whether any address range is W^X or otherwise protected at the point `poke` writes. The
>    handlers do not check, but hardware may.
> 3. Whether the remap approach needs `peek`/`poke` to reach a specific physical window.
>
> Items 2 and 3 are answerable only at runtime. Item 1 is answerable with `adb reboot edl` or
> a test-point entry and one Sahara handshake.

## The x20 surface: no bounds check, read and written in one call

> [!danger]
> This is the concrete counterpart to CVE-2019-2318 and CVE-2020-3619 in this specific
> `tz.mbn`. Both are read straight out of the binary.

> [!success]
> **The `x20` model is confirmed.** Not inferred any more — proven by an exact save/restore
> pair around EL1's register file, with no intervening write on the path that dereferences it.

The generic exception entry at `0x86504400` spills the whole lower-EL register file onto EL3's
stack:

```
0x86504410  stp x0,  x1,  [sp]
0x86504414  stp x2,  x3,  [sp, #0x10]
   ... through ...
0x86504438  stp x20, x21, [sp, #0xa0]      <- EL1's x20 preserved here
0x8650443c  stp x22, x23, [sp, #0xb0]
0x86504440  stp x24, x25, [sp, #0xc0]
0x86504444  stp x26, x27, [sp, #0xd0]
0x86504448  stp x28, x29, [sp, #0xe0]
0x8650444c  mrs  x0,  sp_el0
0x86504450  stp x30, x0, [sp, #0xf0]
0x86504454  mrs  x0,  sp_el1
0x86504458  str  x0,  [sp, #0x100]
0x8650445c  ldr  x16, 0x86504468           ; -> 0x865012d0, the common handler
```

and the common handler restores it before returning:

```
0x8650138c  ldp  x20, x21, [sp, #0xa0]      <- restored here
0x86501390  ldp  x22, x23, [sp, #0xb0]
0x865013a0  add  sp, sp, #0x108
0x865013a4  eret
```

**`[sp,#0xa0]` is a symmetric round-trip.** The stub writes EL1's `x20` there and the handler
reads it back from the same slot. Nothing between the `SMC` trap and the `x20` dereference at
`0x86501c94` assigns `x20` — the only write in the entire handler region is the stack restore
itself. Therefore `x20` at the point of use is EL1's `x20`, fully under lower-EL control.

> [!tip]
> This closes the last gap. `tzprobe` existed only to check this empirically, so it is no longer
> load-bearing. Combined with the two findings below, the `x20` surface is fully characterised:
> the caller chooses the pointer, TZ dereferences it unvalidated to `0x198`, and the buffer is
> both read and written within a single dispatch.

### x20 is dereferenced at 11 offsets, up to 408 bytes, and never validated

Every `[x20 + N]` access in the image was enumerated. The offsets touched are:

```
0x000  0x004  0x008  0x010  0x018  0x020  0x030  0x048  0x0f8  0x100  0x198
```

The highest is **`[x20,#0x198]`** — 408 bytes past a pointer the lower EL supplied.

Searching for any validation of `x20` returns essentially nothing:

- `cmp x22, x20` at `0x86501aec` — a register-to-register compare, not a range check
- `cmp w9/w8, #0x200, LSL #12` at `0x86501938` / `0x865018d0` — compares against `0x200000`,
  nothing to do with `x20`

**There is no length argument, no range check and no masking on `x20` anywhere.** TZ
dereferences a lower-EL pointer at up to 408 bytes past its start with no idea how big it is.
That is the CVE-2019-2318 shape — a TrustZone arbitrary read driven by a non-secure caller.

> [!danger]
> **A caller-controlled pointer now lands in TZ-owned state.** Found by a parallel pass over
> `0x86501a00`–`0x86501c00`. This is the write direction that was missing.

### `tz_copy_caller_struct` puts 72 bytes of EL1 data into TZ context

An exception handler reachable from a lower-EL SMC copies a caller-supplied structure into a
TZ-owned per-CPU context block:

```asm
0x86501a74  mov  x20, x0              ; x0 = saved EL1 x1, an arbitrary caller pointer
0x86501a7c  bl   0x86502c2c           ; -> TZ-owned per-CPU context (dest in x22)
0x86501ac8  ldr  x12, [x20, #0x48]    ; EL1 value
0x86501acc  stp  x12, x13, [x22, #0x1c0]   ; -> context[0x38], a controlled pointer
0x86501b00  mov  w8, #0x9
0x86501b04  ldr  x17, [x20], #0x8      ; 9 quadwords from EL1
0x86501b0c  str  x17, [x22], #0x8      ; into TZ context offsets 0x00..0x40
0x86501b10  b.ne 0x86501b04
```

So **EL1 words 0–7 land at `context[0x00..0x38]` and word 8 at `context[0x40]`**, with
`context[0x38]` additionally taken from `source+0x48`.

> [!success]
> **`context[0x40]` is subsequently dereferenced as a pointer** — case `0x11` in the same
> handler uses it as a load address. EL1 therefore chooses the address TZ reads from. That is an
> attacker-controlled read, and it is the first genuinely EL1-influenced pointer in TZ-owned
> state this analysis has produced.

### Two other things in the same range

**Chosen-page memory attributes.** The caller pointer is page-aligned and a length derived from
`align(param_1 + 0x1050) - align(param_1)`, then handed to an attribute-management routine with
flags `0x8065`, then again with `0x800`:

```asm
0x86501a94  and  x19, x20, #0x1000        ; page-align the caller pointer
0x86501aa8  mov  w3, #0x8065
0x86501ab8  bl   0x8650532c                ; first transition
0x86501b34  mov  w3, #0x800
0x86501b44  b    0x8650532c                ; second transition
```

The caller influences the start and the page count. **The semantics of `0x8065`/`0x800` are not
yet resolved** — that is the highest-value unknown now. If those flags carry no-execute or
secure-attribute bits, this is a chosen-page attribute primitive.

**The architectural address-translation instruction is reachable.** For cases `0x2c`/`0x2d`,
`[x20+8]` is used as the operand of `AT S12E1R` / `AT S12E1W` — the caller supplies the
translation-table address that EL3 then reads.

> [!caution]
> **Not yet a write primitive.** The copied context fields are EL1-controlled, but whether any
> of them later reaches a control register, a function pointer, or a secure write outside this
> range is unproven. That is the next question.

## Data flow is TZ → EL1, not a TOCTOU (and that is better)

> [!success]
> **The one open question is settled, and not in the way CVE-2020-3619 describes.** There is no
> time-of-check/time-of-use window here — and that removes the need for one.

The most promising-looking block was re-examined instruction by instruction:

```
0x86501f50  ldr  x9, [x20, #0x30]      ; read from the EL1 buffer
0x86501f58  b.ne 0x86502040            ; ... and branch on it
0x86501f88  ldr  x4, [x24]             ; value comes from a TZ-owned object
0x86501f9c  str  x4, [x20]             ; ... and is written to the EL1 buffer
0x86501fa0  ldr  x5, [x24, #0x8]
0x86501fa8  str  x5, [x20, #0x8]       ; second word, same direction
```

**The written value is loaded from `x24`, never from `x20`.** And `x24` is never itself derived
from `x20` — across the whole handler region it appears only as a frame restore
(`0x86501b54` / `0x86502068`, `stp`/`ldp x24, x23, [sp, #0x20]`) and as an operand of TZ-owned
accesses.

So the flow is strictly one-directional: **TZ-owned memory → EL1 buffer**. TZ never re-reads
the EL1 buffer between validating something in it and writing through it. **CVE-2020-3619's
TOCTOU shape does not apply to this code.**

> [!tip]
> **Why that is a better result, not a worse one.** Without a TOCTOU, an attacker does not need
> to win a race. The EL1-supplied values — `[x20,#0x10]`, `#0x18`, `#0x20` as arguments, and
> `[x20,#0x30]` as a path selector — **steer** the dispatch, and the offsets are attacker-chosen
> and unvalidated out to `0x198`. That is a controlled read primitive into TZ state with no
> timing requirement at all, which is a far more reliable thing to build on than a race that has
> to be won against a concurrent mapper.

> [!caution]
> What this does **not** give is the direction we actually want. The write into EL1 memory is
> fed from TZ-owned state, so it is a *read* of secure data rather than a write of chosen data.
> Turning that into arbitrary write needs either a different handler where an EL1-supplied value
> flows *into* the stored word, or a TZ-owned object whose contents the caller can influence.
> That is now a well-defined search rather than an open question.

### The same buffer is read and written within a single dispatch

In the SMC ID-dispatch window (`0x86501c94`–`0x86502050`) the caller's buffer is both sourced
and written, with values flowing between offsets:

```
; arguments read from the caller's buffer
0x86501d94  ldr  x18, [x20]
0x86501d9c  ldr  x3,  [x20, #0x8]
0x86501da4  ldr  x4,  [x20, #0x10]
0x86501dac  ldr  x5,  [x20, #0x18]
0x86501db4  ldr  x6,  [x20, #0x20]

; ... and later, results written back into it
0x86501e88  ldr  x0,  [x20, #0x10]      ; re-read
0x86501e8c  ldr  w1,  [x20, #0x18]      ; re-read
0x86501e94  str  xzr, [x20]             ; write

; and the most interesting block:
0x86501f50  ldr  x9,  [x20, #0x30]      ; read a further offset
0x86501f9c  str  x4,  [x20]             ; x4 came from [x20,#0x10]
0x86501fa8  str  x5,  [x20, #0x8]        ; x5 came from [x20,#0x18]
```

Values read from one offset are written to another, and the same offsets are re-read later in
the same call. **That is the CVE-2020-3619 shape** — non-secure memory touched more than once
during one TrustZone operation, which is what turns a read primitive into a write.

> [!caution]
> **What is and is not established.** Established statically: EL1 controls `x20` at the trap
> (save/restore pair, above); TZ dereferences it at 11 offsets up to `0x198` with no validation;
> and there is no TOCTOU, because the written value comes from TZ-owned state rather than being
> re-read from the EL1 buffer. **Not** established: a *write of caller-chosen data*. The EL3
> into-EL1 direction is a read of secure state, not a write primitive. Closing that gap means
> finding a handler where an EL1-supplied value flows into the stored word.

## The route to EL2, and why it runs through EL3

> [!danger]
> **EL2 is downstream of EL3 on this device.** There is no shortcut. The chain is fixed:

```
1. Arbitrary physical memory R/W   <- HAVE IT: Firehose peek/poke, verified live
2. Code execution in the programmer
3. Remap / patch the PBL
4. Boot a modified tz.mbn          <- EL3 achieved
5. Patch SCR_EL3 so HCE = 1
6. HVC now routes to EL2           <- EL2, finally
```

Steps 5 and 6 are the actual EL2 enablement, and they are trivial *once* step 4 lands —
`SCR_EL3` is written as a constant in three places (`0x865001a0`, `0x865010f4`,
`0x8650128c`) and the handler explicitly clears `HCE` before every `ERET`, so both would need
patching in a modified `tz.mbn`. All the difficulty is upstream of that.

> [!warning]
> **Honest expectation-setting.** Aleph Security completed this chain on MSM8937 (Nokia 6). On
> MSM8953 they demonstrated code execution and PBL extraction on `mido` but **did not complete
> the secure-boot bypass** — that is the published high-water mark for this SoC. On MSM8917 their
> PBL route failed at flash initialisation. So this is a genuine research effort, not a
> known-good recipe, and it may not land on sakura.

### What blocks each step right now

| Step | Blocker | Needs |
| --- | --- | --- |
| 2 | Programmer's EL and page-table layout unknown; WX pages not located | Device in EDL, Firehose memory analysis |
| 3 | PBL page set and MMU init behaviour unknown | Steps 1-2 done |
| 4 | Whether a patched PBL is enough, or SBL1 also resists | Step 3 done |
| 5-6 | — | Nothing, once EL3 exists |

### Ruled out as routes to EL2

- **`/dev/mem`** — `CONFIG_STRICT_DEVMEM=y`; reads of the secure region EFAULT.
- **In-kernel inspection** — the secure world is not a System RAM region, and `/proc/iomem` is
  redacted by `kptr_restrict`.
- **SMC from a kernel module** — attempted twice; both attempts faulted at
  `0xffffffffffffffff` (EC `0x25` DABT, FSC `0x06` level-2 translation fault). Cause not yet
  established; the first version's fault was self-inflicted (wrote `SP_EL0` to a kernel
  `vzalloc` address), the second did not touch `SP_EL0` and faulted identically.
- **Replacing `tz.mbn` on flash** — write gate is open (`devinfo+0x18 == 1`) but PBL/SBL
  signature verification still rejects the image at boot.
- **Bootloader unlock** — locally token-gated, and unlock does not lift the critical-partition
  restriction.

## Live result: the gate is open

> [!success]
> Tested on the device in authenticated EDL. **The PBL accepts the stock sakura programmer with
> no authentication, and `peek` works as an arbitrary physical memory read.**

```
$ lsusb | grep 05c6
05c6:9008 Qualcomm, Inc. Gobi Wireless Modem (QDL mode)

$ edl --loader=.../prog_emmc_firehose_8953_ddr.mbn
main - Device detected :)
main - Mode detected: firehose
```

No account, no token, no post-Sahara challenge. The stock programmer is accepted as-is, so
every concern raised in [[Flash and delivery paths for MSM8953]] about PK hashes and OEM
authorisation turned out not to apply to this device.

### The decompilation predicted the device's behaviour

Peeking address `0x0` was refused by the programmer itself:

```
firehose - [LIB]: Error: ... <data><log value="Invalid parameters" /></data>
```

That is exactly the guard in the decompiled `peek` handler — *"Invalid parameters"* is emitted
when `local_440 == 0 || size == 0`. The static analysis and the live device agree.

### Arbitrary physical read confirmed

```
$ edl --loader=... peekhex 0x1000 32
b'2c008de528108de54400000a010053e30190a0e32d9084052c608de22d008415'
```

Little-endian, that decodes as plausible ARM code (`e58d002c` = `str r2,[sp,#44]`,
`e58d1028` = `str r8,[sp,#40]`). An arbitrary 32-bit address was read with no range check,
exactly as `FUN_0802e194` shows.

### Write path accepted, not yet proven to change a value

`pokehex 0x1000 2c008de528108de5` — writing the eight bytes **already present** — completed
with exit 0, no *"Invalid parameters"*, and the follow-up read showed the bytes unchanged. That
proves the programmer accepts and executes the write path, but it is a semantic no-op.

> [!question]
> **Still to do:** a *value-changing* write, to prove a value actually lands. That genuinely
> modifies device memory, so it wants a deliberate choice of address. A read-modify-write
> (peek, poke a different pattern, read back, restore) on an address that is known to be data
> rather than executing code would close it.

### Partition layout (from Firehose `printgpt`)

| Partition | Offset | Length |
| --- | --- | --- |
| `sbl1` | `0x00180000` | `0x80000` (512 KiB) |
| `sbl1bak` | `0x00200000` | `0x80000` |
| `aboot` | `0x00c00000` | `0x100000` (1 MiB) |
| `dip` | `0x00e00000` | `0x100000` |
| **`tz`** | **`0x01000000`** | **`0x200000` (2 MiB)** |
| `tzbak` | `0x01200000` | `0x200000` |
| `devinfo` | `0x01800000` | `0x80000` |

There are also four custom partitions `bk1`–`bk4` of type `EFI_LINUX_DAYA`, which is not a
standard GUID and is worth identifying.

The `tz` partition is 2 MiB and `tz.mbn` is 1.46 MiB, so a modified secure-world image would fit
without resizing.

> [!warning]
> This does not remove the need for [[A read-then-write primitive via a cross-CPU mailbox]]. The
> two are independent: a TZ bug gives code execution *inside* the secure world, while the
> Firehose angle gives host-driven memory access *outside* it. A Firehose win would make the TZ
> bug unnecessary for most practical purposes — which is worth weighing before investing more
> in the bug hunt.

## SBL1 image authentication is a dead end

> [!summary]
> A full pass over `sbl1.mbn` found **no authentication bypass**, and discovered why:
> SBL1 does not hold the keys. This closes the "attack the boot chain directly" option and
> concentrates the remaining effort on the Firehose route.

`sbl1.mbn` (401,492 bytes, ARM32, entry `0x08008010`, code at VA `0x08005000` + file offset
minus `0x3000`) contains **no RSA, no ECDSA, no SHA implementation, no public-key modulus, no
certificate chain and no root hash**. The only `SHA1`/`SHA256` strings are diagnostic labels at
file offsets `0x514e2` and `0x514fa`.

Authentication is delegated through indirect objects and function tables:

```asm
080220bc: bl   0x08022e48      ; obtain a security object
080220c0: ldr  r0, [r0, #4]
080220c2: ldr  r1, [r0, #0x28]  ; vtable entry
080220c6: blx  r1               ; invoke it
```

The same shape appears on the per-segment path at `0x0802242e`. The actual crypto lives below
SBL1 — most consistently, in a PBL-backed secure-boot service with its root trust in ROM or
fuses.

> [!danger]
> **This is the important consequence:** there is nothing to patch. The usual "find the
> signature check and NOP it" approach is unavailable, because the check is not in this image.
> Rewriting `sbl1.mbn` on disk does not weaken authentication, and there is no embedded key to
> replace or disable.

### What SBL1 *does* do defensively

It validates extensively before loading, which argues against a malformed-header attack:

- Range registration with addition-overflow checking at `0x0802138c`
- Per-segment containment and wraparound rejection at `0x08024f6e` (via `MVN`/`CMP`/`BHS`)
- Interval overlap detection at `0x08021400`
- 64-bit-style paired arithmetic with `SUBS`/`SBCS`/`BHS` before the copy at `0x080226d4`
- Authenticate-then-continue structure at `0x08022430`–`0x08022446`, where a null return
  aborts the load

### The one candidate worth a fuzzing slot

```asm
08022400: ldrh r0, [r6, #4]      ; 16-bit count
08022402: ldr  r1, [r6, #0x2c]   ; record size
08022404: mul  r0, r0, r1        ; 32-bit product
08022408: uxth r7, r0            ; TRUNCATED to 16 bits
08022412: bl   0x0802138c        ; span registered for range validation
```

The **truncated** span is what gets registered as a validated range. If a crafted image can
make `count × record_size` wrap such that the low 16 bits are small while the real span is
large, and a later copy uses the untruncated value, that is a classic truncation bug. However
the copy path independently re-checks with 64-bit paired arithmetic, so this is a fuzzing
target, not a demonstrated bypass.

Separately, **no explicit small cap** on the segment count was found before the walk begins;
it is bounded only indirectly by the downstream validators.

> [!question]
> **Anti-rollback is the softer spot.** At `0x08024f14` SBL1 compares an incoming version
> against a stored value at `[record + 0x34]` and writes it back if newer. The backing store
> was not resolved — it is *not* visibly a fuse read. If that record lives in a partition rather
> than hardware, a downgrade attack becomes plausible: reflash an older but **correctly signed**
> component. That is not an unsigned-image bypass, but it can surface a component that predates
> a later fix. Worth resolving before writing SBL1 off entirely.

## ABOOT fastboot: what unlock actually buys

> [!summary]
> `emmc_appsboot.mbn` (689,564 bytes, ARM32, entry `0x8f600000`) implements legacy RSA boot
> verification, **not** AVB, and its unlock path is gated on a token plus the device serial
> rather than a plain mutable bit.

### The command table

Name/handler pairs alternate from VA `0x8f672190`:

| Command | Handler | | Command | Handler |
| --- | --- | --- | --- | --- |
| `flash:` | `0x8f633c70` | | `oem unlock` | `0x8f631c20` |
| `erase:` | `0x8f634030` | | `oem lock` | `0x8f631bc0` |
| `continue` | `0x8f631554` | | `oem device-info` | `0x8f62d90c` |
| `reboot` | `0x8f62da50` | | `oem edl` | `0x8f62df08` |
| `reboot-bootloader` | `0x8f62da98` | | `set_active` | `0x8f62dd2c` |

> [!tip]
> **`oem edl` exists at `0x8f62df08`.** That is the clean way into the Firehose path for
> [[The sakura Firehose programmer is an arbitrary memory primitive]] — no test points, no
> key-combo guesswork. It also means live test 1 does not need the device to be rebooted by
> hand.

### Unlocked mode skips boot verification

There is a literal string at VA `0x8f672ff8`:

```
Device is unlocked! Skipping verification...
```

Combined with legacy verifier strings — `platform/msm_shared/boot_verifier.c`,
`OEM_KEYSTORE/VERIFIED_BOOT_SIG`, and `RSA_KEY` / `PUB_KEY` / VB signature errors — this is a
**pre-AVB signature scheme**, and unlocked mode bypasses it. There are no `vbmeta`, `vbmota` or
`flashing` literals, so the "flash a `vbmota` with verification disabled" trick that works on
some devices is **not** indicated here. Unlock would give unsigned `boot.img`, which is the
lk2nd/pocketboot capability already in hand — not new ground on its own.

### The protected-partition list, and the answer

> [!danger]
> **Settled: unlocking does NOT permit flashing `tz`.** Read out of the code, no device
> required.

`FUN_8f62d67c` is the protected-name check. Its real loop bound was mis-rendered by the
decompiler; the instructions give it away:

```asm
8f62d69c  ldr  r4, [0x8f62d6e8]     ; table base
8f62d6a0  add  r6, r4, #0x30        ; end = base + 48 = 12 entries
8f62d6a8  ldr  r1, [r4, #4]!        ; load, post-increment
8f62d6ac  bl   0x8f642364           ; strcmp(name, entry)
8f62d6b4  beq  <match>              ; return 1 on match
8f62d6b8  cmp  r4, r6
8f62d6bc  bne  0x8f62d6a4
```

> [!warning]
> **Correction.** The twelve protected names, read from the table at VA `0x8f672218`, are:
>
> ```
> aboot, rpm, tz, sbl, sdi, sbl1, xbl, hyp, pmic, bootloader, devinfo, partition
> ```
>
> `sbl1` **is** present (entry 6, VA `0x8f66a4dc`). An earlier pass reported it missing and that
> claim is wrong — the table starts one slot later than assumed, which is why the first entry
> resolved to a code address. Every boot-chain partition is protected.

### Why unlock does not lift the restriction

The flash path in `FUN_8f633310` reads two separate fields from a struct at `0x8f68b920`:

```asm
8f633400  ldr  r3, [r6,#0x10]     ; unlock flag
8f633404  cmp  r3, #0x0
8f633408  bne  0x8f63341c         ; unlocked -> skip the first protected check
8f63340c  bl   0x8f62d67c         ; locked  -> check the list
8f633418  beq  0x8f633754         ; not protected -> allow
...
8f63341c  bl   0x8f62ae48
8f633420  cmp  r0, #0x1
8f633424  ble  0x8f633434         ; refuse
8f633428  ldr  r3, [r6,#0x18]     ; the OTHER flag: "critical"
8f63342c  cmp  r3, #0x0
8f633430  beq  0x8f63361c
...
8f63361c  bl   0x8f62d67c         ; protected list checked AGAIN
8f63362c  movw r0, 0x400c         ; "Critical partition flashing is not allowed"
```

The decisive point is what `oem unlock` actually writes. At `0x8f631c88`:

```asm
8f631c80  mov  r0, #0x0           ; selector 0
8f631c84  mov  r1, #0x1           ; value 1
8f631c88  bl   0x8f631a3c
```

`FUN_8f631a3c` dispatches on the selector: `0` writes the field at `+0x10`, `1` writes the field
at `+0x18`. `oem lock` calls it as `(0, 0)`. **Neither ever writes `+0x18`.**

So the unlocked path skips the *first* protected check, but the `critical` flag at `+0x18`
remains zero, control reaches `0x8f63361c`, and the list is consulted again — where `tz` is
found and refused with *"Critical partition flashing is not allowed"* (VA `0x8f67400c`).

> [!success]
> **Consequence:** the cheap route is closed. Unlocking still skips boot-image verification
> (unsigned `boot.img`, which lk2nd already does) but buys nothing on `tz`, `sbl1` or `aboot`.
> Reaching EL3 still requires either the Firehose arbitrary memory primitive or a genuine
> secure-boot bypass. That is a genuinely useful negative — it was the cheapest remaining path,
> and it is not there.

## CORRECTION: this device is not protected, and I misread the branch

> [!danger]
> **The conclusion above is wrong.** I misidentified `0x8f633434` as a refusal path. It is not.
> Re-derived against the live `devinfo`, the actual state of this device is that critical
> partitions are **not** blocked.

### What I got wrong

`0x8f633434` loads a pointer and calls `FUN_8f642610`, which I assumed printed an error. It is
plain `strlen` — it walks a string counting bytes to NUL. The following instruction pair is a
**partition-name comparison**, not a message:

```asm
8f633434  movw r0, 0x396c        ; VA 0x8f67396c = "avb_custom_key"
8f63. .  bl  0x8f642610          ; r0 = strlen("avb_custom_key") = 13
8f633440  movw r1, 0x396c
8f633448  cpy  r2, r0
8f63344c  cpy  r0, r5            ; r0 = the partition being flashed
8f633450  bl  0x8f6426c8         ; compare(name, "avb_custom_key", 13)
8f633458  beq  0x8f6335f8
```

`"avb_custom_key"` is a *partition name*. Both the `beq 0x8f633434` branches and the
`critical != 0` fall-through land on a dispatcher, not on a refusal.

### The live `devinfo` state

Read over Firehose (`edl r devinfo devinfo.bin`, 8 MiB). The magic is `ANDROID-BOOT!` — hyphen,
not underscore, which is why a string search for the underscore form finds nothing.

```
+0x00  ANDROID-BOOT!...
+0x10  01 00 00 00   -> 1     the unlock/"tested" flag
+0x18  01 00 00 00   -> 1     the "critical" flag
+0x60  msm8953-MSM8953_DAISY2.0_20200508...
```

**`+0x18` is already 1.** The refusal string *"Critical partition flashing is not allowed"*
lives at `0x8f63361c`, which is only reachable via `beq 0x8f63361c` — i.e. **only when the
critical flag is zero**. On this device it is one, so that branch is dead.

### Corrected control flow

```asm
8f63341c  bl   0x8f62ae48        ; keymaster TZ-app probe
8f633420  cmp  r0, #0x1
8f633424  ble  0x8f633434        ; ==1 -> "avb_custom_key" dispatcher (not a block)
8f633428  ldr  r3, [r6,#0x18]    ; critical flag
8f633430  beq  0x8f63361c        ; only here -> "Critical partition flashing is not allowed"
                                  ; else fall through to 0x8f633434
```

`FUN_8f62ae48` probes the string `"keymaster"` (VA `0x8f66bcc8`) via `FUN_8f607270`, memoising
1 or 2 at `0x8f68b910`. The surrounding strings — `generate_Token_failed`, `verify`,
`compare_ok`, `compare_fail`, `sn_ok`, `sn_fail`, `Failure to load TZ app: lksecapp` — confirm
this is the secure-token verification path, the same one `oem unlock` uses.

> [!success]
> **Net effect:** on a device where `devinfo+0x18 == 1`, the protected-partition refusal is
> unreachable. Aleph reported the same `0x10`/`0x18` fields on the Xiaomi Note 5A as the lock
> bits that gate critical bootloader state — and on this device they are already set, so that
> particular unlock step is moot rather than blocked.

> [!question]
> **Two things to confirm before acting on this.**
>
> 1. Whether fastboot `flash:` to `tz` is in fact accepted. The cheapest safe test is to flash
>    the **stock** `tz.mbn` back to the `tz` partition — writing identical content, semantically
>    a no-op like the poke test. A refusal proves the restriction is live; success means the
>    write path is open.
> 2. The `devinfo` build string reads `msm8953-MSM8953_DAISY2.0_20200508...`. That is a
>    **shared platform identifier for the MSM8953** in that ROM, not a per-handset marker — it
>    appears on the sakura. The target is a Xiaomi Redmi 6 Pro (`sakura`); `daisy` is the
>    Redmi 6A / Mi A2 Lite, the Android Go A/B variant of the same platform. A previous note
>    here read `DAISY2.0` as evidence the handset was a daisy; that was wrong, and nothing
>    depends on it.

## Device identity: sakura

> [!success]
> The target is a **Xiaomi Redmi 6 Pro, codename `sakura`**. The stock ROM used throughout is
> `d1s-sakura-india-p-stable-symbols-20200508`, and the images in this repo are **byte-identical**
> to it, so every result in these notes is on-target:

| File | Size | SHA-256 (first 32 hex) | Match to stock |
| --- | --- | --- | --- |
| `tz.mbn` | 1,531,776 | `7f21871366071836a2fcbda0969621c6` | identical |
| `sbl1.mbn` | 401,492 | `e463227e8c345e47f4f0a64aac75a081` | identical |
| `emmc_appsboot.mbn` | 689,564 | `fc57d7097087e0c12ef83117f2e75313` | identical |
| `prog_emmc_firehose_8953_ddr.mbn` | 399,552 | `fc3df4df9472cfec21ecba444cf324df` | — |
| `rpm.mbn` | 174,468 | `4f1a0bc6616f9e16b22a5dddcaf721ad` | — |
| `devcfg.mbn` | 40,028 | `2fe876124f8912b51203eb522d7b6b17` | — |
| `keymaster64.mbn` | 271,480 | `4c34aa326931d0ca32e1af46dfdfe819` | — |
| `lksecapp.mbn` | 57,352 | `7909ee8d443b7161f46f3276063cc5fe` | — |

A side observation worth keeping: the Firehose programmer that the PBL accepted is the
`sakura` one, and it was accepted without authentication. Since `daisy` is the same MSM8953
platform, that also suggests programmer authentication is platform-level rather than
per-handset — which is why a cross-device loader is worth keeping in mind.

### Unlock is not a mutable bit

`oem unlock` at `0x8f631c20` calls the token verifier at `0x8f631c44`:

```asm
0800...: bl  0x8f60a7f4          ; verify token
        cbnz r?, <abort>         ; nonzero -> "Token verification failed"
        bl  0x8f631a3c          ; success -> set unlock state
```

The failure string is *"Token verification failed"* at VA `0x8f673810`, printed at
`0x8f631c50`. The verifier `0x8f60a7f4` allocates and zeroes 128-byte buffers, derives and
compares token material, and calls `0x8f60a1cc` and `0x8f6426c8`. That is substantive
cryptographic work, not an unconditional state write.

State is then persisted through `0x8f631a3c` and `0x8f63174c`.

> [!danger]
> **The critical detail: no server call is visible.** The token is verified **entirely locally**,
> against the device serial number and a secret. That changes the shape of the problem. Unlock
> is not gated on a Xiaomi cloud entitlement at the moment it executes — it is a local
> cryptographic check, which means the question becomes whether the secret and the token
> derivation can be recovered from the image, and whether a valid token can be minted locally.
>
> That is a well-trodden area of Android device research, and it is the cheapest remaining
> route to an unlocked device. It does not by itself grant EL3, but it feeds the
> "does unlocked permit `flash tz`" question above, which is the one that would.

> [!info]
> Related globals: a struct at VA `0x8f68b920` carries a "tested" flag at `+16`
> (`0x8f633400`) and a "critical" flag at `+24` (`0x8f633428`). Whether the "critical" flag
> gates the protected list was not resolved and is worth checking.

> [!caution]
> `fastboot boot` is present but gated — the string *"fastboot boot command is not available"*
> exists, so it is refused in at least some states. GPT-update code and strings are present but
> reachability was not confirmed.

## Two independent problems

| | Problem | Status |
| --- | --- | --- |
| A | Memory-corruption bug in `tz.mbn` → EL3 code execution | In progress. Read primitive and a mailbox read-then-write found; gate unproven. |
| B | Get that code to survive boot past PBL/SBL | **No longer blocked.** Device-specific signed Firehose programmer is present and exposes `peek`/`poke`. |

> [!info]
> **Correction.** I previously recorded Problem B as blocked, on the grounds that Firehose can
> write eMMC but not bypass PBL/SLB authentication, and that no public route existed. That was
> half right: Firehose alone indeed does not bypass verification, but I had not checked whether
> *this* programmer exposes memory primitives, and I was researching Xiaomi codenames
> generically rather than looking at the loader actually sitting in the project directory. The
> loader was there the whole time.

## Public vulnerability research

See [[TrustZone vulnerability inventory for MSM8953]].

> [!summary]
> No verified AArch64 MSM8937/8953 EL3 exploit PoC exists. This is original RE, not exploit
> reuse.

Notable correction: CVE-2014-9711 is a Websense TRITON XSS and is unrelated to Android;
Towelroot is CVE-2014-3153, a Linux futex bug — neither is an EL3 issue. Closest public work
is CVE-2015-6639 (PRDiag/QSEECOM) and an IPQ40xx QSEE PoC that is explicitly ARMv7-only.

## Ghidra state

- `tz.mbn` imported and analysed; 2954 functions.
- 48 function bodies created at the 49 exception jump-table targets (1 already existed),
  named `ec_<idx>_handler` / `default_handler`; plus `el3_exception_common` at `0x865012d0`.
- Program saved.
- Tooling: `gh` is a dispatcher at `~/.local/bin/gh` routing the five GhidraMCP bridge verbs
  to the skill client and everything else to the GitHub CLI at `/usr/bin/gh`.

## A read-then-write primitive via a cross-CPU mailbox

> [!success]
> This is the strongest result so far. It comes from chasing a single global,
> `DAT_8650ebb8`, that two different exception handlers touch.

### The two halves

**Producer — `ec_21_handler` (`0x8650247c`)** stores an attacker-controlled 32-bit value:

```c
if (FUN_865000d4() + 1 == (ulong)DAT_8650d0a9) {     // doorbell: "CPU+1 is asking"
    DAT_8650d0a9 = 0;                                 // acknowledge
    DAT_8650ebb8 = (uint32_t)*(x20 + 8);               // read from an EL1-chosen address
}
```

`x20` is EL1's `x20` at trap time, so `*(x20 + 8)` is a 32-bit read at an address the caller
picks. `DAT_8650d0a9` is a doorbell naming the requesting CPU.

**Consumer — `FUN_86501b4c` (`0x86501b4c`)** writes that value back out:

```c
uVar3 = (uint32_t)*param_1 & 0x3f00ffff;              // read the FID from the return slot
...
if (uVar3 == 0x04000000) {
    if (DAT_8650ebb8 != 0xdeadbeef) {                 // slot already filled
        *param_1 = (ulong)DAT_8650ebb8;               // <-- 64-bit write
        goto done;
    }
    DAT_8650d0a9 = FUN_865000d4() + 1;                 // otherwise arm the doorbell
}
```

`param_1` is **also** EL1-controlled. `FUN_86501b4c` is called from `el3_exception_common` at
`0x865013b8`, and `x0` is set two instructions earlier by the frame restore:

```
8650135c  ldp x30, x0, [sp, #0xf0]     ; x0 = saved SP_EL0
865013b8  bl  0x86501b4c                ; param_1 = SP_EL0 at trap time
```

So `param_1` is the EL1 stack pointer at the moment of the trap — caller-chosen.

### Why this is more than the SMC return ABI

Everywhere else in this dispatcher, `*x20 = 0` or `*x20 = 1` writes a *constant* into the
caller's return slot. That is the SMC return-value ABI and is harmless. Here the value being
written is **whatever the caller previously asked TZ to read**, and the destination is a
**different, also caller-chosen** address:

- call 1: `x20` -> address A, so TZ reads 32 bits from `A + 8` into the mailbox
- call 2: `SP_EL0` -> address B, so TZ writes those 32 bits (zero-extended to 64) to `[B]`

A and B are independent. That is a 32-bit arbitrary read followed by a 64-bit arbitrary write
of the result.

> [!question]
> The gate is a cross-CPU doorbell: the producer only fires when the trapping CPU's id plus
> one equals the value in `DAT_8650d0a9`. The intended flow is that one CPU arms the doorbell
> and a *peer* CPU fills the slot. Whether a caller can satisfy that gate unilaterally —
> by choosing which CPU traps and what `x20` holds — needs runtime validation. That is the
> open question, and it decides whether this is a usable primitive or an intra-service
> mailbox that happens to be reachable.

> [!caution]
> **Not yet an exploit.** It requires EL1 code execution to set `x20` and `SP_EL0` before the
> SMC, and the mailbox gate has not been shown satisfiable from a single thread. What is
> verified is the data flow and the absence of any ownership check on either endpoint.

## x20 audit: first pass

Systematic decompilation of the handlers that touch `[x20 + N]`. Result so far: **the data
flow is one-directional — EL3 *reads* through attacker-controlled pointers into TZ-owned
memory.** No write back through an `x20`-derived address was found, other than the `0`/`1` SMC
return value already covered above.

| Handler | Address | What it does with `x20` |
| --- | --- | --- |
| `ec_1d` | `0x86502420` | Reads `[x20+0xf8]`, `[x20+0x100]`, `[x20+8]`; stores into a TZ per-CPU block from `FUN_86502c2c(3)` |
| `ec_21` | `0x8650247c` | Reads `[x20+8]`, `+0x10`, `+0x18`, `+0x20`, `+0x40`; copies into a TZ object. Also `DAT_8650ebb8 = (uint)[x20+8]` — an attacker-controlled 32-bit value stored in a global |
| `ec_2a` | `0x8650257c` | Passes `x20[1]` and `x20[2]` (i.e. `[x20+8]`, `[x20+0x10]`) into `FUN_86505478` as two fully attacker-controlled 64-bit values |

Representative, `ec_1d`:

```c
lVar1 = FUN_86502c2c(3);                             // TZ-owned per-CPU block
*(lVar1 + 0xf8)   = *(x20 + 0xf8);                   // attacker addr -> TZ state
*(lVar1 + 0x100)  = *(x20 + 0x100);                  // attacker addr -> TZ state
if (*(long *)(x20 + 8) != 0) { ... }                 // attacker addr, branch
```

> [!tip]
> This is an **arbitrary 64-bit read primitive feeding TZ state** rather than an arbitrary
> write. That is still a genuine finding — EL3 will dereference any address an EL1 caller
> supplies, with no ownership check. The escalation path is whether any of the receiving TZ
> fields is later *used* as a pointer or length. That is the thing to trace next.

### The `ec_2a` chain

`ec_2a` is the most interesting lead, but it is also the deepest:

```
ec_2a (0x8650257c)
  -> FUN_86505478(x20[1], x20[2])        stores both at obj+0x60 / obj+0x68
    -> FUN_86506a1c(obj)                 state-machine setup, steps 1/2/3
      -> FUN_86506a68(obj, step)         walks a linked list at obj+0x28/+0x30/+0x40
        -> FUN_86506b30(obj, node, ...)  512-iteration page-table walker
```

`FUN_86506b30` steps `param_4 += 0x1000` for 0x200 iterations and tests `(uVar2 & 3) == 3`,
which is the ARM PTE-validity test — so it is a page-table/TLB maintenance routine. The two
attacker-controlled values at `obj+0x60`/`obj+0x68` are not visibly consumed within this
window; they are reached later through `param_1` offsets.

> [!caution]
> `ec_2a` is EC `0x2a` = "System register access from EL2, AArch64". If that decode is right,
> TZ contains a handler for traps originating *at EL2* — which is odd given `SCR_EL3.HCE` is
> never set. Either the path is vestigial, or my EC assignment for `0x2a` is wrong. Worth
> resolving, because it bears on the [[EL2 is architecturally unreachable]] conclusion.

### The memory-attribute helper

`FUN_8650532c` is an `SCmMemProtect`-style call: it masks with `0x1ffffffff` (33-bit physical
address space) and forwards start/end/page-count/flags to the memory-subsystem service object.
Its caller at `0x86502548` computes `x2 = (x9 + 0xfff) >> 10` — a page count derived from
caller data. Changing memory attributes on attacker-chosen pages would be a meaningful
primitive on its own.

> [!danger]
> **Net status: no arbitrary write through `x20` has been demonstrated.** What is established
> is that EL3 performs unchecked reads at EL1-supplied addresses and copies the results into
> secure-world state. Turning that into code execution requires finding a receiving field that
> is subsequently dereferenced or used as a length — which is not yet found.

## Live test candidates

> [!info]
> Everything below needs the handset. Nothing here has been attempted. Collected here so the
> device work can be done in one sitting rather than rediscovered piecemeal, and ordered so
> each test either confirms a static finding or kills it cheaply.

Ordered by value per unit of effort. Tests 1–3 are cheap and either confirm the foundation of
all the static work or invalidate it.

### 1. Confirm the SMC argument register — does the x20 model hold?

> [!question]
> Every static finding rests on `x20` being EL1's `x20` at trap time. It is inferred from
> three instructions (no handler assigns it; the restores read `[sp,#0xa0]`; the generic entry
> stub stored EL1's `x20` there), not observed. If this is wrong, the whole x20 analysis is
> void.

Method: kernel module that sets `x20` to a known scratch address, issues an SMC that TZ
answers, and checks whether the value TZ read came from that address. A one-shot confirmation
that makes the rest trustworthy.

### 2. Mailbox doorbell gate — is the primitive usable?

> [!danger]
> This decides whether [[A read-then-write primitive via a cross-CPU mailbox]] is a real
> arbitrary read-then-write or an unreachable intra-service mailbox.

Method: drive the EC `0x21` producer with `x20` pointing at known data, pinned to successive
CPUs, and observe whether `DAT_8650ebb8` gets populated. The gate is
`FUN_865000d4() + 1 == DAT_8650d0a9`; the question is whether one thread can arm
`DAT_8650d0a9` and then satisfy the producer on the right CPU, or whether it genuinely needs a
peer. If satisfiable, follow through the consumer at masked FID `0x04000000` and confirm
`[SP_EL0]` receives the value.

### 3. Which handset is this, and is there a `hyp` partition?

> [!tip]
> Cheapest test in the list, and it settles the [[EL2 is architecturally unreachable]] note.

Method: read the GPT partition list. The codename decides which Firehose loader applies
([[Flash and delivery paths for MSM8953]]), and a `hyp` partition would force a revision of
the EL2 conclusion.

### 4. Runtime confirmation of `SCR_EL3`

Method: any TZ-exposed path that reports the EL3 `SCR_EL3` value, or boot-time tracing. The
static claim is that TZ writes `0xE00` and never sets `HCE`. Worth confirming on the live
device, since a single differing value would undermine the note.

### 5. Enumerate accepted SMC function IDs at runtime

Method: sweep masked FIDs and record which return a result versus `-1`. Gives the real,
device-accepted ABI rather than the static dispatch map, and may surface IDs the static map
misses.

### 6. EDL read/write round-trip

> [!warning]
> Brick risk. Do this only after 1–5, and on a sacrificial partition first.

Method per [[Flash and delivery paths for MSM8953]] phase 4: enter EDL 9008, load a Firehose
programmer for this exact device, print the GPT, read a known partition and compare it
byte-for-byte against the existing `/dev/mem` dump, then write and read back a non-critical
partition. The point is to prove Firehose genuinely writes eMMC rather than accepting XML and
returning misleading success.

> [!caution]
> Test 1 is the one to do first. It is the cheapest test on the list and it either validates
> or invalidates the assumption every other static finding rests on.

## Next steps

- [ ] Audit every handler that touches `[x20 + N]` for anything beyond
      compare-and-write-constant. Prioritise `0x86502420` (uses `[x20+0xf8]`, `[x20+0x100]`,
      `[x20+8]`), `0x8650247c` (offsets `0x8,0x10,0x18,0x20,0x40` copied into an allocated
      object), `0x8650257c` (two caller args into indirect callbacks), and the copy/zero
      helpers `0x8650532c` / `0x86505478` for length validation.
- [ ] Resolve the two 64→32 truncation sites at `0x865025d0` / `0x865025e8` and check whether
      their results feed later range checks.
- [ ] Confirm which handset this is — `daisy` / `sakura` / `whyred` / `rosy` map to different
      devices and different loader availability.
- [ ] Confirm EDL works on the exact handset and read back a partition byte-for-byte against
      the existing `/dev/mem` dump.
