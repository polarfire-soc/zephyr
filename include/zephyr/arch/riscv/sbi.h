/*
 * Copyright (c) 2024 sensry.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#ifndef ZEPHYR_INCLUDE_ARCH_RISCV_SBI_H
#define ZEPHYR_INCLUDE_ARCH_RISCV_SBI_H

#include "sbi_ecall_interface.h"

typedef struct sbi_ret{
	int32_t error;
	uint32_t value;
} sbi_ret_t;

sbi_ret_t sbi_ecall(int ext, int fid, unsigned long arg0,
				unsigned long arg1, unsigned long arg2,
				unsigned long arg3, unsigned long arg4,
				unsigned long arg5);

/* --- TIMER --- */
static inline sbi_ret_t sbi_set_timer(uint64_t time_value)
{
#ifdef CONFIG_64BIT
	return sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER, time_value, 0, 0, 0, 0, 0);
#else
	return sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER, time_value, time_value >> 32, 0, 0, 0, 0);
#endif
}

static inline sbi_ret_t sbi_hsm_hart_start(uint64_t hartid)
{
	return sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START, hartid, CONFIG_SRAM_BASE_ADDRESS, 1, 0, 0, 0);
}

#endif /* OPENSBI_H */
