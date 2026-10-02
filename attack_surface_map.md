# MSM8953 `tz.mbn` static attack-surface map

**Scope.** Read-only analysis of the supplied 1,531,776-byte ELF64 AArch64 image. No Ghidra or Ghidra bridge was used. Virtual addresses below are image VAs; the first executable LOAD maps VA `0x86500000` to file offset `0x3000`.

## 1. EL3 exception / SMC dispatch

The handler at `0x86501c20` masks the exception value to 32 bits and performs `ldr x14,[table,index,lsl #3]`, with table VA `0x86508350` (file offset `0xb350`). Entries `0x00..0x38` are valid; `0x39` is the `0xdeaddead` sentinel and the following bytes are not jump-table entries.

|index|target|index|target|
|---:|---:|---:|---:|
|00|8650208c|01|865020d0|
|02|865020f0|03|86502104|
|04|86502124|05|86502134|
|06|8650213c|07|86502144|
|08|86502150|09|8650215c|
|0a|8650219c|0b|865021c4|
|0c|86502220|0d|86502240|
|0e|8650224c|0f|86501dc0|
|10|865022f0|11|86502310|
|12|86502324|13|86502330|
|14|8650233c|15|86502348|
|16|86502360|17|86502368|
|18|86502388|19|86502398|
|1a|86502404|1b|86502410|
|1c|86502418|1d|86502420|
|1e|86501dc0|1f|86501dc0|
|20|86502470|21|8650247c|
|22|865024f4|23|86501dc0|
|24|86502500|25|86502508|
|26|86502514|27|8650252c|
|28|86502540|29|86502548|
|2a|8650257c|2b|86502588|
|2c|86501c50|2d|86501c50|
|2e|865025c8|2f|865025d0|
|30|865025e8|31|86502600|
|32|86502610|33|86501dc0|
|34|86502620|35|86501dc0|
|36|86501dc0|37|86501dc0|
|38|86502668|39|`0xdeaddead` sentinel|

Using the established ESR interpretation, index `0x17` is SMC from AArch64 lower EL and `0x18` is SMC from AArch32 lower EL. HVC from AArch64 lower EL is index `0x2a`. The SMC AArch64 entry at `0x86502368` reads the function ID from `[x20,#8]`, masks it with `0x3fffffff`, and recognizes these IDs:

|SMC ID (masked)|entry/handler|behavior seen|
|---:|---:|---|
|`0x02000102`|`0x86502a58`|common return path|
|`0x02000109`|`0x86502a58`|common return path|
|`0x02000305`|`0x86502a58`|common return path|

The common path at `0x86502a58` stores zero to the return slot. The AArch32 SMC entry (`0x86502388`) returns `-1`. No `0x8200xxxx` or `0xc200xxxx` constant was observed in this direct lower-EL SMC ID switch; the larger vendor/service switch is reached from other exception/message paths, not from this three-ID SMC entry.

A second ID switch in the same EL3 exception subsystem starts at `0x86501c94`: it masks the incoming 64-bit value to `0x3fffffff` (in `w9`) and has the following code-backed cases. These are useful additional callable surface, but should not be mislabeled as direct lower-EL SMC IDs without tracing the caller that selects this path.

|ID|case code / operation address|
|---:|---:|
|`0x02000102`|`0x86501eec` -> `0x86502e48`|
|`0x02000109`|`0x86501d0c` -> `0x865033e8`|
|`0x02000304`|`0x86501e5c`|
|`0x02000500`|`0x86501e00`|
|`0x02000501`|`0x86501e10`|
|`0x02000502`|`0x86501e88` -> `0x86503304`|
|`0x02000901`|`0x86501dcc`|
|`0x02000902`|`0x86501ddc` -> `0x86502efc`|
|`0x02001102`|`0x86501f10` -> `0x86503470` (currently `ret`)|
|`0x04000000`|`0x86501e24`|
|`0x04000020`|`0x86501eac`|
|`0x3400fa04`|`0x86501d34`|

The switch also contains a special family around `0x340105ff..0x34010603`, dispatched through the five-entry table at VA `0x86508328`; its targets are `0x86501d88`, `0x86501dc0`, `0x865027c4`, `0x86501eb0`, and `0x86501ecc`.

## 2. String/interface inventory

### Build and provenance

- `QC_IMAGE_VERSION_STRING=TZ.BF.4.0.5-202044` (file offset `0xe50a4`).
- `OEM_IMAGE_VERSION_STRING=CRM` (0xe50ef).
- `May 17 2019` (0xd3130), consistent with an older 2019 build.
- `msm_hw_id_upper`, `msm_hw_id_lower`, `image_version`, `CORE_RG_INDEX`, and `IS_GPIO_PROTECTED` configuration labels are present.

### Secure services, files, and device interfaces

- `/qsee/img_auth`, `APP_ID`, `/secboot/oem_secapp`, `/secboot/oem_general`, `/secboot/anti_rollback`, `/tz/oem`, `/tz/oem/kdf_fix`.
- `/dev/vmidmt`, `/dev/xpu2`, `/core/hwengines/bam`, `/ac/xpu`, `/dev/icbcfg/boot`, `/dev/ABTimeout`, `/dev/NOCError`, `/dev/BIMCError`, `/dev/ABTimeoutOEM`, `/dev/BIMCErrorOEM`.
- Large inventories of `/dev/buses/qup/blsp_qup_1..12`, UART 1..12, SPI 1..12 and `spi_ssc_1..2`; clock and PMIC paths such as `/clk/qdss`, `/clk/ce1`, `/tz/pmic`.
- Key labels: `core trustzone direct key derivation label`, `core trustzone application key label securemsm`, RPMB version-counter cipher/HMAC labels, `Wrapping key`, `QSEE_Dynamic`, and FDE/keystore labels.
- Macchiato device-authentication/provisioning, X.509, ECDH/ECDSA/RSA, HMAC and cipher error strings indicate substantial cryptographic and certificate-parsing surface.

### Logging, faults, and diagnostics

`NON_SECURE_WDT`, `SECURE_WDT`, `NOC_ERROR`, `BIMC_ERROR`, `SMEM`, `XPU_VIOLATION`, `QSEE_ERR`, `QSEE_STACK_CHK_ERR`, `CRASH_DUMP`, `DEBUG`, and extensive XPU/VMIDMT/SMMU fault-dump strings are present. These are diagnostic code paths, not proof of unauthenticated reachability.

### High-value debug/test indicators

- Certificate metadata includes `SecTools Test User`, `DEBUG`, and a qdst development-attestation CRL URL.
- Strings include `disable_xpu_ac`, `use_root_of_trust_only`, `auth_use_serial_num`, `Unlocked successfully`, `OEM_force_app_key_zero_fuse_address`, and `OEM_force_app_key_zero_fuse_mask`.
- These are strong indicators of factory/development provisioning features, but I found no direct evidence that an unauthenticated public SMC selects them. Treat as **inference/high-interest follow-up**, not a demonstrated bypass.

No clear QSEE app filename list (e.g. named packaged app binaries) was recovered from printable strings; the image contains the QSEE loader/authentication infrastructure rather than an obvious app manifest.

## 3. Ranked bug-surface candidates

These are static candidates, not confirmed vulnerabilities. “High” means the instruction sequence directly demonstrates an unchecked caller-derived pointer/length at a security boundary; “medium” means the risk depends on an unobserved validator or trusted context invariant.

### 1. Caller pointer dereference/write in ID `0x02000502` path — **High/medium**

At `0x86501e88`, the handler loads `x0=[x20,#0x10]` and `w1=[x20,#0x18]`, then calls `0x86503304`. That routine (`0x86503394`-style companion logic) validates only the *value* at the supplied pointer against a small table at `0x86508520`; on a match it performs `ldr w19,[x19]` / `str w19,[x20]` through the caller-derived pointer. The shown path has no explicit ownership/range check before the pointer is dereferenced. Exploitability depends on whether the SMC prologue has already copied/validated this argument into secure memory; the local code does not establish that.

### 2. Caller-controlled pointer plus 32-bit length to memory operation — **High/medium**

At `0x86501e88` the pair `(x0=[x20,#0x10], w1=[x20,#0x18])` is passed to `0x86503304`; at `0x8650331c` the length is copied into `w19` and then written through `x20`-derived state. Separately, several handlers pass caller fields into the generic copy/zero routines: `0x86502548` computes a page count from `x9=[x20,#0x10]` and invokes `0x8650532c` with `x2=(x9+0xfff)>>10`, while `0x86502738` does the same from `[x20,#0x10]` and `[x20,#8]`. Those routines ultimately call a service callback with caller pointers and sizes. The image shows arithmetic and callback use, but not the underlying ownership check; this is a prioritized audit target for integer overflow and shared-buffer validation.

### 3. 64-bit-to-32-bit truncation in status/address-return paths — **Medium**

At `0x865025d0` and `0x865025e8`, caller values are loaded as `w0` and the result from `0x86503478/0x8650347c` is explicitly reduced with `ubfx ... #0x20` before being returned. At `0x86502668`, results from `0x865035fc` and `0x86503608` are masked with `and x14/x15,#0xffffffff`. This is not by itself a write primitive, but it is an exact truncation boundary worth checking where the returned values feed later address/range checks; static evidence supports truncation, not exploitation.

### 4. Unchecked structure fields used as pointers — **Medium**

At `0x86502420`, fields `[x20,#0xf8]`, `[x20,#0x100]`, and `[x20,#8]` are copied/used; when `[x20,#8]` is nonzero, the handler calls `0x86507268` with an external context and fixed size `0x108`. At `0x8650247c`, fields at offsets `0x8,0x10,0x18,0x20,0x40` are copied into a newly allocated object and later used as callback/context state. No local per-field range checks are visible. This is a medium-confidence structure-validation target because the enclosing framework may validate the trap frame before entry.

### 5. HVC lower-EL interface with caller pointers — **Medium**

Index `0x2a` reaches `0x8650257c`, which loads two caller arguments and calls `0x86505478`; that path stores them into a service object and invokes indirect callbacks. The code has no obvious SCR ownership test at the immediate call site. This is a broad attack-surface candidate, not proof of a bug: the service allocator/`0x86505de0` may enforce the required isolation.

**Negative findings.** I did not find a code-backed unchecked variable-length `memcpy` with a clearly attacker-controlled length, nor a demonstrated integer-overflow-to-write sequence. The strongest concrete findings are caller-derived pointer use and the explicit 32-bit truncation sites above.

## 4. Structural notes

- There are no section headers; the image is organized entirely by 13 program headers and LOAD segments.
- File size is `0x175f80`. The last LOAD has file offset/size `0x176000/0`, so there is no meaningful appended payload past the final mapped segment; the file ends at the last populated segment's end (`0x175f80`).
- RW data contains configuration/property tables, device paths, key labels, certificate material and cryptographic constants. The initial data includes Xiaomi attestation/root certificate subjects and `DEBUG`/test certificate metadata. This looks like embedded trust/configuration material, not an obvious raw private-key blob (static strings alone cannot establish key secrecy).
- The `May 17 2019` banner and `TZ.BF.4.0.5-202044` image version date the build.
- I found no obvious self-integrity routine that hashes the complete image or verifies a signature over its own mapped range. This is a negative static result, not proof that boot-chain verification is absent: PBL/SBL external authentication is expected, and this image contains normal cryptographic/X.509 code that may verify other objects.
