// tzprobe v2 - test whether the TrustZone SMC handler uses EL1's x20 as a pointer.
//
// v1 panicked because it wrote SP_EL0 to a kernel vzalloc address, which is not
// accessible at EL0, and the fault returned through the SMC path. v2 leaves
// SP_EL0 completely alone and only manipulates x20.
//
// Static basis: in tz.mbn the EL3 dispatcher dereferences x20 on every path, and
// no handler ever assigns x20 - it is restored from the saved frame slot where
// the generic entry stub stored EL1's x20. If true, EL3 reads and writes through
// a pointer the lower EL fully controls.

#include <linux/module.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/utsname.h>

#define POISON 0xAA

/* Issue SMC with x20 forced to `ptr`, leaving every other register alone. */
static unsigned long smc_with_x20(unsigned long fid, unsigned long ptr, unsigned long in1)
{
    unsigned long x0 = fid, x1 = in1, x2 = 0, x3 = 0;

    /* The pointer must be moved into x20 by the asm itself. A local
     * "register ... x20" variable is not a reliable way to do this: if it is not
     * referenced by the asm, the compiler simply drops it (clang warned
     * "unused variable"), and the register would keep whatever it held. */
    asm volatile(
        "mov x20, %3\n\t"
        "smc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        : "r"(ptr)
        : "x20", "memory");
    return x0;
}

static void probe(const char *label, unsigned long fid, unsigned long in1)
{
    unsigned long *buf = vzalloc(PAGE_SIZE);
    unsigned long before0, before1, ret;

    if (!buf) { pr_err("tzprobe: vzalloc failed\n"); return; }

    memset(buf, POISON, PAGE_SIZE);
    before0 = buf[0]; before1 = buf[1];

    ret = smc_with_x20(fid, (unsigned long)buf, in1);

    pr_info("tzprobe: %s fid=0x%lx in1=0x%lx ret=0x%016lx buf=%016lx\n",
            label, fid, in1, ret, (unsigned long)buf);
    pr_info("tzprobe:   [0] before=0x%016lx after=0x%016lx %s\n",
            before0, buf[0], buf[0] != before0 ? "<== CHANGED" : "");
    pr_info("tzprobe:   [1] before=0x%016lx after=0x%016lx %s\n",
            before1, buf[1], buf[1] != before1 ? "<== CHANGED" : "");

    if (buf[0] != before0 || buf[1] != before1)
        pr_info("tzprobe: VERDICT[%s] = EL3 WROTE THROUGH x20 -> x20 model CONFIRMED\n", label);
    else
        pr_info("tzprobe: VERDICT[%s] = no write observed\n", label);

    vfree(buf);
}

static int __init tzprobe_init(void)
{
    pr_info("tzprobe: loaded on %s\n", utsname()->release);
    probe("psci_version", 0xd0f1, 0);   /* read-only PSCI query */
    probe("unknown",     0x0000, 0);
    probe("zero-fid-1",  0x0001, 0);
    return 0;
}

static void __exit tzprobe_exit(void) { pr_info("tzprobe: unloaded\n"); }

module_init(tzprobe_init);
module_exit(tzprobe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("probe EL1 x20 use by the TrustZone SMC handler");
