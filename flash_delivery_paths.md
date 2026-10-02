# MSM8953 modified boot-chain flashing — delivery-path research

Produced by a research subagent. Independent of the binary analysis in `FINDINGS.md`.

## Executive conclusion

There **is** a currently viable way to write arbitrary bytes to eMMC on many MSM8953 devices:
enter EDL/9008, upload a compatible Firehose programmer, and use Firehose to write the target
partition. That does **not** provide a viable way to boot a modified `sbl1.mbn` or `tz.mbn`.

1. Firehose can often write the partition.
2. The next boot stage still authenticates the image.
3. Xiaomi unlocking disables Android/boot-image restrictions but does **not** remove
   Qualcomm PBL/SBL secure-boot authentication for `sbl1`, `tz`, `devcfg`.

- Writing a modified boot-chain image: possible via EDL if the device accepts the programmer.
- Booting it: generally blocked by signature verification.
- Executing custom EL3 code via unsigned `tz`/`sbl1` replacement: no publicly documented,
  generally working route found.
- Most promising practical route: use EDL to dump, verify GPT, and test writes **only** on a
  sacrificial partition first. Treat a modified `tz`/`sbl1` write as a brick-risk experiment.

## 1. EDL / Qualcomm 9008 — YES for low-level access, PARTIAL for the EL3 goal

The PBL exposes the Sahara protocol over USB in EDL. Sequence:

```
PBL -> USB 9008 -> Sahara handshake -> upload signed Firehose programmer
    -> Firehose XML commands -> read/write/erase eMMC
```

[`bkerler/edl`](https://github.com/bkerler/edl) implements this, with documented examples:

```
edl printgpt
edl r boot_a boot.img
edl w boot_a boot.img
edl e misc
edl rf flash.bin
edl wl dumps
```

### Entering EDL

- `adb reboot edl` — may work if Android and the OEM build permit it.
- `fastboot oem edl` — works on some OEM bootloaders; not universal.
- Test points — most reliable, but requires opening the phone.
- **Volume-key combinations are not reliably EDL.** On many Xiaomi phones Volume Down + Power
  enters ordinary fastboot, not 9008. Must be verified per handset.

### Public MSM8953 programmers

[`bkerler/Loaders`](https://github.com/bkerler/Loaders) includes:

```
qualcomm/factory/msm8953/
qualcomm/model_generic/msm8953/
xiaomi/daisy_prog_emmc_firehose_8953_ddr.mbn
xiaomi/rosy_prog_emmc_firehose_8953_ddr.mbn
```

This is public but unofficial recovered/contributed repair material; provenance and
redistribution rights vary.

### Programmer authentication is the real obstacle

The PBL authenticates the programmer before executing it. A random `prog_firehose` is not
enough. Failures cause: wrong MSM/OEM ID, wrong signing key or PK hash, wrong eMMC config,
wrong DDR init, device-specific restrictions, or Xiaomi account authorisation. Some devices
accept a public signed programmer but then require an authorised Xiaomi account for Firehose
access. This must be tested on the exact handset and firmware generation.

## 2. Xiaomi stock firmware — YES for images, PARTIAL for a programmer

Xiaomi fastboot ROMs are publicly mirrored and often contain `prog_emmc_firehose_8953_ddr.mbn`,
`rawprogram*.xml`, `gpt_main*.bin`, `sbl1.mbn`, `tz.mbn`, `devcfg.mbn`, `emmc_appsboot.mbn`.

- [xiaomifirmwareupdater.com](https://xiaomifirmwareupdater.com/)
- [XiaomiFirmwareUpdater GitHub org](https://github.com/XiaomiFirmwareUpdater)

Inspect without flashing:

```bash
7z l firmware.tgz && 7z x firmware.tgz
find . -iname '*firehose*' -o -iname '*prog*8953*'
```

Codename note — the agent flagged an ambiguity worth confirming against the actual handset:

| Codename | Model |
|---|---|
| `daisy` | Mi A2 Lite / Redmi 6A |
| `sakura` | Redmi 6 Pro |
| `whyred` | Redmi Note 5 (SD) |
| `rosy` | a different Redmi — **not** `whyred` |

**Licensing:** stock firmware is OEM copyrighted. Extracting and using a programmer for
personal repair is common practice, but redistribution may violate Xiaomi's terms or
copyright, and programmers may contain proprietary Qualcomm/OEM code. Not recommended for
distribution.

A stock programmer does **not** guarantee a modified image will boot: PBL authenticates the
next image, SBL authenticates later images, the OEM bootloader enforces policy, anti-rollback
may reject older images, and an unsigned `tz`/`sbl1` can hard-brick the device.

## 3. Xiaomi bootloader unlock — PARTIAL, account-dependent

Historical process ([old official page](https://en.miui.com/unlock/download_en.html)) requires
a Xiaomi account bound to the device, often a 168-hour+ waiting period, SIM/mobile-data
association in some regions, and wipes user data.

As of 2025–2026 it should **not** be described as universally open. Availability varies by
region, device generation, HyperOS vs MIUI, account age/status, China vs global models, and
current quota policy.

Check state without unlocking:

```
fastboot oem device-info
fastboot getvar unlocked
fastboot getvar all
```

Android may expose `ro.boot.verifiedbootstate`, `ro.boot.flash.locked`.

**Unlocking permits:** flashing `boot`, `vendor`, `system`, `recovery`, often `vbmeta`;
unsigned/custom kernel and Android boot images; `fastboot boot`.

**Unlocking does NOT permit:** PBL accepting an unsigned `sbl1`; PBL accepting an unsigned
Firehose programmer; SBL accepting an unsigned `tz`; disabling Qualcomm secure boot; replacing
critical bootloader partitions. The unlock state is an OEM policy state — Qualcomm's
hardware-rooted image authentication is a separate security boundary.

## 4. What fastboot permits when unlocked — NO for unsigned EL3 replacement

Two different fastboot implementations are involved and must not be conflated: the stock
Xiaomi bootloader, and `lk2nd`'s own implementation after `lk2nd` is loaded.

On an unlocked Xiaomi device, ordinary fastboot commonly allows Android-facing partitions.
Whether `fastboot flash sbl1 / tz / devcfg` is permitted depends on the OEM bootloader,
partition table, build, and critical-unlock state. Many Qualcomm bootloaders protect critical
partitions separately; some names are not exposed at all, some return "partition not found" /
"not allowed", some accept the write but the image is later rejected cryptographically.

**A successful write does not imply the image will execute.**

## 5. lk2nd — YES in scope, NO as a secure-boot bypass

From the [`lk2nd` source](https://github.com/msm8953-mainline/lk2nd): it is a *secondary*
bootloader, does **not** replace the stock bootloader, is packaged into an Android boot image
and loaded by the stock bootloader, and provides standard Android fastboot. It can flash the
real Android boot image at a 1 MiB offset so the lk2nd payload survives.

```text
fastboot flash boot lk2nd.img
fastboot flash lk2nd lk2nd.img
fastboot flash boot boot.img
```

Its MSM8953 fastboot registers generic `flash:`, `erase:`, `boot`, `continue`, `reboot`, and
its eMMC code can address GPT partitions including `tz`, `sbl1`, `devcfg`, `rpm`, `aboot`,
`hyp`.

This demonstrates a custom LK-derived fastboot can issue eMMC partition writes. It does **not**
prove the stock Xiaomi bootloader permits those writes, that lk2nd can replace the original
boot chain, that the written image passes secure boot, or that lk2nd has an unrestricted raw
eMMC console.

The existing `lk2nd-msm8953-edt.img` is a useful RAM/pre-boot execution environment but is
**not** a Qualcomm PBL bypass.

## 6. `devcfg` / `fsc` / device-programmer alternatives — NO documented bypass

`devcfg.mbn` is a boot-chain device-configuration image, not a flashing bypass. `fsc`/`fsg`
usually refer to modem filesystem areas in particular vendor layouts. `rawprogram.xml`
describes Firehose writes. A Firehose programmer is what gets uploaded after Sahara.

No reliable public evidence was found that any MSM8953 Xiaomi fastboot command bypasses PBL
programmer auth, SBL/TZ image auth, Xiaomi critical-partition policy, or anti-rollback.

The practical raw-programming interfaces are: (1) Firehose via EDL, (2) a custom bootloader
like lk2nd with eMMC write support, (3) hardware eMMC access / chip-off.

## 7. Bottom line and recommended sequence

**Viable now:** EDL + Firehose (device-dependent); official Xiaomi unlock (partial, account
gated); lk2nd (for its intended scope); stock ROMs (for known-good images and possibly a
programmer).

**Dead ends / blocked:** assuming Volume Down + Power enters EDL; assuming unlock permits
unsigned `tz`/`sbl1`; using lk2nd to replace PBL/SBL; treating `devcfg`/`fsc` as a universal
raw-write bypass; assuming any 8953 programmer works on every OEM device; relying on
discontinued Qualcomm/Xiaomi service accounts; flashing before taking a complete verified dump;
assuming a successful Firehose write means the code will boot.

**Phase 1 — preserve the device.** Record model, codename, board revision, eMMC config,
firmware build, bootloader state. Dump GPT (main+backup), `sbl1`, `tz`, `devcfg`, `rpm`,
`aboot`, rollback/security partitions. Hash everything, keep two copies. Prove read/write on a
disposable partition before touching boot-critical ones.

**Phase 2 — establish EDL.** `adb reboot edl` → `fastboot oem edl` → test points. Confirm USB
PID `9008`, not ordinary fastboot.

**Phase 3 — identify a loader.** Prefer the exact loader from the stock fastboot package for
the exact device/build; else the matching entry in `bkerler/Loaders`. Let Sahara report MSM ID,
OEM ID, model ID, PK hash. Do not substitute a loader merely because its filename says `8953`.
If rejected, diagnose authentication rather than retrying blindly.

**Phase 4 — verify Firehose reads.** `printgpt`; read a small known partition; compare
byte-for-byte against the existing `/dev/mem` dump; read the target partition and verify
offsets/sizes; test a write on a non-critical partition; read back and hash-verify. The goal
is to prove Firehose genuinely writes eMMC rather than accepting XML and returning
misleading success.

**Phase 5 — evaluate the modified image.** Determine the exact signature format, whether the
chain is Qualcomm/OEM signed, whether anti-rollback is enforced, and which of PBL/SBL1/later
bootloader authenticates the target.

**Phase 6 — only then a critical write.** Confirm a working EDL recovery path, confirm the
original programmer can rewrite the original partition, keep GPT backups, make a minimal
change retaining headers and non-code metadata, write one partition, never erase GPT or
neighbouring boot-chain partitions, and be ready to restore immediately.

## Could not be verified

Whether the specific handset accepts the public `daisy`/`MSM8953` loaders; whether Xiaomi
authorisation is required after Sahara on that unit; whether stock fastboot exposes `sbl1`,
`tz`, or `devcfg`; whether a "critical unlock" state exists on that build; whether any current
Xiaomi cloud account can unlock the device in the requester's region; whether any public
MSM8953 secure-boot bypass exists for unsigned `tz`/`sbl1`; whether a particular Firehose
binary permits writes or only reads.

**Bottom line: public evidence supports raw eMMC access through EDL, not a general-purpose
method for booting unsigned EL3 code.**
