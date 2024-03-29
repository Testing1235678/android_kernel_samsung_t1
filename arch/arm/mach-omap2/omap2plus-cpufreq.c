/*
 *  OMAP2PLUS cpufreq driver
 *
 *  CPU frequency scaling for OMAP using OPP information
 *
 *  Copyright (C) 2005 Nokia Corporation
 *  Written by Tony Lindgren <tony@atomide.com>
 *
 *  Based on cpu-sa1110.c, Copyright (C) 2001 Russell King
 *
 * Copyright (C) 2007-2011 Texas Instruments, Inc.
 * Updated to support OMAP3
 * Rajendra Nayak <rnayak@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/opp.h>
#include <linux/cpu.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>

#include <asm/system.h>
#include <asm/smp_plat.h>
#include <asm/cpu.h>

#include <plat/clock.h>
#include <plat/omap-pm.h>
#include <plat/common.h>

#include <mach/hardware.h>
#include <mach/cpufreq_limits.h>

#include "dvfs.h"

#ifdef CONFIG_SMP
struct lpj_info {
	unsigned long	ref;
	unsigned int	freq;
};

static DEFINE_PER_CPU(struct lpj_info, lpj_ref);
static struct lpj_info global_lpj_ref;
#endif

static struct cpufreq_frequency_table *freq_table;
static atomic_t freq_table_users = ATOMIC_INIT(0);
static struct clk *mpu_clk;
static char *mpu_clk_name;
static struct device *mpu_dev;
static DEFINE_MUTEX(omap_cpufreq_lock);

static unsigned int max_thermal;
static unsigned int max_capped;
static unsigned int max_freq;
static unsigned int current_target_freq;
static unsigned int screen_off_max_freq;
static bool omap_cpufreq_ready;
static bool omap_cpufreq_suspended;

static unsigned int omap_getspeed(unsigned int cpu);
static int omap_target(struct cpufreq_policy *policy,
		       unsigned int target_freq,
		       unsigned int relation);
#ifdef CONFIG_DVFS_LIMIT
static DEFINE_MUTEX(omap_cpufreq_alloc_lock);
struct freq_lock_info {
	unsigned int dev_id;
	unsigned int cur_lock_freq;
	unsigned int value[DVFS_LOCK_ID_END];
	unsigned int init_level;
	unsigned int max_level;
	bool disable_lock;
	unsigned int freq_table_min;
	unsigned int freq_table_max;
	bool is_ceil;
};

/* CPU & BUS freq lock information */
#define MIN_LIMIT		0
#define MAX_LIMIT		1
static struct freq_lock_info cpufreq_lock_type[2] = {
	{
		.disable_lock	= false,
		.is_ceil = true,
	},
	{
		.disable_lock	= false,
		.is_ceil = false,
	}
};
bool cpufreq_compare(bool is_bigger, int reg, int level)
{
	if (is_bigger && reg > level)
		return true;
	else if (is_bigger && reg <= level)
		return false;

	if (!is_bigger && reg < level)
		return true;
	else if (!is_bigger && reg >= level)
		return false;

	return false;
}
static int omap_cpufreq_policy_notifier_call(struct notifier_block *this,
				unsigned long code, void *data)
{
	struct cpufreq_policy *policy = data;

	switch (code) {
	case CPUFREQ_ADJUST:
		if ((!strnicmp(policy->governor->name,
					"powersave", CPUFREQ_NAME_LEN))
				|| (!strnicmp(policy->governor->name,
					"performance", CPUFREQ_NAME_LEN))
				|| (!strnicmp(policy->governor->name,
					"userspace", CPUFREQ_NAME_LEN))) {

			pr_debug("cpufreq governor is changed to %s\n",
					policy->governor->name);

			cpufreq_lock_type[MIN_LIMIT].disable_lock = true;
			cpufreq_lock_type[MAX_LIMIT].disable_lock = true;
		} else {
			cpufreq_lock_type[MAX_LIMIT].disable_lock = false;
			cpufreq_lock_type[MIN_LIMIT].disable_lock = false;
		}
	case CPUFREQ_INCOMPATIBLE:
	case CPUFREQ_NOTIFY:
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block omap_cpufreq_policy_notifier = {
	.notifier_call = omap_cpufreq_policy_notifier_call,
};


int omap_cpufreq_alloc(unsigned int nId, unsigned long req_freq, int type)
{
	unsigned int cur_freq;
	unsigned long omap_freq;
	struct freq_lock_info *cpufreq_lock = &cpufreq_lock_type[type];

	mutex_lock(&omap_cpufreq_alloc_lock);

	if (cpufreq_lock->dev_id & (1 << nId)) {
		pr_err(
		"[CPUFREQ]This device [%d] already locked cpufreq\n", nId);
		mutex_unlock(&omap_cpufreq_alloc_lock);
		return 0;
	}

	if (cpufreq_lock_type[MAX_LIMIT].init_level < req_freq) {

		pr_err("[CPUFREQ] Wrong request cpufreq %lu (MAX level is %u)\n",
			req_freq, cpufreq_lock_type[MAX_LIMIT].init_level);

		req_freq = cpufreq_lock_type[MAX_LIMIT].init_level;
	}
	if (cpufreq_lock_type[MIN_LIMIT].init_level > req_freq) {
		pr_err("[CPUFREQ] Wrong request cpufreq %lu (MIN level is %u)\n",
			req_freq, cpufreq_lock_type[MIN_LIMIT].init_level);

		req_freq = cpufreq_lock_type[MIN_LIMIT].init_level;
	}

	cpufreq_lock->dev_id |= (1 << nId);
	cpufreq_lock->value[nId] = req_freq;

	/* If the requested cpufreq is higher than current min frequency */
	if (cpufreq_compare(cpufreq_lock->is_ceil,
				req_freq, cpufreq_lock->cur_lock_freq)) {

		struct device *mpu_dev = omap2_get_mpuss_device();

		omap_freq = req_freq * 1000;

		/* Find out near frequency*/
		if (cpufreq_lock->is_ceil)
			opp_find_freq_ceil(mpu_dev, &omap_freq);
		else
			opp_find_freq_floor(mpu_dev, &omap_freq);

		cpufreq_lock->cur_lock_freq = omap_freq/1000;

	}
	/* If current frequency is lower than requested freq,
	 * it needs to update
	 */
	cur_freq = omap_getspeed(0);


	pr_debug("[CPUFREQ] curr freq=%d KHz cur_lock_freq=%d KHz\n",
				cur_freq, cpufreq_lock->cur_lock_freq);

	if (cpufreq_compare(cpufreq_lock->is_ceil,
				cpufreq_lock->cur_lock_freq, cur_freq)) {

		struct cpufreq_policy *policy = cpufreq_cpu_get(0);
		omap_target(policy,
					cpufreq_lock->cur_lock_freq,
					CPUFREQ_RELATION_H);

		pr_debug("[CPUFREQ] Need to update current target(%d KHz)\n",
			cpufreq_lock->cur_lock_freq);
	}

	mutex_unlock(&omap_cpufreq_alloc_lock);

	return 0;
}

int omap_cpufreq_max_limit(unsigned int nId, unsigned long req_freq)
{
	return omap_cpufreq_alloc(nId, req_freq, MAX_LIMIT);
}

int omap_cpufreq_min_limit(unsigned int nId, unsigned long req_freq)
{
	return omap_cpufreq_alloc(nId, req_freq, MIN_LIMIT);
}
void omap_cpufreq_lock_free(unsigned int nId, int type)

{
	unsigned int i;

	struct freq_lock_info *cpufreq_lock = &cpufreq_lock_type[type];

	mutex_lock(&omap_cpufreq_alloc_lock);

	cpufreq_lock->dev_id &= ~(1 << nId);
	cpufreq_lock->value[nId] = cpufreq_lock->init_level;
	cpufreq_lock->cur_lock_freq = cpufreq_lock->init_level;
	if (cpufreq_lock->dev_id) {
		for (i = 0; i < DVFS_LOCK_ID_END; i++) {
			if (cpufreq_compare(cpufreq_lock->is_ceil ,
					cpufreq_lock->value[i] ,
					cpufreq_lock->cur_lock_freq))
				cpufreq_lock->cur_lock_freq =
					cpufreq_lock->value[i];
		}
	}
	pr_debug("[CPUFREQ] cur_lock_freq=%d KHz\n",
				cpufreq_lock->cur_lock_freq);
	mutex_unlock(&omap_cpufreq_alloc_lock);


	return;
}
void omap_cpufreq_min_limit_free(unsigned int nId)
{
	return omap_cpufreq_lock_free(nId, MIN_LIMIT);
}
void omap_cpufreq_max_limit_free(unsigned int nId)
{

	return omap_cpufreq_lock_free(nId, MAX_LIMIT);
}
#endif


static unsigned int omap_getspeed(unsigned int cpu)
{
	unsigned long rate;

	if (cpu >= NR_CPUS)
		return 0;

	rate = clk_get_rate(mpu_clk) / 1000;
	return rate;
}

static int omap_cpufreq_scale(unsigned int target_freq, unsigned int cur_freq)
{
	unsigned int i;
	int ret;
	struct cpufreq_freqs freqs;

	freqs.new = target_freq;
	freqs.old = omap_getspeed(0);



#ifdef CONFIG_DVFS_LIMIT
	if (!max_capped && cpufreq_lock_type[MIN_LIMIT].disable_lock == false) {

		if (target_freq < cpufreq_lock_type[MIN_LIMIT].cur_lock_freq)
			freqs.new = cpufreq_lock_type[MIN_LIMIT].cur_lock_freq;

		if (target_freq > cpufreq_lock_type[MAX_LIMIT].cur_lock_freq)
			freqs.new = cpufreq_lock_type[MAX_LIMIT].cur_lock_freq;
	}
#endif
	/*
	 * If the new frequency is more than the thermal max allowed
	 * frequency, go ahead and scale the mpu device to proper frequency.
	 */
	if (freqs.new > max_thermal)
		freqs.new = max_thermal;

	if (max_capped && freqs.new > max_capped)
		freqs.new = max_capped;


	if ((freqs.old == freqs.new) && (cur_freq == freqs.new))
		return 0;

	get_online_cpus();

	/* notifiers */
	for_each_online_cpu(freqs.cpu)
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);

#ifdef CONFIG_CPU_FREQ_DEBUG
	pr_info("cpufreq-omap: transition: %u --> %u\n", freqs.old, freqs.new);
#endif

	ret = omap_device_scale(mpu_dev, mpu_dev, freqs.new * 1000);

	freqs.new = omap_getspeed(0);

#ifdef CONFIG_SMP
	/*
	 * Note that loops_per_jiffy is not updated on SMP systems in
	 * cpufreq driver. So, update the per-CPU loops_per_jiffy value
	 * on frequency transition. We need to update all dependent CPUs.
	 */
	for_each_possible_cpu(i) {
		struct lpj_info *lpj = &per_cpu(lpj_ref, i);
		if (!lpj->freq) {
			lpj->ref = per_cpu(cpu_data, i).loops_per_jiffy;
			lpj->freq = freqs.old;
		}

		per_cpu(cpu_data, i).loops_per_jiffy =
			cpufreq_scale(lpj->ref, lpj->freq, freqs.new);
	}

	/* And don't forget to adjust the global one */
	if (!global_lpj_ref.freq) {
		global_lpj_ref.ref = loops_per_jiffy;
		global_lpj_ref.freq = freqs.old;
	}
	loops_per_jiffy = cpufreq_scale(global_lpj_ref.ref, global_lpj_ref.freq,
					freqs.new);
#endif

	/* notifiers */
	for_each_online_cpu(freqs.cpu)
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	put_online_cpus();

	return ret;
}

static unsigned int omap_thermal_lower_speed(void)
{
	unsigned int max = 0;
	unsigned int curr;
	int i;

	curr = max_thermal;

	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++)
		if (freq_table[i].frequency > max &&
		    freq_table[i].frequency < curr)
			max = freq_table[i].frequency;

	if (!max)
		return curr;

	return max;
}

void omap_thermal_throttle(void)
{
	unsigned int cur;

	if (!omap_cpufreq_ready) {
		pr_warn_once("%s: Thermal throttle prior to CPUFREQ ready\n",
			     __func__);
		return;
	}

	mutex_lock(&omap_cpufreq_lock);

	max_thermal = omap_thermal_lower_speed();

	pr_warn("%s: temperature too high, cpu throttle at max %u\n",
		__func__, max_thermal);

	if (!omap_cpufreq_suspended) {
		cur = omap_getspeed(0);
		if (cur > max_thermal)
			omap_cpufreq_scale(max_thermal, cur);
	}

	mutex_unlock(&omap_cpufreq_lock);
}

void omap_thermal_unthrottle(void)
{
	unsigned int cur;

	if (!omap_cpufreq_ready)
		return;

	mutex_lock(&omap_cpufreq_lock);

	if (max_thermal == max_freq) {
		pr_warn("%s: not throttling\n", __func__);
		goto out;
	}

	max_thermal = max_freq;

	pr_warn("%s: temperature reduced, ending cpu throttling\n", __func__);

	if (!omap_cpufreq_suspended) {
		cur = omap_getspeed(0);
		omap_cpufreq_scale(current_target_freq, cur);
	}

out:
	mutex_unlock(&omap_cpufreq_lock);
}

static int omap_verify_speed(struct cpufreq_policy *policy)
{
	if (!freq_table)
		return -EINVAL;
	return cpufreq_frequency_table_verify(policy, freq_table);
}

static int omap_target(struct cpufreq_policy *policy,
		       unsigned int target_freq,
		       unsigned int relation)
{
	unsigned int i;
	int ret = 0;

	if (!freq_table) {
		dev_err(mpu_dev, "%s: cpu%d: no freq table!\n", __func__,
				policy->cpu);
		return -EINVAL;
	}

	ret = cpufreq_frequency_table_target(policy, freq_table, target_freq,
			relation, &i);
	if (ret) {
		dev_dbg(mpu_dev, "%s: cpu%d: no freq match for %d(ret=%d)\n",
			__func__, policy->cpu, target_freq, ret);
		return ret;
	}

	mutex_lock(&omap_cpufreq_lock);

	current_target_freq = freq_table[i].frequency;

	if (!omap_cpufreq_suspended)
		ret = omap_cpufreq_scale(current_target_freq, policy->cur);


	mutex_unlock(&omap_cpufreq_lock);

	return ret;
}

static void omap_cpu_early_suspend(struct early_suspend *h)
{
	unsigned int cur;

	mutex_lock(&omap_cpufreq_lock);

	if (screen_off_max_freq) {
		max_capped = screen_off_max_freq;

		cur = omap_getspeed(0);
		if (cur > max_capped)
			omap_cpufreq_scale(max_capped, cur);
	}

	mutex_unlock(&omap_cpufreq_lock);
}

static void omap_cpu_late_resume(struct early_suspend *h)
{
	unsigned int cur;

	mutex_lock(&omap_cpufreq_lock);

	if (max_capped) {
		max_capped = 0;

		cur = omap_getspeed(0);
		if (cur != current_target_freq)
			omap_cpufreq_scale(current_target_freq, cur);
	}

	mutex_unlock(&omap_cpufreq_lock);
}

static struct early_suspend omap_cpu_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 50,
	.suspend = omap_cpu_early_suspend,
	.resume = omap_cpu_late_resume,
};

static inline void freq_table_free(void)
{
	if (atomic_dec_and_test(&freq_table_users))
		opp_free_cpufreq_table(mpu_dev, &freq_table);
}

static int __cpuinit omap_cpu_init(struct cpufreq_policy *policy)
{
	int result = 0;
	int i;

	mpu_clk = clk_get(NULL, mpu_clk_name);
	if (IS_ERR(mpu_clk))
		return PTR_ERR(mpu_clk);

	if (policy->cpu >= NR_CPUS) {
		result = -EINVAL;
		goto fail_ck;
	}

	policy->cur = policy->min = policy->max = omap_getspeed(policy->cpu);

	if (atomic_inc_return(&freq_table_users) == 1)
		result = opp_init_cpufreq_table(mpu_dev, &freq_table);

	if (result) {
		dev_err(mpu_dev, "%s: cpu%d: failed creating freq table[%d]\n",
				__func__, policy->cpu, result);
		goto fail_ck;
	}

	result = cpufreq_frequency_table_cpuinfo(policy, freq_table);
	if (result)
		goto fail_table;

	cpufreq_frequency_table_get_attr(freq_table, policy->cpu);

	policy->min = 300000;
	policy->max = policy->cpuinfo.max_freq;
	policy->cur = omap_getspeed(policy->cpu);

	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++)
		max_freq = max(freq_table[i].frequency, max_freq);
	max_thermal = max_freq;

	/*
	 * On OMAP SMP configuartion, both processors share the voltage
	 * and clock. So both CPUs needs to be scaled together and hence
	 * needs software co-ordination. Use cpufreq affected_cpus
	 * interface to handle this scenario. Additional is_smp() check
	 * is to keep SMP_ON_UP build working.
	 */
	if (is_smp()) {
		policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
		cpumask_setall(policy->cpus);
	}

	/* FIXME: what's the actual transition time? */
	policy->cpuinfo.transition_latency = 300 * 1000;

#ifdef CONFIG_DVFS_LIMIT
	if (policy->cpu == 0) {
		cpufreq_lock_type[MIN_LIMIT].cur_lock_freq =
				policy->cpuinfo.min_freq;
		cpufreq_lock_type[MIN_LIMIT].init_level =
				policy->cpuinfo.min_freq;

		cpufreq_lock_type[MAX_LIMIT].cur_lock_freq =
				policy->cpuinfo.max_freq;
		cpufreq_lock_type[MAX_LIMIT].init_level =
				policy->cpuinfo.max_freq;
	}
#endif


	return 0;

fail_table:
	freq_table_free();
fail_ck:
	clk_put(mpu_clk);
	return result;
}

static int omap_cpu_exit(struct cpufreq_policy *policy)
{
	freq_table_free();
	clk_put(mpu_clk);
	return 0;
}

static ssize_t show_screen_off_freq(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", screen_off_max_freq);
}

static ssize_t store_screen_off_freq(struct cpufreq_policy *policy,
	const char *buf, size_t count)
{
	unsigned int freq = 0;
	int ret;
	int index;

	if (!freq_table)
		return -EINVAL;

	ret = sscanf(buf, "%u", &freq);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&omap_cpufreq_lock);

	ret = cpufreq_frequency_table_target(policy, freq_table, freq,
		CPUFREQ_RELATION_H, &index);
	if (ret)
		goto out;

	screen_off_max_freq = freq_table[index].frequency;

	ret = count;

out:
	mutex_unlock(&omap_cpufreq_lock);
	return ret;
}

struct freq_attr omap_cpufreq_attr_screen_off_freq = {
	.attr = { .name = "screen_off_max_freq",
		  .mode = 0644,
		},
	.show = show_screen_off_freq,
	.store = store_screen_off_freq,
};

/*
 * OMAP4 MPU voltage control via cpufreq by Michael Huang (coolbho3k)
 *
 * Note: Each opp needs to have a discrete entry in both volt data and
 * dependent volt data (in opp4xxx_data.c), or voltage control breaks. Make a
 * new voltage entry for each opp. Keep this in mind when adding extra
 * frequencies.
 */

/* struct opp is defined elsewhere, but not in any accessible header files */
struct opp {
        struct list_head node;

        bool available;
        unsigned long rate;
        unsigned long u_volt;

        struct device_opp *dev_opp;
};

static ssize_t show_uv_mv_table(struct cpufreq_policy *policy, char *buf)
{
	int i = 0;
	unsigned long volt_cur;
	char *out = buf;
	struct opp *opp_cur;

	/* Reverse order sysfs entries for consistency */
	while(freq_table[i].frequency != CPUFREQ_TABLE_END)
                i++;

	/* For each entry in the cpufreq table, print the voltage */
	for(i--; i >= 0; i--) {
		if(freq_table[i].frequency != CPUFREQ_ENTRY_INVALID) {
			/* Find the opp for this frequency */
			opp_cur = opp_find_freq_exact(mpu_dev,
				freq_table[i].frequency*1000, true);
			/* sprint the voltage (mV)/frequency (MHz) pairs */
			volt_cur = opp_cur->u_volt;
			out += sprintf(out, "%umhz: %lu mV\n",
				freq_table[i].frequency/1000, volt_cur/1000);
		}
	}
        return out-buf;
}

static ssize_t store_uv_mv_table(struct cpufreq_policy *policy,
	const char *buf, size_t count)
{
	int i = 0;
	unsigned long volt_cur, volt_old;
	int ret;
	char size_cur[16];
	struct opp *opp_cur;
	struct voltagedomain *mpu_voltdm;
	mpu_voltdm = voltdm_lookup("mpu");

	while(freq_table[i].frequency != CPUFREQ_TABLE_END)
		i++;

	for(i--; i >= 0; i--) {
		if(freq_table[i].frequency != CPUFREQ_ENTRY_INVALID) {
			ret = sscanf(buf, "%lu", &volt_cur);
			if(ret != 1) {
				return -EINVAL;
			}

			/* Alter voltage. First do it in our opp */
			opp_cur = opp_find_freq_exact(mpu_dev,
				freq_table[i].frequency*1000, true);
			opp_cur->u_volt = volt_cur*1000;

			/* Then we need to alter voltage domains */
			/* Save our old voltage */
			volt_old = mpu_voltdm->vdd->volt_data[i].volt_nominal;
			/* Change our main and dependent voltage tables */
			mpu_voltdm->vdd->
				volt_data[i].volt_nominal = volt_cur*1000;
			mpu_voltdm->vdd->dep_vdd_info->
				dep_table[i].main_vdd_volt = volt_cur*1000;

			/* Alter current voltage in voltdm, if appropriate */
			if(volt_old == mpu_voltdm->curr_volt) {
				mpu_voltdm->curr_volt = volt_cur*1000;
			}

			/* Non-standard sysfs interface: advance buf */
			ret = sscanf(buf, "%s", size_cur);
			buf += (strlen(size_cur)+1);
		}
		else {
			pr_err("%s: frequency entry invalid for %u\n",
				__func__, freq_table[i].frequency);
		}
	}
	return count;
}

static struct freq_attr omap_uv_mv_table = {
	.attr = {.name = "UV_mV_table", .mode=0644,},
	.show = show_uv_mv_table,
	.store = store_uv_mv_table,
};

static struct freq_attr *omap_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&omap_cpufreq_attr_screen_off_freq,
	&omap_uv_mv_table,
	NULL,
};

static struct cpufreq_driver omap_driver = {
	.flags		= CPUFREQ_STICKY,
	.verify		= omap_verify_speed,
	.target		= omap_target,
	.get		= omap_getspeed,
	.init		= omap_cpu_init,
	.exit		= omap_cpu_exit,
	.name		= "omap2plus",
	.attr		= omap_cpufreq_attr,
};

static int omap_cpufreq_suspend_noirq(struct device *dev)
{
	mutex_lock(&omap_cpufreq_lock);
	omap_cpufreq_suspended = true;
	mutex_unlock(&omap_cpufreq_lock);
	return 0;
}

static int omap_cpufreq_resume_noirq(struct device *dev)
{
	unsigned int cur;

	mutex_lock(&omap_cpufreq_lock);
	cur = omap_getspeed(0);
	if (cur != current_target_freq)
		omap_cpufreq_scale(current_target_freq, cur);

	omap_cpufreq_suspended = false;
	mutex_unlock(&omap_cpufreq_lock);
	return 0;
}

static struct dev_pm_ops omap_cpufreq_driver_pm_ops = {
	.suspend_noirq = omap_cpufreq_suspend_noirq,
	.resume_noirq = omap_cpufreq_resume_noirq,
};

static struct platform_driver omap_cpufreq_platform_driver = {
	.driver.name = "omap_cpufreq",
	.driver.pm = &omap_cpufreq_driver_pm_ops,
};
static struct platform_device omap_cpufreq_device = {
	.name = "omap_cpufreq",
};

static int __init omap_cpufreq_init(void)
{
	int ret;

	if (cpu_is_omap24xx())
		mpu_clk_name = "virt_prcm_set";
	else if (cpu_is_omap34xx())
		mpu_clk_name = "dpll1_ck";
	else if (cpu_is_omap443x())
		mpu_clk_name = "dpll_mpu_ck";
	else if (cpu_is_omap446x())
		mpu_clk_name = "virt_dpll_mpu_ck";

	if (!mpu_clk_name) {
		pr_err("%s: unsupported Silicon?\n", __func__);
		return -EINVAL;
	}

	mpu_dev = omap2_get_mpuss_device();
	if (!mpu_dev) {
		pr_warning("%s: unable to get the mpu device\n", __func__);
		return -EINVAL;
	}

	register_early_suspend(&omap_cpu_early_suspend_handler);

	ret = cpufreq_register_driver(&omap_driver);
	omap_cpufreq_ready = !ret;

	if (!ret) {
		int t;

		t = platform_device_register(&omap_cpufreq_device);
		if (t)
			pr_warn("%s_init: platform_device_register failed\n",
				__func__);
		t = platform_driver_register(&omap_cpufreq_platform_driver);
		if (t)
			pr_warn("%s_init: platform_driver_register failed\n",
				__func__);
	}

#ifdef CONFIG_DVFS_LIMIT
	cpufreq_register_notifier(&omap_cpufreq_policy_notifier,
						CPUFREQ_POLICY_NOTIFIER);
#endif

	return ret;
}

static void __exit omap_cpufreq_exit(void)
{
	cpufreq_unregister_driver(&omap_driver);

	unregister_early_suspend(&omap_cpu_early_suspend_handler);
	platform_driver_unregister(&omap_cpufreq_platform_driver);
	platform_device_unregister(&omap_cpufreq_device);
}

MODULE_DESCRIPTION("cpufreq driver for OMAP2PLUS SOCs");
MODULE_LICENSE("GPL");
late_initcall(omap_cpufreq_init);
module_exit(omap_cpufreq_exit);
