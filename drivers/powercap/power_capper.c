// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/thermal.h>
#include <asm/msr.h>
#include <asm/tsc.h>

#include "power_capper.h"

static struct kobject * pcapper_kobj;
struct task_struct * pcapper_thread;

static pwrmon_log_t * pwrmon_log[2];
static int last_log_idx[2];

static int running_status = 0;

static unsigned int power_unit;
static unsigned int energy_unit;
static unsigned int time_unit;
static const int dram_energy_unit = 15300;

static u64 target_power;
static int log_storage = 0;
static u64 new_target_ratio = 1;

extern int powerclamp_set_cur_state(struct thermal_cooling_device *cdev,
			unsigned long new_target_ratio);


static int rapl_check_unit_core(void)
{
	int cpu = 0;
	u64 msr_val;
	u32 value;
	
	if (rdmsrl_safe_on_cpu(cpu, MSR_RAPL_POWER_UNIT, &msr_val)) {
		pr_err("Failed to read power unit MSR 0x%x on CPU %d, exit.\n",
			   MSR_RAPL_POWER_UNIT, cpu);
		return -ENODEV;
	}
	
	value = (msr_val & ENERGY_UNIT_MASK) >> ENERGY_UNIT_OFFSET;
	energy_unit = ENERGY_UNIT_SCALE * 1000000 / (1 << value);
	
	value = (msr_val & POWER_UNIT_MASK) >> POWER_UNIT_OFFSET;
	power_unit = 1000000 / (1 << value);
	
	value = (msr_val & TIME_UNIT_MASK) >> TIME_UNIT_OFFSET;
	time_unit = 1000000 / (1 << value);
	
	pr_info("Units: energy=%dpJ, time=%dus, power=%duW\n",
			 energy_unit, time_unit, power_unit);
	
	return 0;
}

static inline void cali_idle_ratio (u32 power_level, u32 target_level) ;
static inline void set_idle_ratio (u32 power_level, u32 target_level) 
{
	u64 diff;

	if(power_level < target_level){
		cali_idle_ratio(power_level, target_level);
		return;
	}

	diff = (power_level - target_level);
	diff = (diff * 100) / (power_level - 50);

	if(new_target_ratio != 1)
		new_target_ratio += diff;
	else
		new_target_ratio = diff;

	new_target_ratio = new_target_ratio * 19 / 20;
	new_target_ratio = new_target_ratio == 0 ? 1 : new_target_ratio;

	powerclamp_set_cur_state(NULL, new_target_ratio);

	return;
}

static inline void cali_idle_ratio (u32 power_level, u32 target_level) 
{
	u32 diff;

	if(new_target_ratio == 1)
		return;
	if(power_level > target_level){
		set_idle_ratio(power_level, target_level);
		return;
	}
	
	diff = 1 + (target_level - power_level) * 100 / target_level;

	if(diff > 5) {
		diff -= 5;
		new_target_ratio = new_target_ratio > diff ? 
			new_target_ratio - diff : 1;

		powerclamp_set_cur_state(NULL, new_target_ratio);
	}

	return;
}
struct pkg_cstate_info {
	bool skip;
	int msr_index;
	int cstate_id;
};
#define PKG_CSTATE_INIT(id) {				\
		.msr_index = MSR_PKG_C##id##_RESIDENCY, \
		.cstate_id = id				\
			}
static struct pkg_cstate_info pkg_cstates[] = {
	PKG_CSTATE_INIT(2),
	PKG_CSTATE_INIT(3),
	PKG_CSTATE_INIT(6),
	PKG_CSTATE_INIT(7),
	PKG_CSTATE_INIT(8),
	PKG_CSTATE_INIT(9),
	PKG_CSTATE_INIT(10),
	{NULL},
};
static inline u64 pkg_state_counter(void)
{
	u64 val;
	u64 count = 0;
	struct pkg_cstate_info *info = pkg_cstates;

	while (info->msr_index) {
		if (!info->skip) {
			if (!rdmsrl_safe(info->msr_index, &val))
				count += val;
			else
				info->skip = true;
		}
		info++;
	}

	return count;
}

static inline int get_energy_now(msr_rapl_t * data) {
	int cpu = 0;
	u32 msr;
	u64 value_package, value_dram, final;
	

	data->cycles = get_cycles();

	msr = MSR_PKG_ENERGY_STATUS;
	if (rdmsrl_safe_on_cpu(cpu, msr, &value_package)) {
		pr_debug("failed to read msr 0x%x on cpu %d\n", msr, cpu);
		return -EIO;
	}
	msr = MSR_DRAM_ENERGY_STATUS;
	if (rdmsrl_safe_on_cpu(cpu, msr, &value_dram)) {
		pr_debug("failed to read msr 0x%x on cpu %d\n", msr, cpu);
		return -EIO;
	}
	final = value_package & ENERGY_STATUS_MASK;
	final *= energy_unit;
	data->e_package = div64_u64(final, ENERGY_UNIT_SCALE);
	
	final = value_dram & ENERGY_STATUS_MASK;
	final *= dram_energy_unit;
	data->e_dram = div64_u64(final, ENERGY_UNIT_SCALE);
	data->e_idle = pkg_state_counter();
	return 0;
}

int power_monitor(void * arg) {
	msr_rapl_t now = {0,};
	msr_rapl_t last = {0,};
	pwrmon_log_t * log;
	u64 total_energy, diff_idle;
#ifdef INTERVAL_TEST
	msr_rapl_t last_pow = {0,};
	u64 tmp_cycles, tmp_duration, tmp_package_energy, tmp_dram_energy, tmp_energy;
	int index = 0;
#endif
	
	last_log_idx[0] = 0;
	last_log_idx[1] = 0;
	
	get_energy_now(&last);
	
	while (running_status) {
		
		if (last_log_idx[log_storage] == MAX_NUMBER_OF_LOG)
			last_log_idx[log_storage] = 0;

#ifdef INTERVAL_TEST
		if(!index){
			msleep(25);
			get_energy_now(&last_pow);
			index++;
		}
		else {
			msleep(75);
			get_energy_now(&now);

			log = &(pwrmon_log[log_storage][last_log_idx[log_storage]]);

			log->tsc = now.cycles;
			tmp_cycles = now.cycles - last_pow.cycles;
			now.cycles -= last.cycles;

			log->duration = div64_u64 ((now.cycles * 1000000), tsc_khz); //unit = ns
			tmp_duration = div64_u64 ((tmp_cycles * 1000000), tsc_khz); //unit = ns

			log->package_energy = now.e_package - last.e_package;
			log->dram_energy = now.e_dram - last.e_dram;
			tmp_package_energy = now.e_package - last_pow.e_package;
			tmp_dram_energy = now.e_dram - last_pow.e_dram;

			diff_idle = 100*(now.e_idle - last.e_idle);
			log->cur_idle_rate = div64_u64(diff_idle, now.cycles);

			total_energy = log->package_energy + log->dram_energy;
			tmp_energy = tmp_package_energy + tmp_dram_energy;

			log->average_power = div64_u64(total_energy * 1000, log->duration);
			log->change_power = div64_u64(tmp_energy * 1000, tmp_duration);

			log->target_power = target_power;

			last_log_idx[log_storage]++;

			last.cycles = log->tsc;
			last.e_package = now.e_package;
			last.e_dram = now.e_dram;
			last.e_idle = now.e_idle;

			if (log->average_power > target_power) {
				set_idle_ratio(log->change_power, target_power);
			} else
				cali_idle_ratio(log->change_power, target_power);

			log->idle_rate = new_target_ratio;

			index--;
		}
#else	
		msleep(100);
		
		if (last_log_idx[log_storage] == MAX_NUMBER_OF_LOG)
			last_log_idx[log_storage] = 0;
		
		get_energy_now(&now);
		
		log = &(pwrmon_log[log_storage][last_log_idx[log_storage]]);
		
		log->tsc = now.cycles;
		now.cycles -= last.cycles;
		log->duration = div64_u64 ((now.cycles * 1000000), tsc_khz); //unit = ns
		log->package_energy = now.e_package - last.e_package;
		log->dram_energy = now.e_dram - last.e_dram;
		diff_idle = 100*(now.e_idle - last.e_idle);
		log->cur_idle_rate = div64_u64(diff_idle, now.cycles);
		total_energy = log->package_energy + log->dram_energy;
		log->average_power = div64_u64(total_energy * 1000, log->duration);
		log->target_power = target_power;
		
		last_log_idx[log_storage]++;
		
		last.cycles = log->tsc;
		last.e_package = now.e_package;
		last.e_dram = now.e_dram;
		last.e_idle = now.e_idle;
		
		if (log->average_power > target_power) {
			set_idle_ratio(log->average_power, target_power);
		} else
			cali_idle_ratio(log->average_power, target_power);

		log->idle_rate = new_target_ratio;
		
	}
#endif
	pr_info("I`m stopping\n");
	return 0;
}

int start_monitor (void) {
	pcapper_thread = kthread_create (power_monitor,
									 (void *) NULL,
									 "power_monitor");
	if (likely (!IS_ERR(pcapper_thread))) {
		wake_up_process(pcapper_thread);
	}
	return 1;
}

static ssize_t start_store (struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count)
{
	int input;
	sscanf(buf, "%d", &input);
	if (input > 0 && running_status == 0) {
		running_status = 1;
		start_monitor();
	}
	return count;
}

static ssize_t stop_store (struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count)
{
	int input;
	sscanf(buf, "%d", &input);
	
	if (input > 0) {
		running_status = 0;
	}
	return count;
}
static ssize_t set_target_store (struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count)
{
	int input;
	sscanf(buf, "%d", &input);
	if (input > 0) {
		target_power = input;
	}
	return count;
}

static ssize_t status_show (struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	if (running_status == 0)
	return sprintf(buf, "stopped");
	else if (running_status == 1)
	return sprintf(buf, "running");
	return sprintf(buf,"unknown");
}

static ssize_t dump_log_show (struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i,rt;
	pwrmon_log_t * data;
	int logs = log_storage;
	
	last_log_idx[!log_storage] = 0;
	log_storage = !log_storage;
	
	for (i=0; i < (last_log_idx[logs]-1) ;i++) {
		data = &(pwrmon_log[logs][i]);
#ifdef INTERVAL_TEST
		rt = sprintf(buf,"%s%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu\n",
					 buf, data->average_power,data->change_power,data->target_power,data->cur_idle_rate,data->idle_rate,data->duration,
					 data->package_energy, data->dram_energy, data->tsc);
#else
		rt = sprintf(buf,"%s%u,%u,%llu,%llu,%llu,%llu,%llu,%llu\n",
					 buf, data->average_power,data->target_power,data->cur_idle_rate,data->idle_rate,data->duration,
					 data->package_energy, data->dram_energy, data->tsc);
#endif
	}
	if (i==0){
		rt = sprintf(buf,"NONE");
	}
	return rt;
}

#define PCAPPER_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)
#define PCAPPER_ATTR_WO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_WO(_name)

PCAPPER_ATTR_WO(start);
PCAPPER_ATTR_WO(stop);
PCAPPER_ATTR_WO(set_target);
PCAPPER_ATTR_RO(status);
PCAPPER_ATTR_RO(dump_log);

static struct attribute *pwrmon_attr[] = {
	&start_attr.attr,
	&stop_attr.attr,
	&set_target_attr.attr,
	&status_attr.attr,
	&dump_log_attr.attr,
	NULL,
};

static struct attribute_group pwrmon_attr_group = {
	.attrs = pwrmon_attr,
	.name = "power_monitor",
};

static int __init powercapper_init(void)
{
	int retval;
	
	pcapper_kobj = kobject_create_and_add("powercapper", NULL);
	retval = sysfs_create_group(pcapper_kobj, &pwrmon_attr_group);
	if (retval) {
		pr_err("%s: sysfs group create failed\n", __func__);
		goto exit;
	}
	
	pwrmon_log[0] = kmalloc(sizeof(pwrmon_log_t)*MAX_NUMBER_OF_LOG, GFP_KERNEL);
	if (!pwrmon_log[0]) {
		pr_err("%s: memory allocation failed\n", __func__);
		goto exit_kobj;
	}
	pwrmon_log[1] = kmalloc(sizeof(pwrmon_log_t)*MAX_NUMBER_OF_LOG, GFP_KERNEL);
	if (!pwrmon_log[1]) {
		pr_err("%s: memory allocation failed\n", __func__);
		goto exit_free;
	}

	pr_info("power capper is loaded\n");
	rapl_check_unit_core();
	powerclamp_set_cur_state(NULL, new_target_ratio);
	
	return 0;
exit_free:
	kfree(pwrmon_log[0]);
exit_kobj:
	sysfs_remove_group(pcapper_kobj, &pwrmon_attr_group);
exit:
	kobject_put(pcapper_kobj);
	return retval;
	
}

static void __exit powercapper_exit(void)
{
	sysfs_remove_group(pcapper_kobj, &pwrmon_attr_group);
	kobject_put(pcapper_kobj);
	kfree(pwrmon_log[0]);
	kfree(pwrmon_log[1]);
	pr_info("power capper is unloaded\n");
}

module_init(powercapper_init);
module_exit(powercapper_exit);

MODULE_AUTHOR("Mongmio");
MODULE_DESCRIPTION("automatic power capping using idle injection");
MODULE_LICENSE("GPL");
