/*
 * ADRV9009
 *
 * Copyright 2018 Analog Devices Inc.
 *
 * Licensed under the GPL-2.
 */

#ifndef IIO_TRX_ADRV9009_H_
#define IIO_TRX_ADRV9009_H_

#include "talise/talise_types.h"
#include "talise/talise_gpio_types.h"
#include "talise/linux_hal.h"

#include "talise/talise.h"
#include "talise/talise_jesd204.h"
#include "talise/talise_arm.h"
#include "talise/talise_radioctrl.h"
#include "talise/talise_cals.h"
#include "talise/talise_error.h"
#include "talise/talise_agc.h"
#include "talise/talise_rx.h"
#include "talise/talise_tx.h"
#include "talise/talise_user.h"
#include "talise/talise_gpio.h"

#define MIN_GAIN_mdB		0
#define MAX_RX_GAIN_mdB		30000
#define MAX_OBS_RX_GAIN_mdB	30000
#define RX_GAIN_STEP_mdB	500

enum debugfs_cmd {
	DBGFS_NONE,
	DBGFS_INIT,
	DBGFS_BIST_FRAMER_A_PRBS,
	DBGFS_BIST_FRAMER_B_PRBS,
	DBGFS_BIST_TONE,
};


enum adrv9009_bist_mode {
	BIST_DISABLE,
	BIST_INJ_TX,
	BIST_INJ_RX,
};

enum adrv9009_rx_ext_info {
	RSSI,
	RX_QEC,
	RX_HD2,
	RX_RF_BANDWIDTH,
	RX_POWERDOWN,
	RX_GAIN_CTRL_PIN_MODE,
};

enum adrv9009_tx_ext_info {
	TX_QEC,
	TX_LOL,
	TX_RF_BANDWIDTH,
	TX_POWERDOWN,
	TX_ATTN_CTRL_PIN_MODE,
	TX_PA_PROTECTION,
};

enum adrv9009_iio_voltage_in {
	CHAN_RX1,
	CHAN_RX2,
	CHAN_OBS_RX1,
	CHAN_OBS_RX2,
	CHAN_AUXADC0,
	CHAN_AUXADC1,
	CHAN_AUXADC2,
	CHAN_AUXADC3,
};

enum adrv9009_iio_voltage_out {
	CHAN_TX1,
	CHAN_TX2,
	CHAN_AUXDAC0,
	CHAN_AUXDAC1,
	CHAN_AUXDAC2,
	CHAN_AUXDAC3,
	CHAN_AUXDAC4,
	CHAN_AUXDAC5,
	CHAN_AUXDAC6,
	CHAN_AUXDAC7,
	CHAN_AUXDAC8,
	CHAN_AUXDAC9,
	CHAN_AUXDAC10,
	CHAN_AUXDAC11,
};

enum adrv9009_gain_tables {
	RX1_GT,
	RX2_GT,
	RX1_RX2_GT,
	ORX_RX1_GT,
	ORX_RX2_GT,
	ORX_RX1_RX2_GT,
	NUM_GT,
};

enum ad937x_device_id {
	ID_ADRV9009,
	ID_ADRV90081,
	ID_ADRV90082,
};

enum adrv9009_sysref_req_mode {
	SYSREF_CONT_ON,
	SYSREF_CONT_OFF,
	SYSREF_PULSE,
};

struct adrv9009_rf_phy;
struct adrv9009_debugfs_entry {
	struct adrv9009_rf_phy *phy;
	const char *propname;
	void *out_value;
	u32 val;
	u8 size;
	u8 cmd;
};

enum adrv9009_clocks {
	RX_SAMPL_CLK,
	OBS_SAMPL_CLK,
	TX_SAMPL_CLK,
	NUM_ADRV9009_CLKS,
};

enum adrv9009_radio_states {
	RADIO_OFF,
	RADIO_ON,
	RADIO_FORCE_OFF,
	RADIO_RESTORE_STATE,
};

struct adrv9009_clock {
	struct clk_hw		hw;
	struct spi_device	*spi;
	struct adrv9009_rf_phy	*phy;
	unsigned long		rate;
	enum adrv9009_clocks 	source;
};

#define to_clk_priv(_hw) container_of(_hw, struct adrv9009_clock, hw)
#define MAX_NUM_GAIN_TABLES 10

struct gain_table_info {
	u64 start;
	u64 end;
	u8 max_index;
	u8 dest;
	s32 *abs_gain_tbl;
	taliseRxGainTable_t *gainTablePtr;
	taliseOrxGainTable_t *orx_gainTablePtr;
};

struct adrv9009_rf_phy {
	struct spi_device 	*spi;
	const struct firmware 	*fw;
	const struct firmware 	*stream;
	taliseDevice_t 		talise_device;
	taliseDevice_t 		*talDevice;
	taliseInit_t 		talInit;
	taliseAuxDac_t		auxdac;
	taliseAgcCfg_t 		rxAgcCtrl;
	taliseArmGpioConfig_t	arm_gpio_config;
	taliseOrxLoCfg_t 	orx_lo_cfg; /* Fixme: TALISE_setOrxLoCfg */
	taliseFhmConfig_t	fhm_config;
	taliseFhmMode_t		fhm_mode;
	taliseRxGainCtrlPin_t	rx1_gain_ctrl_pin;
	taliseRxGainCtrlPin_t	rx2_gain_ctrl_pin;
	taliseTxAttenCtrlPin_t	tx1_atten_ctrl_pin;
	taliseTxAttenCtrlPin_t	tx2_atten_ctrl_pin;
	taliseTxPaProtectCfg_t	tx_pa_protection;
	taliseRxHd2Config_t	rx_hd2_config;

	int16_t rxFirCoefs[72];
	int16_t obsrxFirCoefs[72];
	int16_t txFirCoefs[80];
	int16_t current_loopBandwidth_kHz[2];
	uint8_t loopFilter_stability;
	u64 trx_lo_frequency;
	u64 aux_lo_frequency;
	bool tx_pa_protection_enabled;

	struct adrv9009_hal	linux_hal;
	struct clk 		*dev_clk;
	struct clk 		*fmc_clk;
	struct clk 		*jesd_rx_clk;
	struct clk 		*jesd_tx_clk;
	struct clk 		*jesd_rx_os_clk;
	struct clk 		*clk_ext_lo_rx;
	struct clk 		*clk_ext_lo_tx;
	struct clk 		*clks[NUM_ADRV9009_CLKS];
	struct adrv9009_clock	clk_priv[NUM_ADRV9009_CLKS];
	struct clk_onecell_data	clk_data;
	struct adrv9009_debugfs_entry debugfs_entry[338];
	struct bin_attribute 	bin;
	struct bin_attribute 	bin_gt;
	struct iio_dev 		*indio_dev;

	struct gpio_desc	*sysref_req_gpio;
	struct gain_table_info  gt_info[NUM_GT];

	u8 			device_id;
	char			*bin_gt_attr_buf;
	char			*bin_attr_buf;
	u32 			adrv9009_debugfs_entry_index;
	u8			obs_rx_path_source;
	u32			tracking_cal_mask;
	bool			saved_radio_state;
	u32			init_cal_mask;
	u32			cal_mask;
};

int adrv9009_hdl_loopback(struct adrv9009_rf_phy *phy, bool enable);
int adrv9009_register_axi_converter(struct adrv9009_rf_phy *phy);
struct adrv9009_rf_phy *adrv9009_spi_to_phy(struct spi_device *spi);
int adrv9009_spi_read(struct spi_device *spi, u32 reg);
int adrv9009_spi_write(struct spi_device *spi, u32 reg, u32 val);

#endif
