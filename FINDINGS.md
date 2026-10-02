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

> [!question]
> Whether *this* sakura programmer is vulnerable is not established — it may or may not share
> lineage with the Note 4/Mi 5X/Mi A1/Mi Max 2 programmers that were tested, and the memory
> map and W^X regions differ per build. But it is now a concrete binary to analyse rather than
> a research question, and it is the highest-value target in the project.

> [!warning]
> This does not remove the need for [[A read-then-write primitive via a cross-CPU mailbox]]. The
> two are independent: a TZ bug gives code execution *inside* the secure world, while the
> Firehose angle gives host-driven memory access *outside* it. A Firehose win would make the TZ
> bug unnecessary for most practical purposes — which is worth weighing before investing more
> in the bug hunt.

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
