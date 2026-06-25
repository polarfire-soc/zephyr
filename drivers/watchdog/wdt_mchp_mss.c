/*
 * Copyright (c) 2026 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#define DT_DRV_COMPAT microchip_wdt_mss

LOG_MODULE_REGISTER(wdt_mchp_mss, CONFIG_WDT_LOG_LEVEL);

#define WDOG_REFRESH_OFFSET 0x000u
#define WDOG_CONTROL_OFFSET 0x004u
#define WDOG_STATUS_OFFSET  0x008u
#define WDOG_TIME_OFFSET    0x00Cu
#define MSVP_OFFSET         0x010u
#define TRIGGER_OFFSET      0x014u
#define FORCE_OFFSET        0x018u

#define CONTROL_INTEN_MSVP       BIT(0)
#define CONTROL_INTEN_TRIG       BIT(1)
#define CONTROL_INTEN_SLEEP      BIT(2)
#define CONTROL_ACTIVE_SLEEP     BIT(3)
#define CONTROL_ENABLE_FORBIDDEN BIT(4)

#define STATUS_MVRP_TRIPPED BIT(0)
#define STATUS_WDOG_TRIPPED BIT(1)
#define STATUS_FORBIDDEN    BIT(2)
#define STATUS_TRIGGERED    BIT(3)
#define STATUS_LOCKED       BIT(4)
#define STATUS_DEVRST       BIT(5)

#define WDOG_TIME_MASK 0xFFFFFFu
#define MSVP_MASK      0xFFFFFFu
#define TRIGGER_MASK   0xFFFu
#define FORCE_MASK     0xFFFFu

#define REFRESH_VALUE 0xDEADC0DEu
#define TRIGGER_VALUE 0x0Cu

struct wdt_mchp_mss_dev_data {
	wdt_callback_t callback;
	bool interrupt_enabled;
	uint8_t installed_timeout_cnt;
	struct k_sem lock;
};

struct wdt_mchp_mss_dev_config {
	uintptr_t base_address;
	uint32_t clock_freq;
	void (*irq_config_func)(const struct device *dev);
};

static int wdt_reset_type_set(uint8_t flag)
{
	int ret_val = 0;

	switch (flag) {
	case WDT_FLAG_RESET_NONE:
		ret_val = -ENOTSUP;
		break;

	case WDT_FLAG_RESET_CPU_CORE:
	case WDT_FLAG_RESET_SOC:
		break;

	default:
		ret_val = -EINVAL;
		break;
	}

	return ret_val;
}

static int wdt_mchp_mss_setup(const struct device *dev, uint8_t options)
{
	struct wdt_mchp_mss_dev_data *data = dev->data;
	const struct wdt_mchp_mss_dev_config *cfg = dev->config;
	uint32_t control;

	k_sem_take(&data->lock, K_FOREVER);

	if (data->installed_timeout_cnt == 0) {
		k_sem_give(&data->lock);
		LOG_ERR("No valid timeout installed");
		return -EINVAL;
	}

	/* WDT_OPT_PAUSE_HALTED_BY_DBG is supported by default by the peripheral */
	if ((options & WDT_OPT_PAUSE_IN_SLEEP) != 0) {
		LOG_ERR("unsupported option selected %s", __func__);
		return -ENOTSUP;
	}

	control = CONTROL_INTEN_MSVP;
	sys_write32(control, cfg->base_address + WDOG_CONTROL_OFFSET);
	sys_write32(REFRESH_VALUE, cfg->base_address + WDOG_REFRESH_OFFSET);

	k_sem_give(&data->lock);

	return 0;
}

static int wdt_mchp_mss_disable(const struct device *dev)
{
	struct wdt_mchp_mss_dev_data *data = dev->data;
	const struct wdt_mchp_mss_dev_config *cfg = dev->config;
	uint32_t irq_key = irq_lock();
	uint32_t timer_disable = 0;

	data->installed_timeout_cnt = 0;

	sys_write32(timer_disable, cfg->base_address + WDOG_TIME_OFFSET);
	irq_unlock(irq_key);

	return 0;
}

static int wdt_mchp_mss_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	struct wdt_mchp_mss_dev_data *data = dev->data;
	uint32_t irq_key = 0;
	uint8_t ret;

	k_sem_take(&data->lock, K_FOREVER);

	data->callback = cfg->callback;
	data->interrupt_enabled = ((data->callback != NULL) ? true : false);

	/* Set the behaviour of the watchdog peripheral based on the flags supplied */
	ret = wdt_reset_type_set(cfg->flags);
	if (ret < 0) {
		k_sem_give(&data->lock);
		LOG_ERR("error in setting reset type %d", ret);
		return ret;
	}

	if (data->interrupt_enabled != 0) {
		data->callback = cfg->callback;
	}

	irq_key = irq_lock();
	ret = (data->installed_timeout_cnt)++;
	irq_unlock(irq_key);
	k_sem_give(&data->lock);

	return (int)ret;
}

static int wdt_mchp_mss_feed(const struct device *dev, int channel_id)
{
	struct wdt_mchp_mss_dev_data *data = dev->data;
	const struct wdt_mchp_mss_dev_config *cfg = dev->config;

	if ((channel_id < 0) || (channel_id >= (data->installed_timeout_cnt))) {
		LOG_ERR("Invalid channel selected");
		return -EINVAL;
	}
	if (data->installed_timeout_cnt == 0) {
		LOG_ERR("No valid timeout installed");
		return -EINVAL;
	}

	if (false == k_is_in_isr()) {
		k_sem_take(&data->lock, K_FOREVER);
	}

	sys_write32(REFRESH_VALUE, cfg->base_address + WDOG_REFRESH_OFFSET);

	if (false == k_is_in_isr()) {
		k_sem_give(&data->lock);
	}

	return 0;
}

static void wdt_mchp_mss_mvrp_isr(const struct device *dev)
{
	struct wdt_mchp_mss_dev_data *data = dev->data;
	const struct wdt_mchp_mss_dev_config *cfg = dev->config;
	uint32_t status = sys_read32(cfg->base_address + WDOG_STATUS_OFFSET);

	if (status & STATUS_MVRP_TRIPPED) {
		if (data->callback != NULL) {
			data->callback(dev, 0);
		}
	}
}

static void wdt_mchp_mss_trig_isr(const struct device *dev)
{
	struct wdt_mchp_mss_dev_data *data = dev->data;
	const struct wdt_mchp_mss_dev_config *cfg = dev->config;
	uint32_t status = sys_read32(cfg->base_address + WDOG_STATUS_OFFSET);

	if (status & STATUS_TRIGGERED) {
		if (data->callback != NULL) {
			data->callback(dev, 0);
		}
	}
}

static int wdt_mchp_mss_init(const struct device *dev)
{
	struct wdt_mchp_mss_dev_data *data = dev->data;
	const struct wdt_mchp_mss_dev_config *const cfg = dev->config;

	/* Initialize the semaphore for thread safety */
	k_sem_init(&data->lock, 1, 1);

	data->installed_timeout_cnt = 0;
	cfg->irq_config_func(dev);

	return 0;
}

static DEVICE_API(wdt, wdt_mchp_mss_api) = {
	.setup = wdt_mchp_mss_setup,
	.disable = wdt_mchp_mss_disable,
	.install_timeout = wdt_mchp_mss_install_timeout,
	.feed = wdt_mchp_mss_feed,
};

#define WDT_MCHP_MSS_MVRP_IRQ_CONNECT(n, m)                                                        \
	IRQ_CONNECT(DT_INST_IRQN_BY_IDX(n, m), DT_INST_IRQ_BY_IDX(n, m, priority),                 \
		    wdt_mchp_mss_mvrp_isr, DEVICE_DT_INST_GET(n), 0);                              \
	irq_enable(DT_INST_IRQN_BY_IDX(n, m));

#define WDT_MCHP_MSS_TRIG_IRQ_CONNECT(n, m)                                                        \
	IRQ_CONNECT(DT_INST_IRQN_BY_IDX(n, m), DT_INST_IRQ_BY_IDX(n, m, priority),                 \
		    wdt_mchp_mss_trig_isr, DEVICE_DT_INST_GET(n), 0);                              \
	irq_enable(DT_INST_IRQN_BY_IDX(n, m));

/* WDT driver configuration structure for instance n */
#define WDT_MCHP_MSS_CONFIG_DEFN(n)                                                                \
	static const struct wdt_mchp_mss_dev_config wdt_mchp_mss_dev_config_##n = {                \
		.base_address = DT_INST_REG_ADDR(n),                                               \
		.clock_freq = DT_INST_PROP(n, clock_frequency),                                    \
		.irq_config_func = wdt_mchp_irq_config_##n}
/* Define and initialize the wdt device. */
#define WDT_MCHP_MSS_DEVICE_INIT(n)                                                                \
	static struct wdt_mchp_mss_dev_data wdt_mchp_mss_dev_data_##n;                             \
	static void wdt_mchp_irq_config_##n(const struct device *dev)                              \
	{                                                                                          \
		WDT_MCHP_MSS_MVRP_IRQ_CONNECT(n, 1);                                               \
		WDT_MCHP_MSS_TRIG_IRQ_CONNECT(n, 0);                                               \
	}                                                                                          \
	WDT_MCHP_MSS_CONFIG_DEFN(n);                                                               \
	DEVICE_DT_INST_DEFINE(n, wdt_mchp_mss_init, NULL, &wdt_mchp_mss_dev_data_##n,              \
			      &wdt_mchp_mss_dev_config_##n, PRE_KERNEL_1,                          \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &wdt_mchp_mss_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_MCHP_MSS_DEVICE_INIT);
