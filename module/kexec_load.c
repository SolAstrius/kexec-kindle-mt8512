// SPDX-License-Identifier: GPL-2.0
/*
 * kexec_load.c - kexec loader for a kernel built without CONFIG_KEXEC
 * (MediaTek MT8512, Cortex-A53 in AArch32, Amazon Linux 4.9.77).
 *
 *   relocate.S       position-independent copy + jump trampoline
 *   idmap_handoff.c  identity map, cache flush, MMU-off jump
 *   kx_consys.c      connsys (WiFi/BT) power-off before the jump
 *   kx_optee.c       OP-TEE shared-memory cache flush before the jump
 *   kexec_load.c     this file: procfs staging, segment build, indirection
 *                    list, destination-collision avoidance, dryrun/go
 *
 * procfs (/proc/kexec_min/):
 *   kernel   (w)  raw zImage bytes     (cat kernel.bin  > .../kernel)
 *   initrd   (w)  raw initrd bytes     (cat ramdisk.bin > .../initrd)
 *   dtb      (w)  raw DTB bytes        (cat payload.dtb > .../dtb)
 *   params   (w)  KEY=VAL, one per write: kernel_phys / initrd_phys / dtb_phys
 *   ctl      (w)  "dryrun" | "conn-off" | "go" | "reset"
 *   status   (r)  staged sizes, computed plan, validation, online CPU count
 *
 * Default destinations, as U-Boot loads the stock FIT image:
 *   kernel -> 0x40008000   initrd -> 0x44080000   dtb -> 0x44000000
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cpumask.h>
#include <asm/page.h>

/* ---- contract with relocate.S (do not redefine) -------------------------- */
extern char kx_relocate[];
extern char kx_relocate_end[];
extern u32  kx_start_address;
extern u32  kx_indirection_page;
extern u32  kx_mach_type;
extern u32  kx_boot_atags;

/* ---- contract with the other objects of this module ---------------------- */
extern void kexec_handoff(unsigned long reboot_code_phys); /* never returns */
extern int kx_consys_off(void);
extern int kx_optee_flush(void);

/* kexec indirection-list entry bits (from include/linux/kexec.h) */
#define IND_DESTINATION  (1UL << 0)
#define IND_INDIRECTION  (1UL << 1)
#define IND_DONE         (1UL << 2)
#define IND_SOURCE       (1UL << 3)

/* Physical RAM window (1 GiB at 0x40000000). */
#define RAM_BASE   0x40000000UL
#define RAM_END    0x80000000UL

/* Regions a destination must NEVER touch (phys). {start, end} half-open. */
static const struct { unsigned long start, end; const char *name; } forbidden[] = {
	{ 0x43000000UL, 0x43030000UL, "ATF" },
	{ 0x43030000UL, 0x43500000UL, "OP-TEE (+shm)" },
	{ 0x44600000UL, 0x45300000UL, "display/pipeline" },
	{ 0x5f000000UL, 0x5f400000UL, "consys/wifi" },
};

/* Sanity caps on staged inputs. */
#define MAX_KERNEL  (32UL << 20)
#define MAX_INITRD  (32UL << 20)
#define MAX_DTB     (256UL << 10)

struct staging {
	void *buf;		/* vmalloc'd, allocated on first write */
	size_t cap;		/* allocated capacity */
	size_t len;		/* highest byte written */
};

struct segment {
	struct staging *src;
	unsigned long dest;	/* physical destination */
	const char *name;
};

/* One page held for the relocation, tracked so we can free on reset/unload. */
struct held_page {
	struct held_page *next;
	unsigned long va;	/* __get_free_page virtual addr, or 0 if quarantine-only */
};

static DEFINE_MUTEX(kx_lock);
static bool kx_conn_off_done;

static struct staging s_kernel, s_initrd, s_dtb;

static unsigned long dest_kernel = 0x40008000UL;
static unsigned long dest_initrd = 0x44080000UL;
static unsigned long dest_dtb    = 0x44000000UL;

/* pages we must keep alive until the jump (or free on reset) */
static struct held_page *held_list;	/* used pages: control, list, safe-source */
static struct held_page *quarantine;	/* pages that collided with a dest; held so
					   the allocator won't hand them back */

/* last computed plan, for status */
static struct {
	bool built;
	bool valid;
	unsigned long control_phys;
	unsigned long list_phys;
	unsigned long entry;
	unsigned long dtb_phys;
	unsigned int  nr_src_pages;
	char err[128];
} plan;

/* -------------------------------------------------------------------------- */

static void free_held(struct held_page **list)
{
	struct held_page *h = *list, *n;

	while (h) {
		n = h->next;
		if (h->va)
			free_page(h->va);
		kfree(h);
		h = n;
	}
	*list = NULL;
}

static void reset_plan(void)
{
	free_held(&held_list);
	free_held(&quarantine);
	memset(&plan, 0, sizeof(plan));
}

static void reset_all(void)
{
	reset_plan();
	if (s_kernel.buf) { vfree(s_kernel.buf); memset(&s_kernel, 0, sizeof(s_kernel)); }
	if (s_initrd.buf) { vfree(s_initrd.buf); memset(&s_initrd, 0, sizeof(s_initrd)); }
	if (s_dtb.buf)    { vfree(s_dtb.buf);    memset(&s_dtb,    0, sizeof(s_dtb)); }
}

static bool range_forbidden(unsigned long start, unsigned long len, const char **why)
{
	unsigned long end = start + len;
	size_t i;

	if (start < RAM_BASE || end > RAM_END || end < start) {
		*why = "outside RAM";
		return true;
	}
	for (i = 0; i < ARRAY_SIZE(forbidden); i++) {
		if (start < forbidden[i].end && end > forbidden[i].start) {
			*why = forbidden[i].name;
			return true;
		}
	}
	return false;
}

/* Does phys page [pa, pa+PAGE_SIZE) fall inside any destination segment? */
static bool page_hits_dest(unsigned long pa, struct segment *segs, int nseg)
{
	int i;

	for (i = 0; i < nseg; i++) {
		unsigned long ds = segs[i].dest;
		unsigned long de = ds + PAGE_ALIGN(segs[i].src->len);

		if (pa < de && pa + PAGE_SIZE > ds)
			return true;
	}
	return false;
}

/*
 * Allocate one zeroed page whose physical address does NOT collide with any
 * destination segment (mirrors kimage_alloc_page). Colliding pages are parked
 * on the quarantine list so the buddy allocator won't return them again; they
 * are freed on reset. Returns the kernel VA, or 0 on OOM.
 */
static unsigned long alloc_safe_page(struct segment *segs, int nseg)
{
	int tries = 0;

	for (;;) {
		unsigned long va = get_zeroed_page(GFP_KERNEL);
		unsigned long pa;
		struct held_page *h;

		if (!va)
			return 0;
		pa = virt_to_phys((void *)va);

		if (!page_hits_dest(pa, segs, nseg)) {
			h = kzalloc(sizeof(*h), GFP_KERNEL);
			if (!h) { free_page(va); return 0; }
			h->va = va;
			h->next = held_list;
			held_list = h;
			return va;
		}

		/* collides: park it and keep looking */
		h = kzalloc(sizeof(*h), GFP_KERNEL);
		if (!h) { free_page(va); return 0; }
		h->va = va;
		h->next = quarantine;
		quarantine = h;

		if (++tries > 200000)		/* pathological; bail rather than hang */
			return 0;
	}
}

/* -------------------------------------------------------------------------- */
/* Build the whole relocation image. Returns 0 on success, sets plan.err.     */

static int build_image(void)
{
	struct segment segs[3];
	int nseg = 0, i;
	const char *why = NULL;
	unsigned long ctl_va, ctl_pa;
	unsigned long *cur_list = NULL;	/* current list page (VA) */
	unsigned long list_pa_first = 0;
	unsigned int list_idx = 0;	/* entries used in current list page */
	unsigned int per_page = PAGE_SIZE / sizeof(unsigned long); /* 1024 */
	unsigned int nsrc = 0;
	size_t off, blob;

	reset_plan();

	if (!s_kernel.len || !s_initrd.len || !s_dtb.len) {
		scnprintf(plan.err, sizeof(plan.err),
			  "incomplete staging (kernel=%zu initrd=%zu dtb=%zu)",
			  s_kernel.len, s_initrd.len, s_dtb.len);
		return -EINVAL;
	}

	segs[nseg++] = (struct segment){ &s_kernel, dest_kernel, "kernel" };
	segs[nseg++] = (struct segment){ &s_initrd, dest_initrd, "initrd" };
	segs[nseg++] = (struct segment){ &s_dtb,    dest_dtb,    "dtb" };

	/* validate destinations: forbidden ranges + inter-segment overlap */
	for (i = 0; i < nseg; i++) {
		if (range_forbidden(segs[i].dest, PAGE_ALIGN(segs[i].src->len), &why)) {
			scnprintf(plan.err, sizeof(plan.err),
				  "%s dest %#lx+%#zx hits %s",
				  segs[i].name, segs[i].dest, segs[i].src->len, why);
			return -EINVAL;
		}
	}
	for (i = 0; i < nseg; i++) {
		int j;
		for (j = i + 1; j < nseg; j++) {
			unsigned long a0 = segs[i].dest, a1 = a0 + PAGE_ALIGN(segs[i].src->len);
			unsigned long b0 = segs[j].dest, b1 = b0 + PAGE_ALIGN(segs[j].src->len);
			if (a0 < b1 && b0 < a1) {
				scnprintf(plan.err, sizeof(plan.err),
					  "segments %s and %s overlap", segs[i].name, segs[j].name);
				return -EINVAL;
			}
		}
	}

	/* control code page: copy the kx_relocate blob to offset 0 */
	blob = (size_t)(kx_relocate_end - kx_relocate);
	if (blob > PAGE_SIZE) {
		scnprintf(plan.err, sizeof(plan.err), "relocate blob too big (%zu)", blob);
		return -EINVAL;
	}
	ctl_va = alloc_safe_page(segs, nseg);
	if (!ctl_va) { scnprintf(plan.err, sizeof(plan.err), "OOM control page"); return -ENOMEM; }
	memcpy((void *)ctl_va, kx_relocate, blob);
	ctl_pa = virt_to_phys((void *)ctl_va);

	/*
	 * Build the indirection list. For each segment:
	 *   IND_DESTINATION(dest)
	 *   IND_SOURCE(safe_page_phys) * ceil(len/PAGE)
	 * then a final IND_DONE. Chain list pages with IND_INDIRECTION, reserving
	 * the last slot of a full page for the chain pointer.
	 */
	for (i = 0; i < nseg; i++) {
		size_t len = segs[i].src->len;
		size_t done = 0;
		unsigned long dest = segs[i].dest;

		/* IND_DESTINATION */
		if (!cur_list || list_idx >= per_page - 1) {
			unsigned long nxt = alloc_safe_page(segs, nseg);
			unsigned long nxt_pa;
			if (!nxt) { scnprintf(plan.err, sizeof(plan.err), "OOM list page"); return -ENOMEM; }
			nxt_pa = virt_to_phys((void *)nxt);
			if (!cur_list) {
				list_pa_first = nxt_pa;
			} else {
				cur_list[list_idx] = nxt_pa | IND_INDIRECTION;
			}
			cur_list = (unsigned long *)nxt;
			list_idx = 0;
		}
		cur_list[list_idx++] = (dest & PAGE_MASK) | IND_DESTINATION;

		while (done < len) {
			size_t chunk = min((size_t)PAGE_SIZE, len - done);
			unsigned long safe = alloc_safe_page(segs, nseg);
			unsigned long safe_pa;

			if (!safe) { scnprintf(plan.err, sizeof(plan.err), "OOM source page"); return -ENOMEM; }
			memcpy((void *)safe, segs[i].src->buf + done, chunk);
			/* remainder of page is already zero (get_zeroed_page) */
			safe_pa = virt_to_phys((void *)safe);

			if (list_idx >= per_page - 1) {
				unsigned long nxt = alloc_safe_page(segs, nseg);
				unsigned long nxt_pa;
				if (!nxt) { scnprintf(plan.err, sizeof(plan.err), "OOM list page"); return -ENOMEM; }
				nxt_pa = virt_to_phys((void *)nxt);
				cur_list[list_idx] = nxt_pa | IND_INDIRECTION;
				cur_list = (unsigned long *)nxt;
				list_idx = 0;
			}
			cur_list[list_idx++] = (safe_pa & PAGE_MASK) | IND_SOURCE;
			nsrc++;
			done += PAGE_SIZE;
		}
	}
	/* terminator */
	if (list_idx >= per_page) {
		unsigned long nxt = alloc_safe_page(segs, nseg);
		if (!nxt) { scnprintf(plan.err, sizeof(plan.err), "OOM list page (done)"); return -ENOMEM; }
		cur_list[per_page - 1] = virt_to_phys((void *)nxt) | IND_INDIRECTION;
		cur_list = (unsigned long *)nxt;
		list_idx = 0;
	}
	cur_list[list_idx++] = IND_DONE;

	/* patch the four data words in the control-page copy of the blob */
	off = (size_t)((char *)&kx_start_address - kx_relocate);
	*(u32 *)(ctl_va + off) = (u32)segs[0].dest;		/* new kernel entry */
	off = (size_t)((char *)&kx_indirection_page - kx_relocate);
	*(u32 *)(ctl_va + off) = (u32)list_pa_first;
	off = (size_t)((char *)&kx_mach_type - kx_relocate);
	*(u32 *)(ctl_va + off) = 0xffffffffU;			/* ~0 => DT boot */
	off = (size_t)((char *)&kx_boot_atags - kx_relocate);
	*(u32 *)(ctl_va + off) = (u32)segs[2].dest;		/* DTB phys */

	plan.built        = true;
	plan.valid        = true;
	plan.control_phys = ctl_pa;
	plan.list_phys    = list_pa_first;
	plan.entry        = segs[0].dest;
	plan.dtb_phys     = segs[2].dest;
	plan.nr_src_pages = nsrc;
	plan.err[0]       = '\0';
	return 0;
}

/* -------------------------------------------------------------------------- */
/* number of online CPUs, without the (non-exported) num_online_cpus symbol   */
static unsigned int online_cpus(void)
{
	unsigned int n = 0, cpu;

	for_each_online_cpu(cpu)
		n++;
	return n;
}

/* -------------------------------------------------------------------------- */
/* staging write handler shared by kernel/initrd/dtb                          */

static ssize_t stage_write(struct staging *st, size_t cap_max, const char *name,
			   const char __user *ubuf, size_t count, loff_t *ppos)
{
	size_t need;

	if (*ppos < 0)
		return -EINVAL;
	need = (size_t)*ppos + count;
	if (need > cap_max) {
		pr_err("kexec_min: %s too large (%zu > %zu)\n", name, need, cap_max);
		return -EFBIG;
	}

	mutex_lock(&kx_lock);
	if (!st->buf) {
		st->buf = vmalloc(cap_max);
		if (!st->buf) { mutex_unlock(&kx_lock); return -ENOMEM; }
		st->cap = cap_max;
		st->len = 0;
	}
	if (*ppos == 0)		/* a fresh `cat >` restarts the buffer */
		st->len = 0;
	if (copy_from_user(st->buf + *ppos, ubuf, count)) {
		mutex_unlock(&kx_lock);
		return -EFAULT;
	}
	if (need > st->len)
		st->len = need;
	mutex_unlock(&kx_lock);

	*ppos += count;
	return count;
}

static ssize_t w_kernel(struct file *f, const char __user *b, size_t c, loff_t *p)
{ return stage_write(&s_kernel, MAX_KERNEL, "kernel", b, c, p); }
static ssize_t w_initrd(struct file *f, const char __user *b, size_t c, loff_t *p)
{ return stage_write(&s_initrd, MAX_INITRD, "initrd", b, c, p); }
static ssize_t w_dtb(struct file *f, const char __user *b, size_t c, loff_t *p)
{ return stage_write(&s_dtb, MAX_DTB, "dtb", b, c, p); }

/* minimal unsigned-long parser (hex 0x.. or decimal); kstrtoul isn't exported */
static int parse_ul(const char *s, unsigned long *out)
{
	unsigned long v = 0;
	int base = 10;

	if (!*s)
		return -EINVAL;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
		if (!*s)
			return -EINVAL;
	}
	for (; *s; s++) {
		unsigned int d;

		if (*s >= '0' && *s <= '9')      d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else return -EINVAL;
		v = v * base + d;
	}
	*out = v;
	return 0;
}

/* params: KEY=VAL lines (kernel_phys / initrd_phys / dtb_phys) */
static ssize_t w_params(struct file *f, const char __user *ubuf, size_t count, loff_t *ppos)
{
	char kbuf[128];
	unsigned long val;
	char *eq;

	if (count == 0 || count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	strim(kbuf);

	eq = strchr(kbuf, '=');
	if (!eq)
		return -EINVAL;
	*eq = '\0';
	if (parse_ul(eq + 1, &val))
		return -EINVAL;

	mutex_lock(&kx_lock);
	if      (!strcmp(kbuf, "kernel_phys")) dest_kernel = val;
	else if (!strcmp(kbuf, "initrd_phys")) dest_initrd = val;
	else if (!strcmp(kbuf, "dtb_phys"))    dest_dtb    = val;
	else { mutex_unlock(&kx_lock); return -EINVAL; }
	mutex_unlock(&kx_lock);

	pr_info("kexec_min: param %s = %#lx\n", kbuf, val);
	return count;
}

/* ctl: dryrun | conn-off | go | reset */
static ssize_t w_ctl(struct file *f, const char __user *ubuf, size_t count, loff_t *ppos)
{
	char cmd[16];
	size_t n = min(count, sizeof(cmd) - 1);
	int rc;
	unsigned int nc;

	if (copy_from_user(cmd, ubuf, n))
		return -EFAULT;
	cmd[n] = '\0';
	strim(cmd);

	if (!strcmp(cmd, "reset")) {
		mutex_lock(&kx_lock);
		reset_all();
		mutex_unlock(&kx_lock);
		pr_info("kexec_min: staging + plan reset\n");
		return count;
	}

	if (!strcmp(cmd, "dryrun")) {
		mutex_lock(&kx_lock);
		rc = build_image();
		mutex_unlock(&kx_lock);
		if (rc) {
			pr_err("kexec_min: dryrun FAILED: %s\n", plan.err);
			return rc;
		}
		pr_info("kexec_min: dryrun OK\n");
		pr_info("  control_phys = %#lx\n", plan.control_phys);
		pr_info("  list_phys    = %#lx\n", plan.list_phys);
		pr_info("  entry        = %#lx (r2/dtb = %#lx, mach = ~0)\n",
			plan.entry, plan.dtb_phys);
		pr_info("  src pages    = %u  (kernel=%zu initrd=%zu dtb=%zu bytes)\n",
			plan.nr_src_pages, s_kernel.len, s_initrd.len, s_dtb.len);
		return count;
	}

	if (!strcmp(cmd, "conn-off")) {
		mutex_lock(&kx_lock);
		if (!kx_conn_off_done) {
			rc = kx_consys_off();
			if (rc) {
				mutex_unlock(&kx_lock);
				return rc;
			}
			kx_conn_off_done = true;
		}
		mutex_unlock(&kx_lock);
		return count;
	}

	if (!strcmp(cmd, "go")) {
		mutex_lock(&kx_lock);
		if (!kx_conn_off_done) {
			mutex_unlock(&kx_lock);
			pr_err("kexec_min: refusing go: conn-off has not completed\n");
			return -EPERM;
		}
		nc = online_cpus();
		if (nc != 1) {
			mutex_unlock(&kx_lock);
			pr_err("kexec_min: refusing go: %u CPUs online (offline all but one first: echo 0 > /sys/devices/system/cpu/cpuN/online)\n", nc);
			return -EBUSY;
		}
		rc = build_image();
		if (rc) {
			mutex_unlock(&kx_lock);
			pr_err("kexec_min: go aborted, build failed: %s\n", plan.err);
			return rc;
		}
		rc = kx_optee_flush();
		if (rc) {
			mutex_unlock(&kx_lock);
			pr_err("kexec_min: go aborted, OP-TEE cache flush failed: %d\n", rc);
			return rc;
		}
		/* keep the lock: we are not coming back */
		pr_emerg("kexec_min: JUMPING NOW: entry=%#lx dtb=%#lx control=%#lx list=%#lx\n",
			 plan.entry, plan.dtb_phys, plan.control_phys, plan.list_phys);
		kexec_handoff(plan.control_phys);	/* never returns */

		/* unreachable */
		mutex_unlock(&kx_lock);
		pr_err("kexec_min: handoff returned?!\n");
		return -EIO;
	}

	pr_err("kexec_min: unknown ctl '%s' (dryrun|conn-off|go|reset)\n", cmd);
	return -EINVAL;
}

/* status (read) */
static int status_show(struct seq_file *m, void *v)
{
	mutex_lock(&kx_lock);
	seq_printf(m, "staged: kernel=%zu initrd=%zu dtb=%zu bytes\n",
		   s_kernel.len, s_initrd.len, s_dtb.len);
	seq_printf(m, "dest:   kernel=%#lx initrd=%#lx dtb=%#lx\n",
		   dest_kernel, dest_initrd, dest_dtb);
	seq_printf(m, "cpus online: %u\n", online_cpus());
	seq_printf(m, "conn off: %s\n", kx_conn_off_done ? "yes" : "no");
	seq_printf(m, "relocate blob: %zu bytes\n", (size_t)(kx_relocate_end - kx_relocate));
	if (plan.built) {
		seq_printf(m, "plan: %s\n", plan.valid ? "VALID" : "INVALID");
		seq_printf(m, "  control_phys=%#lx list_phys=%#lx\n",
			   plan.control_phys, plan.list_phys);
		seq_printf(m, "  entry=%#lx dtb_phys=%#lx src_pages=%u\n",
			   plan.entry, plan.dtb_phys, plan.nr_src_pages);
	} else {
		seq_printf(m, "plan: not built (write 'dryrun' to ctl)\n");
	}
	if (plan.err[0])
		seq_printf(m, "last error: %s\n", plan.err);
	mutex_unlock(&kx_lock);
	return 0;
}
static int status_open(struct inode *ino, struct file *file)
{ return single_open(file, status_show, NULL); }

/* -------------------------------------------------------------------------- */

static const struct file_operations fops_kernel = { .owner = THIS_MODULE, .write = w_kernel, .llseek = default_llseek };
static const struct file_operations fops_initrd = { .owner = THIS_MODULE, .write = w_initrd, .llseek = default_llseek };
static const struct file_operations fops_dtb    = { .owner = THIS_MODULE, .write = w_dtb,    .llseek = default_llseek };
static const struct file_operations fops_params = { .owner = THIS_MODULE, .write = w_params };
static const struct file_operations fops_ctl    = { .owner = THIS_MODULE, .write = w_ctl };
static const struct file_operations fops_status = {
	.owner = THIS_MODULE, .open = status_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};

static struct proc_dir_entry *kx_dir;

static int __init kx_init(void)
{
	kx_dir = proc_mkdir("kexec_min", NULL);
	if (!kx_dir)
		return -ENOMEM;

	if (!proc_create_data("kernel", 0200, kx_dir, &fops_kernel, NULL) ||
	    !proc_create_data("initrd", 0200, kx_dir, &fops_initrd, NULL) ||
	    !proc_create_data("dtb",    0200, kx_dir, &fops_dtb,    NULL) ||
	    !proc_create_data("params", 0200, kx_dir, &fops_params, NULL) ||
	    !proc_create_data("ctl",    0200, kx_dir, &fops_ctl,    NULL) ||
	    !proc_create_data("status", 0444, kx_dir, &fops_status, NULL)) {
		remove_proc_subtree("kexec_min", NULL);
		return -ENOMEM;
	}

	pr_info("kexec_min: loaded. relocate blob = %zu bytes. "
		"stage kernel/initrd/dtb, then 'dryrun' then (single-CPU) 'go'.\n",
		(size_t)(kx_relocate_end - kx_relocate));
	return 0;
}

static void __exit kx_exit(void)
{
	remove_proc_subtree("kexec_min", NULL);
	mutex_lock(&kx_lock);
	reset_all();
	mutex_unlock(&kx_lock);
	pr_info("kexec_min: unloaded\n");
}

module_init(kx_init);
module_exit(kx_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kexec loader for kernels built without CONFIG_KEXEC (MT8512 Kindles)");
MODULE_AUTHOR("Sol Astrius Phoenix <me@danielsol.dev>");
