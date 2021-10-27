// SPDX-FileCopyrightText: Copyright (c) 2018 Sungkyunkwan university
// SPDX-License-Identifier: GPL-2.0-or-later
// idle injection based power-capping using intel_powerclamp driver

#ifndef __POWER_CAPPER_H
#define __POWER_CAPPER_H

/* bitmasks for RAPL MSRs, used by primitive access functions */
#define ENERGY_STATUS_MASK      0xffffffff
#define POWER_UNIT_OFFSET       0
#define POWER_UNIT_MASK         0x0F

#define ENERGY_UNIT_OFFSET      0x08
#define ENERGY_UNIT_MASK        0x1F00

#define TIME_UNIT_OFFSET        0x10
#define TIME_UNIT_MASK          0xF0000

#define ENERGY_UNIT_SCALE    1000 /* scale from driver unit to powercap unit */

#define MAX_NUMBER_OF_LOG 512

enum rapl_domain_type {
	RAPL_DOMAIN_PACKAGE, /* entire package/socket */
	RAPL_DOMAIN_PP0, /* core power plane */
	RAPL_DOMAIN_PP1, /* graphics uncore */
	RAPL_DOMAIN_DRAM,/* DRAM control_type */
	RAPL_DOMAIN_PLATFORM, /* PSys control_type */
	RAPL_DOMAIN_MAX,
};

/* per domain data, some are optional */
enum rapl_primitives {
	ENERGY_COUNTER,
	POWER_LIMIT1,
	POWER_LIMIT2,
	FW_LOCK,
	
	PL1_ENABLE,  /* power limit 1, aka long term */
	PL1_CLAMP,   /* allow frequency to go below OS request */
	PL2_ENABLE,  /* power limit 2, aka short term, instantaneous */
	PL2_CLAMP,
	
	TIME_WINDOW1, /* long term */
	TIME_WINDOW2, /* short term */
	THERMAL_SPEC_POWER,
	MAX_POWER,
	
	MIN_POWER,
	MAX_TIME_WINDOW,
	THROTTLED_TIME,
	PRIORITY_LEVEL,
	
	/* below are not raw primitive data */
	AVERAGE_POWER,
	NR_RAPL_PRIMITIVES,
};
typedef struct msr_rapl {
	u64 cycles;
	u64 e_package;
	u64 e_dram;
    u64 e_idle;
} msr_rapl_t;

typedef struct pwrmon_log {
	u64 duration; //ns
	u64 package_energy;
	u64 dram_energy;
	u64 idle_rate;
	u64 cur_idle_rate;
	u32 average_power;
	u32 change_power;
	u32 target_power;
	u64 tsc; //result of rdtsc
} pwrmon_log_t;

#endif
