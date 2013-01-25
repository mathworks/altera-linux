/*
 *  Copyright (C) 2012 Altera Corporation <www.altera.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

static DEFINE_SPINLOCK(_lock);

#define SOCFPGA_OSC1_CLK	10000000
#define SOCFPGA_MPU_CLK		800000000
#define SOCFPGA_MAIN_QSPI_CLK		432000000
#define SOCFPGA_MAIN_NAND_SDMMC_CLK	250000000
#define SOCFPGA_S2F_USR_CLK		125000000

#define SOCFPGA_MAIN_PLL_CLK		1200000000
#define SOCFPGA_PER_PLL_CLK		900000000
#define SOCFPGA_SDRAM_PLL_CLK		800000000

#define CLKMGR_PERPLLGRP_EN	0xA0

#define CLKMGR_QSPI_CLK_EN				11
#define CLKMGR_NAND_CLK_EN				10
#define CLKMGR_NAND_X_CLK_EN			9
#define CLKMGR_SDMMC_CLK_EN			8
#define CLKMGR_S2FUSR_CLK_EN			7
#define CLKMGR_GPIO_CLK_EN				6
#define CLKMGR_CAN1_CLK_EN				5
#define CLKMGR_CAN0_CLK_EN				4
#define CLKMGR_SPI_M_CLK_EN			3
#define CLKMGR_USB_MP_CLK_EN			2
#define CLKMGR_EMAC1_CLK_EN			1
#define CLKMGR_EMAC0_CLK_EN			0

void __iomem *clk_mgr_base_addr;

void __init socfpga_init_clocks(void)
{
	struct clk *clk;
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "altr,clk-mgr");
	clk_mgr_base_addr = of_iomap(np, 0);

	clk = clk_register_fixed_rate(NULL, "main_pll_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_MAIN_PLL_CLK);
	clk_register_clkdev(clk, "main_pll_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "per_pll_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_PER_PLL_CLK);
	clk_register_clkdev(clk, "per_pll_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "sdram_pll_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_SDRAM_PLL_CLK);
	clk_register_clkdev(clk, "sdram_pll_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "osc1_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_OSC1_CLK);
	clk_register_clkdev(clk, "osc1_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "mpu_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_MPU_CLK);
	clk_register_clkdev(clk, "mpu_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "main_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_MPU_CLK/2);
	clk_register_clkdev(clk, "main_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "dbg_base_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_MPU_CLK/2);
	clk_register_clkdev(clk, "dbg_base_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "smp_twd", NULL, CLK_IS_ROOT,
			SOCFPGA_MPU_CLK/4);
	clk_register_clkdev(clk, NULL, "smp_twd");

	clk = clk_register_fixed_rate(NULL, "main_qspi_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_MAIN_QSPI_CLK);
	clk_register_clkdev(clk, "main_qspi_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "main_nand_sdmmc_clk", NULL,
			CLK_IS_ROOT, SOCFPGA_MAIN_NAND_SDMMC_CLK);
	clk_register_clkdev(clk, "main_nand_sdmmc_clk", NULL);

	clk = clk_register_fixed_rate(NULL, "s2f_usr_clk", NULL, CLK_IS_ROOT,
			SOCFPGA_S2F_USR_CLK);
	clk_register_clkdev(clk, "s2f_usr_clk", NULL);

	clk = clk_register_gate(NULL, "gmac0_clk", "per_pll_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_EMAC0_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "ff700000.stmmac");

	clk = clk_register_gate(NULL, "gmac1_clk", "per_pll_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_EMAC1_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "ff702000.stmmac");

	clk = clk_register_gate(NULL, "spi0_clk", "per_pll_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_SPI_M_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "fff00000.spi");

	clk = clk_register_gate(NULL, "spi1_clk", "per_pll_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_SPI_M_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "fff01000.spi");

	clk = clk_register_gate(NULL, "gpio0_clk", "per_pll_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_GPIO_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "ff708000.gpio");

	clk = clk_register_gate(NULL, "gpio1_clk", "per_pll_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_GPIO_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "ff709000.gpio");

	clk = clk_register_gate(NULL, "gpio2_clk", "per_pll_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_GPIO_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "ff70a000.gpio");

	clk = clk_register_gate(NULL, "nand_clk", "main_nand_sdmmc_clk", 0,
			clk_mgr_base_addr + CLKMGR_PERPLLGRP_EN,
			CLKMGR_NAND_CLK_EN, 0, &_lock);
	clk_register_clkdev(clk, NULL, "ff900000.nand");
}
