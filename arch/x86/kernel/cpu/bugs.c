/*
 *  Copyright (C) 1994  Linus Torvalds
 *
 *  Cyrix stuff, June 1998 by:
 *	- Rafael R. Reilova (moved everything from head.S),
 *        <rreilova@ececs.uc.edu>
 *	- Channing Corn (tests & fixes),
 *	- Andrew D. Balsa (code cleanup).
 */
#include <linux/init.h>
#include <linux/utsname.h>
#include <linux/device.h>

#include <asm/nospec-branch.h>
#include <asm/cmdline.h>
#include <asm/bugs.h>
#include <asm/processor.h>
#include <asm/processor-flags.h>
#include <asm/i387.h>
#include <asm/msr.h>
#include <asm/paravirt.h>
#include <asm/alternative.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/intel-family.h>

#ifdef CONFIG_X86_32
#ifndef CONFIG_XEN
static int __init no_halt(char *s)
{
	WARN_ONCE(1, "\"no-hlt\" is deprecated, please use \"idle=poll\"\n");
	boot_cpu_data.hlt_works_ok = 0;
	return 1;
}

__setup("no-hlt", no_halt);
#endif

static int __init no_387(char *s)
{
	boot_cpu_data.hard_math = 0;
	write_cr0(X86_CR0_TS | X86_CR0_EM | X86_CR0_MP | read_cr0());
	return 1;
}

__setup("no387", no_387);

static double __initdata x = 4195835.0;
static double __initdata y = 3145727.0;

/*
 * This used to check for exceptions..
 * However, it turns out that to support that,
 * the XMM trap handlers basically had to
 * be buggy. So let's have a correct XMM trap
 * handler, and forget about printing out
 * some status at boot.
 *
 * We should really only care about bugs here
 * anyway. Not features.
 */
static void __init check_fpu(void)
{
	s32 fdiv_bug;

	if (!boot_cpu_data.hard_math) {
#ifndef CONFIG_MATH_EMULATION
		printk(KERN_EMERG "No coprocessor found and no math emulation present.\n");
		printk(KERN_EMERG "Giving up.\n");
		for (;;) ;
#endif
		return;
	}

	/*
	 * trap_init() enabled FXSR and company _before_ testing for FP
	 * problems here.
	 *
	 * Test for the divl bug..
	 */
	__asm__("fninit\n\t"
		"fldl %1\n\t"
		"fdivl %2\n\t"
		"fmull %2\n\t"
		"fldl %1\n\t"
		"fsubp %%st,%%st(1)\n\t"
		"fistpl %0\n\t"
		"fwait\n\t"
		"fninit"
		: "=m" (*&fdiv_bug)
		: "m" (*&x), "m" (*&y));

#ifndef CONFIG_XEN
	boot_cpu_data.fdiv_bug = fdiv_bug;
	if (boot_cpu_data.fdiv_bug)
		printk(KERN_WARNING "Hmm, FPU with FDIV bug.\n");
#endif
}

static void __init check_hlt(void)
{
#ifndef CONFIG_XEN
	if (boot_cpu_data.x86 >= 5 || paravirt_enabled())
		return;

	printk(KERN_INFO "Checking 'hlt' instruction... ");
	if (!boot_cpu_data.hlt_works_ok) {
		printk("disabled\n");
		return;
	}
	halt();
	halt();
	halt();
	halt();
	printk(KERN_CONT "OK.\n");
#endif
}

/*
 *	Most 386 processors have a bug where a POPAD can lock the
 *	machine even from user space.
 */

static void __init check_popad(void)
{
#ifndef CONFIG_X86_POPAD_OK
	int res, inp = (int) &res;

	printk(KERN_INFO "Checking for popad bug... ");
	__asm__ __volatile__(
	  "movl $12345678,%%eax; movl $0,%%edi; pusha; popa; movl (%%edx,%%edi),%%ecx "
	  : "=&a" (res)
	  : "d" (inp)
	  : "ecx", "edi");
	/*
	 * If this fails, it means that any user program may lock the
	 * CPU hard. Too bad.
	 */
	if (res != 12345678)
		printk(KERN_CONT "Buggy.\n");
	else
		printk(KERN_CONT "OK.\n");
#endif
}

/*
 * Check whether we are able to run this kernel safely on SMP.
 *
 * - In order to run on a i386, we need to be compiled for i386
 *   (for due to lack of "invlpg" and working WP on a i386)
 * - In order to run on anything without a TSC, we need to be
 *   compiled for a i486.
 */

static void __init check_config(void)
{
/*
 * We'd better not be a i386 if we're configured to use some
 * i486+ only features! (WP works in supervisor mode and the
 * new "invlpg" and "bswap" instructions)
 */
#if defined(CONFIG_X86_WP_WORKS_OK) || defined(CONFIG_X86_INVLPG) || \
	defined(CONFIG_X86_BSWAP)
	if (boot_cpu_data.x86 == 3)
		panic("Kernel requires i486+ for 'invlpg' and other features");
#endif
}
#endif /* CONFIG_X86_32 */

static void __init spectre_v2_select_mitigation(void);

void __init check_bugs(void)
{
#ifdef CONFIG_X86_32
	/*
	 * Regardless of whether PCID is enumerated, the SDM says
	 * that it can't be enabled in 32-bit mode.
	 */
	setup_clear_cpu_cap(X86_FEATURE_PCID);
#endif

	identify_boot_cpu();

#ifndef CONFIG_SMP
	printk(KERN_INFO "CPU: ");
	print_cpu_info(&boot_cpu_data);
#endif

	/* Select the proper spectre mitigation before patching alternatives */
	spectre_v2_select_mitigation();


#ifdef CONFIG_X86_32
	check_config();
	check_fpu();
	check_hlt();
	check_popad();
	init_utsname()->machine[1] =
		'0' + (boot_cpu_data.x86 > 6 ? 6 : boot_cpu_data.x86);
	alternative_instructions();

#else /* CONFIG_X86_64 */

	alternative_instructions();
#ifndef CONFIG_XEN
	/*
	 * Make sure the first 2MB area is not mapped by huge pages
	 * There are typically fixed size MTRRs in there and overlapping
	 * MTRRs into large pages causes slow downs.
	 *
	 * Right now we don't do that with gbpages because there seems
	 * very little benefit for that case.
	 */
	if (!direct_gbpages)
		set_memory_4k((unsigned long)__va(0), 1);
#endif /* CONFIG_XEN */
#endif
}

/* The kernel command line selection */
enum spectre_v2_mitigation_cmd {
	SPECTRE_V2_CMD_NONE,
	SPECTRE_V2_CMD_AUTO,
	SPECTRE_V2_CMD_FORCE,
	SPECTRE_V2_CMD_RETPOLINE,
	SPECTRE_V2_CMD_RETPOLINE_GENERIC,
	SPECTRE_V2_CMD_RETPOLINE_AMD,
};

static const char *spectre_v2_strings[] = {
	[SPECTRE_V2_NONE]			= "Vulnerable",
	[SPECTRE_V2_RETPOLINE_MINIMAL]		= "Vulnerable: Minimal generic ASM retpoline",
	[SPECTRE_V2_RETPOLINE_MINIMAL_AMD]	= "Vulnerable: Minimal AMD ASM retpoline",
	[SPECTRE_V2_RETPOLINE_GENERIC]		= "Mitigation: Full generic retpoline",
	[SPECTRE_V2_RETPOLINE_AMD]		= "Mitigation: Full AMD retpoline",
};

#undef pr_fmt
#define pr_fmt(fmt)     "Spectre V2 mitigation: " fmt

static enum spectre_v2_mitigation spectre_v2_enabled = SPECTRE_V2_NONE;

static bool x86_bug_spectre_v1, x86_bug_spectre_v2, x86_bug_meltdown;

void setup_force_cpu_bugs(unsigned long __unused)
{
	x86_bug_spectre_v1 = true;
	x86_bug_spectre_v2 = true;

	if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD)
		x86_bug_meltdown = false;
	else
		x86_bug_meltdown = true;
}

static void __init spec2_print_if_insecure(const char *reason)
{
	if (x86_bug_spectre_v2)
		pr_info("%s\n", reason);
}

static void __init spec2_print_if_secure(const char *reason)
{
	if (!x86_bug_spectre_v2)
		pr_info("%s\n", reason);
}

static inline bool retp_compiler(void)
{
#ifdef RETPOLINE
	return true;
#else
	return false;
#endif
}

static inline bool match_option(const char *arg, int arglen, const char *opt)
{
	int len = strlen(opt);

	return len == arglen && !strncmp(arg, opt, len);
}

static enum spectre_v2_mitigation_cmd __init spectre_v2_parse_cmdline(void)
{
	char arg[20];
	int ret;

	ret = cmdline_find_option(boot_command_line, "spectre_v2", arg,
				  sizeof(arg));
	if (ret > 0)  {
		if (match_option(arg, ret, "off")) {
			goto disable;
		} else if (match_option(arg, ret, "on")) {
			spec2_print_if_secure("force enabled on command line.");
			return SPECTRE_V2_CMD_FORCE;
		} else if (match_option(arg, ret, "retpoline")) {
			spec2_print_if_insecure("retpoline selected on command line.");
			return SPECTRE_V2_CMD_RETPOLINE;
		} else if (match_option(arg, ret, "retpoline,amd")) {
			if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD) {
				pr_err("retpoline,amd selected but CPU is not AMD. Switching to AUTO select\n");
				return SPECTRE_V2_CMD_AUTO;
			}
			spec2_print_if_insecure("AMD retpoline selected on command line.");
			return SPECTRE_V2_CMD_RETPOLINE_AMD;
		} else if (match_option(arg, ret, "retpoline,generic")) {
			spec2_print_if_insecure("generic retpoline selected on command line.");
			return SPECTRE_V2_CMD_RETPOLINE_GENERIC;
		} else if (match_option(arg, ret, "auto")) {
			return SPECTRE_V2_CMD_AUTO;
		}
	}

	if (!cmdline_find_option_bool(boot_command_line, "nospectre_v2"))
		return SPECTRE_V2_CMD_AUTO;
disable:
	spec2_print_if_insecure("disabled on command line.");
	return SPECTRE_V2_CMD_NONE;
}

/* Check for Skylake-like CPUs (for RSB handling) */
static bool __init is_skylake_era(void)
{
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL &&
	    boot_cpu_data.x86 == 6) {
		switch (boot_cpu_data.x86_model) {
		case INTEL_FAM6_SKYLAKE_MOBILE:
		case INTEL_FAM6_SKYLAKE_DESKTOP:
		case INTEL_FAM6_SKYLAKE_X:
		case INTEL_FAM6_KABYLAKE_MOBILE:
		case INTEL_FAM6_KABYLAKE_DESKTOP:
			return true;
		}
	}
	return false;
}

static void __init spectre_v2_select_mitigation(void)
{
	enum spectre_v2_mitigation_cmd cmd = spectre_v2_parse_cmdline();
	enum spectre_v2_mitigation mode = SPECTRE_V2_NONE;

	/*
	 * If the CPU is not affected and the command line mode is NONE or AUTO
	 * then nothing to do.
	 */
	if (!x86_bug_spectre_v2 &&
	    (cmd == SPECTRE_V2_CMD_NONE || cmd == SPECTRE_V2_CMD_AUTO))
		return;

	switch (cmd) {
	case SPECTRE_V2_CMD_NONE:
		return;

	case SPECTRE_V2_CMD_FORCE:
		/* FALLTRHU */
	case SPECTRE_V2_CMD_AUTO:
		goto retpoline_auto;

	case SPECTRE_V2_CMD_RETPOLINE_AMD:
#ifdef CONFIG_RETPOLINE
			goto retpoline_amd;
#endif
		break;
	case SPECTRE_V2_CMD_RETPOLINE_GENERIC:
#ifdef CONFIG_RETPOLINE
			goto retpoline_generic;
#endif
		break;
	case SPECTRE_V2_CMD_RETPOLINE:
#ifdef CONFIG_RETPOLINE
			goto retpoline_auto;
#endif
		break;
	}
	pr_err("kernel not compiled with retpoline; no mitigation available!");
	return;

retpoline_auto:
	if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD) {
	retpoline_amd:
		if (!boot_cpu_has(X86_FEATURE_LFENCE_RDTSC)) {
			pr_err("LFENCE not serializing. Switching to generic retpoline\n");
			goto retpoline_generic;
		}
		mode = retp_compiler() ? SPECTRE_V2_RETPOLINE_AMD :
					 SPECTRE_V2_RETPOLINE_MINIMAL_AMD;
		setup_force_cpu_cap(X86_FEATURE_RETPOLINE_AMD);
		setup_force_cpu_cap(X86_FEATURE_RETPOLINE);
	} else {
	retpoline_generic:
		mode = retp_compiler() ? SPECTRE_V2_RETPOLINE_GENERIC :
					 SPECTRE_V2_RETPOLINE_MINIMAL;
		setup_force_cpu_cap(X86_FEATURE_RETPOLINE);
	}

	spectre_v2_enabled = mode;
	pr_info("%s\n", spectre_v2_strings[mode]);

	/*
	 * If neither SMEP or KPTI are available, there is a risk of
	 * hitting userspace addresses in the RSB after a context switch
	 * from a shallow call stack to a deeper one. To prevent this fill
	 * the entire RSB, even when using IBRS.
	 *
	 * Skylake era CPUs have a separate issue with *underflow* of the
	 * RSB, when they will predict 'ret' targets from the generic BTB.
	 * The proper mitigation for this is IBRS. If IBRS is not supported
	 * or deactivated in favour of retpolines the RSB fill on context
	 * switch is required.
	 */
	if ((!boot_cpu_has(X86_FEATURE_KAISER) &&
	     !boot_cpu_has(X86_FEATURE_SMEP)) || is_skylake_era()) {
		setup_force_cpu_cap(X86_FEATURE_RSB_CTXSW);
		pr_info("Filling RSB on context switch\n");
	}
}

#undef pr_fmt

#ifdef CONFIG_SYSFS
ssize_t cpu_show_meltdown(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	if (!x86_bug_meltdown)
		return sprintf(buf, "Not affected\n");
	if (boot_cpu_has(X86_FEATURE_KAISER))
		return sprintf(buf, "Mitigation: PTI\n");
	return sprintf(buf, "Vulnerable\n");
}

ssize_t cpu_show_spectre_v1(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	if (!x86_bug_spectre_v1)
		return sprintf(buf, "Not affected\n");
	return sprintf(buf, "Mitigation: Barriers\n");
}

ssize_t cpu_show_spectre_v2(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	if (!x86_bug_spectre_v2)
		return sprintf(buf, "Not affected\n");
	return sprintf(buf, "Vulnerable\n");
}
#endif
