// SPDX-License-Identifier: GPL-2.0
/*
 * Power the MT8512 connsys (WiFi/BT) domain fully off before the jump.
 *
 * Unloading Amazon's WMT driver asserts the conn reset but leaves the power
 * domain on and bus protection released. The next kernel's WMT power-on
 * sequence assumes the cold-boot state and fails from there (chip ID poll
 * times out), so WiFi never comes up. This does the scpsys-style power-off
 * that MediaTek's scpsys driver has for this domain but ships commented out:
 * bus protection, isolation, reset, clock gate, power off.
 *
 * Called via ctl "conn-off" after the WMT modules have been removed.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>

#define SPM_PA 0x10006000
#define INFRA_PA 0x10001000
#define RGU_PA 0x10007000
#define CONN_PWR_CON 0x32c
#define PWR_STATUS 0x180 /* MT8512 scpsys offsets */
#define PWR_STATUS_2ND 0x184
#define CONN_STA BIT(1)
#define PWR_RST_B BIT(0)
#define PWR_ISO BIT(1)
#define PWR_ON BIT(2)
#define PWR_ON_2ND BIT(3)
#define PWR_CLK_DIS BIT(4)
#define RGU_SWSYSRST 0x18
#define RGU_KEY 0x88000000
#define RGU_CONN_RST BIT(12)

/* Commented-out MT8512_POWER_DOMAIN_CONN bp_table in mtk-scpsys.c. */
struct kx_bp { u16 set, en, sta; u32 bit; };
static const struct kx_bp kx_conn_bp[] = {
	{ 0x2a0, 0x2a4, 0x228, BIT(13) },
	{ 0x2a8, 0x2ac, 0x258, BIT(18) },
	{ 0x2a0, 0x2a4, 0x228, BIT(14) },
	{ 0x2a8, 0x2ac, 0x258, BIT(21) },
};

static int kx_wait(void __iomem *reg, u32 mask, u32 want, const char *what)
{
	int i;

	for (i = 0; i < 100000; i++) { /* at most 1 s */
		if ((readl(reg) & mask) == want)
			return 0;
		udelay(10);
	}
	pr_err("consys_off: timeout waiting for %s: reg=%08x mask=%08x want=%08x\n",
	       what, readl(reg), mask, want);
	return -ETIMEDOUT;
}

int kx_consys_off(void)
{
	void __iomem *spm, *infra, *rgu;
	u32 val;
	int i, ret = -ENOMEM;

	spm = ioremap(SPM_PA, 0x1000);
	infra = ioremap(INFRA_PA, 0x1000);
	rgu = ioremap(RGU_PA, 0x100);
	if (!spm || !infra || !rgu) {
		pr_err("consys_off: ioremap failed\n");
		goto out;
	}

	val = readl(spm + CONN_PWR_CON);
	pr_info("consys_off: before pwr=%08x sta=%08x/%08x bp=%08x/%08x reset=%08x\n",
		val, readl(spm + PWR_STATUS), readl(spm + PWR_STATUS_2ND),
		readl(infra + 0x2a4), readl(infra + 0x2ac),
		readl(rgu + RGU_SWSYSRST));
	if ((val & (PWR_ON | PWR_ON_2ND)) != (PWR_ON | PWR_ON_2ND) ||
	    (readl(spm + PWR_STATUS) & CONN_STA) != CONN_STA ||
	    (readl(spm + PWR_STATUS_2ND) & CONN_STA) != CONN_STA) {
		pr_err("consys_off: conn not fully on; refusing unknown state\n");
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(kx_conn_bp); i++) {
		const struct kx_bp *bp = &kx_conn_bp[i];

		/* set register is write-one-to-set; never RMW it. */
		writel(bp->bit, infra + bp->set);
		ret = kx_wait(infra + bp->sta, bp->bit, bp->bit, "bus protection ack");
		if (ret)
			goto out;
		if (!(readl(infra + bp->en) & bp->bit)) {
			pr_err("consys_off: bus protection step %d did not latch\n", i);
			ret = -EIO;
			goto out;
		}
	}

	/* scpsys_power_off: isolate, reset, gate clock, drop power requests. */
	val |= PWR_ISO;
	writel(val, spm + CONN_PWR_CON);
	val &= ~PWR_RST_B;
	writel(val, spm + CONN_PWR_CON);
	val |= PWR_CLK_DIS;
	writel(val, spm + CONN_PWR_CON);
	val &= ~PWR_ON;
	writel(val, spm + CONN_PWR_CON);
	val &= ~PWR_ON_2ND;
	writel(val, spm + CONN_PWR_CON);
	ret = kx_wait(spm + PWR_STATUS, CONN_STA, 0, "primary power off");
	if (ret)
		goto out;
	ret = kx_wait(spm + PWR_STATUS_2ND, CONN_STA, 0, "secondary power off");
	if (ret)
		goto out;

	val = readl(rgu + RGU_SWSYSRST);
	writel(RGU_KEY | (val & 0xffff) | RGU_CONN_RST, rgu + RGU_SWSYSRST);
	ret = kx_wait(rgu + RGU_SWSYSRST, RGU_CONN_RST, RGU_CONN_RST,
		      "conn reset");
	if (ret)
		goto out;
	pr_info("consys_off: power off confirmed pwr=%08x sta=%08x/%08x bp=%08x/%08x reset=%08x\n",
		readl(spm + CONN_PWR_CON), readl(spm + PWR_STATUS),
		readl(spm + PWR_STATUS_2ND), readl(infra + 0x2a4),
		readl(infra + 0x2ac), readl(rgu + RGU_SWSYSRST));
	ret = 0;
out:
	if (rgu)
		iounmap(rgu);
	if (infra)
		iounmap(infra);
	if (spm)
		iounmap(spm);
	return ret;
}
