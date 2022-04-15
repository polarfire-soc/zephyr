/*
 * Copyright (c) 2021 Microchip Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief codes required for Risc-V multicore and Zephyr smp support
 *
 */
#include <device.h>
#include <kernel.h>
#include <kernel_structs.h>
#include <ksched.h>
#include <soc.h>
#include <init.h>
#include <rv_smp_defs.h>

volatile struct {
	arch_cpustart_t fn;
	void *arg;
} riscv_cpu_init[CONFIG_MP_NUM_CPUS];

/*
 * Collection of flags to control wake up of harts. This is trickier than
 * expected due to the fact that the wfi can be triggered when in the
 * debugger so we have to stage things carefully to ensure we only wake
 * up at the correct time.
 *
 * initial implementation which assumes any monitor hart is hart id 0 and
 * SMP harts have contiguous hart IDs. CONFIG_SMP_BASE_CPU will have minimum
 * value of 1 for systems with monitor hart and zero otherwise.
 * 
 */

//#define WAKE_FLAG_COUNT (CONFIG_SMP_BASE_CPU + CONFIG_MP_NUM_CPUS)
#define WAKE_FLAG_COUNT (CONFIG_MP_TOTAL_NUM_CPUS)

/* we will index directly off of mhartid so need to be careful... */
volatile __noinit ulong_t hart_wake_flags[WAKE_FLAG_COUNT];

volatile char *riscv_cpu_sp;
/*
 * _curr_cpu is used to record the struct of _cpu_t of each cpu.
 * for efficient usage in assembly
 */
volatile _cpu_t *_curr_cpu[CONFIG_MP_NUM_CPUS];

/* Called from Zephyr initialization */
void arch_start_cpu(int cpu_num, k_thread_stack_t *stack, int sz,
		    arch_cpustart_t fn, void *arg)
{
	int hart_num = cpu_num + CONFIG_SMP_BASE_CPU;

	/* Used to avoid empty loops which can cause debugger issues
	 * and also for retry count on interrupt to keep sending every now and again...
	 */
	volatile int counter = 0; 

	_curr_cpu[cpu_num] = &(_kernel.cpus[cpu_num]);
	riscv_cpu_init[cpu_num].fn = fn;
	riscv_cpu_init[cpu_num].arg = arg;

	/* set the initial sp of target sp through riscv_cpu_sp
	 * Controlled sequencing of start up will ensure
	 * only one secondary cpu can read it per time 
	 */
	riscv_cpu_sp = Z_THREAD_STACK_BUFFER(stack) + sz;

	/* wait slave cpu to start */
	while (hart_wake_flags[hart_num] != RV_WAKE_WAIT) {
		counter++;
	}
		
	hart_wake_flags[hart_num] = RV_WAKE_GO;
	RISCV_CLINT->MSIP[hart_num] = 0x01U;   /*raise soft interrupt for hart(x) where x== hart ID*/

	while (hart_wake_flags[hart_num] != RV_WAKE_DONE) {
		counter++;
		if(0 == (counter % 64)) {
			RISCV_CLINT->MSIP[hart_num] = 0x01U;   /* Another nudge... */
		}
	}

    RISCV_CLINT->MSIP[hart_num] = 0x00U;   /* Clear int now we are done */
}

/* the C entry of slave cores */
void z_riscv_secondary_start(int cpu_num)
{
	arch_cpustart_t fn;
#if 0
#ifdef CONFIG_SMP
	z_icache_setup();
	z_irq_setup();

#endif
#endif
#if defined(CONFIG_SCHED_IPI_SUPPORTED)
    irq_enable(RISCV_MACHINE_SOFT_IRQ);
#endif
#ifdef CONFIG_PMP_STACK_GUARD
	z_riscv_configure_interrupt_stack_guard();
#endif

	/* call the function set by arch_start_cpu */
	fn = riscv_cpu_init[cpu_num].fn;

	fn(riscv_cpu_init[cpu_num].arg);
}

#if defined(CONFIG_SCHED_IPI_SUPPORTED)
static void sched_ipi_handler(const void *unused)
{
    ulong_t hart_id;

    ARG_UNUSED(unused);

    __asm__ volatile("csrr %0, mhartid" : "=r" (hart_id));
    RISCV_CLINT->MSIP[hart_id] = 0x00U;   /* Clear soft interrupt for hart(x) where x== hart ID*/
    z_sched_ipi();
}

void arch_sched_ipi(void)
{
    ulong_t i;

    /* broadcast sched_ipi request to other cores
     * if the target is current core, hardware will ignore it
     */
    for (i = 0; i < CONFIG_MP_NUM_CPUS; i++) {
        RISCV_CLINT->MSIP[i + CONFIG_SMP_BASE_CPU] = 0x01U;   /*raise soft interrupt for hart(x) where x== hart ID*/
    }
}

static int riscv_smp_init(const struct device *dev)
{
    ARG_UNUSED(dev);

    /*
     * Set up handler from main hart and enable IPI interrupt for it.
     * Secondary harts will just enable the interrupt as same isr table is
     * used by all...
     */
    IRQ_CONNECT(RISCV_MACHINE_SOFT_IRQ, 0, sched_ipi_handler, NULL, 0);
    irq_enable(RISCV_MACHINE_SOFT_IRQ);

    return 0;
}

SYS_INIT(riscv_smp_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif
