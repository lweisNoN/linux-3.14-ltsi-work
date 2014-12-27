/*
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 *
 * Copyright (C) 2010 John Crispin <blogic@openwrt.org>
 * Copyright (C) 2010 Thomas Langer <thomas.langer@lantiq.com>
 */

#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/irqdomain.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <asm/bootinfo.h>
#include <asm/irq_cpu.h>

#include <lantiq_soc.h>
#include <irq.h>

/* register definitions - internal irqs */
#define LTQ_ICU_IM0_ISR		0x0000
#define LTQ_ICU_IM0_IER		0x0008
#define LTQ_ICU_IM0_IOSR	0x0010
#define LTQ_ICU_IM0_IRSR	0x0018
#define LTQ_ICU_IM0_IMR		0x0020
#define LTQ_ICU_IM1_ISR		0x0028
#define LTQ_ICU_OFFSET		(LTQ_ICU_IM1_ISR - LTQ_ICU_IM0_ISR)

/* register definitions - external irqs */
#define LTQ_EIU_EXIN_C		0x0000
#define LTQ_EIU_EXIN_INIC	0x0004
#define LTQ_EIU_EXIN_INC	0x0008
#define LTQ_EIU_EXIN_INEN	0x000C

/* number of external interrupts */
#define MAX_EIU			6

/* the performance counter */
#define LTQ_PERF_IRQ		(INT_NUM_IM4_IRL0 + 31)

/*
 * irqs generated by devices attached to the EBU need to be acked in
 * a special manner
 */
#define LTQ_ICU_EBU_IRQ		22

#define ltq_icu_w32(m, x, y)	ltq_w32((x), ltq_icu_membase[m] + (y))
#define ltq_icu_r32(m, x)	ltq_r32(ltq_icu_membase[m] + (x))

#define ltq_eiu_w32(x, y)	ltq_w32((x), ltq_eiu_membase + (y))
#define ltq_eiu_r32(x)		ltq_r32(ltq_eiu_membase + (x))

/* our 2 ipi interrupts for VSMP */
#define MIPS_CPU_IPI_RESCHED_IRQ	0
#define MIPS_CPU_IPI_CALL_IRQ		1

/* we have a cascade of 8 irqs */
#define MIPS_CPU_IRQ_CASCADE		8

#if defined(CONFIG_MIPS_MT_SMP) || defined(CONFIG_MIPS_MT_SMTC)
int gic_present;
#endif

static int exin_avail;
static struct resource ltq_eiu_irq[MAX_EIU];
static void __iomem *ltq_icu_membase[MAX_IM];
static void __iomem *ltq_eiu_membase;
static struct irq_domain *ltq_domain;

int ltq_eiu_get_irq(int exin)
{
	if (exin < exin_avail)
		return ltq_eiu_irq[exin].start;
	return -1;
}

void ltq_disable_irq(struct irq_data *d)
{
	u32 ier = LTQ_ICU_IM0_IER;
	int offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	int im = offset / INT_NUM_IM_OFFSET;

	offset %= INT_NUM_IM_OFFSET;
	ltq_icu_w32(im, ltq_icu_r32(im, ier) & ~BIT(offset), ier);
}

void ltq_mask_and_ack_irq(struct irq_data *d)
{
	u32 ier = LTQ_ICU_IM0_IER;
	u32 isr = LTQ_ICU_IM0_ISR;
	int offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	int im = offset / INT_NUM_IM_OFFSET;

	offset %= INT_NUM_IM_OFFSET;
	ltq_icu_w32(im, ltq_icu_r32(im, ier) & ~BIT(offset), ier);
	ltq_icu_w32(im, BIT(offset), isr);
}

static void ltq_ack_irq(struct irq_data *d)
{
	u32 isr = LTQ_ICU_IM0_ISR;
	int offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	int im = offset / INT_NUM_IM_OFFSET;

	offset %= INT_NUM_IM_OFFSET;
	ltq_icu_w32(im, BIT(offset), isr);
}

void ltq_enable_irq(struct irq_data *d)
{
	u32 ier = LTQ_ICU_IM0_IER;
	int offset = d->hwirq - MIPS_CPU_IRQ_CASCADE;
	int im = offset / INT_NUM_IM_OFFSET;

	offset %= INT_NUM_IM_OFFSET;
	ltq_icu_w32(im, ltq_icu_r32(im, ier) | BIT(offset), ier);
}

static int ltq_eiu_settype(struct irq_data *d, unsigned int type)
{
	int i;

	for (i = 0; i < MAX_EIU; i++) {
		if (d->hwirq == ltq_eiu_irq[i].start) {
			int val = 0;
			int edge = 0;

			switch (type) {
			case IRQF_TRIGGER_NONE:
				break;
			case IRQF_TRIGGER_RISING:
				val = 1;
				edge = 1;
				break;
			case IRQF_TRIGGER_FALLING:
				val = 2;
				edge = 1;
				break;
			case IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING:
				val = 3;
				edge = 1;
				break;
			case IRQF_TRIGGER_HIGH:
				val = 5;
				break;
			case IRQF_TRIGGER_LOW:
				val = 6;
				break;
			default:
				pr_err("invalid type %d for irq %ld\n",
					type, d->hwirq);
				return -EINVAL;
			}

			if (edge)
				irq_set_handler(d->hwirq, handle_edge_irq);

			ltq_eiu_w32(ltq_eiu_r32(LTQ_EIU_EXIN_C) |
				(val << (i * 4)), LTQ_EIU_EXIN_C);
		}
	}

	return 0;
}

static unsigned int ltq_startup_eiu_irq(struct irq_data *d)
{
	int i;

	ltq_enable_irq(d);
	for (i = 0; i < MAX_EIU; i++) {
		if (d->hwirq == ltq_eiu_irq[i].start) {
			/* by default we are low level triggered */
			ltq_eiu_settype(d, IRQF_TRIGGER_LOW);
			/* clear all pending */
			ltq_eiu_w32(ltq_eiu_r32(LTQ_EIU_EXIN_INC) & ~BIT(i),
				LTQ_EIU_EXIN_INC);
			/* enable */
			ltq_eiu_w32(ltq_eiu_r32(LTQ_EIU_EXIN_INEN) | BIT(i),
				LTQ_EIU_EXIN_INEN);
			break;
		}
	}

	return 0;
}

static void ltq_shutdown_eiu_irq(struct irq_data *d)
{
	int i;

	ltq_disable_irq(d);
	for (i = 0; i < MAX_EIU; i++) {
		if (d->hwirq == ltq_eiu_irq[i].start) {
			/* disable */
			ltq_eiu_w32(ltq_eiu_r32(LTQ_EIU_EXIN_INEN) & ~BIT(i),
				LTQ_EIU_EXIN_INEN);
			break;
		}
	}
}

static struct irq_chip ltq_irq_type = {
	"icu",
	.irq_enable = ltq_enable_irq,
	.irq_disable = ltq_disable_irq,
	.irq_unmask = ltq_enable_irq,
	.irq_ack = ltq_ack_irq,
	.irq_mask = ltq_disable_irq,
	.irq_mask_ack = ltq_mask_and_ack_irq,
};

static struct irq_chip ltq_eiu_type = {
	"eiu",
	.irq_startup = ltq_startup_eiu_irq,
	.irq_shutdown = ltq_shutdown_eiu_irq,
	.irq_enable = ltq_enable_irq,
	.irq_disable = ltq_disable_irq,
	.irq_unmask = ltq_enable_irq,
	.irq_ack = ltq_ack_irq,
	.irq_mask = ltq_disable_irq,
	.irq_mask_ack = ltq_mask_and_ack_irq,
	.irq_set_type = ltq_eiu_settype,
};

static void ltq_hw_irqdispatch(int module)
{
	u32 irq;

	irq = ltq_icu_r32(module, LTQ_ICU_IM0_IOSR);
	if (irq == 0)
		return;

	/*
	 * silicon bug causes only the msb set to 1 to be valid. all
	 * other bits might be bogus
	 */
	irq = __fls(irq);
	do_IRQ((int)irq + MIPS_CPU_IRQ_CASCADE + (INT_NUM_IM_OFFSET * module));

	/* if this is a EBU irq, we need to ack it or get a deadlock */
	if ((irq == LTQ_ICU_EBU_IRQ) && (module == 0) && LTQ_EBU_PCC_ISTAT)
		ltq_ebu_w32(ltq_ebu_r32(LTQ_EBU_PCC_ISTAT) | 0x10,
			LTQ_EBU_PCC_ISTAT);
}

#define DEFINE_HWx_IRQDISPATCH(x)					\
	static void ltq_hw ## x ## _irqdispatch(void)			\
	{								\
		ltq_hw_irqdispatch(x);					\
	}
DEFINE_HWx_IRQDISPATCH(0)
DEFINE_HWx_IRQDISPATCH(1)
DEFINE_HWx_IRQDISPATCH(2)
DEFINE_HWx_IRQDISPATCH(3)
DEFINE_HWx_IRQDISPATCH(4)

#if MIPS_CPU_TIMER_IRQ == 7
static void ltq_hw5_irqdispatch(void)
{
	do_IRQ(MIPS_CPU_TIMER_IRQ);
}
#else
DEFINE_HWx_IRQDISPATCH(5)
#endif

#ifdef CONFIG_MIPS_MT_SMP
void __init arch_init_ipiirq(int irq, struct irqaction *action)
{
	setup_irq(irq, action);
	irq_set_handler(irq, handle_percpu_irq);
}

static void ltq_sw0_irqdispatch(void)
{
	do_IRQ(MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_RESCHED_IRQ);
}

static void ltq_sw1_irqdispatch(void)
{
	do_IRQ(MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_CALL_IRQ);
}
static irqreturn_t ipi_resched_interrupt(int irq, void *dev_id)
{
	scheduler_ipi();
	return IRQ_HANDLED;
}

static irqreturn_t ipi_call_interrupt(int irq, void *dev_id)
{
	smp_call_function_interrupt();
	return IRQ_HANDLED;
}

static struct irqaction irq_resched = {
	.handler	= ipi_resched_interrupt,
	.flags		= IRQF_PERCPU,
	.name		= "IPI_resched"
};

static struct irqaction irq_call = {
	.handler	= ipi_call_interrupt,
	.flags		= IRQF_PERCPU,
	.name		= "IPI_call"
};
#endif

asmlinkage void plat_irq_dispatch(void)
{
	unsigned int pending = read_c0_status() & read_c0_cause() & ST0_IM;
	unsigned int i;

	if ((MIPS_CPU_TIMER_IRQ == 7) && (pending & CAUSEF_IP7)) {
		do_IRQ(MIPS_CPU_TIMER_IRQ);
		goto out;
	} else {
		for (i = 0; i < MAX_IM; i++) {
			if (pending & (CAUSEF_IP2 << i)) {
				ltq_hw_irqdispatch(i);
				goto out;
			}
		}
	}
	pr_alert("Spurious IRQ: CAUSE=0x%08x\n", read_c0_status());

out:
	return;
}

static int icu_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	struct irq_chip *chip = &ltq_irq_type;
	int i;

	if (hw < MIPS_CPU_IRQ_CASCADE)
		return 0;

	for (i = 0; i < exin_avail; i++)
		if (hw == ltq_eiu_irq[i].start)
			chip = &ltq_eiu_type;

	irq_set_chip_and_handler(hw, chip, handle_level_irq);

	return 0;
}

static const struct irq_domain_ops irq_domain_ops = {
	.xlate = irq_domain_xlate_onetwocell,
	.map = icu_map,
};

static struct irqaction cascade = {
	.handler = no_action,
	.name = "cascade",
};

int __init icu_of_init(struct device_node *node, struct device_node *parent)
{
	struct device_node *eiu_node;
	struct resource res;
	int i, ret;

	for (i = 0; i < MAX_IM; i++) {
		if (of_address_to_resource(node, i, &res))
			panic("Failed to get icu memory range");

		if (request_mem_region(res.start, resource_size(&res),
					res.name) < 0)
			pr_err("Failed to request icu memory");

		ltq_icu_membase[i] = ioremap_nocache(res.start,
					resource_size(&res));
		if (!ltq_icu_membase[i])
			panic("Failed to remap icu memory");
	}

	/* the external interrupts are optional and xway only */
	eiu_node = of_find_compatible_node(NULL, NULL, "lantiq,eiu-xway");
	if (eiu_node && !of_address_to_resource(eiu_node, 0, &res)) {
		/* find out how many external irq sources we have */
		exin_avail = of_irq_count(eiu_node);

		if (exin_avail > MAX_EIU)
			exin_avail = MAX_EIU;

		ret = of_irq_to_resource_table(eiu_node,
						ltq_eiu_irq, exin_avail);
		if (ret != exin_avail)
			panic("failed to load external irq resources");

		if (request_mem_region(res.start, resource_size(&res),
							res.name) < 0)
			pr_err("Failed to request eiu memory");

		ltq_eiu_membase = ioremap_nocache(res.start,
							resource_size(&res));
		if (!ltq_eiu_membase)
			panic("Failed to remap eiu memory");
	}

	/* turn off all irqs by default */
	for (i = 0; i < MAX_IM; i++) {
		/* make sure all irqs are turned off by default */
		ltq_icu_w32(i, 0, LTQ_ICU_IM0_IER);
		/* clear all possibly pending interrupts */
		ltq_icu_w32(i, ~0, LTQ_ICU_IM0_ISR);
	}

	mips_cpu_irq_init();

	for (i = 0; i < MAX_IM; i++)
		setup_irq(i + 2, &cascade);

	if (cpu_has_vint) {
		pr_info("Setting up vectored interrupts\n");
		set_vi_handler(2, ltq_hw0_irqdispatch);
		set_vi_handler(3, ltq_hw1_irqdispatch);
		set_vi_handler(4, ltq_hw2_irqdispatch);
		set_vi_handler(5, ltq_hw3_irqdispatch);
		set_vi_handler(6, ltq_hw4_irqdispatch);
		set_vi_handler(7, ltq_hw5_irqdispatch);
	}

	ltq_domain = irq_domain_add_linear(node,
		(MAX_IM * INT_NUM_IM_OFFSET) + MIPS_CPU_IRQ_CASCADE,
		&irq_domain_ops, 0);

#if defined(CONFIG_MIPS_MT_SMP)
	if (cpu_has_vint) {
		pr_info("Setting up IPI vectored interrupts\n");
		set_vi_handler(MIPS_CPU_IPI_RESCHED_IRQ, ltq_sw0_irqdispatch);
		set_vi_handler(MIPS_CPU_IPI_CALL_IRQ, ltq_sw1_irqdispatch);
	}
	arch_init_ipiirq(MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_RESCHED_IRQ,
		&irq_resched);
	arch_init_ipiirq(MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_CALL_IRQ, &irq_call);
#endif

#if !defined(CONFIG_MIPS_MT_SMP) && !defined(CONFIG_MIPS_MT_SMTC)
	set_c0_status(IE_IRQ0 | IE_IRQ1 | IE_IRQ2 |
		IE_IRQ3 | IE_IRQ4 | IE_IRQ5);
#else
	set_c0_status(IE_SW0 | IE_SW1 | IE_IRQ0 | IE_IRQ1 |
		IE_IRQ2 | IE_IRQ3 | IE_IRQ4 | IE_IRQ5);
#endif

	/* tell oprofile which irq to use */
	cp0_perfcount_irq = irq_create_mapping(ltq_domain, LTQ_PERF_IRQ);

	/*
	 * if the timer irq is not one of the mips irqs we need to
	 * create a mapping
	 */
	if (MIPS_CPU_TIMER_IRQ != 7)
		irq_create_mapping(ltq_domain, MIPS_CPU_TIMER_IRQ);

	return 0;
}

unsigned int get_c0_compare_int(void)
{
	return MIPS_CPU_TIMER_IRQ;
}

static struct of_device_id __initdata of_irq_ids[] = {
	{ .compatible = "lantiq,icu", .data = icu_of_init },
	{},
};

void __init arch_init_irq(void)
{
	of_irq_init(of_irq_ids);
}
