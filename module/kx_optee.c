// SPDX-License-Identifier: GPL-2.0
/*
 * Empty OP-TEE's shared-memory cache immediately before kexec handoff.
 *
 * OP-TEE caches shared-memory buffers owned by the running kernel (the
 * "shm cache", enabled by the optee driver at probe). Across a kexec those
 * entries survive in the secure world and point at the OLD kernel's memory;
 * the new kernel then fails every RPC that references them:
 *     optee: handle_rpc_func_cmd: tee_shm_get_va ecca4640 failed
 * (seen in the kexec'd stock kernel: fbe_key never gets its key -> userspace
 * reboots). Upstream fixed this in 5.14 with an optee .shutdown hook that calls
 * OPTEE_SMC_DISABLE_SHM_CACHE until the cache is empty (commit
 * "optee: Disable shm cache during shutdown"). Amazon's 4.9 has no such hook,
 * so the "go" path calls this after the image is built.
 *
 * Each OPTEE_SMC_DISABLE_SHM_CACHE returns one cached entry (OK) until
 * ENOTAVAIL. The returned buffers belong to the kernel that is about to be
 * replaced, so they are deliberately not freed. Disabling the cache is safe for
 * the old kernel too: OP-TEE just stops caching (the cache is only re-enabled by
 * OPTEE_SMC_ENABLE_SHM_CACHE, which the new kernel's optee probe issues).
 */
#include <linux/module.h>
#include <linux/arm-smccc.h>

/* drivers/tee/optee/optee_smc.h */
#define OPTEE_SMC_DISABLE_SHM_CACHE	0xb200000a	/* fast call, trusted OS, fn 10 */
#define OPTEE_SMC_RETURN_OK		0x0
#define OPTEE_SMC_RETURN_ENOTAVAIL	0x7
#define KX_MAX_ENTRIES			256

int kx_optee_flush(void)
{
	struct arm_smccc_res res;
	int n;

	for (n = 0; n < KX_MAX_ENTRIES; n++) {
		arm_smccc_smc(OPTEE_SMC_DISABLE_SHM_CACHE, 0, 0, 0, 0, 0, 0, 0, &res);
		if (res.a0 == OPTEE_SMC_RETURN_ENOTAVAIL) {
			pr_info("optee_flush: shm cache empty and disabled (%d stale entries dropped)\n", n);
			return 0;
		}
		if (res.a0 != OPTEE_SMC_RETURN_OK) {
			pr_err("optee_flush: unexpected return %#lx after %d entries\n", res.a0, n);
			return -EIO;
		}
		pr_info("optee_flush: dropped cached shm %#lx:%#lx\n", res.a1, res.a2);
	}
	pr_err("optee_flush: gave up after %d entries\n", n);
	return -EIO;
}
