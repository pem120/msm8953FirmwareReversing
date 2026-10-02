# Flash and delivery paths for MSM8953

~research #edl #firehose #msm8953 #xiaomi

> [!abstract]
> Can a modified `sbl1.mbn` or `tz.mbn` actually be written to this phone, and will it boot?
> This is the delivery-side half of [[MSM8953 TrustZone EL3 research]] and is deliberately
> kept separate from the binary analysis.
>
> **Short answer: the first yes, the second no.** The two are often conflated, and that
> conflation is the main risk to this project.

## Executive conclusion

There **is** a currently viable way to write arbitrary bytes to eMMC on many MSM8953 devices:
enter Qualcomm EDL/9008, upload a compatible Firehose programmer, and use Firehose commands to
write the target partition. That does **not** provide a viable way to boot a modified `sbl1.mbn`
or `tz.mbn`.

1. Firehose can often write the partition.
2. The next boot stage still authenticates the image.
3. Xiaomi bootloader unlocking generally disables Android/boot-image restrictions, but does
   **not** remove Qualcomm PBL/SBL secure-boot authentication for `sbl1`, `tz`, `devcfg` and
   related boot-chain images.

| Question | Answer |
| --- | --- |
| Writing a modified boot-chain image | Technically possible via EDL if the device accepts the programmer |
| Booting that modified image on an ordinary secure-boot Xiaomi MSM8953 | Generally blocked by signature verification |
| Executing custom EL3 code via a simple unsigned `tz.mbn`/`sbl1.mbn` replacement | No publicly documented, generally working route was found |
| Most promising practical route | Use EDL to dump, verify the GPT, and test writes **only** on a sacrificial partition first |

> [!danger]
> A modified `tz` or `sbl1` write should be treated as a **brick-risk experiment, not an
> expected boot path**.

## EDL and Qualcomm 9008

> [!success]
> **Verdict: YES for low-level access, PARTIAL for the requested EL3 goal.**

Qualcomm's boot ROM (the PBL) exposes the Sahara protocol over USB in Emergency Download
Mode. On a normal MSM8953 the host generally sees a Qualcomm HS-USB QDLoader 9008 device.

```
PBL
  -> USB 9008
  -> Sahara handshake
  -> upload signed Firehose programmer
  -> Firehose XML command protocol
  -> read/write/erase eMMC
```

The open-source [`bkerler/edl`](https://github.com/bkerler/edl) project documents and
implements this: Sahara communication and chip identification, uploading a Firehose loader,
automatic loader lookup, Firehose XML operations, and eMMC GPT/partition read/write/erase plus
raw-image operations. Documented examples include:

```
edl printgpt
edl r boot_a boot.img
edl w boot_a boot.img
edl e misc
edl rf flash.bin
edl wl dumps
```

The project also documents the 9008 USB requirement and the use of Qualcomm USB drivers or
libusb.

### Entering EDL

These mechanisms are not equivalent:

- `adb reboot edl` — may work if Android and the OEM build expose or permit it.
- `fastboot oem edl` — may work on some OEM bootloaders; **not universal**.
- Test points / board-level methods — the most reliable on a dead or locked device, but
  requires opening the phone.
- USB cable / test-point methods — device-specific.
- **Volume-key combinations are not reliably EDL.** On many Xiaomi phones Volume Down + Power
  enters ordinary fastboot, not 9008. Some board revisions have additional combinations, but
  this must be verified for the exact handset.

> [!tip]
> `bkerler/edl`'s README explicitly discusses USB PID `0x9008`, and describes test-point and
> other recovery methods for devices that enumerate in alternate Qualcomm download modes.

### The Sahara handshake

1. The device sends a `HELLO`.
2. The host responds with `HELLO_RESP`.
3. The host queries chip information and/or receives the programmer request.
4. The host sends the Firehose ELF/MBN image in Sahara packets.
5. The device jumps to the uploaded programmer.
6. Communication changes to Firehose XML commands.

The exact command set varies by Sahara version. The current `bkerler/edl` fork supports newer
Sahara V3 chip identification and explains that older `MSM_HW_ID_READ` and PK-hash queries may
not work on newer devices. MSM8953-era devices commonly use older Sahara behaviour, but the
tool's explicit chip information output should be trusted over assumptions.

### Public MSM8953 programmers

The most concrete source found is the public
[`bkerler/Loaders`](https://github.com/bkerler/Loaders) repository, whose tree includes:

```
qualcomm/factory/msm8953/
qualcomm/model_generic/msm8953/
xiaomi/daisy_prog_emmc_firehose_8953_ddr.mbn
xiaomi/rosy_prog_emmc_firehose_8953_ddr.mbn
```

The MSM8953 factory directory contains multiple loader binaries keyed by MSM/HWID and hash. The
Xiaomi directory includes named loaders for devices such as `daisy` and `rosy`. The full tree
can be inspected via the
[Loaders Git tree API](https://api.github.com/repos/bkerler/Loaders/git/trees/main?recursive=1).

> [!caution]
> This is genuinely public but **not** an official Qualcomm distribution. The repository
> describes the loaders as recovered or contributed repair material. Provenance and
> redistribution rights vary.

### Programmer authentication is the real obstacle

The PBL authenticates the programmer before executing it. A random `prog_firehose` file is not
sufficient. A loader may fail because of:

- Wrong MSM ID or OEM ID
- Wrong signing key or PK hash
- Wrong eMMC configuration
- Wrong DDR initialization
- Device-specific Firehose restrictions
- Xiaomi authorisation requirements

> [!warning]
> Some Qualcomm/Xiaomi devices accept a public signed programmer but then require an authorised
> Xiaomi account for actual Firehose access. Other models have publicly usable programmers.
> This has to be tested on the exact handset and firmware generation.

## Xiaomi stock-firmware route

> [!success]
> **Verdict: YES for obtaining legitimate stock images; PARTIAL for obtaining a usable
> Firehose loader.**

Xiaomi fastboot ROM packages are publicly mirrored and commonly contain an `images/` directory
which, depending on the package and device, may include:

```
prog_emmc_firehose_8953_ddr.mbn
prog_firehose_ddr.elf
rawprogram*.xml
patch*.xml
gpt_main*.bin
gpt_backup*.bin
sbl1.mbn
tz.mbn
devcfg.mbn
emmc_appsboot.mbn
```

Public firmware archives include
[xiaomifirmwareupdater.com](https://xiaomifirmwareupdater.com/) and the
[XiaomiFirmwareUpdater GitHub organisation](https://github.com/XiaomiFirmwareUpdater), plus
Xiaomi's own delivery infrastructure where a suitable official package URL exists for the exact
model, region and build. Exact contents differ per device and release.

Inspect a package without flashing:

```bash
7z l firmware.tgz
7z l firmware.tar
7z x firmware.tgz
7z x firmware.tar
find . -iname '*firehose*' -o -iname '*prog*8953*'
```

### Codename ambiguity — confirm this

> [!question]
> The research agent flagged that Redmi 6 Pro maps to `sakura` in some material while other
> project material uses related board names, and that the exact product should be verified.
> **This is still unresolved and it matters**, because loader availability is per-device.

| Codename | Model |
| --- | --- |
| `daisy` | Mi A2 Lite / Redmi 6A |
| `sakura` | Redmi 6 Pro |
| `whyred` | Redmi Note 5 / 5 Plus (Snapdragon) |
| `rosy` | a different Redmi model — **not** `whyred` |

### Licensing and legality

Stock firmware is OEM copyrighted material. Downloading firmware for repairing one's own
device is common, but redistribution may violate Xiaomi's terms or copyright, Firehose
programmers may contain proprietary Qualcomm/OEM code, public loader repositories may have
inconsistent provenance, and commercial "authorized EDL" services may use credentials or
software not licensed for redistribution. **Use them only where legally permitted; do not
redistribute extracted OEM binaries.**

### Does a stock programmer guarantee modified-image flashing?

> [!danger]
> **No.** It may permit raw Firehose writes, but after reboot PBL authenticates the next boot
> image, `sbl1` authenticates or loads later secure images, the OEM bootloader enforces its own
> policy, anti-rollback metadata may reject older images, and a corrupted or unsigned
> `tz`/`sbl1` can hard-brick the device.

## Xiaomi bootloader unlock status

> [!caution]
> **Verdict: PARTIAL; the service exists but is heavily restricted and account-dependent.**

Xiaomi's historical official process is documented on the
[old official page](https://en.miui.com/unlock/download_en.html): download Mi Unlock, sign in
with a Xiaomi/Mi Account, enter fastboot with Volume Down + Power, connect, click Unlock. It
has historically required a Xiaomi account bound to the device in Android developer settings, a
working SIM/mobile-data association in some regions, an account with suitable region and
eligibility, a waiting period (historically often 168 hours and sometimes longer), a user data
erase, and a functioning Xiaomi unlock backend.

As of the 2025–2026 period it should **not** be described as universally open. Xiaomi has
continued to operate an official unlock mechanism, but availability and requirements vary by
region, device generation, HyperOS versus older MIUI, account age and status, China versus
global models, and Xiaomi's current application and quota policy. Some newer regional
programmes have added application, quota, or longer waiting requirements, and the service can
reject a device even when the user has the correct account.

### Determining unlock state without unlocking

Usually possible, at least partially:

```
fastboot oem device-info
fastboot getvar unlocked
fastboot getvar all
```

The graphical fastboot screen may display "unlocked", and Android may expose
`ro.boot.verifiedbootstate` or `ro.boot.flash.locked`. These values are not equally reliable on
every Xiaomi build. The definitive behaviour is whether the bootloader accepts
unlock-dependent commands — but querying the state does not unlock the device.

### What unlocking changes

**Unlocking normally permits:** flashing ordinary Android partitions such as `boot`, `vendor`,
`system`, `recovery` and often `vbmeta`; booting unsigned or modified kernel/Android boot
images subject to device-specific AVB behaviour; running `fastboot boot` or flashing a custom
recovery where supported.

**Unlocking does not automatically mean:** PBL accepting an unsigned `sbl1`; PBL accepting an
unsigned Firehose programmer; SBL accepting an unsigned `tz`; Qualcomm secure boot being
disabled; the device's critical bootloader partitions being replaceable.

> [!warning]
> The unlock state is an OEM bootloader policy state. **Qualcomm hardware-rooted image
> authentication is a separate security boundary.**

## What ordinary fastboot permits when unlocked

> [!danger]
> **Verdict: NO for a general unsigned EL3 replacement; PARTIAL for partition writes.**

Two different fastboot implementations are involved and must not be conflated:

1. The stock Xiaomi first-stage/bootloader fastboot.
2. `lk2nd`'s own fastboot implementation, after `lk2nd` has been loaded.

On an unlocked Xiaomi device, ordinary fastboot commonly allows flashing Android-facing
partitions. Whether it exposes and permits

```
fastboot flash sbl1 sbl1.mbn
fastboot flash tz tz.mbn
fastboot flash devcfg devcfg.mbn
```

depends on the OEM bootloader, partition table, build, and critical-unlock state. Many
Qualcomm bootloaders protect "critical" partitions separately: normal unlock permits common
Android partitions, but a critical unlock or an OEM-specific policy is needed for
bootloader-critical partitions. Some partition names are not exposed at all; some commands
return "partition not found", "not allowed" or "flashing is not allowed"; and some
bootloaders accept the write but later reject the image cryptographically.

> [!tip]
> **Even if the command succeeds, it does not imply that the image can execute.**

The images in question generally reside in the eMMC user area as GPT partitions rather than
being inherently inaccessible because they sit in a special eMMC boot area. The blocker is
software policy plus the Qualcomm boot chain's signature enforcement.

## lk2nd capabilities and limits

> [!success]
> **Verdict: YES for normal fastboot partition operations; NO as a secure-boot bypass.**

From the [`lk2nd` source](https://github.com/msm8953-mainline/lk2nd), lk2nd is a secondary
bootloader that does **not** replace the stock bootloader, is packaged into an Android boot
image, is loaded by the stock bootloader, provides standard Android fastboot, and can flash
the actual Android boot image at a 1 MiB offset so the lk2nd payload is preserved.

```
fastboot flash boot lk2nd.img
fastboot flash lk2nd lk2nd.img
fastboot flash boot boot.img
```

Its MSM8953 fastboot registers generic `flash:`, `erase:`, `boot`, `continue` and `reboot`,
and its eMMC flashing code can address GPT partitions. The source's critical partition list
includes names such as `tz`, `sbl1`, `devcfg`, `rpm`, `aboot` and `hyp`.

This demonstrates that a custom LK-derived fastboot can be made to issue eMMC partition
writes. It does **not** prove that the stock Xiaomi bootloader permits those writes, that
lk2nd can replace the original boot chain, that the written image will pass secure boot, or
that lk2nd has a built-in unrestricted raw eMMC flashing console.

> [!tip]
> The existing `lk2nd-msm8953-edt.img` is useful as a RAM/pre-boot execution environment, but
> it is **not** a Qualcomm PBL bypass.

A modified lk2nd could theoretically be extended with custom eMMC write code or diagnostic
commands, but that still leaves the secure-boot problem for `tz` and `sbl1`.

## devcfg, fsc and device-programmer alternatives

> [!danger]
> **Verdict: NO documented MSM8953/Xiaomi bypass found.**

Some Qualcomm toolchains expose commands or files called `devcfg`, `fsc`, `fsg`,
`rawprogram.xml`, `patch.xml` or "device programmer". These refer to different things:

- `devcfg.mbn` is a boot-chain/device-configuration image, not a universal flashing bypass.
- `fsc`/`fsg` commonly refer to modem filesystem/configuration areas in particular vendor
  layouts.
- `rawprogram.xml` describes Firehose writes.
- A Firehose programmer is the executable uploaded after Sahara.

No reliable public evidence was found that an MSM8953 Xiaomi fastboot command such as a
generic `devcfg` or `fsc` operation bypasses Qualcomm PBL programmer authentication, SBL/TZ
image authentication, Xiaomi critical-partition policy, or anti-rollback.

The practical raw-programming interfaces are: (1) Firehose through EDL, (2) a custom bootloader
such as lk2nd with eMMC write support, (3) hardware eMMC access or chip-off methods. Of these,
Firehose is the most generally useful for writing boot-chain partitions, provided the device
accepts a compatible programmer.

## Bottom line

### Currently usable

| Mechanism | Usability |
| --- | --- |
| EDL + public Firehose loader | **Partial to yes**, device-dependent |
| Official Xiaomi bootloader unlock | **Partial** — cloud service, account- and region-gated |
| `lk2nd` | **Yes**, for its intended scope (Android partition ops) |
| Stock Xiaomi fastboot ROMs | **Yes** for known-good images, and possibly a programmer |

### Dead ends and blocked

- Assuming Volume Down + Power always enters EDL — it usually means fastboot
- Assuming an unlocked bootloader permits unsigned `tz.mbn` or `sbl1.mbn`
- Using `fastboot flash boot` or lk2nd to replace PBL/SBL
- Treating `devcfg` or `fsc` as a universal Qualcomm raw-write bypass
- Expecting a generic MSM8953 programmer to work on every OEM/device
- Relying on discontinued or unauthorised Qualcomm/Xiaomi service accounts
- Flashing a modified secure-world image before taking a complete verified dump
- Assuming a successful Firehose write means the next boot will execute the modified code

> [!warning]
> The publicly documented evidence supports **raw eMMC access through EDL**, not a
> general-purpose method for booting unsigned EL3 code.

## Recommended sequence

### Phase 1 — preserve the device

- Record the exact model, codename, board revision, RAM/eMMC configuration, firmware build and
  bootloader state.
- Using the existing diagnostic-kernel capability, dump GPT main and backup, `sbl1`, `tz`,
  `devcfg`, `rpm`, `aboot`/`emmc_appsboot`, and relevant metadata and rollback/security
  partitions.
- Hash every dump and keep at least two independent copies.
- **Do not begin with `sbl1` or `tz`; first prove reads and writes work on a disposable
  partition.**

### Phase 2 — establish EDL

1. Try `adb reboot edl` if Android is available.
2. Try the device's vendor fastboot EDL command if present.
3. Otherwise use the exact board's known test-point method.
4. Confirm the host sees Qualcomm USB PID `9008`, not ordinary fastboot.

On Linux, install the required USB access and run the EDL tool with verbose output.
`bkerler/edl`'s README includes installation and driver guidance.

### Phase 3 — identify a loader

1. Start with the exact loader from the stock fastboot package for the exact device/build.
2. If unavailable, try the matching loader from
   [`bkerler/Loaders`](https://github.com/bkerler/Loaders) — MSM8953 factory/model-generic
   entries, or `daisy_prog_emmc_firehose_8953_ddr.mbn` for daisy.
3. Let Sahara report MSM ID, OEM ID, model ID and PK hash.
4. **Do not substitute a loader merely because its filename contains `8953`.**
5. If rejected, stop and diagnose authentication rather than repeatedly retrying.

### Phase 4 — verify Firehose read access

1. Print the GPT.
2. Read back a small known partition.
3. Compare byte-for-byte against the previously obtained diagnostic-kernel `/dev/mem` dump.
4. Read the target partition and verify offsets and sizes.
5. Test a write on a non-boot-critical partition only.
6. Read it back and verify the written hash.

> [!tip]
> The objective is to establish that the Firehose session is genuinely writing eMMC, not
> merely accepting XML and returning misleading responses.

### Phase 5 — evaluate the modified image

Before attempting any secure-world modification, determine which exact signature format the
target image uses, whether its certificate chain is Qualcomm/OEM signed, whether the device
enforces anti-rollback, whether the target is authenticated by PBL, SBL1 or a later bootloader,
and whether a known exploit or hardware fault is available to bypass that verification.

> [!danger]
> Without such a bypass, writing an unsigned modified `tz` or `sbl1` is expected to produce a
> boot failure or a brick.

### Phase 6 — only then consider a critical write

1. Confirm a working EDL recovery path before changing anything.
2. Confirm the original programmer can rewrite the original partition.
3. Confirm the complete partition dump and GPT backup.
4. Prepare a minimal modification, retaining image headers, sizes and all non-code metadata.
5. Write only one target partition.
6. Do not erase GPT or neighbouring boot-chain partitions.
7. Be prepared to restore the original image immediately through EDL.

> [!warning]
> This is an experimental brick-risk operation, not a documented route to EL3 execution.

## What could not be verified

All of the following require testing on the exact handset and firmware build:

- Whether a specific Redmi 6A / 6 Pro / Note 5 unit accepts the public `daisy`, MSM8953, or
  other Firehose loader
- Whether Xiaomi authorisation is required after Sahara on that unit
- Whether the stock Xiaomi fastboot implementation exposes `sbl1`, `tz`, or `devcfg`
- Whether a particular Xiaomi build has a separate "critical unlock" state, and how it applies
  to those partitions
- Whether any current Xiaomi cloud account can unlock those legacy devices in the requester's
  region
- Whether any public MSM8953 secure-boot bypass exists that permits a modified `tz.mbn` or
  `sbl1.mbn` to execute
- Whether a particular Firehose binary's write implementation permits the target partition
  rather than merely reading it
