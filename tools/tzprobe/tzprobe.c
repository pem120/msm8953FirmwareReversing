// tzprobe v3 - determine whether the TrustZone SMC handler uses EL1's x20 as a pointer,
// without faulting the kernel.
//
// v1 and v2 both faulted at 0xffffffffffffffff (EC 0x25 DABT, FSC 0x06 level-2 translation
// fault). v1 wrote SP_EL0 to a kernel vzalloc address, which is self-inflicted since EL0
// cannot access vmalloc memory. v2 never touched SP_EL0 and faulted identically, which points
// at TZ dereferencing an SP_EL0 that is uninitialised in this context.
//
// v3 therefore gives the SMC a *real user-space* address for SP_EL0. A user virtual address
// is valid in both EL0 and EL1 because they share TTBR0, so whatever TZ dereferences will
// resolve. x20 gets the kernel alias of the same page, so if TZ uses x20 as a pointer we see
// the write without EL0 permission getting in the way.

#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/utsname.h>
#include <linux/uaccess.h>
#include <asm/pgtable.h>

#define POISON 0xAA

/* Issue SMC with x20 = kptr and SP_EL0 = uptr. */
static unsigned long smc_x20_sp0(unsigned long fid, unsigned long kptr, unsigned long uptr)
{
    unsigned long x0 = fid, x1 = 0, x2 = 0, x3 = 0;

    asm volatile(
        "msr sp_el0, %4\n\t"
        "mov x20, %3\n\t"
        "smc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        : "r"(kptr), "r"(uptr)
        : "x20", "memory");
    return x0;
}

static void probe(const char *label, unsigned long fid)
{
    unsigned long uptr, kptr, ret, k0, k1;
    unsigned long *kbuf;
    struct page *page;

    /* A page from this process's own user stack: already mapped and writable,
     * and - crucially - a *user* virtual address, so it is valid at EL0 and EL1
     * alike because they share TTBR0. Whatever TZ dereferences will resolve,
     * which is what v1 and v2 got wrong. */
    uptr = current->mm->start_stack - 0x2000;
    uptr &= ~0xfffUL;

    if (get_user_pages_fast(uptr, 1, 0, &page) != 1) {
        pr_err("tzprobe: get_user_pages_fast failed for %016lx\n", uptr);
        return;
    }
    kbuf = page_address(page);
    kptr = (unsigned long)kbuf;
    if (!kbuf) { pr_err("tzprobe: page_address NULL\n"); put_page(page); return; }

    kbuf[0] = POISON; kbuf[1] = POISON;
    k0 = kbuf[0]; k1 = kbuf[1];

    ret = smc_x20_sp0(fid, kptr, uptr);

    pr_info("tzprobe: %-9s fid=0x%lx ret=0x%016lx kptr=%016lx uptr=%016lx\n",
            label, fid, ret, kptr, uptr);
    pr_info("tzprobe:   x20 target [0] %016lx -> %016lx %s\n", k0, kbuf[0],
            kbuf[0] != k0 ? "<== CHANGED" : "");
    pr_info("tzprobe:   x20 target [1] %016lx -> %016lx %s\n", k1, kbuf[1],
            kbuf[1] != k1 ? "<== CHANGED" : "");

    if (kbuf[0] != k0 || kbuf[1] != k1)
        pr_info("tzprobe: VERDICT[%s] = EL3 wrote through x20 -> x20 model CONFIRMED\n", label);
    else
        pr_info("tzprobe: VERDICT[%s] = no write through x20\n", label);

    put_page(page);
}

static int __init tzprobe_init(void)
{
    pr_info("tzprobe: loaded on %s\n", utsname()->release);
    probe("psci_ver", 0xd0f1);   /* read-only PSCI_VERSION query */
    probe("fid_0",    0x0000);
    probe("fid_1",    0x0001);
    return 0;
}

static void __exit tzprobe_exit(void) { pr_info("tzprobe: unloaded\n"); }

module_init(tzprobe_init);
module_exit(tzprobe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("probe EL1 x20 use by the TrustZone SMC handler");
