/*
 * Copyright (c) 2020 Nuvoton Technology Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/pinctrl/npcx-pinctrl.h>
#include <zephyr/kernel.h>
#include <soc.h>

#include "soc_gpio.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pimux_npcx, LOG_LEVEL_ERR);

/* Driver config */
struct npcx_scfg_config {
	/* scfg device base address */
	uintptr_t base_scfg;
	uintptr_t base_glue;
};

/*
 * Get io list which default functionality are not IOs. Then switch them to
 * GPIO in pin-mux init function.
 *
 * def-io-conf-list {
 *               pinmux = <&alt0_gpio_no_spip
 *                         &alt0_gpio_no_fpip
 *                         ...>;
 *               };
 */
#define NPCX_NO_GPIO_ALT_ITEM(node_id, prop, idx) {				\
	  .group = DT_PHA(DT_PROP_BY_IDX(node_id, prop, idx), alts, group),	\
	  .bit = DT_PHA(DT_PROP_BY_IDX(node_id, prop, idx), alts, bit),		\
	  .inverted = DT_PHA(DT_PROP_BY_IDX(node_id, prop, idx), alts, inv),	\
	},

static const struct npcx_alt def_alts[] = {
	DT_FOREACH_PROP_ELEM(DT_INST(0, nuvoton_npcx_pinctrl_def), pinmux,
				NPCX_NO_GPIO_ALT_ITEM)
};

static const struct npcx_lvol def_lvols[] = NPCX_DT_IO_LVOL_ITEMS_DEF_LIST;

#if DT_HAS_COMPAT_STATUS_OKAY(nuvoton_npcx_pslctrl_def)
static const struct npcx_psl_in psl_in_confs[] = NPCX_DT_PSL_IN_ITEMS_LIST;
#endif

static const struct npcx_scfg_config npcx_scfg_cfg = {
	.base_scfg = DT_REG_ADDR_BY_NAME(DT_NODELABEL(scfg), scfg),
	.base_glue = DT_REG_ADDR_BY_NAME(DT_NODELABEL(scfg), glue),
};

/* Driver convenience defines */
#define HAL_SFCG_INST() (struct scfg_reg *)(npcx_scfg_cfg.base_scfg)

#define HAL_GLUE_INST() (struct glue_reg *)(npcx_scfg_cfg.base_glue)

/* Pin-control local functions */
static void npcx_pinctrl_alt_sel(const struct npcx_alt *alt, int alt_func)
{
	const uint32_t scfg_base = npcx_scfg_cfg.base_scfg;
	uint8_t alt_mask = BIT(alt->bit);

	/*
	 * alt_fun == 0 means select GPIO, otherwise Alternate Func.
	 * inverted == 0:
	 *    Set devalt bit to select Alternate Func.
	 * inverted == 1:
	 *    Clear devalt bit to select Alternate Func.
	 */
	if (!!alt_func != !!alt->inverted) {
		NPCX_DEVALT(scfg_base, alt->group) |=  alt_mask;
	} else {
		NPCX_DEVALT(scfg_base, alt->group) &= ~alt_mask;
	}
}

/* Platform specific pin-control functions */
void npcx_lvol_pads_configure(void)
{
	const uint32_t scfg_base = npcx_scfg_cfg.base_scfg;

	for (int i = 0; i < ARRAY_SIZE(def_lvols); i++) {
		NPCX_LV_GPIO_CTL(scfg_base, def_lvols[i].ctrl)
					|= BIT(def_lvols[i].bit);
		LOG_DBG("IO%x%x turn on low-voltage", def_lvols[i].io_port,
							def_lvols[i].io_bit);
	}
}

void npcx_lvol_restore_io_pads(void)
{
	for (int i = 0; i < ARRAY_SIZE(def_lvols); i++) {
		npcx_gpio_enable_io_pads(
				npcx_get_gpio_dev(def_lvols[i].io_port),
				def_lvols[i].io_bit);
	}
}

void npcx_lvol_suspend_io_pads(void)
{
	for (int i = 0; i < ARRAY_SIZE(def_lvols); i++) {
		npcx_gpio_disable_io_pads(
				npcx_get_gpio_dev(def_lvols[i].io_port),
				def_lvols[i].io_bit);
	}
}

bool npcx_lvol_is_enabled(int port, int pin)
{
	for (int i = 0; i < ARRAY_SIZE(def_lvols); i++) {
		if (def_lvols[i].io_port == port &&
		    def_lvols[i].io_bit == pin) {
			return true;
		}
	}

	return false;
}

void npcx_pinctrl_i2c_port_sel(int controller, int port)
{
	struct glue_reg *const inst_glue = HAL_GLUE_INST();

	if (port != 0) {
		inst_glue->SMB_SEL |= BIT(controller);
	} else {
		inst_glue->SMB_SEL &= ~BIT(controller);
	}
}

int npcx_pinctrl_flash_write_protect_set(void)
{
	struct scfg_reg *inst_scfg = HAL_SFCG_INST();

	inst_scfg->DEV_CTL4 |= BIT(NPCX_DEV_CTL4_WP_IF);
	if (!IS_BIT_SET(inst_scfg->DEV_CTL4, NPCX_DEV_CTL4_WP_IF)) {
		return -EIO;
	}

	return 0;
}

bool npcx_pinctrl_flash_write_protect_is_set(void)
{
	struct scfg_reg *inst_scfg = HAL_SFCG_INST();

	return IS_BIT_SET(inst_scfg->DEV_CTL4, NPCX_DEV_CTL4_WP_IF);
}

void npcx_pinctrl_psl_output_set_inactive(void)
{
	struct gpio_reg *const inst = (struct gpio_reg *)
						NPCX_DT_PSL_OUT_CONTROLLER(0);
	int pin = NPCX_DT_PSL_OUT_PIN(0);

	/* Set PSL_OUT to inactive level by setting related bit of PDOUT */
	inst->PDOUT |= BIT(pin);
}

#if DT_HAS_COMPAT_STATUS_OKAY(nuvoton_npcx_pslctrl_def)
static void npcx_pinctrl_psl_detect_mode_sel(uint32_t offset, bool edge_mode)
{
	struct glue_reg *const inst_glue = HAL_GLUE_INST();

	if (edge_mode) {
		inst_glue->PSL_CTS |= NPCX_PSL_CTS_MODE_BIT(offset);
	} else {
		inst_glue->PSL_CTS &= ~NPCX_PSL_CTS_MODE_BIT(offset);
	}
}

bool npcx_pinctrl_psl_input_asserted(uint32_t i)
{
	struct glue_reg *const inst_glue = HAL_GLUE_INST();

	if (i >= ARRAY_SIZE(psl_in_confs)) {
		return false;
	}

	return IS_BIT_SET(inst_glue->PSL_CTS,
				NPCX_PSL_CTS_EVENT_BIT(psl_in_confs[i].offset));
}

void npcx_pinctrl_psl_input_configure(void)
{
	/* Configure detection type of PSL input pads */
	for (int i = 0; i < ARRAY_SIZE(psl_in_confs); i++) {
		/* Detection polarity select */
		npcx_pinctrl_alt_sel(&psl_in_confs[i].polarity,
			(psl_in_confs[i].flag & NPCX_PSL_ACTIVE_HIGH) != 0);
		/* Detection mode select */
		npcx_pinctrl_psl_detect_mode_sel(psl_in_confs[i].offset,
			(psl_in_confs[i].flag & NPCX_PSL_MODE_EDGE) != 0);
	}

	/* Configure pin-mux for all PSL input pads from GPIO to PSL */
	for (int i = 0; i < ARRAY_SIZE(psl_in_confs); i++) {
		npcx_pinctrl_alt_sel(&psl_in_confs[i].pinctrl, 1);
	}
}
#endif

void npcx_host_interface_sel(enum npcx_hif_type hif_type)
{
	struct scfg_reg *inst_scfg = HAL_SFCG_INST();

	SET_FIELD(inst_scfg->DEVCNT, NPCX_DEVCNT_HIF_TYP_SEL_FIELD, hif_type);
}

/* Pin-control driver registration */
static int npcx_scfg_init(const struct device *dev)
{
	struct scfg_reg *inst_scfg = HAL_SFCG_INST();

	/*
	 * Set bit 7 of DEVCNT again for npcx7 series. Please see Errata
	 * for more information. It will be fixed in next chip.
	 */
	if (IS_ENABLED(CONFIG_SOC_SERIES_NPCX7)) {
		inst_scfg->DEVCNT |= BIT(7);
	}

	/* Change all pads whose default functionality isn't IO to GPIO */
	for (int i = 0; i < ARRAY_SIZE(def_alts); i++) {
		npcx_pinctrl_alt_sel(&def_alts[i], 0);
	}

	/* Configure default low-voltage pads */
	npcx_lvol_pads_configure();

	return 0;
}

SYS_INIT(npcx_scfg_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
