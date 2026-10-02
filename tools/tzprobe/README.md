# tzprobe

Out-of-tree kernel module that tests whether the TrustZone SMC handler uses EL1's `x20` as a
pointer, which is the load-bearing assumption behind the `x20` findings in FINDINGS.md.

Built and run against `msm8953/linux` (kernel `7.0.9-msm8953+`, matching the target exactly)
with the same clang the kernel was built with:

    make -C ~/Projects/Android/msm8953/linux M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 modules

## Status: inconclusive, two kernel panics

Both runs faulted at `0xffffffffffffffff`, EC `0x25` DABT (current EL), FSC `0x06` level-2
translation fault, in `probe+0x90`.

- v1 wrote `SP_EL0` to a kernel `vzalloc` address. That address is not accessible at EL0, and
  the fault returned through the SMC path. Self-inflicted.
- v2 removed all `SP_EL0` manipulation and only set `x20`. It faulted at the *same* address,
  which suggests TZ is dereferencing `SP_EL0` (uninitialised in this context) rather than the
  fault being self-inflicted. **Not established.**

So the `x20` model remains *inferred from static analysis*, not observed. It is the one finding
the rest of the `x20` work depends on.

## Note for anyone modifying this

`x20` must be moved by the asm itself:

    asm volatile("mov x20, %3\n\t"
                 "smc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                 : "r"(ptr)
                 : "x20", "memory");

A `register unsigned long x20 __asm__("x20")` local is *not* sufficient — if the asm never
references it, clang discards it ("unused variable") and the register keeps whatever it held,
silently making the test meaningless. That bug was present in v1 and caught by the compiler.

Next step is to give the SMC a valid user-space `SP_EL0` so TZ's dereference cannot fault, which
would let the call complete and give a real answer either way.
