/*
 * ADRV9009/8 RF Transceiver
 *
 * Copyright 2018 Analog Devices Inc.
 *
 * Licensed under the GPL-2.
 */
//#define DEBUG
//#define _DEBUG

#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>

#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>

#include <asm/unaligned.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>

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

#include "talise/linux_hal.h"

#include "adrv9009.h"

#define FIRMWARE	"TaliseTDDArmFirmware.bin"
#define STREAM		"TaliseStream.bin"

// 10 -bit:
//
// factor  v=(1+0.5*0)+((0.00143136(1+1)*(1.094*c-511))/(1+0)
//
// v = 0.00313182 (c - 147.79)
// v = 0.00313182 (c + 11.8622)
// v = 0.00313182 (c + 171.514)
// v = 0.00313182 (c + 331.166)
//
// 11-bit:
//
// factor  v=(1+0.5*0)+((0.00143136(1+0)*(1.094*c-511))/(1+0)
//
// v = 0.00156591 (c + 171.514)
// v = 0.00156591 (c + 490.818)
// v = 0.00156591 (c + 810.121)
// v = 0.00156591 (c + 1129.42)
//
// 12-bit:
//
// factor  v=(1+0.5*0)+((0.00143136(1+0)*(1.094*c-511))/(1+1)
//
// v = 0.000782954 (c + 810.121)
// v = 0.000782954 (c + 1448.73)
// v = 0.000782954 (c + 2087.34)
// v = 0.000782954 (c + 2725.94)

// Reak 12-bits 10,11
//v = 0.805861 c

static const int adrv9009_auxdac_scale_val2_lut[TAL_AUXDACRES_10BIT + 1]  = {
	782954,  /* TAL_AUXDACRES_12BIT */
	565910,  /* TAL_AUXDACRES_11BIT */
	131820,  /* TAL_AUXDACRES_10BIT */
};

static const int adrv9009_auxdac_scale_val1_lut[TAL_AUXDACRES_10BIT + 1] = {
	0, /* TAL_AUXDACRES_12BIT */
	1, /* TAL_AUXDACRES_11BIT */
	3, /* TAL_AUXDACRES_10BIT */
};

static const int adrv9009_auxdac_offset_val1_lut
	[TAL_AUXDACRES_10BIT + 1][TAL_AUXDACVREF_2P5V + 1] = {
	{810, 1449, 2087, 2726}, /* TAL_AUXDACRES_12BIT */
	{172, 491, 810, 1129},   /* TAL_AUXDACRES_11BIT */
	{148, 12, 172, 331},     /* TAL_AUXDACRES_10BIT */
};

static const char * const adrv9009_actions[] = {
	"NO_ACTION",
	"WARN_RESET_LOG",
	"WARN_RERUN_TRCK_CAL",
	"WARN_RESET_GPIO",
	"ERR_CHECK_TIMER",
	"ERR_RESET_ARM",
	"ERR_RERUN_INIT_CALS",
	"ERR_RESET_SPI",
	"ERR_RESET_GPIO",
	"ERR_CHECK_PARAM",
	"ERR_RESET_FULL",
	"ERR_RESET_JESD204FRAMERA",
	"ERR_RESET_JESD204FRAMERB",
	"ERR_RESET_JESD204DEFRAMERA",
	"ERR_RESET_JESD204DEFRAMERB",
	"ERR_BBIC_LOG_ERROR",
	"ERR_REDUCE_TXSAMPLE_PWR",
};

static const char * const adrv9009_gpint_diag[][8] = {
	{
		"stream rx1_enable fall edge error",
		"stream tx1_enable fall edge error",
		"stream orx2_enable rise edge error",
		"stream orx1_enable rise edge error",
		"stream rx2_enable rise edge error",
		"stream tx2_enable rise edge error",
		"stream rx1_enable rise edge error",
		"stream tx1_enable rise edge error",
	}, {
		"stream lpbk2_enable fall edge error",
		"stream lpbk2_enable rise edge error",
		"stream lpbk2_enable rise edge error",
		"stream lpbk1_enable rise edge error",
		"stream orx2_enable fall edge error",
		"stream orx1_enable fall edge error",
		"stream rx2_enable fall edge error",
		"stream tx2_enable fall edge error",
	}, {
		"stream gpio3 fall edge error",
		"stream gpio2 fall edge error",
		"stream gpio1 fall edge error",
		"stream gpio0 fall edge error",
		"stream gpio3 rise edge error",
		"stream gpio2 rise edge error",
		"stream gpio1 rise edge error",
		"stream gpio0 rise edge error",
	}, {
		"stream orx2low to rx2high stream error",
		"stream orx1low to orx1high stream error",
		"stream rx2low to orx2high stream error",
		"stream rx1low to orx1high stream error",
		"stream Erroneous completion of pin mode stream for gp_irq rise edge",
		"deframerA BD - Bad Disparity Error",
		"deframerA NIT - Not In Table Error",
		"deframerA UEK - Unexpected K Error",
	}, {
		"deframerA ILD - Inter-Lane De-skew",
		"deframerA ILS - InitialLane Sync",
		"deframerA GCS - Good CheckSum",
		"deframerA FS - Frame Sync",
		"deframerA CSG - Code Group Sync",
		"deframerA CMM - RESERVED",
		"deframerA Pointers out of alignment",
		"deframerA Sysref misalignment to global LMFC",
	}, {
		"deframerB BD - Bad Disparity Error",
		"deframerB NIT - Not In Table Error",
		"deframerB UEK - Unexpected K Error",
		"deframerB ILD - Inter-Lane De-skew",
		"deframerB ILS - InitialLane Sync",
		"deframerB GCS - Good CheckSum",
		"deframerB FS - Frame Sync",
		"deframerB CSG - Code Group Sync",
	}, {
		"deframerB CMM - RESERVED",
		"deframerB Pointers out of alignment",
		"deframerB Sysref misalignment to global LMFC",
		"framerA Asynchronous FIFO pointer offset error",
		"framerA Misalignment to current LMFC error",
		"framerB Asynchronous FIFO pointer offset error",
		"framerB Misalignment to current LMFC error",
		"PA protection error for chan 1",
	}, {
		"PA protection error for chan 2",
		"ARM Calibration error",
		"CLK PLL lock detect reset",
		"AUX PLL lock detect reset",
		"RF PLL lock detect reset",
		"ARM WatchDog Timeout",
		"ARM Force GP_INT",
		"ARM System Error",
	}, {
		"ARM Data Memory Parity Error",
		"ARM Program Memory Parity Error",
		"ARM Command Wait TimeOut",
	}
};

enum adrv9009_iio_dev_attr {
	ADRV9009_ENSM_MODE,
	ADRV9009_ENSM_MODE_AVAIL,
	ADRV9009_INIT_CAL,
};

int adrv9009_spi_read(struct spi_device *spi, unsigned reg)
{
	unsigned char buf[3];
	int ret;

	buf[0] = 0x80 | (reg >> 8);
	buf[1] = reg & 0xFF;
	ret = spi_write_then_read(spi, &buf[0], 2, &buf[2], 1);

	dev_dbg(&spi->dev, "%s: REG: 0x%X VAL: 0x%X (%d)\n",
		__func__, reg, buf[2], ret);

	if (ret < 0) {
		dev_err(&spi->dev, "%s: failed (%d)\n",
			__func__, ret);
		return ret;
	}

	return buf[2];
}

int adrv9009_spi_write(struct spi_device *spi, unsigned reg, unsigned val)
{
	unsigned char buf[3];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xFF;
	buf[2] = val;

	ret = spi_write_then_read(spi, buf, 3, NULL, 0);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: failed (%d)\n",
			__func__, ret);
		return ret;
	}

	dev_dbg(&spi->dev, "%s: REG: 0x%X VAL: 0x%X (%d)\n",
		__func__, reg, val, ret);

	return 0;
}

static int adrv9009_sysref_req(struct adrv9009_rf_phy *phy,
			       enum adrv9009_sysref_req_mode mode)
{
	int ret;

	if (!IS_ERR(phy->sysref_req_gpio)) {
		if (mode == SYSREF_CONT_ON)
			ret = gpiod_direction_output(phy->sysref_req_gpio, 1);
		else if (mode == SYSREF_CONT_OFF)
			ret = gpiod_direction_output(phy->sysref_req_gpio, 0);
		else if (mode == SYSREF_PULSE) {
			gpiod_direction_output(phy->sysref_req_gpio, 1);
			mdelay(1);
			ret = gpiod_direction_output(phy->sysref_req_gpio, 0);
		} else
			ret = -EINVAL;
	} else
		ret = -ENODEV;

	if (ret)
		dev_err(&phy->spi->dev, "%s: failed (%d)\n", __func__, ret);

	return ret;
}

static int adrv9009_set_radio_state(struct adrv9009_rf_phy *phy,
				    enum adrv9009_radio_states state)
{
	u32 radioStatus;
	int ret = 0;

	switch (state) {
	case RADIO_OFF:
		ret = TALISE_radioOff(phy->talDevice);
		break;
	case RADIO_ON:
		ret = TALISE_radioOn(phy->talDevice);
		break;
	case RADIO_FORCE_OFF:
		if (!(phy->talDevice->devStateInfo.devState & TAL_STATE_RADIOON)) {
			phy->saved_radio_state = false;
			return 0;
		}
		ret = TALISE_radioOff(phy->talDevice);
		if (ret == TALACT_NO_ACTION)
			phy->saved_radio_state = true;
		else
			dev_err(&phy->spi->dev, "%s: failed\n", __func__);

		/* read radio state to make sure ARM is in radioOff /IDLE */
		ret = TALISE_getRadioState(phy->talDevice, &radioStatus);
		if (ret != TALACT_NO_ACTION)
			return -EFAULT;
		/* throw error if not in radioOff/IDLE state */
		if ((radioStatus & 0x03) != 2) {
			dev_err(&phy->spi->dev, "%s: failed\n", __func__);
			return -EFAULT;
		}
		break;
	case RADIO_RESTORE_STATE:
		if (phy->saved_radio_state)
			ret = TALISE_radioOn(phy->talDevice);
		else
			ret = TALISE_radioOff(phy->talDevice);

		break;
	}

	return ret;
}

static const char * const adrv9009_ilas_mismatch_table[] = {
	"device ID",
	"bank ID",
	"lane ID",
	"lanes per converter",
	"scrambling",
	"octets per frame",
	"frames per multiframe",
	"number of converters",
	"sample resolution",
	"control bits per sample",
	"bits per sample",
	"samples per frame",
	"control words per frame",
	"high density",
	"checksum"
};

static int adrv9009_setup(struct adrv9009_rf_phy *phy)
{
	uint8_t mcsStatus = 0;
	uint8_t pllLockStatus = 0;

	uint32_t initCalMask = phy->init_cal_mask =
		TAL_TX_BB_FILTER | TAL_ADC_TUNER | TAL_TIA_3DB_CORNER |
		TAL_DC_OFFSET | TAL_RX_GAIN_DELAY | TAL_FLASH_CAL |
		TAL_PATH_DELAY | TAL_TX_LO_LEAKAGE_INTERNAL | TAL_TX_QEC_INIT |
		TAL_LOOPBACK_RX_LO_DELAY | TAL_LOOPBACK_RX_RX_QEC_INIT |
		TAL_RX_QEC_INIT | TAL_ORX_QEC_INIT | TAL_TX_DAC | TAL_ADC_STITCHING;

	uint32_t trackingCalMask = phy->tracking_cal_mask =  TAL_TRACK_NONE;
	uint8_t errorFlag = 0;
	uint16_t deframerStatus = 0;
	uint8_t framerStatus = 0;
	uint32_t ret = TALACT_NO_ACTION;
	unsigned long lane_rate_kHz;
	long dev_clk, fmc_clk;

	phy->talInit.spiSettings.MSBFirst = 1;
	phy->talInit.spiSettings.autoIncAddrUp = 1;
	phy->talInit.spiSettings.fourWireMode = 1;
	phy->talInit.spiSettings.cmosPadDrvStrength = TAL_CMOSPAD_DRV_2X;


	/**********************************************************/
	/**********************************************************/
	/************ Talise Initialization Sequence *************/
	/**********************************************************/
	/**********************************************************/

	/** < Insert User System Clock(s) Initialization Code Here >
	 * System Clock should provide a device clock and SYSREF signal
	 * to the Talise device.
	 **/

	dev_clk = clk_round_rate(phy->dev_clk,
				 phy->talInit.clocks.deviceClock_kHz * 1000);
	fmc_clk = clk_round_rate(phy->fmc_clk,
				 phy->talInit.clocks.deviceClock_kHz * 1000);

	if (dev_clk > 0 && fmc_clk > 0 && fmc_clk == dev_clk &&
	    (dev_clk / 1000) == phy->talInit.clocks.deviceClock_kHz) {
		clk_set_rate(phy->fmc_clk, (unsigned long) dev_clk);
		clk_set_rate(phy->dev_clk, (unsigned long) dev_clk);
	} else {
		dev_err(&phy->spi->dev, "Requesting device clock %u failed got %ld",
			phy->talInit.clocks.deviceClock_kHz * 1000, dev_clk);
		return -EINVAL;
	}

	lane_rate_kHz = phy->talInit.tx.txProfile.txInputRate_kHz *
			phy->talInit.jesd204Settings.deframerA.M *
			(20 / hweight8(
				 phy->talInit.jesd204Settings.deframerA.deserializerLanesEnabled));
	ret = clk_set_rate(phy->jesd_tx_clk, lane_rate_kHz);
	if (ret < 0)
		return ret;

	lane_rate_kHz = phy->talInit.rx.rxProfile.rxOutputRate_kHz *
			phy->talInit.jesd204Settings.framerA.M *
			(20 / hweight8(phy->talInit.jesd204Settings.framerA.serializerLanesEnabled));

	ret = clk_set_rate(phy->jesd_rx_clk, lane_rate_kHz);
	if (ret < 0)
		return ret;

	lane_rate_kHz = phy->talInit.obsRx.orxProfile.orxOutputRate_kHz *
			phy->talInit.jesd204Settings.framerB.M *
			(20 / hweight8(phy->talInit.jesd204Settings.framerB.serializerLanesEnabled));
	ret = clk_set_rate(phy->jesd_rx_os_clk, lane_rate_kHz);
	if (ret < 0)
		return ret;


	/*** < Insert User BBIC JESD204B Initialization Code Here > ***/

	/*******************************/
	/**** Talise Initialization ***/
	/*******************************/

	/*Open Talise Hw Device*/
	ret = TALISE_openHw(phy->talDevice);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/* Toggle RESETB pin on Talise device */
	ret = TALISE_resetDevice(phy->talDevice);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/* Fixme: Need to wait until TX DIV40 MMCM is enabled */
	msleep(100);

	ret = clk_prepare_enable(phy->jesd_tx_clk);
	if (ret < 0)
		return ret;

	/* TALISE_initialize() loads the Talise device data structure
	 * settings for the Rx/Tx/ORx profiles, FIR filters, digital
	 * filter enables, calibrates the CLKPLL, loads the user provided Rx
	 * gain tables, and configures the JESD204b serializers/framers/deserializers
	 * and deframers.
	 */
	ret = TALISE_initialize(phy->talDevice, &phy->talInit);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*******************************/
	/***** CLKPLL Status Check *****/
	/*******************************/
	ret = TALISE_getPllsLockStatus(phy->talDevice, &pllLockStatus);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/* Assert that Talise CLKPLL is locked */
	if ((pllLockStatus & 0x01) == 0)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*******************************************************/
	/**** Perform MultiChip Sync (MCS) on Talise Device ***/
	/*******************************************************/
	ret = TALISE_enableMultichipSync(phy->talDevice, 1, &mcsStatus);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*< user code - Request minimum 3 SYSREF pulses from Clock Device - > */
	adrv9009_sysref_req(phy, SYSREF_PULSE);

	/*******************/
	/**** Verify MCS ***/
	/*******************/
	ret = TALISE_enableMultichipSync(phy->talDevice, 0, &mcsStatus);
	if ((mcsStatus & 0x0B) != 0x0B)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*******************************************************/
	/**** Prepare Talise Arm binary and Load Arm and    ****/
	/**** Stream processor Binaryes 					****/
	/*******************************************************/
	if (pllLockStatus & 0x01) {
		ret = TALISE_initArm(phy->talDevice, &phy->talInit);
		/*< user code- load Talise stream binary into streamBinary[4096] >*/
		/*< user code- load ARM binary byte array into armBinary[114688] >*/

		ret = TALISE_loadStreamFromBinary(phy->talDevice, (u8 *) phy->stream->data);
		if (ret != TALACT_NO_ACTION)
			dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

		ret = TALISE_loadArmFromBinary(phy->talDevice, (u8 *) phy->fw->data,
					       phy->fw->size);
		if (ret != TALACT_NO_ACTION)
			dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

		/* TALISE_verifyArmChecksum() will timeout after 200ms
		 * if ARM checksum is not computed
		 */
		ret = TALISE_verifyArmChecksum(phy->talDevice);
		if (ret != TAL_ERR_OK)
			dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	} else {
		/*< user code- check settings for proper CLKPLL lock  > ***/
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);
	}


	TALISE_setArmGpioPins(phy->talDevice, &phy->arm_gpio_config);

	/*******************************/
	/**Set RF PLL LO Frequencies ***/
	/*******************************/
	phy->current_loopBandwidth_kHz[0] = 50;
	TALISE_setRfPllLoopFilter(phy->talDevice, phy->current_loopBandwidth_kHz[0],
				  phy->loopFilter_stability);

	ret = TALISE_setRfPllFrequency(phy->talDevice, TAL_RF_PLL,
				       phy->trx_lo_frequency);

	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*** < wait 200ms for PLLs to lock - user code here > ***/

	ret = TALISE_getPllsLockStatus(phy->talDevice, &pllLockStatus);
	if ((pllLockStatus & 0x07) != 0x07)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/****************************************************/
	/**** Run Talise ARM Initialization Calibrations ***/
	/****************************************************/
	/*** < User: Turn ON the PA (if any), and open any switches on ORx input used to isolate it for calibrations > ***/
	/*** < User: Open any switches on the Rx input (if used) to isolate Rx input and provide required VSWR at input > ***/
	ret = TALISE_runInitCals(phy->talDevice,
				 initCalMask & ~TAL_TX_LO_LEAKAGE_EXTERNAL);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	ret = TALISE_waitInitCals(phy->talDevice, 20000, &errorFlag);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	if (errorFlag) {
		/*< user code - Check error flag to determine ARM  error> */
		dev_err(&phy->spi->dev, "%s:%d (ret %d) errorFlag %x", __func__, __LINE__, ret,
			errorFlag);
	}

	/*************************************************************************/
	/*****  TALISE ARM Initialization External LOL Calibrations with PA  *****/
	/*************************************************************************/
	/*** < Action: Please ensure PA is enabled operational at this time > ***/
	if (initCalMask & TAL_TX_LO_LEAKAGE_EXTERNAL) {
		ret = TALISE_runInitCals(phy->talDevice, TAL_TX_LO_LEAKAGE_EXTERNAL);
		if (ret != TALACT_NO_ACTION)
			dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

		ret = TALISE_waitInitCals(phy->talDevice, 20000, &errorFlag);
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

		if (errorFlag)
			dev_err(&phy->spi->dev, "%s:%d (ret %d) errorFlag %x", __func__, __LINE__, ret,
				errorFlag);
	}

	/***************************************************/
	/**** Enable Talise JESD204B Framer ***/
	/***************************************************/

	ret = TALISE_enableFramerLink(phy->talDevice, TAL_FRAMER_A, 0);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	ret |= TALISE_enableFramerLink(phy->talDevice, TAL_FRAMER_A, 1);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*************************************************/
	/**** Enable SYSREF to Talise JESD204B Framer ***/
	/*************************************************/
	/*** < User: Make sure SYSREF is stopped/disabled > ***/

	ret = TALISE_enableSysrefToFramer(phy->talDevice, TAL_FRAMER_A, 1);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/***************************************************/
	/**** Enable Talise JESD204B Framer ***/
	/***************************************************/

	ret = TALISE_enableFramerLink(phy->talDevice, TAL_FRAMER_B, 0);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	ret |= TALISE_enableFramerLink(phy->talDevice, TAL_FRAMER_B, 1);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*************************************************/
	/**** Enable SYSREF to Talise JESD204B Framer ***/
	/*************************************************/
	/*** < User: Make sure SYSREF is stopped/disabled > ***/

	ret = TALISE_enableSysrefToFramer(phy->talDevice, TAL_FRAMER_B, 1);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/***************************************************/
	/**** Enable  Talise JESD204B Deframer ***/
	/***************************************************/

	ret = TALISE_enableDeframerLink(phy->talDevice, TAL_DEFRAMER_A, 0);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	ret |= TALISE_enableDeframerLink(phy->talDevice, TAL_DEFRAMER_A, 1);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/***************************************************/
	/**** Enable SYSREF to Talise JESD204B Deframer ***/
	/***************************************************/
	ret = TALISE_enableSysrefToDeframer(phy->talDevice, TAL_DEFRAMER_A, 1);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/*** < User Sends SYSREF Here > ***/


	adrv9009_sysref_req(phy, SYSREF_CONT_ON);

	ret = clk_prepare_enable(phy->jesd_rx_clk);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(phy->jesd_rx_os_clk);
	if (ret < 0)
		return ret;

	adrv9009_sysref_req(phy, SYSREF_CONT_OFF);

	/*** < User Sends SYSREF Here > ***/

	/*** < Insert User JESD204B Sync Verification Code Here > ***/

	/**************************************/
	/**** Check Talise Deframer Status ***/
	/**************************************/
	ret = TALISE_readDeframerStatus(phy->talDevice, TAL_DEFRAMER_A,
					&deframerStatus);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	if ((deframerStatus & 0xF7) != 0x86)
		dev_warn(&phy->spi->dev, "TAL_DEFRAMER_A deframerStatus 0x%X", deframerStatus);

	/************************************/
	/**** Check Talise Framer Status ***/
	/************************************/
	ret = TALISE_readFramerStatus(phy->talDevice, TAL_FRAMER_A, &framerStatus);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	if ((framerStatus & 0x07) != 0x05)
		dev_warn(&phy->spi->dev, "TAL_FRAMER_A framerStatus 0x%X", framerStatus);

	/************************************/
	/**** Check Talise Framer Status ***/
	/************************************/
	ret = TALISE_readFramerStatus(phy->talDevice, TAL_FRAMER_B, &framerStatus);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	if ((framerStatus & 0x07) != 0x05)
		dev_warn(&phy->spi->dev, "TAL_FRAMER_B framerStatus 0x%X", framerStatus);

	/*** < User: When links have been verified, proceed > ***/

	/***********************************************
	 * Allow Rx1/2 QEC tracking and Tx1/2 QEC       *
	 * tracking to run when in the radioOn state    *
	 * Tx calibrations will only run if radioOn and *
	 * the obsRx path is set to OBS_INTERNAL_CALS   *
	 * **********************************************/

	TALISE_setGpIntMask(phy->talDevice, TAL_GP_MASK_AUX_SYNTH_UNLOCK);

	ret = TALISE_enableTrackingCals(phy->talDevice, trackingCalMask);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	ret = TALISE_setupRxAgc(phy->talDevice, &phy->rxAgcCtrl);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);

	/* Function to turn radio on, Enables transmitters and receivers */
	/* that were setup during TALISE_initialize() */
	ret = TALISE_radioOn(phy->talDevice);
	if (ret != TALACT_NO_ACTION)
		dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);


	clk_set_rate(phy->clks[RX_SAMPL_CLK],
		     phy->talInit.rx.rxProfile.rxOutputRate_kHz * 1000);
	clk_set_rate(phy->clks[OBS_SAMPL_CLK],
		     phy->talInit.obsRx.orxProfile.orxOutputRate_kHz * 1000);
	clk_set_rate(phy->clks[TX_SAMPL_CLK],
		     phy->talInit.tx.txProfile.txInputRate_kHz * 1000);


	TALISE_setRxTxEnable(phy->talDevice, TAL_RX1RX2_EN, TAL_TX1TX2);

	adrv9009_sysref_req(phy, SYSREF_CONT_ON);

	TALISE_setupAuxDacs(phy->talDevice, &phy->auxdac);
	TALISE_setPaProtectionCfg(phy->talDevice, &phy->tx_pa_protection);



	return 0;

}

static void adrv9009_shutdown(struct adrv9009_rf_phy *phy)
{
	/***********************************************
	 * Shutdown Procedure *
	 * **********************************************/
	/* Function to turn radio on, Disables transmitters and receivers */
	TALISE_radioOff(phy->talDevice);
	TALISE_shutdown(phy->talDevice);
	TALISE_closeHw(phy->talDevice);

	adrv9009_sysref_req(phy, SYSREF_CONT_OFF);

	clk_disable_unprepare(phy->jesd_rx_clk);
	clk_disable_unprepare(phy->jesd_rx_os_clk);
	clk_disable_unprepare(phy->jesd_tx_clk);

	memset(&phy->talise_device.devStateInfo, 0,
	       sizeof(phy->talise_device.devStateInfo));
}

static ssize_t adrv9009_phy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	bool enable;
	int ret = 0;
	u32 val;

	mutex_lock(&indio_dev->mlock);

	switch ((u32)this_attr->address & 0xFF) {
	case ADRV9009_ENSM_MODE:
		if (sysfs_streq(buf, "radio_on"))
			val = RADIO_ON;
		else if (sysfs_streq(buf, "radio_off"))
			val = RADIO_OFF;
		else
			break;

		ret = adrv9009_set_radio_state(phy, val);
		break;
	case ADRV9009_INIT_CAL:
		ret = strtobool(buf, &enable);
		if (ret)
			break;

		val = (u32)this_attr->address >> 8;

		if (val) {
			if (enable)
				phy->cal_mask |= val;
			else
				phy->cal_mask &= ~val;
		} else if (enable) {
			uint8_t errorFlag = 0;

			adrv9009_set_radio_state(phy, RADIO_FORCE_OFF);

			ret = TALISE_runInitCals(phy->talDevice, phy->cal_mask);
			if (ret != TALACT_NO_ACTION) {
				/*** < User: decide what to do based on Talise recovery action returned > ***/
				dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);
			}

			ret = TALISE_waitInitCals(phy->talDevice, 20000, &errorFlag);
			if (ret != TALACT_NO_ACTION) {
				/*** < User: decide what to do based on Talise recovery action returned > ***/
				dev_err(&phy->spi->dev, "%s:%d (ret %d)", __func__, __LINE__, ret);
			}

			if (errorFlag) {
				/*< user code - Check error flag to determine ARM  error> */
				dev_err(&phy->spi->dev, "%s:%d (ret %d) errorFlag %x", __func__, __LINE__, ret,
					errorFlag);
			} else {
				/*< user code - Calibrations completed successfully > */
			}

			adrv9009_set_radio_state(phy, RADIO_RESTORE_STATE);
		}
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : len;
}

static ssize_t adrv9009_phy_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	int ret = 0;
	u32 val;

	mutex_lock(&indio_dev->mlock);
	switch ((u32)this_attr->address & 0xFF) {
	case ADRV9009_ENSM_MODE:
		ret = sprintf(buf, "%s\n",
			      (phy->talDevice->devStateInfo.devState & TAL_STATE_RADIOON) ?
			      "radio_on" : "radio_off");
		break;
	case ADRV9009_ENSM_MODE_AVAIL:
		ret = sprintf(buf, "%s\n", "radio_on radio_off");
		break;
	case ADRV9009_INIT_CAL:
		val = (u32)this_attr->address >> 8;

		if (val)
			ret = sprintf(buf, "%d\n", !!(phy->cal_mask & val));
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static IIO_DEVICE_ATTR(ensm_mode, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_ENSM_MODE);

static IIO_DEVICE_ATTR(ensm_mode_available, S_IRUGO,
		       adrv9009_phy_show,
		       NULL,
		       ADRV9009_ENSM_MODE_AVAIL);

static IIO_DEVICE_ATTR(calibrate, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_INIT_CAL);

static IIO_DEVICE_ATTR(calibrate_rx_qec_en, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_INIT_CAL | (TAL_RX_QEC_INIT << 8));

static IIO_DEVICE_ATTR(calibrate_tx_qec_en, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_INIT_CAL | (TAL_TX_QEC_INIT << 8));

static IIO_DEVICE_ATTR(calibrate_tx_lol_en, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_INIT_CAL | (TAL_TX_LO_LEAKAGE_INTERNAL << 8));

static IIO_DEVICE_ATTR(calibrate_tx_lol_ext_en, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_INIT_CAL | (TAL_TX_LO_LEAKAGE_EXTERNAL << 8));

static IIO_DEVICE_ATTR(calibrate_rx_phase_correction_en, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_INIT_CAL | (TAL_RX_PHASE_CORRECTION << 8));

static IIO_DEVICE_ATTR(calibrate_frm_en, S_IRUGO | S_IWUSR,
		       adrv9009_phy_show,
		       adrv9009_phy_store,
		       ADRV9009_INIT_CAL | (TAL_FHM_CALS << 8));


static struct attribute *adrv9009_phy_attributes[] = {
	&iio_dev_attr_ensm_mode.dev_attr.attr,
	&iio_dev_attr_ensm_mode_available.dev_attr.attr,
	&iio_dev_attr_calibrate.dev_attr.attr,
	&iio_dev_attr_calibrate_rx_qec_en.dev_attr.attr,
	&iio_dev_attr_calibrate_tx_qec_en.dev_attr.attr,
	&iio_dev_attr_calibrate_tx_lol_en.dev_attr.attr,
	&iio_dev_attr_calibrate_tx_lol_ext_en.dev_attr.attr,
	&iio_dev_attr_calibrate_rx_phase_correction_en.dev_attr.attr,
	&iio_dev_attr_calibrate_frm_en.dev_attr.attr,
	NULL,
};

static const struct attribute_group adrv9009_phy_attribute_group = {
	.attrs = adrv9009_phy_attributes,
};

static int adrv9009_phy_reg_access(struct iio_dev *indio_dev,
				   u32 reg, u32 writeval,
				   u32 *readval)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	int ret;

	mutex_lock(&indio_dev->mlock);
	if (readval == NULL)
		ret = adrv9009_spi_write(phy->spi, reg, writeval);
	else {
		*readval = adrv9009_spi_read(phy->spi, reg);
		ret = 0;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

enum lo_ext_info {
	LOEXT_FREQ,
	FHM_ENABLE,
};

static ssize_t adrv9009_phy_lo_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	u64 readin;
	u8 status;
	u16 loop_bw;
	bool enable;
	int ret = 0;

	switch (private) {
	case LOEXT_FREQ:

		ret = kstrtoull(buf, 10, &readin);
		if (ret)
			return ret;

		mutex_lock(&indio_dev->mlock);

		adrv9009_set_radio_state(phy, RADIO_FORCE_OFF);

		if (readin >= 3000000000ULL)
			loop_bw = 300;
		else
			loop_bw = 50;

		if (loop_bw != phy->current_loopBandwidth_kHz[chan->channel]) {
			TALISE_setPllLoopFilter(phy->talDevice, TAL_RF_PLL + chan->channel, loop_bw,
						phy->loopFilter_stability);
			phy->current_loopBandwidth_kHz[chan->channel] = loop_bw;
		}

		ret = TALISE_setRfPllFrequency(phy->talDevice, TAL_RF_PLL + chan->channel,
					       readin);
		if (ret != TALACT_NO_ACTION) {
			adrv9009_set_radio_state(phy, RADIO_RESTORE_STATE);
			break;
		}

		ret = TALISE_getPllsLockStatus(phy->talDevice, &status);
		if (!((status & BIT(chan->channel + 1) || (ret != TALACT_NO_ACTION))))
			ret = -EFAULT;

		phy->trx_lo_frequency = readin;
		adrv9009_set_radio_state(phy, RADIO_RESTORE_STATE);
		break;
	case FHM_ENABLE:
		ret = strtobool(buf, &enable);
		if (ret)
			return ret;

		mutex_lock(&indio_dev->mlock);
		adrv9009_set_radio_state(phy, RADIO_FORCE_OFF);

		phy->fhm_mode.fhmEnable = enable;

		ret = TALISE_setFhmConfig(phy->talDevice, &phy->fhm_config);
		if (ret != TALACT_NO_ACTION)
			break;
		adrv9009_set_radio_state(phy, RADIO_RESTORE_STATE);

		if (phy->talDevice->devStateInfo.devState & TAL_STATE_RADIOON) {
			TALISE_setFhmMode(phy->talDevice, &phy->fhm_mode);
			if (ret != TALACT_NO_ACTION)
				break;
		} else
			ret = -EPROTO;

		break;
	default:
		ret = -EINVAL;
		break;

	}

	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : len;
}

static ssize_t adrv9009_phy_lo_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	taliseFhmMode_t	fhm_mode;
	u64 val;
	int ret;

	mutex_lock(&indio_dev->mlock);
	switch (private) {
	case LOEXT_FREQ:
		ret = TALISE_getRfPllFrequency(phy->talDevice, TAL_RF_PLL + chan->channel,
					       &val);
		break;
	case FHM_ENABLE:
		ret = TALISE_getFhmMode(phy->talDevice, &fhm_mode);
		val = fhm_mode.fhmEnable;
		break;
	default:
		ret = 0;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : sprintf(buf, "%llu\n", val);
}

#define _ADRV9009_EXT_LO_INFO(_name, _ident) { \
.name = _name, \
.read = adrv9009_phy_lo_read, \
.write = adrv9009_phy_lo_write, \
.private = _ident, \
}

static const struct iio_chan_spec_ext_info adrv9009_phy_ext_lo_info[] = {
	/* Ideally we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz. Using scale is a bit ugly.
	 */
	_ADRV9009_EXT_LO_INFO("frequency", LOEXT_FREQ),
	_ADRV9009_EXT_LO_INFO("frequency_hopping_mode_enable", FHM_ENABLE),
	{ },
};


static const struct iio_chan_spec_ext_info adrv9009_phy_ext_auxlo_info[] = {
	/* Ideally we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz. Using scale is a bit ugly.
	 */
	_ADRV9009_EXT_LO_INFO("frequency", LOEXT_FREQ),
	{ },
};

static int adrv9009_set_agc_mode(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan, u32 mode)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	unsigned val;
	int ret;

	switch (mode) {
	case 0:
		val = TAL_MGC;
		break;
	case 1:
		val = TAL_AGCFAST;
		break;
	case 2:
		val = TAL_AGCSLOW;
		break;
	case 3:
		val = TAL_HYBRID;
		break;
	default:
		return -EINVAL;
	}

	ret = TALISE_setRxGainControlMode(phy->talDevice, val);
	if (ret)
		return -EFAULT;

	return 0;
}

static int adrv9009_get_agc_mode(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);

	return phy->talDevice->devStateInfo.gainMode;
}

static const char * const adrv9009_agc_modes[] =
{"manual", "fast_attack", "slow_attack", "hybrid"};

static const struct iio_enum adrv9009_agc_modes_available = {
	.items = adrv9009_agc_modes,
	.num_items = ARRAY_SIZE(adrv9009_agc_modes),
	.get = adrv9009_get_agc_mode,
	.set = adrv9009_set_agc_mode,

};

static const char * const adrv9009_obs_rx_port[] = {
	"OBS_TX_LO", "OBS_SN_LO"
};

static const taliseObsRxLoSource_t adrv9009_obs_rx_port_lut[] = {
	TAL_OBSLO_RF_PLL, TAL_OBSLO_AUX_PLL
};

static int adrv9009_set_obs_rx_path(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan, u32 mode)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	int ret;

	ret = TALISE_setOrxLoSource(phy->talDevice, adrv9009_obs_rx_port_lut[mode]);
	if (!ret)
		phy->obs_rx_path_source = mode;

	return ret;

}

static int adrv9009_get_obs_rx_path(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	return phy->obs_rx_path_source;

}

static const struct iio_enum adrv9009_rf_obs_rx_port_available = {
	.items = adrv9009_obs_rx_port,
	.num_items = ARRAY_SIZE(adrv9009_obs_rx_port),
	.get = adrv9009_get_obs_rx_path,
	.set = adrv9009_set_obs_rx_path,
};

static ssize_t adrv9009_phy_rx_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	taliseTxChannels_t txchan;
	taliseRxORxChannels_t rxchan;
	bool enable;
	int ret = 0;
	u32 mask;

	ret = strtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&indio_dev->mlock);

	switch (private) {
		case RSSI:
			break;
	case RX_QEC:

		switch (chan->channel) {
		case CHAN_RX1:
			mask = TAL_TRACK_RX1_QEC;
			break;
		case CHAN_RX2:
			mask = TAL_TRACK_RX2_QEC;
			break;
		case CHAN_OBS_RX1:
			mask = TAL_TRACK_ORX1_QEC;
			break;
		case CHAN_OBS_RX2:
			mask = TAL_TRACK_ORX2_QEC;
			break;
		default:
			ret = -EINVAL;
			goto out;
		}

		if (enable)
			phy->tracking_cal_mask |= mask;
		else
			phy->tracking_cal_mask &= ~mask;

		adrv9009_set_radio_state(phy, RADIO_FORCE_OFF);
		ret = TALISE_enableTrackingCals(phy->talDevice, phy->tracking_cal_mask);
		adrv9009_set_radio_state(phy, RADIO_RESTORE_STATE);

		break;
	case RX_HD2:

		switch (chan->channel) {
		case CHAN_RX1:
			mask = TAL_TRACK_RX1_HD2;
			break;
		case CHAN_RX2:
			mask = TAL_TRACK_RX2_HD2;
			break;
		default:
			ret = -EINVAL;
			goto out;
		}

		if (enable)
			phy->tracking_cal_mask |= mask;
		else
			phy->tracking_cal_mask &= ~mask;


		adrv9009_set_radio_state(phy, RADIO_FORCE_OFF);
		ret = TALISE_enableTrackingCals(phy->talDevice, phy->tracking_cal_mask);
		adrv9009_set_radio_state(phy, RADIO_RESTORE_STATE);
		break;

	case RX_POWERDOWN:

		TALISE_getRxTxEnable(phy->talDevice, &rxchan, &txchan);

		switch (chan->channel) {
		case CHAN_RX1:
			if (enable)
				rxchan &= ~TAL_RX1_EN;
			else if (rxchan & TAL_ORX1ORX2_EN)
				ret = -EINVAL;
			else
				rxchan |= TAL_RX1_EN;

			break;
		case CHAN_RX2:
			if (enable)
				rxchan &= ~TAL_RX2_EN;
			else if (rxchan & TAL_ORX1ORX2_EN)
				ret = -EINVAL;
			else
				rxchan |= TAL_RX2_EN;

			break;
		case CHAN_OBS_RX1:
			if (enable)
				rxchan &= ~TAL_ORX1_EN;
			else if (rxchan & TAL_RX1RX2_EN)
				ret = -EINVAL;
			else
				rxchan |= TAL_ORX1_EN;

			break;
		case CHAN_OBS_RX2:
			if (enable)
				rxchan &= ~TAL_ORX2_EN;
			else if (rxchan & TAL_RX1RX2_EN)
				ret = -EINVAL;
			else
				rxchan |= TAL_ORX2_EN;

			break;
		default:
			ret = -EINVAL;
		}

		if (ret == 0)
			ret = TALISE_setRxTxEnable(phy->talDevice, rxchan, txchan);

		break;
	case RX_GAIN_CTRL_PIN_MODE:

		switch (chan->channel) {
		case CHAN_RX1:
			phy->rx1_gain_ctrl_pin.enable = enable;
			ret = TALISE_setRxGainCtrlPin(phy->talDevice, TAL_RX1, &phy->rx1_gain_ctrl_pin);
			break;
		case CHAN_RX2:
			phy->rx2_gain_ctrl_pin.enable = enable;
			ret = TALISE_setRxGainCtrlPin(phy->talDevice, TAL_RX2, &phy->rx2_gain_ctrl_pin);
			break;
		default:
			ret = -EINVAL;
		}
		break;
	default:
		ret = -EINVAL;
	}

out:
	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : len;
}



static ssize_t adrv9009_phy_rx_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	taliseTxChannels_t txchan;
	taliseRxORxChannels_t rxchan;
	taliseRxGainCtrlPin_t rxGainCtrlPin;
	taliseRxChannels_t rxChannel;
	int ret = 0;
	u16 dec_pwr_mdb;
	u32 mask;

	mutex_lock(&indio_dev->mlock);

	switch (private) {
	case RSSI:
		switch (chan->channel) {
		case CHAN_RX1:
			rxChannel = TAL_RX1;
			break;
		case CHAN_RX2:
			rxChannel = TAL_RX2;
			break;
		default:
			ret = -EINVAL;
		}

		if (ret == 0)
			ret = TALISE_getRxDecPower(phy->talDevice, rxChannel,
						   &dec_pwr_mdb);

		if (ret == 0)
			ret = sprintf(buf, "%u.%02u dB\n", dec_pwr_mdb / 1000,
					      dec_pwr_mdb % 1000);
		break;
	case RX_QEC:
		switch (chan->channel) {
		case CHAN_RX1:
			mask = TAL_TRACK_RX1_QEC;
			break;
		case CHAN_RX2:
			mask = TAL_TRACK_RX2_QEC;
			break;
		case CHAN_OBS_RX1:
			mask = TAL_TRACK_ORX1_QEC;
			break;
		case CHAN_OBS_RX2:
			mask = TAL_TRACK_ORX2_QEC;
			break;
		default:
			ret = -EINVAL;
		}

		if (ret == 0)
			ret = sprintf(buf, "%d\n", !!(mask & phy->tracking_cal_mask));

		break;
	case RX_HD2:
		switch (chan->channel) {
		case CHAN_RX1:
			mask = TAL_TRACK_RX1_HD2;
			break;
		case CHAN_RX2:
			mask = TAL_TRACK_RX2_HD2;
			break;

		default:
			ret = -EINVAL;
		}

		if (ret == 0)
			ret = sprintf(buf, "%d\n", !!(mask & phy->tracking_cal_mask));

		break;
	case RX_RF_BANDWIDTH:
		switch (chan->channel) {
		case CHAN_RX1:
		case CHAN_RX2:
			ret = phy->talDevice->devStateInfo.rxBandwidth_Hz;
			break;
		case CHAN_OBS_RX1:
		case CHAN_OBS_RX2:
			ret = phy->talDevice->devStateInfo.orxBandwidth_Hz;
			break;
		default:
			ret = -EINVAL;
		}

		if (ret > 0)
			ret = sprintf(buf, "%u\n", ret);

		break;
	case RX_POWERDOWN:
		ret = TALISE_getRxTxEnable(phy->talDevice, &rxchan, &txchan);

		switch (chan->channel) {
		case CHAN_RX1:
			ret = !(rxchan & TAL_RX1_EN);
			break;
		case CHAN_RX2:
			ret = !(rxchan & TAL_RX2_EN);
			break;
		case CHAN_OBS_RX1:
			ret = !(rxchan & TAL_ORX1_EN);
			break;
		case CHAN_OBS_RX2:
			ret = !(rxchan & TAL_ORX2_EN);
			break;
		default:
			ret = -EINVAL;
		}

		if (ret >= 0)
			ret = sprintf(buf, "%u\n", ret);

		break;
	case RX_GAIN_CTRL_PIN_MODE:

		switch (chan->channel) {
		case CHAN_RX1:
			ret = TALISE_getRxGainCtrlPin(phy->talDevice, TAL_RX1, &rxGainCtrlPin);
			break;
		case CHAN_RX2:
			ret = TALISE_getRxGainCtrlPin(phy->talDevice, TAL_RX1, &rxGainCtrlPin);
			break;
		default:
			ret = -EINVAL;
		}

		if (ret == 0)
			ret = sprintf(buf, "%u\n", rxGainCtrlPin.enable);
		break;

	default:
		ret = -EINVAL;

	}

	mutex_unlock(&indio_dev->mlock);

	return ret;
}

#define _ADRV9009_EXT_RX_INFO(_name, _ident) { \
.name = _name, \
.read = adrv9009_phy_rx_read, \
.write = adrv9009_phy_rx_write, \
.private = _ident, \
}

static const taliseTrackingCalibrations_t tx_track_cal_mask[][2] = {
	[TX_QEC] = {TAL_TRACK_TX1_QEC, TAL_TRACK_TX2_QEC},
	[TX_LOL] = {TAL_TRACK_TX1_LOL, TAL_TRACK_TX2_LOL},
};

static ssize_t adrv9009_phy_tx_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	taliseTxChannels_t txchan;
	taliseRxORxChannels_t rxchan;
	taliseTxAttenCtrlPin_t txAttenCtrlPin;
	u32 mask;
	int val, ret = 0;

	if (chan->channel > CHAN_TX2)
		return -EINVAL;

	mutex_lock(&indio_dev->mlock);
	switch (private) {
	case TX_QEC:
	case TX_LOL:
		mask = tx_track_cal_mask[private][chan->channel];
		val = !!(mask & phy->tracking_cal_mask);
		break;
	case TX_RF_BANDWIDTH:
		switch (chan->channel) {
		case CHAN_TX1:
		case CHAN_TX2:
			val = phy->talDevice->devStateInfo.txBandwidth_Hz;
			break;
		default:
			ret = -EINVAL;
		}
		break;
	case TX_POWERDOWN:
		TALISE_getRxTxEnable(phy->talDevice, &rxchan, &txchan);

		switch (chan->channel) {
		case CHAN_TX1:
			val = !(txchan & TAL_TX1);
			break;
		case CHAN_TX2:
			val = !(txchan & TAL_TX2);
			break;
		default:
			ret = -EINVAL;
		}

		break;
	case TX_ATTN_CTRL_PIN_MODE:

		switch (chan->channel) {
		case CHAN_TX1:
			ret = TALISE_getTxAttenCtrlPin(phy->talDevice, TAL_TX1, &txAttenCtrlPin);
			break;
		case CHAN_TX2:
			ret = TALISE_getTxAttenCtrlPin(phy->talDevice, TAL_TX2, &txAttenCtrlPin);
			break;
		default:
			ret = -EINVAL;
		}

		val = txAttenCtrlPin.enable;
		break;
	case TX_PA_PROTECTION:
		val = phy->tx_pa_protection_enabled;
		break;

	default:
		ret = -EINVAL;

	}

	if (ret == 0)
		ret = sprintf(buf, "%d\n", val);

	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static ssize_t adrv9009_phy_tx_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	taliseTxChannels_t txchan;
	taliseRxORxChannels_t rxchan;
	bool enable;
	int ret = 0;
	u32 mask;

	if (chan->channel > CHAN_TX2)
		return -EINVAL;

	ret = strtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&indio_dev->mlock);

	switch (private) {
	case TX_QEC:
	case TX_LOL:
		mask = tx_track_cal_mask[private][chan->channel];

		if (enable)
			phy->tracking_cal_mask |= mask;
		else
			phy->tracking_cal_mask &= ~mask;

		adrv9009_set_radio_state(phy, RADIO_FORCE_OFF);
		ret = TALISE_enableTrackingCals(phy->talDevice, phy->tracking_cal_mask);
		adrv9009_set_radio_state(phy, RADIO_RESTORE_STATE);

		break;

		ret = -EINVAL;
		break;
	case TX_POWERDOWN:
		TALISE_getRxTxEnable(phy->talDevice, &rxchan, &txchan);

		switch (chan->channel) {
		case CHAN_TX1:
			if (enable)
				txchan &= ~TAL_TX1;
			else
				txchan |= TAL_TX1;
			break;
		case CHAN_TX2:
			if (enable)
				txchan &= ~TAL_TX2;
			else
				txchan |= TAL_TX2;
			break;
		}

		if (ret == 0)
			ret = TALISE_setRxTxEnable(phy->talDevice, rxchan, txchan);

		break;
	case TX_ATTN_CTRL_PIN_MODE:
		switch (chan->channel) {
		case CHAN_TX1:
			phy->tx1_atten_ctrl_pin.enable = enable;
			ret = TALISE_setTxAttenCtrlPin(phy->talDevice, TAL_TX1,
						       &phy->tx1_atten_ctrl_pin);
			break;
		case CHAN_TX2:
			phy->tx2_atten_ctrl_pin.enable = enable;
			ret = TALISE_setTxAttenCtrlPin(phy->talDevice, TAL_TX2,
						       &phy->tx2_atten_ctrl_pin);
			break;
		default:
			ret = -EINVAL;
		}
		break;
	case TX_PA_PROTECTION:
		TALISE_enablePaProtection(phy->talDevice, enable);
		if (ret == TALACT_NO_ACTION)
			phy->tx_pa_protection_enabled = enable;

		break;

	default:
		ret = -EINVAL;

	}

	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : len;
}

#define _ADRV9009_EXT_TX_INFO(_name, _ident) { \
.name = _name, \
.read = adrv9009_phy_tx_read, \
.write = adrv9009_phy_tx_write, \
.private = _ident, \
}

static const struct iio_chan_spec_ext_info adrv9009_phy_rx_ext_info[] = {
	/* Ideally we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz. Using scale is a bit ugly.
	 */
	IIO_ENUM_AVAILABLE_SHARED("gain_control_mode", 0,  &adrv9009_agc_modes_available),
	IIO_ENUM("gain_control_mode", false, &adrv9009_agc_modes_available),
	_ADRV9009_EXT_RX_INFO("rssi", RSSI),
	_ADRV9009_EXT_RX_INFO("quadrature_tracking_en", RX_QEC),
	_ADRV9009_EXT_RX_INFO("hd2_tracking_en", RX_HD2), /* 2nd Harmonic Distortion */
	_ADRV9009_EXT_RX_INFO("rf_bandwidth", RX_RF_BANDWIDTH),
	_ADRV9009_EXT_RX_INFO("powerdown", RX_POWERDOWN),
	_ADRV9009_EXT_RX_INFO("gain_control_pin_mode_en", RX_GAIN_CTRL_PIN_MODE),
	{ },
};

static const struct iio_chan_spec_ext_info adrv9009_phy_obs_rx_ext_info[] = {
	/* Ideally we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz. Using scale is a bit ugly.
	 */
	IIO_ENUM_AVAILABLE_SHARED("rf_port_select", 0, &adrv9009_rf_obs_rx_port_available),
	IIO_ENUM("rf_port_select", false, &adrv9009_rf_obs_rx_port_available),
	_ADRV9009_EXT_RX_INFO("quadrature_tracking_en", RX_QEC),
	_ADRV9009_EXT_RX_INFO("rf_bandwidth", RX_RF_BANDWIDTH),
	_ADRV9009_EXT_RX_INFO("powerdown", RX_POWERDOWN),
	{ },
};

static struct iio_chan_spec_ext_info adrv9009_phy_tx_ext_info[] = {
	_ADRV9009_EXT_TX_INFO("quadrature_tracking_en", TX_QEC),
	_ADRV9009_EXT_TX_INFO("lo_leakage_tracking_en", TX_LOL),
	_ADRV9009_EXT_TX_INFO("rf_bandwidth", TX_RF_BANDWIDTH),
	_ADRV9009_EXT_TX_INFO("powerdown", TX_POWERDOWN),
	_ADRV9009_EXT_TX_INFO("atten_control_pin_mode_en", TX_ATTN_CTRL_PIN_MODE),
	_ADRV9009_EXT_TX_INFO("pa_protection_en", TX_PA_PROTECTION),
	{ },
};
static int adrv9009_gainindex_to_gain(struct adrv9009_rf_phy *phy, int channel,
				      unsigned index, int *val, int *val2)
{
	int code;

	switch (channel) {
	case CHAN_RX1:
		if (phy->gt_info[RX1_RX2_GT].abs_gain_tbl) {
			code = phy->gt_info[RX1_RX2_GT].abs_gain_tbl[index];
			break;
		}

		if (phy->gt_info[RX1_GT].abs_gain_tbl) {
			code = phy->gt_info[RX1_GT].abs_gain_tbl[index];
			break;
		}

		code = MAX_RX_GAIN_mdB -
		       (phy->talDevice->devStateInfo.gainIndexes.rx1MaxGainIndex - index) *
		       RX_GAIN_STEP_mdB;
		break;
	case CHAN_RX2:
		if (phy->gt_info[RX1_RX2_GT].abs_gain_tbl) {
			code = phy->gt_info[RX1_RX2_GT].abs_gain_tbl[index];
			break;
		}

		if (phy->gt_info[RX2_GT].abs_gain_tbl) {
			code = phy->gt_info[RX2_GT].abs_gain_tbl[index];
			break;
		}

		code = MAX_RX_GAIN_mdB -
		       (phy->talDevice->devStateInfo.gainIndexes.rx2MaxGainIndex - index) *
		       RX_GAIN_STEP_mdB;
		break;
	case CHAN_OBS_RX1:
		if (phy->gt_info[ORX_RX1_RX2_GT].abs_gain_tbl) {
			code = phy->gt_info[ORX_RX1_RX2_GT].abs_gain_tbl[index];
			break;
		}

		if (phy->gt_info[ORX_RX1_GT].abs_gain_tbl) {
			code = phy->gt_info[ORX_RX1_GT].abs_gain_tbl[index];
			break;
		}

		code = MAX_OBS_RX_GAIN_mdB -
		       (phy->talDevice->devStateInfo.gainIndexes.orx1MaxGainIndex - index) *
		       RX_GAIN_STEP_mdB;
		break;
	case CHAN_OBS_RX2:
		if (phy->gt_info[ORX_RX1_RX2_GT].abs_gain_tbl) {
			code = phy->gt_info[ORX_RX1_RX2_GT].abs_gain_tbl[index];
			break;
		}

		if (phy->gt_info[RX2_GT].abs_gain_tbl) {
			code = phy->gt_info[ORX_RX2_GT].abs_gain_tbl[index];
			break;
		}

		code = MAX_OBS_RX_GAIN_mdB -
		       (phy->talDevice->devStateInfo.gainIndexes.orx2MaxGainIndex - index) *
		       RX_GAIN_STEP_mdB;
		break;
	default:
		return -EINVAL;

	}

	*val = code / 1000;
	*val2 = (code % 1000) * 1000;
	if (!*val)
		*val2 *= -1;

	return 0;
}

static int find_table_index(struct adrv9009_rf_phy *phy,
			    enum adrv9009_gain_tables table, int gain)
{
	u32 i, nm1, n;

	for (i = 0; i < phy->gt_info[table].max_index; i++) {
		if (phy->gt_info[table].abs_gain_tbl[i] > gain) {
			nm1 = abs(phy->gt_info[table].abs_gain_tbl[
					  (i > 0) ? i - 1 : i] - gain);
			n = abs(phy->gt_info[table].abs_gain_tbl[i]
				- gain);
			if (nm1 < n)
				return (i > 0) ? i - 1 : i;
			else
				return i;
		}
	}

	return -EINVAL;
}

static int adrv9009_gain_to_gainindex(struct adrv9009_rf_phy *phy, int channel,
				      int val, int val2, unsigned *index)
{
	int ret, gain = ((abs(val) * 1000) + (abs(val2) / 1000));

	switch (channel) {
	case CHAN_RX1:
		if (phy->gt_info[RX1_RX2_GT].abs_gain_tbl) {
			ret = find_table_index(phy, RX1_RX2_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		if (phy->gt_info[RX1_GT].abs_gain_tbl) {
			ret = find_table_index(phy, RX1_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		gain = clamp(gain, MIN_GAIN_mdB, MAX_RX_GAIN_mdB);
		*index = (gain - MAX_RX_GAIN_mdB) / RX_GAIN_STEP_mdB +
			 phy->talDevice->devStateInfo.gainIndexes.rx1MaxGainIndex;
		break;

	case CHAN_RX2:
		if (phy->gt_info[ORX_RX1_RX2_GT].abs_gain_tbl) {
			ret = find_table_index(phy, ORX_RX1_RX2_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		if (phy->gt_info[RX2_GT].abs_gain_tbl) {
			ret = find_table_index(phy, RX1_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		gain = clamp(gain, MIN_GAIN_mdB, MAX_RX_GAIN_mdB);
		*index = (gain - MAX_RX_GAIN_mdB) / RX_GAIN_STEP_mdB +
			 phy->talDevice->devStateInfo.gainIndexes.rx2MaxGainIndex;
		break;
	case CHAN_OBS_RX1:
		if (phy->gt_info[ORX_RX1_RX2_GT].abs_gain_tbl) {
			ret = find_table_index(phy, ORX_RX1_RX2_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		if (phy->gt_info[ORX_RX1_GT].abs_gain_tbl) {
			ret = find_table_index(phy, ORX_RX1_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		gain = clamp(gain, MIN_GAIN_mdB, MAX_OBS_RX_GAIN_mdB);
		*index = (gain - MAX_OBS_RX_GAIN_mdB) / RX_GAIN_STEP_mdB +
			 phy->talDevice->devStateInfo.gainIndexes.orx1MaxGainIndex;
		break;

	case CHAN_OBS_RX2:
		if (phy->gt_info[RX1_RX2_GT].abs_gain_tbl) {
			ret = find_table_index(phy, RX1_RX2_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		if (phy->gt_info[ORX_RX2_GT].abs_gain_tbl) {
			ret = find_table_index(phy, ORX_RX2_GT, gain);
			if (ret >= 0) {
				*index = ret;
				break;
			}
		}

		gain = clamp(gain, MIN_GAIN_mdB, MAX_OBS_RX_GAIN_mdB);
		*index = (gain - MAX_OBS_RX_GAIN_mdB) / RX_GAIN_STEP_mdB +
			 phy->talDevice->devStateInfo.gainIndexes.orx2MaxGainIndex;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int adrv9009_phy_read_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int *val,
				 int *val2,
				 long m)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	taliseTxChannels_t txchan;
	taliseRxORxChannels_t rxchan;
	u16 temp;
	int ret;


	mutex_lock(&indio_dev->mlock);
	switch (m) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		if (chan->output) {
			u16 atten_mdb;
			switch (chan->channel) {
			case CHAN_TX1:
				TALISE_getTxAttenuation(phy->talDevice, TAL_TX1, &atten_mdb);
				break;
			case CHAN_TX2:
				TALISE_getTxAttenuation(phy->talDevice, TAL_TX2, &atten_mdb);
				break;
			}

			*val = -1 * (atten_mdb / 1000);
			*val2 = (atten_mdb % 1000) * 1000;
			if (!*val)
				*val2 *= -1;

		} else {
			u8 index;

			ret = TALISE_getRxTxEnable(phy->talDevice, &rxchan, &txchan);
			if (ret)
				break;

			switch (chan->channel) {
			case CHAN_RX1:
				if (rxchan & TAL_RX1_EN)
					ret = TALISE_getRxGain(phy->talDevice, TAL_RX1, &index);
				else
					ret = -EPROTO;
				break;
			case CHAN_RX2:
				if (rxchan & TAL_RX2_EN)
					ret = TALISE_getRxGain(phy->talDevice, TAL_RX2, &index);
				else
					ret = -EPROTO;
				break;
			case CHAN_OBS_RX1:
				if (rxchan & TAL_ORX1_EN)
					ret = TALISE_getObsRxGain(phy->talDevice, TAL_ORX1, &index);
				else
					ret = -EPROTO;
				break;
			case CHAN_OBS_RX2:
				if (rxchan & TAL_ORX2_EN)
					ret = TALISE_getObsRxGain(phy->talDevice, TAL_ORX2, &index);
				else
					ret = -EPROTO;
				break;
			default:
				ret = -EINVAL;
			}

			if (ret)
				break;

			ret = adrv9009_gainindex_to_gain(phy, chan->channel,
							 index, val, val2);
		}
		ret = IIO_VAL_INT_PLUS_MICRO_DB;
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (chan->output)
			*val = clk_get_rate(phy->clks[TX_SAMPL_CLK]);
		else {
			switch (chan->channel) {
			case CHAN_RX1:
			case CHAN_RX2:
				*val = clk_get_rate(phy->clks[RX_SAMPL_CLK]);
				break;
			case CHAN_OBS_RX1:
			case CHAN_OBS_RX2:
				*val = clk_get_rate(phy->clks[OBS_SAMPL_CLK]);
				break;
			}
		}

		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_RAW:
		if (chan->output) {
			*val = phy->auxdac.auxDacValues[chan->channel - CHAN_AUXDAC0];
			ret = IIO_VAL_INT;
		} else {
			taliseAuxAdcConfig_t auxadc = {
				.auxAdcChannelSel = chan->channel - CHAN_AUXADC0,
				.auxAdcMode = TAL_AUXADC_NONPIN_MODE,
				.numSamples = 10,
				.samplingPeriod_us = 50,
			};
			taliseAuxAdcResult_t result;

			TALISE_startAuxAdc(phy->talDevice, &auxadc);
			usleep_range(auxadc.numSamples * auxadc.samplingPeriod_us + 100,
				     auxadc.numSamples * auxadc.samplingPeriod_us + 500);
			ret = TALISE_readAuxAdc(phy->talDevice, &result);
			if (ret == TALACT_NO_ACTION && result.completeIndicator) {
				*val = result.auxAdcCodeAvg;
				ret = IIO_VAL_INT;
			}
		}
		break;
	case IIO_CHAN_INFO_OFFSET:
		if (chan->output) {
			if (chan->channel < CHAN_AUXDAC10) {
				*val = adrv9009_auxdac_offset_val1_lut
				       [phy->auxdac.auxDacResolution[chan->channel - CHAN_AUXDAC0]]
				       [phy->auxdac.auxDacVref[chan->channel - CHAN_AUXDAC0]];  /* AuxDAC */

			} else
				*val = 0;
		} else {
			*val = 45; /* AuxADC */
		}
		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_SCALE:
		if (chan->output) {
			if (chan->channel < CHAN_AUXDAC10) {
				*val = adrv9009_auxdac_scale_val1_lut
				       [phy->auxdac.auxDacResolution[chan->channel - CHAN_AUXDAC0]]; /* AuxDAC */
				*val2 = adrv9009_auxdac_scale_val2_lut
					[phy->auxdac.auxDacResolution[chan->channel - CHAN_AUXDAC0]];  /* AuxDAC */

			} else {
				*val = 0;
				*val2 = 805861;
			}
		} else {
			*val = 0; /* AuxADC */
			*val2 = 775194;
		}

		ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	case IIO_CHAN_INFO_PROCESSED:
		TALISE_getTemperature(phy->talDevice, &temp);
		*val = temp * 1000;
		ret = IIO_VAL_INT;
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&indio_dev->mlock);

	return ret;
};

static int adrv9009_phy_write_raw(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan,
				  int val,
				  int val2,
				  long mask)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	u32 code;
	int ret = 0;

	mutex_lock(&indio_dev->mlock);
	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		if (chan->output) {
			if (val > 0 || (val == 0 && val2 > 0)) {
				ret = -EINVAL;
				goto out;
			}

			code = ((abs(val) * 1000) + (abs(val2) / 1000));

			if (chan->channel == CHAN_TX1)
				ret = TALISE_setTxAttenuation(phy->talDevice, TAL_TX1, code);
			else
				ret = TALISE_setTxAttenuation(phy->talDevice, TAL_TX2, code);
		} else {
			ret = adrv9009_gain_to_gainindex(phy, chan->channel,
							 val, val2, &code);
			if (ret < 0)
				break;

			switch (chan->channel) {
			case CHAN_RX1:
				ret = TALISE_setRxManualGain(phy->talDevice, TAL_RX1, code);
				break;
			case CHAN_RX2:
				ret = TALISE_setRxManualGain(phy->talDevice, TAL_RX2, code);
				break;
			case CHAN_OBS_RX1:
				ret = TALISE_setObsRxManualGain(phy->talDevice, TAL_ORX1, code);
				break;
			case CHAN_OBS_RX2:
				ret = TALISE_setObsRxManualGain(phy->talDevice, TAL_ORX2, code);
				break;
			default:
				ret = -EINVAL;
			}
		}
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		break;
	case IIO_CHAN_INFO_RAW:
		if (chan->output) {
			if (phy->auxdac.auxDacEnables & BIT(chan->channel - CHAN_AUXDAC0)) {
				ret = TALISE_writeAuxDac(phy->talDevice, chan->channel - CHAN_AUXDAC0, val);
				if (ret != TALACT_NO_ACTION)
					ret = -EINVAL;
				phy->auxdac.auxDacValues[chan->channel - CHAN_AUXDAC0] = val;
			} else
				ret = -ENODEV;
		}
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static const struct iio_chan_spec adrv9009_phy_chan[] = {
	{	/* TRX LO */
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = 0,
		.extend_name = "TRX_LO",
		.ext_info = adrv9009_phy_ext_lo_info,
	}, {	/* AUX RX Observation LO */
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = 1,
		.extend_name = "AUX_OBS_RX_LO",
		.ext_info = adrv9009_phy_ext_auxlo_info,
	}, {	/* TX1 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = adrv9009_phy_tx_ext_info,
	}, {	/* RX1 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_RX1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN) | BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = adrv9009_phy_rx_ext_info,
	}, {	/* TX2 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = adrv9009_phy_tx_ext_info,
	}, {	/* RX2 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_RX2,
		.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN) | BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = adrv9009_phy_rx_ext_info,
	}, {	/* RX Sniffer/Observation */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_OBS_RX1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN) | BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = adrv9009_phy_obs_rx_ext_info,
	}, {	/* RX Sniffer/Observation */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_OBS_RX2,
		.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN) | BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = adrv9009_phy_obs_rx_ext_info,
	}, {	/* AUXADC0 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_AUXADC0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXADC1 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_AUXADC1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXADC2 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_AUXADC2,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXADC3 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = CHAN_AUXADC3,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC0 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC1 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC2 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC2,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC3 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC3,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC4 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC4,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC5 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC5,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC6 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC6,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC7 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC7,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC8 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC8,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC9 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC9,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	}, {	/* AUXDAC10 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC10,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE),
	}, {	/* AUXDAC11 */
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = CHAN_AUXDAC11,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE),
	}, {
		.type = IIO_TEMP,
		.indexed = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static const struct iio_info adrv9009_phy_info = {
	.read_raw = &adrv9009_phy_read_raw,
	.write_raw = &adrv9009_phy_write_raw,
	.debugfs_reg_access = &adrv9009_phy_reg_access,
	.attrs = &adrv9009_phy_attribute_group,
	.driver_module = THIS_MODULE,
};

static ssize_t adrv9009_debugfs_read(struct file *file, char __user *userbuf,
				     size_t count, loff_t *ppos)
{
	struct adrv9009_debugfs_entry *entry = file->private_data;
	char buf[700];
	u64 val = 0;
	ssize_t len = 0;
	int ret;

	if (entry->out_value) {
		switch (entry->size) {
		case 1:
			val = *(u8 *)entry->out_value;
			break;
		case 2:
			val = *(u16 *)entry->out_value;
			break;
		case 4:
			val = *(u32 *)entry->out_value;
			break;
		case 5:
			val = *(bool *)entry->out_value;
			break;
		case 8:
			val = *(u64 *)entry->out_value;
			break;
		default:
			ret = -EINVAL;
		}

	} else if (entry->cmd)
		val = entry->val;
	else
		return -EFAULT;

	if (!len)
		len = snprintf(buf, sizeof(buf), "%llu\n", val);

	return simple_read_from_buffer(userbuf, count, ppos, buf, len);
}

static ssize_t adrv9009_debugfs_write(struct file *file,
				      const char __user *userbuf, size_t count, loff_t *ppos)
{
	struct adrv9009_debugfs_entry *entry = file->private_data;
	struct adrv9009_rf_phy *phy = entry->phy;
	taliseTxNcoTestToneCfg_t nco_config;
	u32 val2, val3, val4;
	s64 val;
	char buf[80];
	int ret;

	count = min_t(size_t, count, (sizeof(buf) - 1));
	if (copy_from_user(buf, userbuf, count))
		return -EFAULT;

	buf[count] = 0;

	ret = sscanf(buf, "%lld %i %i %i", &val, &val2, &val3, &val4);
	if (ret < 1)
		return -EINVAL;

	switch (entry->cmd) {
	case DBGFS_INIT:
		if (!(ret == 1 && val == 1))
			return -EINVAL;
		mutex_lock(&phy->indio_dev->mlock);

		adrv9009_shutdown(phy);
		ret = adrv9009_setup(phy);
		if (ret)
			ret = adrv9009_setup(phy);

		mutex_unlock(&phy->indio_dev->mlock);

		return count;
	case DBGFS_BIST_FRAMER_A_PRBS:
	case DBGFS_BIST_FRAMER_B_PRBS:
		mutex_lock(&phy->indio_dev->mlock);
		ret = TALISE_enableFramerTestData(phy->talDevice,
						  entry->cmd == DBGFS_BIST_FRAMER_A_PRBS ? TAL_FRAMER_A : TAL_FRAMER_B,
						  val, TAL_FTD_FRAMERINPUT);
		mutex_unlock(&phy->indio_dev->mlock);
		if (ret)
			return ret;

		entry->val = val;
		return count;
	case DBGFS_BIST_TONE:
		if (ret != 3)
			return -EINVAL;

		nco_config.enable = val;
		nco_config.tx1ToneFreq_kHz = val2;
		nco_config.tx2ToneFreq_kHz = val3;

		mutex_lock(&phy->indio_dev->mlock);
		ret = TALISE_enableTxNco(phy->talDevice, &nco_config);
		mutex_unlock(&phy->indio_dev->mlock);
		if (ret < 0)
			return ret;

		entry->val = val;
		return count;
	default:
		break;
	}

	if (entry->out_value) {
		switch (entry->size) {
		case 1:
			*(u8 *)entry->out_value = val;
			break;
		case 2:
			*(u16 *)entry->out_value = val;
			break;
		case 4:
			*(u32 *)entry->out_value = val;
			break;
		case 5:
			*(bool *)entry->out_value = val;
			break;
		case 8:
			*(u64 *)entry->out_value = val;
			break;
		default:
			ret = -EINVAL;
		}
	}

	return count;
}

static const struct file_operations adrv9009_debugfs_reg_fops = {
	.open = simple_open,
	.read = adrv9009_debugfs_read,
	.write = adrv9009_debugfs_write,
};

static void adrv9009_add_debugfs_entry(struct adrv9009_rf_phy *phy,
				       const char *propname, unsigned int cmd)
{
	unsigned int i = phy->adrv9009_debugfs_entry_index;

	if (WARN_ON(i >= ARRAY_SIZE(phy->debugfs_entry)))
		return;

	phy->debugfs_entry[i].phy = phy;
	phy->debugfs_entry[i].propname = propname;
	phy->debugfs_entry[i].cmd = cmd;

	phy->adrv9009_debugfs_entry_index++;
}

static int adrv9009_register_debugfs(struct iio_dev *indio_dev)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	struct dentry *d;
	int i;

	if (!iio_get_debugfs_dentry(indio_dev))
		return -ENODEV;

	adrv9009_add_debugfs_entry(phy, "initialize", DBGFS_INIT);
	adrv9009_add_debugfs_entry(phy, "bist_framer_a_prbs", DBGFS_BIST_FRAMER_A_PRBS);
	adrv9009_add_debugfs_entry(phy, "bist_framer_b_prbs", DBGFS_BIST_FRAMER_B_PRBS);
	adrv9009_add_debugfs_entry(phy, "bist_tone", DBGFS_BIST_TONE);

	for (i = 0; i < phy->adrv9009_debugfs_entry_index; i++)
		d = debugfs_create_file(
			    phy->debugfs_entry[i].propname, 0644,
			    iio_get_debugfs_dentry(indio_dev),
			    &phy->debugfs_entry[i],
			    &adrv9009_debugfs_reg_fops);
	return 0;
}

static int __adrv9009_of_get_u32(struct iio_dev *indio_dev,
				 struct device_node *np, const char *propname,
				 s64 defval, void *out_value, u32 size)
{
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	u32 tmp;
	u64 tmp64;
	int ret;

	if (size == 8) {
		tmp64 = defval;
		ret = of_property_read_u64(np, propname, &tmp64);
	} else {
		tmp = defval;
		ret = of_property_read_u32(np, propname, &tmp);
	}

	if (out_value) {
		switch (size) {
		case 1:
			*(u8 *)out_value = tmp;
			break;
		case 2:
			*(u16 *)out_value = tmp;
			break;
		case 4:
			*(u32 *)out_value = tmp;
			break;
		case 8:
			*(u64 *)out_value = tmp64;
			break;
		default:
			ret = -EINVAL;
		}
	}

	if (WARN_ON(phy->adrv9009_debugfs_entry_index >=
		    ARRAY_SIZE(phy->debugfs_entry)))
		return ret;

	phy->debugfs_entry[phy->adrv9009_debugfs_entry_index++] =
	(struct adrv9009_debugfs_entry) {
		.out_value = out_value,
		 .propname = propname,
		  .size = size,
		   .phy = phy,
	};

	return ret;
}
#define adrv9009_of_get_u32(iodev, dnp, name, def, outp) \
__adrv9009_of_get_u32(iodev, dnp, name, def, outp, sizeof(*outp))

static int adrv9009_phy_parse_dt(struct iio_dev *iodev, struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct adrv9009_rf_phy *phy = iio_priv(iodev);
	int ret;

#define adrv9009_of_get_u32(iodev, dnp, name, def, outp) \
	__adrv9009_of_get_u32(iodev, dnp, name, def, outp, sizeof(*outp))

#define ADRV9009_OF_PROP(_dt_name, _member_, _default) \
	__adrv9009_of_get_u32(iodev, np, _dt_name, _default, _member_, sizeof(*_member_))

#define ADRV9009_GET_FIR(_dt_base_name, _member, _storage) \
	ADRV9009_OF_PROP(_dt_base_name"-gain_db", &_member.gain_dB, 0); \
	ADRV9009_OF_PROP(_dt_base_name"-num-fir-coefs", &_member.numFirCoefs, 0); \
	ret = of_property_read_u16_array(np, _dt_base_name"-coefs", _storage, _member.numFirCoefs); \
	if (ret < 0) { \
		dev_err(dev, "Failed to read %d FIR coefficients (%d)\n", phy->talInit.rx.rxProfile.rxFir.numFirCoefs, ret); \
		return ret; \
	} \
	_member.coefs = _storage; \

#define ADRV9009_GET_PROFILE(_dt_name, _member) \
	ret = of_property_read_u16_array(np, _dt_name, _member, ARRAY_SIZE(_member)); \
	if (ret < 0) { \
		dev_err(dev, "Failed to read %lu coefficients\n", ARRAY_SIZE(_member)); \
		return ret; \
	} \

	ADRV9009_OF_PROP("adi,default-initial-calibrations-mask", &phy->init_cal_mask,
			 TAL_TX_BB_FILTER | TAL_ADC_TUNER | TAL_TIA_3DB_CORNER |
			 TAL_DC_OFFSET | TAL_RX_GAIN_DELAY | TAL_FLASH_CAL |
			 TAL_PATH_DELAY | TAL_TX_LO_LEAKAGE_INTERNAL | TAL_TX_QEC_INIT |
			 TAL_LOOPBACK_RX_LO_DELAY | TAL_LOOPBACK_RX_RX_QEC_INIT |
			 TAL_RX_QEC_INIT | TAL_ORX_QEC_INIT | TAL_TX_DAC | TAL_ADC_STITCHING |
			 TAL_RX_PHASE_CORRECTION | TAL_FHM_CALS);

	ADRV9009_OF_PROP("adi,rxagc-peak-agc-under-range-low-interval_ns",
			 &phy->rxAgcCtrl.agcPeak.agcUnderRangeLowInterval_ns, 205);
	ADRV9009_OF_PROP("adi,rxagc-peak-agc-under-range-mid-interval",
			 &phy->rxAgcCtrl.agcPeak.agcUnderRangeMidInterval, 2);
	ADRV9009_OF_PROP("adi,rxagc-peak-agc-under-range-high-interval",
			 &phy->rxAgcCtrl.agcPeak.agcUnderRangeHighInterval, 4);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-high-thresh",
			 &phy->rxAgcCtrl.agcPeak.apdHighThresh, 39);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-low-gain-mode-high-thresh",
			 &phy->rxAgcCtrl.agcPeak.apdLowGainModeHighThresh, 36);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-low-thresh",
			 &phy->rxAgcCtrl.agcPeak.apdLowThresh, 23);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-low-gain-mode-low-thresh",
			 &phy->rxAgcCtrl.agcPeak.apdLowGainModeLowThresh, 19);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-upper-thresh-peak-exceeded-cnt",
			 &phy->rxAgcCtrl.agcPeak.apdUpperThreshPeakExceededCnt, 6);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-lower-thresh-peak-exceeded-cnt",
			 &phy->rxAgcCtrl.agcPeak.apdLowerThreshPeakExceededCnt, 3);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-gain-step-attack",
			 &phy->rxAgcCtrl.agcPeak.apdGainStepAttack, 4);
	ADRV9009_OF_PROP("adi,rxagc-peak-apd-gain-step-recovery",
			 &phy->rxAgcCtrl.agcPeak.apdGainStepRecovery, 2);
	ADRV9009_OF_PROP("adi,rxagc-peak-enable-hb2-overload",
			 &phy->rxAgcCtrl.agcPeak.enableHb2Overload, 1);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-overload-duration-cnt",
			 &phy->rxAgcCtrl.agcPeak.hb2OverloadDurationCnt, 1);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-overload-thresh-cnt",
			 &phy->rxAgcCtrl.agcPeak.hb2OverloadThreshCnt, 4);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-high-thresh",
			 &phy->rxAgcCtrl.agcPeak.hb2HighThresh, 181);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-under-range-low-thresh",
			 &phy->rxAgcCtrl.agcPeak.hb2UnderRangeLowThresh, 45);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-under-range-mid-thresh",
			 &phy->rxAgcCtrl.agcPeak.hb2UnderRangeMidThresh, 90);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-under-range-high-thresh",
			 &phy->rxAgcCtrl.agcPeak.hb2UnderRangeHighThresh, 128);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-upper-thresh-peak-exceeded-cnt",
			 &phy->rxAgcCtrl.agcPeak.hb2UpperThreshPeakExceededCnt, 6);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-lower-thresh-peak-exceeded-cnt",
			 &phy->rxAgcCtrl.agcPeak.hb2LowerThreshPeakExceededCnt, 3);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-gain-step-high-recovery",
			 &phy->rxAgcCtrl.agcPeak.hb2GainStepHighRecovery, 2);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-gain-step-low-recovery",
			 &phy->rxAgcCtrl.agcPeak.hb2GainStepLowRecovery, 4);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-gain-step-mid-recovery",
			 &phy->rxAgcCtrl.agcPeak.hb2GainStepMidRecovery, 8);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-gain-step-attack",
			 &phy->rxAgcCtrl.agcPeak.hb2GainStepAttack, 4);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-overload-power-mode",
			 &phy->rxAgcCtrl.agcPeak.hb2OverloadPowerMode, 1);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-ovrg-sel",
			 &phy->rxAgcCtrl.agcPeak.hb2OvrgSel, 0);
	ADRV9009_OF_PROP("adi,rxagc-peak-hb2-thresh-config",
			 &phy->rxAgcCtrl.agcPeak.hb2ThreshConfig, 3);

	ADRV9009_OF_PROP("adi,rxagc-power-power-enable-measurement",
			 &phy->rxAgcCtrl.agcPower.powerEnableMeasurement, 1);
	ADRV9009_OF_PROP("adi,rxagc-power-power-use-rfir-out",
			 &phy->rxAgcCtrl.agcPower.powerUseRfirOut, 1);
	ADRV9009_OF_PROP("adi,rxagc-power-power-use-bbdc2",
			 &phy->rxAgcCtrl.agcPower.powerUseBBDC2, 0);
	ADRV9009_OF_PROP("adi,rxagc-power-under-range-high-power-thresh",
			 &phy->rxAgcCtrl.agcPower.underRangeHighPowerThresh, 9);
	ADRV9009_OF_PROP("adi,rxagc-power-under-range-low-power-thresh",
			 &phy->rxAgcCtrl.agcPower.underRangeLowPowerThresh, 2);
	ADRV9009_OF_PROP("adi,rxagc-power-under-range-high-power-gain-step-recovery",
			 &phy->rxAgcCtrl.agcPower.underRangeHighPowerGainStepRecovery, 4);
	ADRV9009_OF_PROP("adi,rxagc-power-under-range-low-power-gain-step-recovery",
			 &phy->rxAgcCtrl.agcPower.underRangeLowPowerGainStepRecovery, 4);
	ADRV9009_OF_PROP("adi,rxagc-power-power-measurement-duration",
			 &phy->rxAgcCtrl.agcPower.powerMeasurementDuration, 5);
	ADRV9009_OF_PROP("adi,rxagc-power-rx1-tdd-power-meas-duration",
			 &phy->rxAgcCtrl.agcPower.rx1TddPowerMeasDuration, 5);
	ADRV9009_OF_PROP("adi,rxagc-power-rx1-tdd-power-meas-delay",
			 &phy->rxAgcCtrl.agcPower.rx1TddPowerMeasDelay, 1);
	ADRV9009_OF_PROP("adi,rxagc-power-rx2-tdd-power-meas-duration",
			 &phy->rxAgcCtrl.agcPower.rx2TddPowerMeasDuration, 5);
	ADRV9009_OF_PROP("adi,rxagc-power-rx2-tdd-power-meas-delay",
			 &phy->rxAgcCtrl.agcPower.rx2TddPowerMeasDelay, 1);
	ADRV9009_OF_PROP("adi,rxagc-power-upper0-power-thresh",
			 &phy->rxAgcCtrl.agcPower.upper0PowerThresh, 2);
	ADRV9009_OF_PROP("adi,rxagc-power-upper1-power-thresh",
			 &phy->rxAgcCtrl.agcPower.upper1PowerThresh, 0);
	ADRV9009_OF_PROP("adi,rxagc-power-power-log-shift",
			 &phy->rxAgcCtrl.agcPower.powerLogShift, 0);

	ADRV9009_OF_PROP("adi,rxagc-agc-peak-wait-time",
			 &phy->rxAgcCtrl.agcPeakWaitTime, 4);
	ADRV9009_OF_PROP("adi,rxagc-agc-rx1-max-gain-index",
			 &phy->rxAgcCtrl.agcRx1MaxGainIndex, 255);
	ADRV9009_OF_PROP("adi,rxagc-agc-rx1-min-gain-index",
			 &phy->rxAgcCtrl.agcRx1MinGainIndex, 195);
	ADRV9009_OF_PROP("adi,rxagc-agc-rx2-max-gain-index",
			 &phy->rxAgcCtrl.agcRx2MaxGainIndex, 255);
	ADRV9009_OF_PROP("adi,rxagc-agc-rx2-min-gain-index",
			 &phy->rxAgcCtrl.agcRx2MinGainIndex, 195);
	ADRV9009_OF_PROP("adi,rxagc-agc-gain-update-counter_us",
			 &phy->rxAgcCtrl.agcGainUpdateCounter_us, 250);
	ADRV9009_OF_PROP("adi,rxagc-agc-rx1-attack-delay",
			 &phy->rxAgcCtrl.agcRx1AttackDelay, 10);
	ADRV9009_OF_PROP("adi,rxagc-agc-rx2-attack-delay",
			 &phy->rxAgcCtrl.agcRx2AttackDelay, 10);
	ADRV9009_OF_PROP("adi,rxagc-agc-slow-loop-settling-delay",
			 &phy->rxAgcCtrl.agcSlowLoopSettlingDelay, 16);
	ADRV9009_OF_PROP("adi,rxagc-agc-low-thresh-prevent-gain",
			 &phy->rxAgcCtrl.agcLowThreshPreventGain, 0);
	ADRV9009_OF_PROP("adi,rxagc-agc-change-gain-if-thresh-high",
			 &phy->rxAgcCtrl.agcChangeGainIfThreshHigh, 1);
	ADRV9009_OF_PROP("adi,rxagc-agc-peak-thresh-gain-control-mode",
			 &phy->rxAgcCtrl.agcPeakThreshGainControlMode, 1);
	ADRV9009_OF_PROP("adi,rxagc-agc-reset-on-rxon", &phy->rxAgcCtrl.agcResetOnRxon,
			 0);
	ADRV9009_OF_PROP("adi,rxagc-agc-enable-sync-pulse-for-gain-counter",
			 &phy->rxAgcCtrl.agcEnableSyncPulseForGainCounter, 0);
	ADRV9009_OF_PROP("adi,rxagc-agc-enable-ip3-optimization-thresh",
			 &phy->rxAgcCtrl.agcEnableIp3OptimizationThresh, 0);
	ADRV9009_OF_PROP("adi,rxagc-ip3-over-range-thresh",
			 &phy->rxAgcCtrl.ip3OverRangeThresh, 31);
	ADRV9009_OF_PROP("adi,rxagc-ip3-over-range-thresh-index",
			 &phy->rxAgcCtrl.ip3OverRangeThreshIndex, 246);
	ADRV9009_OF_PROP("adi,rxagc-ip3-peak-exceeded-cnt",
			 &phy->rxAgcCtrl.ip3PeakExceededCnt, 4);
	ADRV9009_OF_PROP("adi,rxagc-agc-enable-fast-recovery-loop",
			 &phy->rxAgcCtrl.agcEnableFastRecoveryLoop, 0);


	ADRV9009_OF_PROP("adi,aux-dac-enables", &phy->auxdac.auxDacEnables, 0);

	ADRV9009_OF_PROP("adi,aux-dac-vref0", &phy->auxdac.auxDacVref[0], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution0", &phy->auxdac.auxDacResolution[0],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values0", &phy->auxdac.auxDacValues[0], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref1", &phy->auxdac.auxDacVref[1], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution1", &phy->auxdac.auxDacResolution[1],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values1", &phy->auxdac.auxDacValues[1], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref2", &phy->auxdac.auxDacVref[2], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution2", &phy->auxdac.auxDacResolution[2],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values2", &phy->auxdac.auxDacValues[2], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref3", &phy->auxdac.auxDacVref[3], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution3", &phy->auxdac.auxDacResolution[3],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values3", &phy->auxdac.auxDacValues[3], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref4", &phy->auxdac.auxDacVref[4], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution4", &phy->auxdac.auxDacResolution[4],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values4", &phy->auxdac.auxDacValues[4], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref5", &phy->auxdac.auxDacVref[5], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution5", &phy->auxdac.auxDacResolution[5],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values5", &phy->auxdac.auxDacValues[5], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref6", &phy->auxdac.auxDacVref[6], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution6", &phy->auxdac.auxDacResolution[6],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values6", &phy->auxdac.auxDacValues[6], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref7", &phy->auxdac.auxDacVref[7], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution7", &phy->auxdac.auxDacResolution[7],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values7", &phy->auxdac.auxDacValues[7], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref8", &phy->auxdac.auxDacVref[8], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution8", &phy->auxdac.auxDacResolution[8],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values8", &phy->auxdac.auxDacValues[8], 0);
	ADRV9009_OF_PROP("adi,aux-dac-vref9", &phy->auxdac.auxDacVref[9], 3);
	ADRV9009_OF_PROP("adi,aux-dac-resolution9", &phy->auxdac.auxDacResolution[9],
			 0);
	ADRV9009_OF_PROP("adi,aux-dac-values9", &phy->auxdac.auxDacValues[9], 0);

	ADRV9009_OF_PROP("adi,aux-dac-values10", &phy->auxdac.auxDacValues[10], 0);
	ADRV9009_OF_PROP("adi,aux-dac-values11", &phy->auxdac.auxDacValues[11], 0);


	ADRV9009_OF_PROP("adi,jesd204-framer-a-bank-id",
			 &phy->talInit.jesd204Settings.framerA.bankId, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-device-id",
			 &phy->talInit.jesd204Settings.framerA.deviceId, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-lane0-id",
			 &phy->talInit.jesd204Settings.framerA.lane0Id, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-m",
			 &phy->talInit.jesd204Settings.framerA.M, 4);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-k",
			 &phy->talInit.jesd204Settings.framerA.K, 32);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-f",
			 &phy->talInit.jesd204Settings.framerA.F, 4);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-np",
			 &phy->talInit.jesd204Settings.framerA.Np, 16);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-scramble",
			 &phy->talInit.jesd204Settings.framerA.scramble, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-external-sysref",
			 &phy->talInit.jesd204Settings.framerA.externalSysref, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-serializer-lanes-enabled",
			 &phy->talInit.jesd204Settings.framerA.serializerLanesEnabled, 0x03);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-serializer-lane-crossbar",
			 &phy->talInit.jesd204Settings.framerA.serializerLaneCrossbar, 0xE4);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-lmfc-offset",
			 &phy->talInit.jesd204Settings.framerA.lmfcOffset, 31);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-new-sysref-on-relink",
			 &phy->talInit.jesd204Settings.framerA.newSysrefOnRelink, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-syncb-in-select",
			 &phy->talInit.jesd204Settings.framerA.syncbInSelect, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-over-sample",
			 &phy->talInit.jesd204Settings.framerA.overSample, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-syncb-in-lvds-mode",
			 &phy->talInit.jesd204Settings.framerA.syncbInLvdsMode, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-syncb-in-lvds-pn-invert",
			 &phy->talInit.jesd204Settings.framerA.syncbInLvdsPnInvert, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-a-enable-manual-lane-xbar",
			 &phy->talInit.jesd204Settings.framerA.enableManualLaneXbar, 0);


	ADRV9009_OF_PROP("adi,jesd204-framer-b-bank-id",
			 &phy->talInit.jesd204Settings.framerB.bankId, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-device-id",
			 &phy->talInit.jesd204Settings.framerB.deviceId, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-lane0-id",
			 &phy->talInit.jesd204Settings.framerB.lane0Id, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-m",
			 &phy->talInit.jesd204Settings.framerB.M, 4);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-k",
			 &phy->talInit.jesd204Settings.framerB.K, 32);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-f",
			 &phy->talInit.jesd204Settings.framerB.F, 4);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-np",
			 &phy->talInit.jesd204Settings.framerB.Np, 16);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-scramble",
			 &phy->talInit.jesd204Settings.framerB.scramble, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-external-sysref",
			 &phy->talInit.jesd204Settings.framerB.externalSysref, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-serializer-lanes-enabled",
			 &phy->talInit.jesd204Settings.framerB.serializerLanesEnabled, 0x0C);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-serializer-lane-crossbar",
			 &phy->talInit.jesd204Settings.framerB.serializerLaneCrossbar, 0xE4);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-lmfc-offset",
			 &phy->talInit.jesd204Settings.framerB.lmfcOffset, 31);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-new-sysref-on-relink",
			 &phy->talInit.jesd204Settings.framerB.newSysrefOnRelink, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-syncb-in-select",
			 &phy->talInit.jesd204Settings.framerB.syncbInSelect, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-over-sample",
			 &phy->talInit.jesd204Settings.framerB.overSample, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-syncb-in-lvds-mode",
			 &phy->talInit.jesd204Settings.framerB.syncbInLvdsMode, 1);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-syncb-in-lvds-pn-invert",
			 &phy->talInit.jesd204Settings.framerB.syncbInLvdsPnInvert, 0);
	ADRV9009_OF_PROP("adi,jesd204-framer-b-enable-manual-lane-xbar",
			 &phy->talInit.jesd204Settings.framerB.enableManualLaneXbar, 0);


	ADRV9009_OF_PROP("adi,jesd204-deframer-a-bank-id",
			 &phy->talInit.jesd204Settings.deframerA.bankId, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-device-id",
			 &phy->talInit.jesd204Settings.deframerA.deviceId, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-lane0-id",
			 &phy->talInit.jesd204Settings.deframerA.lane0Id, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-m",
			 &phy->talInit.jesd204Settings.deframerA.M, 4);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-k",
			 &phy->talInit.jesd204Settings.deframerA.K, 32);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-scramble",
			 &phy->talInit.jesd204Settings.deframerA.scramble, 1);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-external-sysref",
			 &phy->talInit.jesd204Settings.deframerA.externalSysref, 1);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-deserializer-lanes-enabled",
			 &phy->talInit.jesd204Settings.deframerA.deserializerLanesEnabled, 0x0F);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-deserializer-lane-crossbar",
			 &phy->talInit.jesd204Settings.deframerA.deserializerLaneCrossbar, 0xE4);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-lmfc-offset",
			 &phy->talInit.jesd204Settings.deframerA.lmfcOffset, 17);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-new-sysref-on-relink",
			 &phy->talInit.jesd204Settings.deframerA.newSysrefOnRelink, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-syncb-out-select",
			 &phy->talInit.jesd204Settings.deframerA.syncbOutSelect, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-np",
			 &phy->talInit.jesd204Settings.deframerA.Np, 16);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-syncb-out-lvds-mode",
			 &phy->talInit.jesd204Settings.deframerA.syncbOutLvdsMode, 1);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-syncb-out-lvds-pn-invert",
			 &phy->talInit.jesd204Settings.deframerA.syncbOutLvdsPnInvert, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-syncb-out-cmos-slew-rate",
			 &phy->talInit.jesd204Settings.deframerA.syncbOutCmosSlewRate, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-syncb-out-cmos-drive-level",
			 &phy->talInit.jesd204Settings.deframerA.syncbOutCmosDriveLevel, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-a-enable-manual-lane-xbar",
			 &phy->talInit.jesd204Settings.deframerA.enableManualLaneXbar, 0);


	ADRV9009_OF_PROP("adi,jesd204-deframer-b-bank-id",
			 &phy->talInit.jesd204Settings.deframerB.bankId, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-device-id",
			 &phy->talInit.jesd204Settings.deframerB.deviceId, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-lane0-id",
			 &phy->talInit.jesd204Settings.deframerB.lane0Id, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-m",
			 &phy->talInit.jesd204Settings.deframerB.M, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-k",
			 &phy->talInit.jesd204Settings.deframerB.K, 32);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-scramble",
			 &phy->talInit.jesd204Settings.deframerB.scramble, 1);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-external-sysref",
			 &phy->talInit.jesd204Settings.deframerB.externalSysref, 1);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-deserializer-lanes-enabled",
			 &phy->talInit.jesd204Settings.deframerB.deserializerLanesEnabled, 0x00);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-deserializer-lane-crossbar",
			 &phy->talInit.jesd204Settings.deframerB.deserializerLaneCrossbar, 0xE4);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-lmfc-offset",
			 &phy->talInit.jesd204Settings.deframerB.lmfcOffset, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-new-sysref-on-relink",
			 &phy->talInit.jesd204Settings.deframerB.newSysrefOnRelink, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-syncb-out-select",
			 &phy->talInit.jesd204Settings.deframerB.syncbOutSelect, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-np",
			 &phy->talInit.jesd204Settings.deframerB.Np, 16);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-syncb-out-lvds-mode",
			 &phy->talInit.jesd204Settings.deframerB.syncbOutLvdsMode, 1);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-syncb-out-lvds-pn-invert",
			 &phy->talInit.jesd204Settings.deframerB.syncbOutLvdsPnInvert, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-syncb-out-cmos-slew-rate",
			 &phy->talInit.jesd204Settings.deframerB.syncbOutCmosSlewRate, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-syncb-out-cmos-drive-level",
			 &phy->talInit.jesd204Settings.deframerB.syncbOutCmosDriveLevel, 0);
	ADRV9009_OF_PROP("adi,jesd204-deframer-b-enable-manual-lane-xbar",
			 &phy->talInit.jesd204Settings.deframerB.enableManualLaneXbar, 0);


	ADRV9009_OF_PROP("adi,jesd204-ser-amplitude",
			 &phy->talInit.jesd204Settings.serAmplitude, 15);
	ADRV9009_OF_PROP("adi,jesd204-ser-pre-emphasis",
			 &phy->talInit.jesd204Settings.serPreEmphasis, 1);
	ADRV9009_OF_PROP("adi,jesd204-ser-invert-lane-polarity",
			 &phy->talInit.jesd204Settings.serInvertLanePolarity, 0);
	ADRV9009_OF_PROP("adi,jesd204-des-invert-lane-polarity",
			 &phy->talInit.jesd204Settings.desInvertLanePolarity, 0);
	ADRV9009_OF_PROP("adi,jesd204-des-eq-setting",
			 &phy->talInit.jesd204Settings.desEqSetting, 1);
	ADRV9009_OF_PROP("adi,jesd204-sysref-lvds-mode",
			 &phy->talInit.jesd204Settings.sysrefLvdsMode, 1);
	ADRV9009_OF_PROP("adi,jesd204-sysref-lvds-pn-invert",
			 &phy->talInit.jesd204Settings.sysrefLvdsPnInvert, 0);


	ADRV9009_OF_PROP("adi,arm-gpio-config-orx1-tx-sel0-pin-gpio-pin-sel",
			 &phy->arm_gpio_config.orx1TxSel0Pin.gpioPinSel, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx1-tx-sel0-pin-polarity",
			 &phy->arm_gpio_config.orx1TxSel0Pin.polarity, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx1-tx-sel0-pin-enable",
			 &phy->arm_gpio_config.orx1TxSel0Pin.enable, 0);

	ADRV9009_OF_PROP("adi,arm-gpio-config-orx1-tx-sel1-pin-gpio-pin-sel",
			 &phy->arm_gpio_config.orx1TxSel1Pin.gpioPinSel, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx1-tx-sel1-pin-polarity",
			 &phy->arm_gpio_config.orx1TxSel1Pin.polarity, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx1-tx-sel1-pin-enable",
			 &phy->arm_gpio_config.orx1TxSel1Pin.enable, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx2-tx-sel0-pin-gpio-pin-sel",
			 &phy->arm_gpio_config.orx2TxSel0Pin.gpioPinSel, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx2-tx-sel0-pin-polarity",
			 &phy->arm_gpio_config.orx2TxSel0Pin.polarity, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx2-tx-sel0-pin-enable",
			 &phy->arm_gpio_config.orx2TxSel0Pin.enable, 0);

	ADRV9009_OF_PROP("adi,arm-gpio-config-orx2-tx-sel1-pin-gpio-pin-sel",
			 &phy->arm_gpio_config.orx2TxSel1Pin.gpioPinSel, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx2-tx-sel1-pin-polarity",
			 &phy->arm_gpio_config.orx2TxSel1Pin.polarity, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-orx2-tx-sel1-pin-enable",
			 &phy->arm_gpio_config.orx2TxSel1Pin.enable, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-en-tx-tracking-cals-gpio-pin-sel",
			 &phy->arm_gpio_config.enTxTrackingCals.gpioPinSel, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-en-tx-tracking-cals-polarity",
			 &phy->arm_gpio_config.enTxTrackingCals.polarity, 0);
	ADRV9009_OF_PROP("adi,arm-gpio-config-en-tx-tracking-cals-enable",
			 &phy->arm_gpio_config.enTxTrackingCals.enable, 0);


	ADRV9009_OF_PROP("adi,orx-lo-cfg-disable-aux-pll-relocking",
			 &phy->orx_lo_cfg.disableAuxPllRelocking, 0);
	ADRV9009_OF_PROP("adi,orx-lo-cfg-gpio-select", &phy->orx_lo_cfg.gpioSelect, 19);


	ADRV9009_OF_PROP("adi,fhm-config-fhm-gpio-pin", &phy->fhm_config.fhmGpioPin, 0);
	ADRV9009_OF_PROP("adi,fhm-config-fhm-min-freq_mhz",
			 &phy->fhm_config.fhmMinFreq_MHz, 100);
	ADRV9009_OF_PROP("adi,fhm-config-fhm-max-freq_mhz",
			 &phy->fhm_config.fhmMaxFreq_MHz, 100);

	ADRV9009_OF_PROP("adi,fhm-mode-fhm-enable", &phy->fhm_mode.fhmEnable, 0);
	ADRV9009_OF_PROP("adi,fhm-mode-enable-mcs-sync", &phy->fhm_mode.enableMcsSync,
			 0);
	ADRV9009_OF_PROP("adi,fhm-mode-fhm-trigger-mode", &phy->fhm_mode.fhmTriggerMode,
			 0);
	ADRV9009_OF_PROP("adi,fhm-mode-fhm-exit-mode", &phy->fhm_mode.fhmExitMode, 0);
	ADRV9009_OF_PROP("adi,fhm-mode-fhm-init-frequency_hz",
			 &phy->fhm_mode.fhmInitFrequency_Hz, 2450000000ULL);

	ADRV9009_OF_PROP("adi,rx1-gain-ctrl-pin-inc-step",
			 &phy->rx1_gain_ctrl_pin.incStep, 1);
	ADRV9009_OF_PROP("adi,rx1-gain-ctrl-pin-dec-step",
			 &phy->rx1_gain_ctrl_pin.decStep, 1);
	ADRV9009_OF_PROP("adi,rx1-gain-ctrl-pin-rx-gain-inc-pin",
			 &phy->rx1_gain_ctrl_pin.rxGainIncPin, 0);
	ADRV9009_OF_PROP("adi,rx1-gain-ctrl-pin-rx-gain-dec-pin",
			 &phy->rx1_gain_ctrl_pin.rxGainDecPin, 1);
	ADRV9009_OF_PROP("adi,rx1-gain-ctrl-pin-enable", &phy->rx1_gain_ctrl_pin.enable,
			 0);

	ADRV9009_OF_PROP("adi,rx2-gain-ctrl-pin-inc-step",
			 &phy->rx2_gain_ctrl_pin.incStep, 1);
	ADRV9009_OF_PROP("adi,rx2-gain-ctrl-pin-dec-step",
			 &phy->rx2_gain_ctrl_pin.decStep, 1);
	ADRV9009_OF_PROP("adi,rx2-gain-ctrl-pin-rx-gain-inc-pin",
			 &phy->rx2_gain_ctrl_pin.rxGainIncPin, 3);
	ADRV9009_OF_PROP("adi,rx2-gain-ctrl-pin-rx-gain-dec-pin",
			 &phy->rx2_gain_ctrl_pin.rxGainDecPin, 4);
	ADRV9009_OF_PROP("adi,rx2-gain-ctrl-pin-enable", &phy->rx2_gain_ctrl_pin.enable,
			 0);

	ADRV9009_OF_PROP("adi,tx1-atten-ctrl-pin-step-size",
			 &phy->tx1_atten_ctrl_pin.stepSize, 0);
	ADRV9009_OF_PROP("adi,tx1-atten-ctrl-pin-tx-atten-inc-pin",
			 &phy->tx1_atten_ctrl_pin.txAttenIncPin, 4);
	ADRV9009_OF_PROP("adi,tx1-atten-ctrl-pin-tx-atten-dec-pin",
			 &phy->tx1_atten_ctrl_pin.txAttenDecPin, 5);
	ADRV9009_OF_PROP("adi,tx1-atten-ctrl-pin-enable",
			 &phy->tx1_atten_ctrl_pin.enable, 0);

	ADRV9009_OF_PROP("adi,tx2-atten-ctrl-pin-step-size",
			 &phy->tx2_atten_ctrl_pin.stepSize, 0);
	ADRV9009_OF_PROP("adi,tx2-atten-ctrl-pin-tx-atten-inc-pin",
			 &phy->tx2_atten_ctrl_pin.txAttenIncPin, 6);
	ADRV9009_OF_PROP("adi,tx2-atten-ctrl-pin-tx-atten-dec-pin",
			 &phy->tx2_atten_ctrl_pin.txAttenDecPin, 7);
	ADRV9009_OF_PROP("adi,tx2-atten-ctrl-pin-enable",
			 &phy->tx2_atten_ctrl_pin.enable, 0);

	ADRV9009_OF_PROP("adi,tx-pa-protection-avg-duration",
			 &phy->tx_pa_protection.avgDuration, 3);
	ADRV9009_OF_PROP("adi,tx-pa-protection-tx-atten-step",
			 &phy->tx_pa_protection.txAttenStep, 2);
	ADRV9009_OF_PROP("adi,tx-pa-protection-tx1-power-threshold",
			 &phy->tx_pa_protection.tx1PowerThreshold, 4096);
	ADRV9009_OF_PROP("adi,tx-pa-protection-tx2-power-threshold",
			 &phy->tx_pa_protection.tx2PowerThreshold, 4096);
	ADRV9009_OF_PROP("adi,tx-pa-protection-peak-count",
			 &phy->tx_pa_protection.peakCount, 4);
	ADRV9009_OF_PROP("adi,tx-pa-protection-tx1-peak-threshold",
			 &phy->tx_pa_protection.tx1PeakThreshold, 128);
	ADRV9009_OF_PROP("adi,tx-pa-protection-tx2-peak-threshold",
			 &phy->tx_pa_protection.tx2PeakThreshold, 128);


	ADRV9009_GET_FIR("adi,rx-profile-rx-fir", phy->talInit.rx.rxProfile.rxFir,
			 phy->rxFirCoefs);

	ADRV9009_OF_PROP("adi,rx-profile-rx-fir-decimation",
			 &phy->talInit.rx.rxProfile.rxFirDecimation, 2);
	ADRV9009_OF_PROP("adi,rx-profile-rx-dec5-decimation",
			 &phy->talInit.rx.rxProfile.rxDec5Decimation, 4);
	ADRV9009_OF_PROP("adi,rx-profile-rhb1-decimation",
			 &phy->talInit.rx.rxProfile.rhb1Decimation, 1);
	ADRV9009_OF_PROP("adi,rx-profile-rx-output-rate_khz",
			 &phy->talInit.rx.rxProfile.rxOutputRate_kHz, 245760);
	ADRV9009_OF_PROP("adi,rx-profile-rf-bandwidth_hz",
			 &phy->talInit.rx.rxProfile.rfBandwidth_Hz, 200000000);
	ADRV9009_OF_PROP("adi,rx-profile-rx-bbf3d-bcorner_khz",
			 &phy->talInit.rx.rxProfile.rxBbf3dBCorner_kHz, 200000);

	ADRV9009_GET_PROFILE("adi,rx-profile-rx-adc-profile",
			     phy->talInit.rx.rxProfile.rxAdcProfile);
	ADRV9009_OF_PROP("adi,rx-profile-rx-ddc-mode",
			 &phy->talInit.rx.rxProfile.rxDdcMode, 0);

	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-a-input-band-width_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandAInputBandWidth_kHz, 0);
	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-a-input-center-freq_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandAInputCenterFreq_kHz, 0);
	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-a-nco1-freq_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandANco1Freq_kHz, 0);
	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-a-nco2-freq_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandANco2Freq_kHz, 0);
	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-binput-band-width_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandBInputBandWidth_kHz, 0);
	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-binput-center-freq_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandBInputCenterFreq_kHz, 0);
	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-bnco1-freq_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandBNco1Freq_kHz, 0);
	ADRV9009_OF_PROP("adi,rx-nco-shifter-band-bnco2-freq_khz",
			 &phy->talInit.rx.rxProfile.rxNcoShifterCfg.bandBNco2Freq_kHz, 0);

	ADRV9009_OF_PROP("adi,rx-gain-control-gain-mode",
			 &phy->talInit.rx.rxGainCtrl.gainMode, 0);
	ADRV9009_OF_PROP("adi,rx-gain-control-rx1-gain-index",
			 &phy->talInit.rx.rxGainCtrl.rx1GainIndex, 255);
	ADRV9009_OF_PROP("adi,rx-gain-control-rx2-gain-index",
			 &phy->talInit.rx.rxGainCtrl.rx2GainIndex, 255);
	ADRV9009_OF_PROP("adi,rx-gain-control-rx1-max-gain-index",
			 &phy->talInit.rx.rxGainCtrl.rx1MaxGainIndex, 255);
	ADRV9009_OF_PROP("adi,rx-gain-control-rx1-min-gain-index",
			 &phy->talInit.rx.rxGainCtrl.rx1MinGainIndex, 195);
	ADRV9009_OF_PROP("adi,rx-gain-control-rx2-max-gain-index",
			 &phy->talInit.rx.rxGainCtrl.rx2MaxGainIndex, 255);
	ADRV9009_OF_PROP("adi,rx-gain-control-rx2-min-gain-index",
			 &phy->talInit.rx.rxGainCtrl.rx2MinGainIndex, 195);

	ADRV9009_OF_PROP("adi,rx-settings-framer-sel", &phy->talInit.rx.framerSel, 0);
	ADRV9009_OF_PROP("adi,rx-settings-rx-channels", &phy->talInit.rx.rxChannels, 3);


	ADRV9009_GET_FIR("adi,orx-profile-rx-fir", phy->talInit.obsRx.orxProfile.rxFir,
			 phy->obsrxFirCoefs);

	ADRV9009_OF_PROP("adi,orx-profile-rx-fir-decimation",
			 &phy->talInit.obsRx.orxProfile.rxFirDecimation, 1);
	ADRV9009_OF_PROP("adi,orx-profile-rx-dec5-decimation",
			 &phy->talInit.obsRx.orxProfile.rxDec5Decimation, 4);
	ADRV9009_OF_PROP("adi,orx-profile-rhb1-decimation",
			 &phy->talInit.obsRx.orxProfile.rhb1Decimation, 2);
	ADRV9009_OF_PROP("adi,orx-profile-orx-output-rate_khz",
			 &phy->talInit.obsRx.orxProfile.orxOutputRate_kHz, 245760);
	ADRV9009_OF_PROP("adi,orx-profile-rf-bandwidth_hz",
			 &phy->talInit.obsRx.orxProfile.rfBandwidth_Hz, 200000000);
	ADRV9009_OF_PROP("adi,orx-profile-rx-bbf3d-bcorner_khz",
			 &phy->talInit.obsRx.orxProfile.rxBbf3dBCorner_kHz, 225000);
	ADRV9009_GET_PROFILE("adi,orx-profile-orx-low-pass-adc-profile",
			     phy->talInit.obsRx.orxProfile.orxLowPassAdcProfile);
	ADRV9009_GET_PROFILE("adi,orx-profile-orx-band-pass-adc-profile",
			     phy->talInit.obsRx.orxProfile.orxBandPassAdcProfile);
	ADRV9009_OF_PROP("adi,orx-profile-orx-ddc-mode",
			 &phy->talInit.obsRx.orxProfile.orxDdcMode, 0);
	ADRV9009_GET_PROFILE("adi,orx-profile-orx-merge-filter",
			     phy->talInit.obsRx.orxProfile.orxMergeFilter);


	ADRV9009_OF_PROP("adi,orx-gain-control-gain-mode",
			 &phy->talInit.obsRx.orxGainCtrl.gainMode, 0);
	ADRV9009_OF_PROP("adi,orx-gain-control-orx1-gain-index",
			 &phy->talInit.obsRx.orxGainCtrl.orx1GainIndex, 255);
	ADRV9009_OF_PROP("adi,orx-gain-control-orx2-gain-index",
			 &phy->talInit.obsRx.orxGainCtrl.orx2GainIndex, 255);
	ADRV9009_OF_PROP("adi,orx-gain-control-orx1-max-gain-index",
			 &phy->talInit.obsRx.orxGainCtrl.orx1MaxGainIndex, 255);
	ADRV9009_OF_PROP("adi,orx-gain-control-orx1-min-gain-index",
			 &phy->talInit.obsRx.orxGainCtrl.orx1MinGainIndex, 195);
	ADRV9009_OF_PROP("adi,orx-gain-control-orx2-max-gain-index",
			 &phy->talInit.obsRx.orxGainCtrl.orx2MaxGainIndex, 255);
	ADRV9009_OF_PROP("adi,orx-gain-control-orx2-min-gain-index",
			 &phy->talInit.obsRx.orxGainCtrl.orx2MinGainIndex, 195);


	ADRV9009_OF_PROP("adi,obs-settings-framer-sel", &phy->talInit.obsRx.framerSel,
			 1);
	ADRV9009_OF_PROP("adi,obs-settings-obs-rx-channels-enable",
			 &phy->talInit.obsRx.obsRxChannelsEnable, 3);
	ADRV9009_OF_PROP("adi,obs-settings-obs-rx-lo-source",
			 &phy->talInit.obsRx.obsRxLoSource, 0);


	ADRV9009_GET_FIR("adi,tx-profile-tx-fir", phy->talInit.tx.txProfile.txFir,
			 phy->txFirCoefs);

	ADRV9009_OF_PROP("adi,tx-profile-dac-div", &phy->talInit.tx.txProfile.dacDiv,
			 1);

	ADRV9009_OF_PROP("adi,tx-profile-tx-fir-interpolation",
			 &phy->talInit.tx.txProfile.txFirInterpolation, 1);
	ADRV9009_OF_PROP("adi,tx-profile-thb1-interpolation",
			 &phy->talInit.tx.txProfile.thb1Interpolation, 2);
	ADRV9009_OF_PROP("adi,tx-profile-thb2-interpolation",
			 &phy->talInit.tx.txProfile.thb2Interpolation, 2);
	ADRV9009_OF_PROP("adi,tx-profile-thb3-interpolation",
			 &phy->talInit.tx.txProfile.thb3Interpolation, 2);
	ADRV9009_OF_PROP("adi,tx-profile-tx-int5-interpolation",
			 &phy->talInit.tx.txProfile.txInt5Interpolation, 1);
	ADRV9009_OF_PROP("adi,tx-profile-tx-input-rate_khz",
			 &phy->talInit.tx.txProfile.txInputRate_kHz, 245760);
	ADRV9009_OF_PROP("adi,tx-profile-primary-sig-bandwidth_hz",
			 &phy->talInit.tx.txProfile.primarySigBandwidth_Hz, 100000000);
	ADRV9009_OF_PROP("adi,tx-profile-rf-bandwidth_hz",
			 &phy->talInit.tx.txProfile.rfBandwidth_Hz, 225000000);
	ADRV9009_OF_PROP("adi,tx-profile-tx-dac3d-bcorner_khz",
			 &phy->talInit.tx.txProfile.txDac3dBCorner_kHz, 225000);
	ADRV9009_OF_PROP("adi,tx-profile-tx-bbf3d-bcorner_khz",
			 &phy->talInit.tx.txProfile.txBbf3dBCorner_kHz, 113000);
	ADRV9009_GET_PROFILE("adi,tx-profile-loop-back-adc-profile",
			     phy->talInit.tx.txProfile.loopBackAdcProfile);


	ADRV9009_OF_PROP("adi,tx-settings-deframer-sel", &phy->talInit.tx.deframerSel,
			 0);
	ADRV9009_OF_PROP("adi,tx-settings-tx-channels", &phy->talInit.tx.txChannels, 3);
	ADRV9009_OF_PROP("adi,tx-settings-tx-atten-step-size",
			 &phy->talInit.tx.txAttenStepSize, 0);
	ADRV9009_OF_PROP("adi,tx-settings-tx1-atten_md-b",
			 &phy->talInit.tx.tx1Atten_mdB, 10000);
	ADRV9009_OF_PROP("adi,tx-settings-tx2-atten_md-b",
			 &phy->talInit.tx.tx2Atten_mdB, 10000);
	ADRV9009_OF_PROP("adi,tx-settings-dis-tx-data-if-pll-unlock",
			 &phy->talInit.tx.disTxDataIfPllUnlock, 0);

	ADRV9009_OF_PROP("adi,dig-clocks-device-clock_khz",
			 &phy->talInit.clocks.deviceClock_kHz, 245760);
	ADRV9009_OF_PROP("adi,dig-clocks-clk-pll-vco-freq_khz",
			 &phy->talInit.clocks.clkPllVcoFreq_kHz, 9830400);
	ADRV9009_OF_PROP("adi,dig-clocks-clk-pll-hs-div",
			 &phy->talInit.clocks.clkPllHsDiv, 1);
	ADRV9009_OF_PROP("adi,dig-clocks-rf-pll-use-external-lo",
			 &phy->talInit.clocks.rfPllUseExternalLo, 0);
	ADRV9009_OF_PROP("adi,dig-clocks-rf-pll-phase-sync-mode",
			 &phy->talInit.clocks.rfPllPhaseSyncMode, 0);


	ADRV9009_OF_PROP("adi,trx-pll-lo-frequency_hz", &phy->trx_lo_frequency,
			 2400000000ULL);
	ADRV9009_OF_PROP("adi,aux-pll-lo-frequency_hz", &phy->aux_lo_frequency,
			 2500000000ULL);


	phy->loopFilter_stability = 3;

	return 0;
}

static int adrv9009_parse_profile(struct adrv9009_rf_phy *phy,
				  char *data, u32 size)
{

	taliseFir_t *fir = NULL;
	struct device *dev = &phy->spi->dev;
	char clocks = 0, tx = 0, rx = 0, obs = 0, lpbk = 0, rxncoshiftercfg = 0,
	     filter = 0, adcprof = 0, header = 0, orxmergefilter = 0,
	     orxlowpassadcprofile = 0, orxbandpassadcprofile = 0, lpbkadcprofile = 0;

	char *line, *ptr = data;
	unsigned int int32, int32_2;
	int ret, num = 0, version = 0, max, sint32, retval;

#define GET_TOKEN(x, n) \
		{ret = sscanf(line, " <" #n "=%u>", &int32);\
		if (ret == 1) { \
			x.n = int32;\
			continue;\
		}}

#define GET_STOKEN(x, n) \
		{ret = sscanf(line, " <" #n "=%d>", &sint32);\
		if (ret == 1) { \
			x.n = sint32;\
			continue;\
		}}

#define SKIP_TOKEN(x, n) \
		{ret = sscanf(line, " <" #n "=%u>", &int32);\
		if (ret == 1) { \
			continue;\
		}}

	while ((line = strsep(&ptr, "\n"))) {
		if (line >= data + size)
			break;

		line = skip_spaces(line);

		if (line[0] == '#' || line[0] == '\r' ||  line[0] == '\0')
			continue;

		if (!header && strstr(line, "<profile Talise")) {
			ret = sscanf(line, " <profile Talise version=%d", &version);

			if (ret == 1 && version == 1)
				header = 1;
			else
				dev_err(dev, "%s: Invalid Version %d",
					__func__, version);

			continue;
		}

		if (!clocks && strstr(line, "<clocks>")) {
			clocks = 1;
			continue;
		}

		if (clocks && strstr(line, "</clocks>")) {
			clocks = 0;
			retval = 0;
			continue;
		}


		if (!lpbk && strstr(line, "<lpbk>")) {
			lpbk = 1;
			continue;
		}

		if (lpbk && strstr(line, "</lpbk>")) {
			lpbk = 0;
			continue;
		}

		if (!rx && strstr(line, "<rx ")) {
			rx = 1;
			fir = &phy->talInit.rx.rxProfile.rxFir;
			continue;
		}

		if (rx && strstr(line, "</rx>")) {
			rx = 0;
			continue;
		}

		if (!obs && strstr(line, "<obsRx ")) {
			obs = 1;
			fir = &phy->talInit.obsRx.orxProfile.rxFir;
			continue;
		}

		if (obs && strstr(line, "</obsRx>")) {
			obs = 0;
			continue;
		}

		if (!tx && strstr(line, "<tx ")) {
			tx = 1;
			fir = &phy->talInit.tx.txProfile.txFir;
			continue;
		}

		if (tx && strstr(line, "</tx>")) {
			tx = 0;
			continue;
		}

		if (rx && !rxncoshiftercfg && strstr(line, "<rxNcoShifterCfg>")) {
			rxncoshiftercfg = 1;
			continue;
		}

		if (rx && rxncoshiftercfg && strstr(line, "</rxNcoShifterCfg>")) {
			rxncoshiftercfg = 0;
			continue;
		}

		if (!filter &&
		    (sscanf(line, " <filter FIR gain_dB=%d numFirCoefs=%d>", &ret, &max) == 2)) {
			fir->gain_dB = ret;
			filter = 1;
			num = 0;
			continue;
		}

		if (filter && strstr(line, "</filter>")) {
			filter = 0;
			fir->numFirCoefs = num;
			continue;
		}

		if (rx && !adcprof && (sscanf(line, " <rxAdcProfile num=%d>", &max) == 1)) {
			adcprof = 1;
			num = 0;
			continue;
		}

		if (adcprof && strstr(line, "</rxAdcProfile>")) {
			adcprof = 0;
			if (num != 42)
				dev_err(dev, "%s:%d: Invalid number (%d) of coefficients",
					__func__, __LINE__, num);

			num = 0;
			continue;
		}

		if (obs && !orxlowpassadcprofile &&
		    (sscanf(line, " <orxLowPassAdcProfile num=%d>", &max) == 1)) {
			orxlowpassadcprofile = 1;
			num = 0;
			continue;
		}

		if (orxlowpassadcprofile && strstr(line, "</orxLowPassAdcProfile>")) {
			orxlowpassadcprofile = 0;
			if (num != 42)
				dev_err(dev, "%s:%d: Invalid number (%d) of coefficients",
					__func__, __LINE__, num);

			num = 0;
			continue;
		}

		if (obs && !orxbandpassadcprofile &&
		    (sscanf(line, " <orxBandPassAdcProfile num=%d>", &max) == 1)) {
			orxbandpassadcprofile = 1;
			num = 0;
			continue;
		}

		if (orxbandpassadcprofile && strstr(line, "</orxBandPassAdcProfile>")) {
			orxbandpassadcprofile = 0;
			if (num != 42)
				dev_err(dev, "%s:%d: Invalid number (%d) of coefficients",
					__func__, __LINE__, num);
			num = 0;
			continue;
		}

		if (obs && !orxmergefilter &&
		    (sscanf(line, " <orxMergeFilter num=%d>", &max) == 1)) {
			orxmergefilter = 1;
			num = 0;
			continue;
		}

		if (orxmergefilter && strstr(line, "</orxMergeFilter>")) {
			orxmergefilter = 0;
			if (num != 13)
				dev_err(dev, "%s:%d: Invalid number (%d) of coefficients",
					__func__, __LINE__, num);
			num = 0;
			continue;
		}

		if (lpbk && !lpbkadcprofile &&
		    (sscanf(line, " <lpbkAdcProfile num=%d>", &max) == 1)) {
			lpbkadcprofile = 1;
			num = 0;
			continue;
		}

		if (lpbkadcprofile && strstr(line, "</lpbkAdcProfile>")) {
			lpbkadcprofile = 0;
			if (num != 42)
				dev_err(dev, "%s:%d: Invalid number (%d) of coefficients",
					__func__, __LINE__, num);
			num = 0;
			continue;
		}

		if (clocks) {
			GET_TOKEN(phy->talInit.clocks, deviceClock_kHz);
			GET_TOKEN(phy->talInit.clocks, clkPllVcoFreq_kHz);
			ret = sscanf(line, " <clkPllHsDiv=%u.%u>", &int32, &int32_2);
			if (ret > 0) {
				if (ret == 1) {
					switch (int32) {
					case 2:
						num = TAL_HSDIV_2;
						break;
					case 3:
						num = TAL_HSDIV_3;
						break;
					case 4:
						num = TAL_HSDIV_4;
						break;
					case 5:
						num = TAL_HSDIV_5;
						break;
					default:
						dev_err(dev, "%s:%d: Invalid number (%d)",
							__func__, __LINE__, int32);
					}
				} else if (ret == 2) {
					if (int32 == 2 && int32_2 == 5)
						num = TAL_HSDIV_2P5;
					else
						dev_err(dev, "%s:%d: Invalid number (%d.%d)",
							__func__, __LINE__, int32, int32_2);
				}
				phy->talInit.clocks.clkPllHsDiv = num;
				continue;
			}
		}

		if (rx && !filter && !adcprof) {
			if (rxncoshiftercfg) {
				GET_TOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandAInputBandWidth_kHz);
				GET_STOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandAInputCenterFreq_kHz);
				GET_STOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandANco1Freq_kHz);
				GET_STOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandANco2Freq_kHz);
				GET_TOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandBInputBandWidth_kHz);
				GET_STOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandBInputCenterFreq_kHz);
				GET_STOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandBNco1Freq_kHz);
				GET_STOKEN(phy->talInit.rx.rxProfile.rxNcoShifterCfg, bandBNco2Freq_kHz);
			} else {
				GET_TOKEN(phy->talInit.rx.rxProfile, rxFirDecimation);
				GET_TOKEN(phy->talInit.rx.rxProfile, rxDec5Decimation);
				GET_TOKEN(phy->talInit.rx.rxProfile, rhb1Decimation);
				GET_TOKEN(phy->talInit.rx.rxProfile, rxOutputRate_kHz);
				GET_TOKEN(phy->talInit.rx.rxProfile, rfBandwidth_Hz);
				GET_TOKEN(phy->talInit.rx.rxProfile, rxBbf3dBCorner_kHz);
				GET_TOKEN(phy->talInit.rx.rxProfile, rxDdcMode);
			}
		}

		if (obs && !filter && !orxlowpassadcprofile && !orxbandpassadcprofile &&
		    !orxmergefilter) {
			SKIP_TOKEN(phy->talInit.obsRx.orxProfile, enAdcStitching);
			GET_TOKEN(phy->talInit.obsRx.orxProfile, rxFirDecimation);
			GET_TOKEN(phy->talInit.obsRx.orxProfile, rxDec5Decimation);
			GET_TOKEN(phy->talInit.obsRx.orxProfile, rhb1Decimation);
			GET_TOKEN(phy->talInit.obsRx.orxProfile, orxOutputRate_kHz);
			GET_TOKEN(phy->talInit.obsRx.orxProfile, rfBandwidth_Hz);
			GET_TOKEN(phy->talInit.obsRx.orxProfile, rxBbf3dBCorner_kHz);
			GET_TOKEN(phy->talInit.obsRx.orxProfile, orxDdcMode);
		}


		if (tx && !filter) {
			GET_TOKEN(phy->talInit.tx.txProfile, dacDiv);
			GET_TOKEN(phy->talInit.tx.txProfile, txFirInterpolation);
			GET_TOKEN(phy->talInit.tx.txProfile, thb1Interpolation);
			GET_TOKEN(phy->talInit.tx.txProfile, thb2Interpolation);
			GET_TOKEN(phy->talInit.tx.txProfile, thb3Interpolation);
			GET_TOKEN(phy->talInit.tx.txProfile, txInt5Interpolation);
			GET_TOKEN(phy->talInit.tx.txProfile, txInputRate_kHz);
			GET_TOKEN(phy->talInit.tx.txProfile, primarySigBandwidth_Hz);
			GET_TOKEN(phy->talInit.tx.txProfile, rfBandwidth_Hz);
			GET_TOKEN(phy->talInit.tx.txProfile, txDac3dBCorner_kHz);
			GET_TOKEN(phy->talInit.tx.txProfile, txBbf3dBCorner_kHz);
		}

		if (filter) {
			if (sscanf(line, " %d", &ret) == 1) {
				if (num >= max)
					return -EINVAL;

				fir->coefs[num++] = ret;
				continue;
			}
		}

		if (adcprof && rx) {
			if (sscanf(line, " %d", &ret) == 1) {
				if (num >= ARRAY_SIZE(phy->talInit.rx.rxProfile.rxAdcProfile) || num > max)
					return -EINVAL;

				phy->talInit.rx.rxProfile.rxAdcProfile[num++] = ret;
				continue;
			}
		}

		if (orxlowpassadcprofile && obs) {
			if (sscanf(line, " %d", &ret) == 1) {
				if (num >= ARRAY_SIZE(phy->talInit.obsRx.orxProfile.orxLowPassAdcProfile) ||
				    num > max)
					return -EINVAL;

				phy->talInit.obsRx.orxProfile.orxLowPassAdcProfile[num++] = ret;
				continue;
			}
		}


		if (orxbandpassadcprofile && obs) {
			if (sscanf(line, " %d", &ret) == 1) {
				if (num >= ARRAY_SIZE(phy->talInit.obsRx.orxProfile.orxBandPassAdcProfile) ||
				    num > max)
					return -EINVAL;

				phy->talInit.obsRx.orxProfile.orxBandPassAdcProfile[num++] = ret;
				continue;
			}
		}

		if (orxmergefilter && obs) {
			if (sscanf(line, " %d", &ret) == 1) {
				if (num >= ARRAY_SIZE(phy->talInit.obsRx.orxProfile.orxMergeFilter) ||
				    num > max)
					continue;

				phy->talInit.obsRx.orxProfile.orxMergeFilter[num++] = ret;
				continue;
			}
		}


		if (lpbkadcprofile && lpbk) {
			if (sscanf(line, " %d", &ret) == 1) {
				if (num >= ARRAY_SIZE(phy->talInit.tx.txProfile.loopBackAdcProfile) ||
				    num > max)
					return -EINVAL;

				phy->talInit.tx.txProfile.loopBackAdcProfile[num++] = ret;
				continue;
			}
		}

		if (lpbk)
			continue; /* Skip unused entries in node <lpbk> */

		if (header && strstr(line, "</profile>"))
			return retval;

		/* We should never end up here */
		dev_err(dev, "%s: Malformed profile entry was %s",
			__func__, line);

		return -EINVAL;

	}

	return -EINVAL;
}

static ssize_t
adrv9009_profile_bin_write(struct file *filp, struct kobject *kobj,
			   struct bin_attribute *bin_attr,
			   char *buf, loff_t off, size_t count)
{

	struct iio_dev *indio_dev = dev_to_iio_dev(kobj_to_dev(kobj));
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	int ret;

	if (off == 0) {
		if (phy->bin_attr_buf == NULL) {
			phy->bin_attr_buf = devm_kzalloc(&phy->spi->dev,
							 bin_attr->size, GFP_KERNEL);
			if (!phy->bin_attr_buf)
				return -ENOMEM;
		} else
			memset(phy->bin_attr_buf, 0, bin_attr->size);
	}

	memcpy(phy->bin_attr_buf + off, buf, count);

	if (strnstr(phy->bin_attr_buf, "</profile>", off + count) == NULL)
		return count;

	ret = adrv9009_parse_profile(phy, phy->bin_attr_buf, off + count);
	if (ret < 0)
		return ret;


	mutex_lock(&phy->indio_dev->mlock);

	adrv9009_shutdown(phy);

	ret = adrv9009_setup(phy);
	if (ret)
		ret = adrv9009_setup(phy);

	mutex_unlock(&phy->indio_dev->mlock);

	return (ret < 0) ? ret : count;
}

static ssize_t
adrv9009_profile_bin_read(struct file *filp, struct kobject *kobj,
			  struct bin_attribute *bin_attr,
			  char *buf, loff_t off, size_t count)
{
	if (off)
		return 0;

	return sprintf(buf, "TBD");
}

static void adrv9009_free_gt(struct adrv9009_rf_phy *phy,
			     struct gain_table_info *table)
{
	int i;

	if (!table)
		return;

	for (i = RX1_GT; i <= ORX_RX1_RX2_GT; i++)
		if (table[i].abs_gain_tbl) {
			devm_kfree(&phy->spi->dev, table[i].abs_gain_tbl);
			table[i].abs_gain_tbl = NULL;
			table[i].dest = 0;
		}
}

static int adrv9009_load_all_gt(struct adrv9009_rf_phy *phy,
				struct gain_table_info *table)
{
	int i, ret;

	if (!table)
		return -ENODEV;

	for (i = RX1_GT; i <= RX1_RX2_GT; i++)
		if (table[i].dest && table[i].abs_gain_tbl) {
			ret = TALISE_programRxGainTable(phy->talDevice, table[i].gainTablePtr,
							table[i].max_index, table[i].dest);
			if (ret < 0)
				return ret;
		}

	for (i = ORX_RX1_GT; i <= ORX_RX1_RX2_GT; i++)
		if (table[i].dest && table[i].abs_gain_tbl) {
			ret = TALISE_programOrxGainTable(phy->talDevice, table[i].orx_gainTablePtr,
							 table[i].max_index, table[i].dest);
			if (ret < 0)
				return ret;
		}

	return 0;
}

static struct gain_table_info *adrv9009_parse_gt(struct adrv9009_rf_phy *phy,
		char *data, u32 size)
{
	struct gain_table_info *table = phy->gt_info;
	bool header_found;
	int i = 0, ret, dest, table_num = 0;
	char *line, *ptr = data;
	u8 *p;
	taliseOrxGainTable_t *gainTablePtr;

	header_found = false;

	while ((line = strsep(&ptr, "\n"))) {
		if (line >= data + size)
			break;

		if (line[0] == '#') /* skip comment lines */
			continue;

		if (strstr(line, "list>")) /* skip <[/]list> */
			continue;

		if (!header_found) {
			char type[40];
			unsigned model;
			u64 start;
			u64 end;

			ret = sscanf(line, " <gaintable AD%i type=%s dest=%i start=%lli end=%lli>",
				     &model , type, &dest, &start, &end);

			if (ret == 5) {
				if (!(model == 9009 || model == 9008)) {
					ret = -EINVAL;
					goto out;
				}
				if (start >= end) {
					ret = -EINVAL;
					goto out;
				}

				if (dest < RX1_GT || dest > ORX_RX1_RX2_GT) {
					ret = -EINVAL;
					goto out;
				}

				p = devm_kzalloc(&phy->spi->dev,
						 sizeof(s32[MAX_GAIN_TABLE_INDEX]) +
						 sizeof(gainTablePtr[MAX_GAIN_TABLE_INDEX]),
						 GFP_KERNEL);
				if (!p) {
					ret = -ENOMEM;
					goto out;
				}

				table[dest].dest = dest;
				table[dest].abs_gain_tbl = (s32 *) p;
				table[dest].gainTablePtr = (taliseRxGainTable_t *)(p +
							   sizeof(s32[MAX_GAIN_TABLE_INDEX]));
				table[dest].orx_gainTablePtr = (taliseOrxGainTable_t *)(p +
							       sizeof(s32[MAX_GAIN_TABLE_INDEX]));

				table[dest].start = start;
				table[dest].end = end;

				header_found = true;
				i = 0;

				continue;
			} else
				header_found = false;
		}

		if (header_found) {
			int a, b, c, d, e, f;
			ret = sscanf(line, " %i,%i,%i,%i,%i,%i", &a, &b, &c, &d, &e, &f);
			if (((ret == 6) && (dest <= RX1_RX2_GT)) || ((ret == 5) &&
					(dest >= ORX_RX1_GT))) {
				if (i >= MAX_GAIN_TABLE_INDEX)
					goto out;

				if ((i > 0) && (a > table[dest].abs_gain_tbl[i - 1]))
					dev_warn(&phy->spi->dev,
						 "Gain table must be monotonic");

				table[dest].abs_gain_tbl[i] = a;

				if (dest <= RX1_RX2_GT) {
					table[dest].gainTablePtr[i].rxFeGain = b;
					table[dest].gainTablePtr[i].extControl = c;
					table[dest].gainTablePtr[i].adcTiaGain = d;
					table[dest].gainTablePtr[i].digGain = e;
					table[dest].gainTablePtr[i].phaseOffset = f;
				} else {
					table[dest].orx_gainTablePtr[i].rxFeGain = b;
					table[dest].orx_gainTablePtr[i].extControl = c;
					table[dest].orx_gainTablePtr[i].adcTiaGain = d;
					table[dest].orx_gainTablePtr[i].digGain = e;
				}
				i++;
				continue;
			} else if (strstr(line, "</gaintable>")) {
				table[dest].max_index = i;
				header_found = false;
				table_num++;

				continue;
			} else {
				dev_err(&phy->spi->dev,
					"ERROR: Malformed gain table");
				goto out_free_tables;
			}
		}
	}

	dev_dbg(&phy->spi->dev, "%s: table_num %d header_found %d",
		__func__, table_num, header_found);

	if (table_num > 0 && !header_found)
		return table;
	else
		return  ERR_PTR(-EFAULT);

out_free_tables:
	adrv9009_free_gt(phy, table);

out:
	return ERR_PTR(ret);
}

static ssize_t
adrv9009_gt_bin_write(struct file *filp, struct kobject *kobj,
		      struct bin_attribute *bin_attr,
		      char *buf, loff_t off, size_t count)
{

	struct iio_dev *indio_dev = dev_to_iio_dev(kobj_to_dev(kobj));
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	struct gain_table_info *table;
	int ret;

	if (off == 0) {
		if (phy->bin_gt_attr_buf == NULL) {
			phy->bin_gt_attr_buf = devm_kzalloc(&phy->spi->dev,
							    bin_attr->size, GFP_KERNEL);
			if (!phy->bin_gt_attr_buf)
				return -ENOMEM;
		} else
			memset(phy->bin_gt_attr_buf, 0, bin_attr->size);
	}

	memcpy(phy->bin_gt_attr_buf + off, buf, count);

	if (strnstr(phy->bin_gt_attr_buf, "</list>", off + count) == NULL)
		return count;

	table = adrv9009_parse_gt(phy, phy->bin_gt_attr_buf, off + count);
	if (IS_ERR_OR_NULL(table))
		return PTR_ERR(table);

	mutex_lock(&phy->indio_dev->mlock);

	ret = adrv9009_load_all_gt(phy, table);

	mutex_unlock(&phy->indio_dev->mlock);

	return (ret < 0) ? ret : count;
}

#define ADRV9009_MAX_CLK_NAME 79

static char *adrv9009_clk_set_dev_name(struct adrv9009_rf_phy *phy,
				       char *dest, const char *name)
{
	size_t len = 0;

	if (name == NULL)
		return NULL;

	if (*name == '-')
		len = strlcpy(dest, dev_name(&phy->spi->dev),
			      ADRV9009_MAX_CLK_NAME);
	else
		*dest = '\0';

	return strncat(dest, name, ADRV9009_MAX_CLK_NAME - len);
}

static unsigned long adrv9009_bb_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct adrv9009_clock *clk_priv = to_clk_priv(hw);
	return clk_priv->rate;
}

static int adrv9009_bb_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct adrv9009_clock *clk_priv = to_clk_priv(hw);
	clk_priv->rate = rate;

	return 0;
}

static long adrv9009_bb_round_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long *prate)
{
	struct adrv9009_clock *clk_priv = to_clk_priv(hw);
	dev_dbg(&clk_priv->spi->dev, "%s: Rate %lu Hz", __func__, rate);

	return rate;
}

static const struct clk_ops bb_clk_ops = {
	.round_rate = adrv9009_bb_round_rate,
	.set_rate = adrv9009_bb_set_rate,
	.recalc_rate = adrv9009_bb_recalc_rate,
};

static int adrv9009_clk_register(struct adrv9009_rf_phy *phy,
				 const char *name, const char *parent_name,
				 const char *parent_name2, unsigned long flags,
				 u32 source)
{
	struct adrv9009_clock *clk_priv = &phy->clk_priv[source];
	struct clk_init_data init;
	struct clk *clk;
	char c_name[ADRV9009_MAX_CLK_NAME + 1], p_name[2][ADRV9009_MAX_CLK_NAME + 1];
	const char *_parent_name[2];

	/* struct adrv9009_clock assignments */
	clk_priv->source = source;
	clk_priv->hw.init = &init;
	clk_priv->spi = phy->spi;
	clk_priv->phy = phy;

	_parent_name[0] = adrv9009_clk_set_dev_name(phy, p_name[0], parent_name);
	_parent_name[1] = adrv9009_clk_set_dev_name(phy, p_name[1], parent_name2);

	init.name = adrv9009_clk_set_dev_name(phy, c_name, name);;
	init.flags = flags;
	init.parent_names = &_parent_name[0];
	init.num_parents = _parent_name[1] ? 2 : _parent_name[0] ? 1 : 0;

	switch (source) {
	case RX_SAMPL_CLK:
		init.ops = &bb_clk_ops;
		clk_priv->rate = phy->talInit.rx.rxProfile.rxOutputRate_kHz;
		break;
	case OBS_SAMPL_CLK:
		init.ops = &bb_clk_ops;
		clk_priv->rate = phy->talInit.obsRx.orxProfile.orxOutputRate_kHz;
		break;
	case TX_SAMPL_CLK:
		init.ops = &bb_clk_ops;
		clk_priv->rate = phy->talInit.tx.txProfile.txInputRate_kHz;
		break;
	default:
		return -EINVAL;
	}

	clk_priv->rate *= 1000;

	clk = devm_clk_register(&phy->spi->dev, &clk_priv->hw);
	phy->clks[source] = clk;

	return 0;
}

static irqreturn_t adrv9009_irq_handler(int irq, void *p)
{
	struct iio_dev *indio_dev = p;
	struct adrv9009_rf_phy *phy = iio_priv(indio_dev);
	taliseGpIntInformation_t gpint_diag;
	u32 gpint_status;
	int i, j, ret;

	ret = TALISE_gpIntHandler(phy->talDevice, &gpint_status, &gpint_diag);

	for (i = 0; i < ARRAY_SIZE(gpint_diag.data); i++) {
		for (j = 0; j < 8; j++)
			if (gpint_diag.data[i] & BIT(j))
				dev_warn(&phy->spi->dev, "%s\n", adrv9009_gpint_diag[i][j]);

	}

	dev_warn(&phy->spi->dev, "GP Interrupt Status 0x%X Action: %s\n",
		 gpint_status, adrv9009_actions[ret]);


	switch (ret) {
	case TALACT_ERR_RESET_JESD204FRAMERA:
	case TALACT_ERR_RESET_JESD204FRAMERB:
	case TALACT_ERR_RESET_JESD204DEFRAMERA:
	case TALACT_ERR_RESET_JESD204DEFRAMERB:
		break;

	case TALACT_ERR_REDUCE_TXSAMPLE_PWR:
		TALISE_clearPaProtectErrorFlags(phy->talDevice);
		msleep(500);
	default:
		break;
	}

	return IRQ_HANDLED;
}

static int adrv9009_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct adrv9009_rf_phy *phy;
	struct clk *clk = NULL;
	const char *name;
	int ret;
	u8 rev;
	taliseArmVersionInfo_t talArmVersionInfo;
	u32 api_vers[4];

	dev_info(&spi->dev, "%s : enter", __func__);

	clk = devm_clk_get(&spi->dev, "jesd_rx_clk");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*phy));
	if (indio_dev == NULL)
		return -ENOMEM;

	phy = iio_priv(indio_dev);
	phy->indio_dev = indio_dev;
	phy->spi = spi;

	ret = adrv9009_phy_parse_dt(indio_dev, &spi->dev);
	if (ret < 0)
		return -ret;

	phy->talDevice = &phy->talise_device;
	phy->linux_hal.spi = spi;
	phy->linux_hal.logLevel = ADIHAL_LOG_ERR | ADIHAL_LOG_WARN;
	phy->talDevice->devHalInfo = &phy->linux_hal;

	phy->linux_hal.reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);

	phy->sysref_req_gpio = devm_gpiod_get(&spi->dev, "sysref-req",
					      GPIOD_OUT_HIGH);

	phy->jesd_rx_clk = clk;

	phy->jesd_tx_clk = devm_clk_get(&spi->dev, "jesd_tx_clk");
	if (IS_ERR(phy->jesd_tx_clk))
		return PTR_ERR(phy->jesd_tx_clk);

	phy->jesd_rx_os_clk = devm_clk_get(&spi->dev, "jesd_rx_os_clk");
	if (IS_ERR(phy->jesd_rx_os_clk))
		return PTR_ERR(phy->jesd_rx_os_clk);

	phy->dev_clk = devm_clk_get(&spi->dev, "dev_clk");
	if (IS_ERR(phy->dev_clk))
		return PTR_ERR(phy->dev_clk);

	phy->fmc_clk = devm_clk_get(&spi->dev, "fmc_clk");
	if (IS_ERR(phy->fmc_clk))
		return PTR_ERR(phy->fmc_clk);

	ret = clk_prepare_enable(phy->fmc_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(phy->dev_clk);
	if (ret)
		return ret;

	if (of_property_read_string(spi->dev.of_node, "arm-firmware-name", &name))
		name = FIRMWARE;

	ret = request_firmware(&phy->fw, name, &spi->dev);
	if (ret) {
		dev_err(&spi->dev,
			"request_firmware() failed with %d\n", ret);
		return ret;
	}

	if (of_property_read_string(spi->dev.of_node, "stream-firmware-name", &name))
		name = STREAM;

	ret = request_firmware(&phy->stream, name, &spi->dev);
	if (ret) {
		dev_err(&spi->dev,
			"request_firmware() failed with %d\n", ret);
		return ret;
	}

	ret = adrv9009_setup(phy);
	if (ret < 0) {
		/* Try once more */
		ret = adrv9009_setup(phy);
		if (ret < 0)
			goto out_unregister_notifier;
	}

	adrv9009_clk_register(phy, "-rx_sampl_clk", NULL, NULL,
			      CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED , RX_SAMPL_CLK);

	adrv9009_clk_register(phy, "-obs_sampl_clk", NULL, NULL,
			      CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED, OBS_SAMPL_CLK);

	adrv9009_clk_register(phy, "-tx_sampl_clk", NULL, NULL,
			      CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED, TX_SAMPL_CLK);

	phy->clk_data.clks = phy->clks;
	phy->clk_data.clk_num = NUM_ADRV9009_CLKS;

	ret = of_clk_add_provider(spi->dev.of_node,
				  of_clk_src_onecell_get, &phy->clk_data);
	if (ret)
		goto out_disable_clocks;

	sysfs_bin_attr_init(&phy->bin);
	phy->bin.attr.name = "profile_config";
	phy->bin.attr.mode = S_IWUSR | S_IRUGO;
	phy->bin.write = adrv9009_profile_bin_write;
	phy->bin.read = adrv9009_profile_bin_read;
	phy->bin.size = 8192;

	sysfs_bin_attr_init(&phy->bin_gt);
	phy->bin_gt.attr.name = "gain_table_config";
	phy->bin_gt.attr.mode = S_IWUSR;
	phy->bin_gt.write = adrv9009_gt_bin_write;
	phy->bin_gt.size = 32768;

	indio_dev->dev.parent = &spi->dev;

	if (spi->dev.of_node)
		indio_dev->name = spi->dev.of_node->name;
	else
		indio_dev->name = "adrv9009-phy";

	indio_dev->info = &adrv9009_phy_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adrv9009_phy_chan;
	indio_dev->num_channels = ARRAY_SIZE(adrv9009_phy_chan);

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		goto out_clk_del_provider;

	ret = adrv9009_register_axi_converter(phy);
	if (ret < 0)
		goto out_iio_device_unregister;

	ret = sysfs_create_bin_file(&indio_dev->dev.kobj, &phy->bin);
	if (ret < 0)
		goto out_iio_device_unregister;

	ret = sysfs_create_bin_file(&indio_dev->dev.kobj, &phy->bin_gt);
	if (ret < 0)
		goto out_iio_device_unregister;

	ret = adrv9009_register_debugfs(indio_dev);
	if (ret < 0)
		dev_warn(&spi->dev, "%s: failed to register debugfs", __func__);


	if (spi->irq) {
		ret = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
						adrv9009_irq_handler,
						IRQF_TRIGGER_RISING  | IRQF_ONESHOT,
						indio_dev->name, indio_dev);

		if (ret) {
			dev_err(&spi->dev,
				"request_irq() failed with %d\n", ret);
			goto out_remove_sysfs_bin;
		}
	}

	TALISE_getArmVersion_v2(phy->talDevice, &talArmVersionInfo);
	TALISE_getApiVersion(phy->talDevice, &api_vers[0], &api_vers[1], &api_vers[2],
			     &api_vers[3]);
	TALISE_getDeviceRev(phy->talDevice, &rev);

	dev_info(&spi->dev,
		 "%s: %s Rev %d, Firmware %u.%u.%u API version: %u.%u.%u.%u successfully initialized",
		 __func__, spi_get_device_id(spi)->name, rev, talArmVersionInfo.majorVer,
		 talArmVersionInfo.minorVer, talArmVersionInfo.rcVer,
		 api_vers[0], api_vers[1], api_vers[2], api_vers[3]);

	return 0;

out_remove_sysfs_bin:
	sysfs_remove_bin_file(&phy->indio_dev->dev.kobj, &phy->bin);
	sysfs_remove_bin_file(&phy->indio_dev->dev.kobj, &phy->bin_gt);
out_iio_device_unregister:
	iio_device_unregister(indio_dev);
out_clk_del_provider:
	of_clk_del_provider(spi->dev.of_node);
out_disable_clocks:
	clk_disable_unprepare(phy->dev_clk);
	clk_disable_unprepare(phy->fmc_clk);
out_unregister_notifier:
	release_firmware(phy->fw);
	release_firmware(phy->stream);

	return ret;
}

static int adrv9009_remove(struct spi_device *spi)
{
	struct adrv9009_rf_phy *phy = adrv9009_spi_to_phy(spi);

	release_firmware(phy->fw);
	release_firmware(phy->stream);
	sysfs_remove_bin_file(&phy->indio_dev->dev.kobj, &phy->bin);
	sysfs_remove_bin_file(&phy->indio_dev->dev.kobj, &phy->bin_gt);
	iio_device_unregister(phy->indio_dev);
	of_clk_del_provider(spi->dev.of_node);
	clk_disable_unprepare(phy->dev_clk);
	clk_disable_unprepare(phy->fmc_clk);

	adrv9009_shutdown(phy);

	return 0;
}

static const struct spi_device_id adrv9009_id[] = {
	{"adrv9009", ID_ADRV9009},
	{"adrv9008-1", ID_ADRV90081},
	{"adrv9008-2", ID_ADRV90082},
	{}
};
MODULE_DEVICE_TABLE(spi, adrv9009_id);

static struct spi_driver adrv9009_driver = {
	.driver = {
		.name	= "adrv9009",
		.owner	= THIS_MODULE,
	},
	.probe		= adrv9009_probe,
	.remove		= adrv9009_remove,
	.id_table	= adrv9009_id,
};
module_spi_driver(adrv9009_driver);

MODULE_AUTHOR("Michael Hennerich <michael.hennerich@analog.com>");
MODULE_DESCRIPTION("Analog Devices ADRV9009 ADC");
MODULE_LICENSE("GPL v2");
