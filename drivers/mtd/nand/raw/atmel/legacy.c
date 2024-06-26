// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2003 Rick Bronson
 *
 *  Derived from drivers/mtd/nand/autcpu12.c
 *	 Copyright (c) 2001 Thomas Gleixner (gleixner@autronix.de)
 *
 *  Derived from drivers/mtd/spia.c
 *	 Copyright (C) 2000 Steven J. Hill (sjhill@cotw.com)
 *
 *
 *  Add Hardware ECC support for AT91SAM9260 / AT91SAM9263
 *     Richard Genoud (richard.genoud@gmail.com), Adeneo Copyright (C) 2007
 *
 *     Derived from Das U-Boot source code
 *     		(u-boot-1.1.5/board/atmel/at91sam9263ek/nand.c)
 *     (C) Copyright 2006 ATMEL Rousset, Lacressonniere Nicolas
 */

#include <common.h>
#include <driver.h>
#include <malloc.h>
#include <init.h>
#include <gpio.h>

#include <of.h>
#include <of_gpio.h>
#include <of_mtd.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/nand.h>
#include <linux/err.h>

#include <io.h>
#include <mach/at91/board.h>

#include <errno.h>

/* Register access macros */
#define ecc_readl(add, reg)				\
	readl(add + ATMEL_ECC_##reg)
#define ecc_writel(add, reg, value)			\
	writel((value), add + ATMEL_ECC_##reg)

#include "atmel_nand_ecc.h"	/* Hardware ECC registers */

/* oob layout for large page size
 * bad block info is on bytes 0 and 1
 * the bytes have to be consecutives to avoid
 * several NAND_CMD_RNDOUT during read
 */
static struct nand_ecclayout atmel_oobinfo_large = {
	.eccbytes = 4,
	.eccpos = {60, 61, 62, 63},
	.oobfree = {
		{2, 58}
	},
};

/* oob layout for small page size
 * bad block info is on bytes 4 and 5
 * the bytes have to be consecutives to avoid
 * several NAND_CMD_RNDOUT during read
 */
static struct nand_ecclayout atmel_oobinfo_small = {
	.eccbytes = 4,
	.eccpos = {0, 1, 2, 3},
	.oobfree = {
		{6, 10}
	},
};

struct atmel_nand_host {
	struct nand_chip	nand_chip;
	void __iomem		*io_base;
	struct atmel_nand_data	*board;
	struct device		*dev;
	void __iomem		*ecc;

	int			pmecc_bytes_per_sector;
	int			pmecc_sector_number;
	int			pmecc_degree;	/* Degree of remainders */
	int			pmecc_cw_len;	/* Length of codeword */

	void __iomem		*pmerrloc_base;
	void __iomem		*pmecc_rom_base;

	/* lookup table for alpha_to and index_of */
	void __iomem		*pmecc_alpha_to;
	void __iomem		*pmecc_index_of;

	/* data for pmecc computation */
	int16_t			*pmecc_partial_syn;
	int16_t			*pmecc_si;
	int16_t			*pmecc_smu;	/* Sigma table */
	int16_t			*pmecc_lmu;	/* polynomal order */
	int			*pmecc_mu;
	int			*pmecc_dmu;
	int			*pmecc_delta;
	struct nand_ecclayout	*ecclayout;
	void			*ecc_code;
};

/*
 * Enable NAND.
 */
static void atmel_nand_enable(struct atmel_nand_host *host)
{
	if (gpio_is_valid(host->board->enable_pin))
		gpio_set_value(host->board->enable_pin, 0);
}

/*
 * Disable NAND.
 */
static void atmel_nand_disable(struct atmel_nand_host *host)
{
	if (gpio_is_valid(host->board->enable_pin))
		gpio_set_value(host->board->enable_pin, 1);
}

/*
 * Hardware specific access to control-lines
 */
static void atmel_nand_cmd_ctrl(struct nand_chip *nand_chip, int cmd, unsigned int ctrl)
{
	struct atmel_nand_host *host = nand_chip->priv;

	if (ctrl & NAND_CTRL_CHANGE) {
		if (ctrl & NAND_NCE)
			atmel_nand_enable(host);
		else
			atmel_nand_disable(host);
	}
	if (cmd == NAND_CMD_NONE)
		return;

	if (ctrl & NAND_CLE)
		writeb(cmd, host->io_base + (1 << host->board->cle));
	else
		writeb(cmd, host->io_base + (1 << host->board->ale));
}

/*
 * Read the Device Ready pin.
 */
static int atmel_nand_device_ready(struct nand_chip *nand_chip)
{
	struct atmel_nand_host *host = nand_chip->priv;

	return gpio_get_value(host->board->rdy_pin);
}

/*
 * Minimal-overhead PIO for data access.
 */
static void atmel_read_buf(struct nand_chip *nand_chip, u8 *buf, int len)
{
	readsb(nand_chip->legacy.IO_ADDR_R, buf, len);
}

static void atmel_read_buf16(struct nand_chip *nand_chip, u8 *buf, int len)
{
	readsw(nand_chip->legacy.IO_ADDR_R, buf, len / 2);
}

static void atmel_write_buf(struct nand_chip *nand_chip, const u8 *buf, int len)
{
	writesb(nand_chip->legacy.IO_ADDR_W, buf, len);
}

static void atmel_write_buf16(struct nand_chip *nand_chip, const u8 *buf, int len)
{
	writesw(nand_chip->legacy.IO_ADDR_W, buf, len / 2);
}

/*
 * Return number of ecc bytes per sector according to sector size and
 * correction capability
 *
 * Following table shows what at91 PMECC supported:
 * Correction Capability	Sector_512_bytes	Sector_1024_bytes
 * =====================	================	=================
 *                2-bits                 4-bytes                  4-bytes
 *                4-bits                 7-bytes                  7-bytes
 *                8-bits                13-bytes                 14-bytes
 *               12-bits                20-bytes                 21-bytes
 *               24-bits                39-bytes                 42-bytes
 */
static int pmecc_get_ecc_bytes(int cap, int sector_size)
{
	int m = 12 + sector_size / 512;
	return (m * cap + 7) / 8;
}

static void __iomem *pmecc_get_alpha_to(struct atmel_nand_host *host)
{
	int table_size;

	table_size = host->board->pmecc_sector_size == 512 ?
		PMECC_LOOKUP_TABLE_SIZE_512 : PMECC_LOOKUP_TABLE_SIZE_1024;

	return host->pmecc_rom_base + host->board->pmecc_lookup_table_offset +
			table_size * sizeof(int16_t);
}

static void pmecc_data_free(struct atmel_nand_host *host)
{
	kfree(host->pmecc_partial_syn);
	kfree(host->pmecc_si);
	kfree(host->pmecc_lmu);
	kfree(host->pmecc_smu);
	kfree(host->pmecc_mu);
	kfree(host->pmecc_dmu);
	kfree(host->pmecc_delta);
}

static int pmecc_data_alloc(struct atmel_nand_host *host)
{
	const int cap = host->board->pmecc_corr_cap;

	host->pmecc_partial_syn = kzalloc((2 * cap + 1) * sizeof(int16_t),
					GFP_KERNEL);
	host->pmecc_si = kzalloc((2 * cap + 1) * sizeof(int16_t), GFP_KERNEL);
	host->pmecc_lmu = kzalloc((cap + 1) * sizeof(int16_t), GFP_KERNEL);
	host->pmecc_smu = kzalloc((cap + 2) * (2 * cap + 1) * sizeof(int16_t),
					GFP_KERNEL);
	host->pmecc_mu = kzalloc((cap + 1) * sizeof(int), GFP_KERNEL);
	host->pmecc_dmu = kzalloc((cap + 1) * sizeof(int), GFP_KERNEL);
	host->pmecc_delta = kzalloc((cap + 1) * sizeof(int), GFP_KERNEL);

	if (host->pmecc_partial_syn &&
			host->pmecc_si &&
			host->pmecc_lmu &&
			host->pmecc_smu &&
			host->pmecc_mu &&
			host->pmecc_dmu &&
			host->pmecc_delta)
		return 0;

	/* error happened */
	pmecc_data_free(host);
	return -ENOMEM;
}

static void pmecc_gen_syndrome(struct mtd_info *mtd, int sector)
{
	struct nand_chip *nand_chip = mtd_to_nand(mtd);
	struct atmel_nand_host *host = nand_chip->priv;
	int i;
	uint32_t value;

	/* Fill odd syndromes */
	for (i = 0; i < host->board->pmecc_corr_cap; i++) {
		value = pmecc_readl_rem_relaxed(host->ecc, sector, i / 2);
		if (i & 1)
			value >>= 16;
		value &= 0xffff;
		host->pmecc_partial_syn[(2 * i) + 1] = (int16_t)value;
	}
}

static void pmecc_substitute(struct mtd_info *mtd)
{
	struct nand_chip *nand_chip = mtd_to_nand(mtd);
	struct atmel_nand_host *host = nand_chip->priv;
	int16_t __iomem *alpha_to = host->pmecc_alpha_to;
	int16_t __iomem *index_of = host->pmecc_index_of;
	int16_t *partial_syn = host->pmecc_partial_syn;
	const int cap = host->board->pmecc_corr_cap;
	int16_t *si;
	int i, j;

	/* si[] is a table that holds the current syndrome value,
	 * an element of that table belongs to the field
	 */
	si = host->pmecc_si;

	memset(&si[1], 0, sizeof(int16_t) * (2 * cap - 1));

	/* Computation 2t syndromes based on S(x) */
	/* Odd syndromes */
	for (i = 1; i < 2 * cap; i += 2) {
		for (j = 0; j < host->pmecc_degree; j++) {
			if (partial_syn[i] & ((unsigned short)0x1 << j))
				si[i] = readw(alpha_to + i * j) ^ si[i];
		}
	}
	/* Even syndrome = (Odd syndrome) ** 2 */
	for (i = 2, j = 1; j <= cap; i = ++j << 1) {
		if (si[j] == 0) {
			si[i] = 0;
		} else {
			int16_t tmp;

			tmp = readw(index_of + si[j]);
			tmp = (tmp * 2) % host->pmecc_cw_len;
			si[i] = readw(alpha_to + tmp);
		}
	}

	return;
}

static void pmecc_get_sigma(struct mtd_info *mtd)
{
	struct nand_chip *nand_chip = mtd_to_nand(mtd);
	struct atmel_nand_host *host = nand_chip->priv;

	int16_t *lmu = host->pmecc_lmu;
	int16_t *si = host->pmecc_si;
	int *mu = host->pmecc_mu;
	int *dmu = host->pmecc_dmu;	/* Discrepancy */
	int *delta = host->pmecc_delta; /* Delta order */
	int cw_len = host->pmecc_cw_len;
	const int16_t cap = host->board->pmecc_corr_cap;
	const int num = 2 * cap + 1;
	int16_t __iomem	*index_of = host->pmecc_index_of;
	int16_t __iomem	*alpha_to = host->pmecc_alpha_to;
	int i, j, k;
	uint32_t dmu_0_count, tmp;
	int16_t *smu = host->pmecc_smu;

	/* index of largest delta */
	int ro;
	int largest;
	int diff;

	dmu_0_count = 0;

	/* First Row */

	/* Mu */
	mu[0] = -1;

	memset(smu, 0, sizeof(int16_t) * num);
	smu[0] = 1;

	/* discrepancy set to 1 */
	dmu[0] = 1;
	/* polynom order set to 0 */
	lmu[0] = 0;
	delta[0] = (mu[0] * 2 - lmu[0]) >> 1;

	/* Second Row */

	/* Mu */
	mu[1] = 0;
	/* Sigma(x) set to 1 */
	memset(&smu[num], 0, sizeof(int16_t) * num);
	smu[num] = 1;

	/* discrepancy set to S1 */
	dmu[1] = si[1];

	/* polynom order set to 0 */
	lmu[1] = 0;

	delta[1] = (mu[1] * 2 - lmu[1]) >> 1;

	/* Init the Sigma(x) last row */
	memset(&smu[(cap + 1) * num], 0, sizeof(int16_t) * num);

	for (i = 1; i <= cap; i++) {
		mu[i + 1] = i << 1;
		/* Begin Computing Sigma (Mu+1) and L(mu) */
		/* check if discrepancy is set to 0 */
		if (dmu[i] == 0) {
			dmu_0_count++;

			tmp = ((cap - (lmu[i] >> 1) - 1) / 2);
			if ((cap - (lmu[i] >> 1) - 1) & 0x1)
				tmp += 2;
			else
				tmp += 1;

			if (dmu_0_count == tmp) {
				for (j = 0; j <= (lmu[i] >> 1) + 1; j++)
					smu[(cap + 1) * num + j] =
							smu[i * num + j];

				lmu[cap + 1] = lmu[i];
				return;
			}

			/* copy polynom */
			for (j = 0; j <= lmu[i] >> 1; j++)
				smu[(i + 1) * num + j] = smu[i * num + j];

			/* copy previous polynom order to the next */
			lmu[i + 1] = lmu[i];
		} else {
			ro = 0;
			largest = -1;
			/* find largest delta with dmu != 0 */
			for (j = 0; j < i; j++) {
				if ((dmu[j]) && (delta[j] > largest)) {
					largest = delta[j];
					ro = j;
				}
			}

			/* compute difference */
			diff = (mu[i] - mu[ro]);

			/* Compute degree of the new smu polynomial */
			if ((lmu[i] >> 1) > ((lmu[ro] >> 1) + diff))
				lmu[i + 1] = lmu[i];
			else
				lmu[i + 1] = ((lmu[ro] >> 1) + diff) * 2;

			/* Init smu[i+1] with 0 */
			for (k = 0; k < num; k++)
				smu[(i + 1) * num + k] = 0;

			/* Compute smu[i+1] */
			for (k = 0; k <= lmu[ro] >> 1; k++) {
				int16_t a, b, c;

				if (!(smu[ro * num + k] && dmu[i]))
					continue;
				a = readw(index_of + dmu[i]);
				b = readw(index_of + dmu[ro]);
				c = readw(index_of + smu[ro * num + k]);
				tmp = a + (cw_len - b) + c;
				a = readw(alpha_to + tmp % cw_len);
				smu[(i + 1) * num + (k + diff)] = a;
			}

			for (k = 0; k <= lmu[i] >> 1; k++)
				smu[(i + 1) * num + k] ^= smu[i * num + k];
		}

		/* End Computing Sigma (Mu+1) and L(mu) */
		/* In either case compute delta */
		delta[i + 1] = (mu[i + 1] * 2 - lmu[i + 1]) >> 1;

		/* Do not compute discrepancy for the last iteration */
		if (i >= cap)
			continue;

		for (k = 0; k <= (lmu[i + 1] >> 1); k++) {
			tmp = 2 * (i - 1);
			if (k == 0) {
				dmu[i + 1] = si[tmp + 3];
			} else if (smu[(i + 1) * num + k] && si[tmp + 3 - k]) {
				int16_t a, b, c;
				a = readw(index_of +
						smu[(i + 1) * num + k]);
				b = si[2 * (i - 1) + 3 - k];
				c = readw(index_of + b);
				tmp = a + c;
				tmp %= cw_len;
				dmu[i + 1] = readw(alpha_to + tmp) ^
					dmu[i + 1];
			}
		}
	}

	return;
}

static int pmecc_err_location(struct mtd_info *mtd)
{
	struct nand_chip *nand_chip = mtd_to_nand(mtd);
	struct atmel_nand_host *host = nand_chip->priv;
	const int cap = host->board->pmecc_corr_cap;
	const int num = 2 * cap + 1;
	int sector_size = host->board->pmecc_sector_size;
	int err_nbr = 0;	/* number of error */
	int roots_nbr;		/* number of roots */
	int i, ret;
	uint32_t val;
	int16_t *smu = host->pmecc_smu;

	pmerrloc_writel(host->pmerrloc_base, ELDIS, PMERRLOC_DISABLE);

	for (i = 0; i <= host->pmecc_lmu[cap + 1] >> 1; i++) {
		pmerrloc_writel_sigma_relaxed(host->pmerrloc_base, i,
				      smu[(cap + 1) * num + i]);
		err_nbr++;
	}

	val = (err_nbr - 1) << 16;
	if (sector_size == 1024)
		val |= 1;

	pmerrloc_writel(host->pmerrloc_base, ELCFG, val);
	pmerrloc_writel(host->pmerrloc_base, ELEN,
			sector_size * 8 + host->pmecc_degree * cap);

	ret = wait_on_timeout(PMECC_MAX_TIMEOUT_MS,
		pmerrloc_readl_relaxed(host->pmerrloc_base, ELISR)
					 & PMERRLOC_CALC_DONE);
	if (ret) {
		dev_err(host->dev, "PMECC: Timeout to calculate error location.\n");
		return -ETIMEDOUT;
	}

	roots_nbr = (pmerrloc_readl_relaxed(host->pmerrloc_base, ELISR)
		& PMERRLOC_ERR_NUM_MASK) >> 8;
	/* Number of roots == degree of smu hence <= cap */
	if (roots_nbr == host->pmecc_lmu[cap + 1] >> 1)
		return err_nbr - 1;

	/* Number of roots does not match the degree of smu
	 * unable to correct error */
	return -1;
}

static void pmecc_correct_data(struct mtd_info *mtd, uint8_t *buf, uint8_t *ecc,
		int sector_num, int extra_bytes, int err_nbr)
{
	struct nand_chip *nand_chip = mtd_to_nand(mtd);
	struct atmel_nand_host *host = nand_chip->priv;
	int i = 0;
	int byte_pos, bit_pos, sector_size;
	uint32_t tmp;
	uint8_t err_byte;

	sector_size = host->board->pmecc_sector_size;

	while (err_nbr) {
		tmp = pmerrloc_readl_el_relaxed(host->pmerrloc_base, i) - 1;
		byte_pos = tmp / 8;
		bit_pos  = tmp % 8;

		if (byte_pos >= (sector_size + extra_bytes))
			BUG();	/* should never happen */

		if (byte_pos < sector_size) {
			err_byte = *(buf + byte_pos);
			*(buf + byte_pos) ^= (1 << bit_pos);
		} else {
			/* Bit flip in OOB area */
			tmp = sector_num * host->pmecc_bytes_per_sector
					+ (byte_pos - sector_size);
			err_byte = ecc[tmp];
			ecc[tmp] ^= (1 << bit_pos);
		}

		i++;
		err_nbr--;
	}

	return;
}

static int pmecc_correction(struct mtd_info *mtd, u32 pmecc_stat, uint8_t *buf)
{
	struct nand_chip *nand_chip = mtd_to_nand(mtd);
	struct atmel_nand_host *host = nand_chip->priv;
	int i, err_nbr, ret, max_bitflips = 0;
	uint8_t *buf_pos;
	uint8_t *ecc_code = host->ecc_code;

	ret = mtd_ooblayout_get_eccbytes(mtd, ecc_code, nand_chip->oob_poi, 0,
					 nand_chip->ecc.total);
	if (ret)
		return ret;

	for (i = 0; i < nand_chip->ecc.bytes * nand_chip->ecc.steps; i++)
		if (ecc_code[i] != 0xff)
			goto normal_check;
	/* Erased page, return OK */
	return 0;

normal_check:
	for (i = 0; i < host->pmecc_sector_number; i++) {
		err_nbr = 0;
		if (pmecc_stat & 0x1) {
			buf_pos = buf + i * host->board->pmecc_sector_size;

			pmecc_gen_syndrome(mtd, i);
			pmecc_substitute(mtd);
			pmecc_get_sigma(mtd);

			err_nbr = pmecc_err_location(mtd);
			if (err_nbr == -1) {
				dev_err(host->dev, "PMECC: Too many errors\n");
				mtd->ecc_stats.failed++;
				return -EBADMSG;
			} else {
				pmecc_correct_data(mtd, buf_pos, ecc_code, i,
					host->pmecc_bytes_per_sector, err_nbr);
				mtd->ecc_stats.corrected += err_nbr;
				max_bitflips = max(max_bitflips, err_nbr);
			}
		}
		pmecc_stat >>= 1;
	}

	return max_bitflips;
}

static int atmel_nand_pmecc_read_page(struct nand_chip *chip, uint8_t *buf,
				      int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct atmel_nand_host *host = chip->priv;
	uint32_t stat;
	int ret;

	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_RST);
	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_DISABLE);
	pmecc_writel(host->ecc, CFG, (pmecc_readl_relaxed(host->ecc, CFG)
		& ~PMECC_CFG_WRITE_OP) | PMECC_CFG_AUTO_ENABLE);

	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_ENABLE);
	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_DATA);

	nand_read_page_op(chip, page, 0, NULL, 0);

	chip->legacy.read_buf(chip, buf, mtd->writesize);
	chip->legacy.read_buf(chip, chip->oob_poi, mtd->oobsize);

	ret = wait_on_timeout(PMECC_MAX_TIMEOUT_MS,
		!(pmecc_readl_relaxed(host->ecc, SR) & PMECC_SR_BUSY));
	if (ret) {
		dev_err(host->dev, "PMECC: Timeout to calculate error location.\n");
		return -ETIMEDOUT;
	}

	stat = pmecc_readl_relaxed(host->ecc, ISR);
	if (stat != 0)
		return pmecc_correction(mtd, stat, buf);

	return 0;
}

static int atmel_nand_pmecc_write_page(struct nand_chip *chip, const uint8_t *buf,
				       int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct atmel_nand_host *host = chip->priv;
	uint8_t *ecc_calc = host->ecc_code;
	int i, j, ret;

	ret = nand_prog_page_begin_op(chip, page, 0, NULL, 0);
	if (ret)
		return ret;

	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_RST);
	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_DISABLE);

	pmecc_writel(host->ecc, CFG, (pmecc_readl_relaxed(host->ecc, CFG) |
		PMECC_CFG_WRITE_OP) & ~PMECC_CFG_AUTO_ENABLE);

	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_ENABLE);
	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_DATA);

	chip->legacy.write_buf(chip, (u8 *)buf, mtd->writesize);

	ret = wait_on_timeout(PMECC_MAX_TIMEOUT_MS,
		!(pmecc_readl_relaxed(host->ecc, SR) & PMECC_SR_BUSY));
	if (ret) {
		dev_err(host->dev, "PMECC: Timeout to get ECC value.\n");
		return -ETIMEDOUT;
	}

	for (i = 0; i < host->pmecc_sector_number; i++) {
		for (j = 0; j < host->pmecc_bytes_per_sector; j++) {
			int pos;

			pos = i * host->pmecc_bytes_per_sector + j;
			ecc_calc[pos] =	pmecc_readb_ecc_relaxed(host->ecc, i, j);
		}
	}

	ret = mtd_ooblayout_set_eccbytes(mtd, ecc_calc,
			chip->oob_poi, 0, chip->ecc.total);
	if (ret)
		return ret;

	chip->legacy.write_buf(chip, chip->oob_poi, mtd->oobsize);

	ret = nand_write_data_op(chip, chip->oob_poi, mtd->oobsize, false);
	if (ret)
		return ret;

	return nand_prog_page_end_op(chip);
}

static void atmel_pmecc_core_init(struct mtd_info *mtd)
{
	struct nand_chip *nand_chip = mtd_to_nand(mtd);
	struct atmel_nand_host *host = nand_chip->priv;
	uint32_t val = 0;
	int eccbytes;

	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_RST);
	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_DISABLE);

	switch (host->board->pmecc_corr_cap) {
	case 2:
		val = PMECC_CFG_BCH_ERR2;
		break;
	case 4:
		val = PMECC_CFG_BCH_ERR4;
		break;
	case 8:
		val = PMECC_CFG_BCH_ERR8;
		break;
	case 12:
		val = PMECC_CFG_BCH_ERR12;
		break;
	case 24:
		val = PMECC_CFG_BCH_ERR24;
		break;
	}

	if (host->board->pmecc_sector_size == 512)
		val |= PMECC_CFG_SECTOR512;
	else if (host->board->pmecc_sector_size == 1024)
		val |= PMECC_CFG_SECTOR1024;

	switch (host->pmecc_sector_number) {
	case 1:
		val |= PMECC_CFG_PAGE_1SECTOR;
		break;
	case 2:
		val |= PMECC_CFG_PAGE_2SECTORS;
		break;
	case 4:
		val |= PMECC_CFG_PAGE_4SECTORS;
		break;
	case 8:
		val |= PMECC_CFG_PAGE_8SECTORS;
		break;
	}

	val |= (PMECC_CFG_READ_OP | PMECC_CFG_SPARE_DISABLE
		| PMECC_CFG_AUTO_DISABLE);
	pmecc_writel(host->ecc, CFG, val);

	eccbytes = host->pmecc_sector_number * host->pmecc_bytes_per_sector;
	pmecc_writel(host->ecc, SAREA, mtd->oobsize - 1);
	pmecc_writel(host->ecc, SADDR, mtd->oobsize - eccbytes);
	pmecc_writel(host->ecc, EADDR, mtd->oobsize - 1);
	/* See datasheet about PMECC Clock Control Register */
	pmecc_writel(host->ecc, CLK, 2);
	pmecc_writel(host->ecc, IDR, 0xff);
	pmecc_writel(host->ecc, CTRL, PMECC_CTRL_ENABLE);
}

static uint16_t *pmecc_galois_table;
static int pmecc_build_galois_table(unsigned int mm, int16_t *index_of,
				    int16_t *alpha_to)
{
	unsigned int i, mask, nn;
	unsigned int p[15];

	nn = (1 << mm) - 1;
	/* set default value */
	for (i = 1; i < mm; i++)
		p[i] = 0;

	/* 1 + X^mm */
	p[0]  = 1;
	p[mm] = 1;

	/* others  */
	switch (mm) {
	case 3:
	case 4:
	case 6:
		p[1] = 1;
		break;
	case 5:
	case 11:
		p[2] = 1;
		break;
	case 7:
	case 10:
		p[3] = 1;
		break;
	case 8:
		p[2] = p[3] = p[4] = 1;
		break;
	case 9:
		p[4] = 1;
		break;
	case 12:
		p[1] = p[4] = p[6] = 1;
		break;
	case 13:
		p[1] = p[3] = p[4] = 1;
		break;
	case 14:
		p[1] = p[6] = p[10] = 1;
		break;
	default:
		/* Error */
		return -EINVAL;
	}

	/* Build alpha ^ mm it will help to generate the field (primitiv) */
	alpha_to[mm] = 0;
	for (i = 0; i < mm; i++)
		if (p[i])
			alpha_to[mm] |= 1 << i;

	/*
	 * Then build elements from 0 to mm - 1. As degree is less than mm
	 * so it is just a logical shift.
	 */
	mask = 1;
	for (i = 0; i < mm; i++) {
		alpha_to[i] = mask;
		index_of[alpha_to[i]] = i;
		mask <<= 1;
	}

	index_of[alpha_to[mm]] = mm;

	/* use a mask to select the MSB bit of the LFSR */
	mask >>= 1;

	/* then finish the building */
	for (i = mm + 1; i <= nn; i++) {
		/* check if the msb bit of the lfsr is set */
		if (alpha_to[i - 1] & mask)
			alpha_to[i] = alpha_to[mm] ^
				((alpha_to[i - 1] ^ mask) << 1);
		else
			alpha_to[i] = alpha_to[i - 1] << 1;

		index_of[alpha_to[i]] = i % nn;
	}

	/* index of 0 is undefined in a multiplicative field */
	index_of[0] = -1;

	return 0;
}

static int __init atmel_pmecc_nand_init_params(struct device *dev,
					       struct atmel_nand_host *host)
{
	struct resource *iores;
	struct nand_chip *nand_chip = &host->nand_chip;
	struct mtd_info *mtd = nand_to_mtd(nand_chip);
	int cap, sector_size, err_no;
	int ret;

	cap = host->board->pmecc_corr_cap;
	sector_size = host->board->pmecc_sector_size;
	dev_info(host->dev, "Initialize PMECC params, cap: %d, sector: %d\n",
		 cap, sector_size);

	iores = dev_request_mem_resource(dev, 1);
	if (IS_ERR(iores))
		return PTR_ERR(iores);
	host->ecc = IOMEM(iores->start);

	iores = dev_request_mem_resource(dev, 2);
	if (IS_ERR(iores)) {
		dev_err(host->dev,
			"Can not get I/O resource for PMECC ERRLOC controller!\n");
		return PTR_ERR(iores);
	}
	host->pmerrloc_base = IOMEM(iores->start);

	iores = dev_request_mem_resource(dev, 3);
	if (IS_ERR(iores))
		return PTR_ERR(iores);
	host->pmecc_rom_base = IOMEM(iores->start);
	if (IS_ERR(host->pmecc_rom_base)) {
		/* Set pmecc_rom_base as the begin of gf table */
		int size = sector_size == 512 ? 0x2000 : 0x4000;
		pmecc_galois_table = xzalloc(2 * size * sizeof(uint16_t));
		host->pmecc_rom_base = pmecc_galois_table;
		ret = pmecc_build_galois_table((sector_size == 512) ?
						PMECC_GF_DIMENSION_13 :
						PMECC_GF_DIMENSION_14,
						host->pmecc_rom_base,
						host->pmecc_rom_base + (size * sizeof(int16_t)));
		if (ret) {
			dev_err(host->dev,
				"Can not get I/O resource for ROM!\n");
			return -EIO;
		}

		host->board->pmecc_lookup_table_offset = 0;
	}

	/* set ECC page size and oob layout */
	switch (mtd->writesize) {
	case 2048:
	case 4096:
	case 8192:
		host->pmecc_degree = (sector_size == 512) ?
					PMECC_GF_DIMENSION_13 : PMECC_GF_DIMENSION_14;
		host->pmecc_cw_len = (1 << host->pmecc_degree) - 1;
		host->pmecc_sector_number = mtd->writesize / sector_size;
		host->pmecc_bytes_per_sector = pmecc_get_ecc_bytes(
			cap, sector_size);
		host->pmecc_alpha_to = pmecc_get_alpha_to(host);
		host->pmecc_index_of = host->pmecc_rom_base +
			host->board->pmecc_lookup_table_offset;

		nand_chip->ecc.steps = host->pmecc_sector_number;
		nand_chip->ecc.bytes = host->pmecc_bytes_per_sector;
		nand_chip->ecc.size = sector_size;
		nand_chip->ecc.strength = cap;

		if (nand_chip->ecc.bytes > mtd->oobsize - 2) {
			dev_err(host->dev, "No room for ECC bytes\n");
			return -EINVAL;
		}
		mtd_set_ooblayout(mtd, nand_get_large_page_ooblayout());
		break;
	case 512:
	case 1024:
		/* TODO */
		dev_warn(host->dev,
			"Unsupported page size for PMECC, use Software ECC\n");
	default:
		/* page size not handled by HW ECC */
		/* switching back to soft ECC */
		nand_chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_SOFT;
		nand_chip->ecc.algo = NAND_ECC_ALGO_HAMMING;
		return 0;
	}

	/* Allocate data for PMECC computation */
	err_no = pmecc_data_alloc(host);
	if (err_no) {
		dev_err(host->dev,
				"Cannot allocate memory for PMECC computation!\n");
		return err_no;
	}

	nand_chip->options |= NAND_NO_SUBPAGE_WRITE;
	nand_chip->ecc.read_page = atmel_nand_pmecc_read_page;
	nand_chip->ecc.write_page = atmel_nand_pmecc_write_page;

	atmel_pmecc_core_init(mtd);

	return 0;
}

/*
 * Calculate HW ECC
 *
 * function called after a write
 *
 * mtd:        MTD block structure
 * dat:        raw data (unused)
 * ecc_code:   buffer for ECC
 */
static int atmel_nand_calculate(struct nand_chip *nand_chip,
		const u_char *dat, unsigned char *ecc_code)
{
	struct atmel_nand_host *host = nand_chip->priv;
	unsigned int ecc_value;

	/* get the first 2 ECC bytes */
	ecc_value = ecc_readl(host->ecc, PR);

	ecc_code[0] = ecc_value & 0xFF;
	ecc_code[1] = (ecc_value >> 8) & 0xFF;

	/* get the last 2 ECC bytes */
	ecc_value = ecc_readl(host->ecc, NPR) & ATMEL_ECC_NPARITY;

	ecc_code[2] = ecc_value & 0xFF;
	ecc_code[3] = (ecc_value >> 8) & 0xFF;

	return 0;
}

/*
 * HW ECC read page function
 *
 * mtd:        mtd info structure
 * chip:       nand chip info structure
 * buf:        buffer to store read data
 */
static int atmel_nand_read_page(struct nand_chip *chip, uint8_t *buf,
				int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct atmel_nand_host *host = chip->priv;
	int eccbytes = chip->ecc.bytes;
	uint32_t *eccpos = host->ecclayout->eccpos;
	uint8_t *p = buf;
	uint8_t *oob = chip->oob_poi;
	uint8_t *ecc_pos;
	int stat;

	nand_read_page_op(chip, page, 0, NULL, 0);

	/* read the page */
	chip->legacy.read_buf(chip, p, mtd->writesize);

	/* move to ECC position if needed */
	if (eccpos[0] != 0) {
		/* This only works on large pages
		 * because the ECC controller waits for
		 * NAND_CMD_RNDOUTSTART after the
		 * NAND_CMD_RNDOUT.
		 * anyway, for small pages, the eccpos[0] == 0
		 */
		chip->legacy.cmdfunc(chip, NAND_CMD_RNDOUT,
				mtd->writesize + eccpos[0], -1);
	}

	/* the ECC controller needs to read the ECC just after the data */
	ecc_pos = oob + eccpos[0];
	chip->legacy.read_buf(chip, ecc_pos, eccbytes);

	/* check if there's an error */
	stat = chip->ecc.correct(chip, p, oob, NULL);

	if (stat < 0)
		mtd->ecc_stats.failed++;
	else
		mtd->ecc_stats.corrected += stat;

	/* get back to oob start (end of page) */
	chip->legacy.cmdfunc(chip, NAND_CMD_RNDOUT, mtd->writesize, -1);

	/* read the oob */
	chip->legacy.read_buf(chip, oob, mtd->oobsize);

	return 0;
}

/*
 * HW ECC Correction
 *
 * function called after a read
 *
 * mtd:        MTD block structure
 * dat:        raw data read from the chip
 * read_ecc:   ECC from the chip (unused)
 * isnull:     unused
 *
 * Detect and correct a 1 bit error for a page
 */
static int atmel_nand_correct(struct nand_chip *nand_chip, u_char *dat,
		u_char *read_ecc, u_char *isnull)
{
	struct atmel_nand_host *host = nand_chip->priv;
	unsigned int ecc_status;
	unsigned int ecc_word, ecc_bit;

	/* get the status from the Status Register */
	ecc_status = ecc_readl(host->ecc, SR);

	/* if there's no error */
	if (likely(!(ecc_status & ATMEL_ECC_RECERR)))
		return 0;

	/* get error bit offset (4 bits) */
	ecc_bit = ecc_readl(host->ecc, PR) & ATMEL_ECC_BITADDR;
	/* get word address (12 bits) */
	ecc_word = ecc_readl(host->ecc, PR) & ATMEL_ECC_WORDADDR;
	ecc_word >>= 4;

	/* if there are multiple errors */
	if (ecc_status & ATMEL_ECC_MULERR) {
		/* check if it is a freshly erased block
		 * (filled with 0xff) */
		if ((ecc_bit == ATMEL_ECC_BITADDR)
				&& (ecc_word == (ATMEL_ECC_WORDADDR >> 4))) {
			/* the block has just been erased, return OK */
			return 0;
		}
		/* it doesn't seems to be a freshly
		 * erased block.
		 * We can't correct so many errors */
		dev_dbg(host->dev, "atmel_nand : multiple errors detected."
				" Unable to correct.\n");
		return -EIO;
	}

	/* if there's a single bit error : we can correct it */
	if (ecc_status & ATMEL_ECC_ECCERR) {
		/* there's nothing much to do here.
		 * the bit error is on the ECC itself.
		 */
		dev_dbg(host->dev, "atmel_nand : one bit error on ECC code."
				" Nothing to correct\n");
		return 0;
	}

	dev_dbg(host->dev, "atmel_nand : one bit error on data."
			" (word offset in the page :"
			" 0x%x bit offset : 0x%x)\n",
			ecc_word, ecc_bit);
	/* correct the error */
	if (nand_chip->options & NAND_BUSWIDTH_16) {
		/* 16 bits words */
		((unsigned short *) dat)[ecc_word] ^= (1 << ecc_bit);
	} else {
		/* 8 bits words */
		dat[ecc_word] ^= (1 << ecc_bit);
	}
	dev_dbg(host->dev, "atmel_nand : error corrected\n");
	return 1;
}

/*
 * Enable HW ECC : unused on most chips
 */
static void atmel_nand_hwctl(struct nand_chip *nand_chip, int mode)
{
}

static int atmel_nand_of_init(struct atmel_nand_host *host, struct device_node *np)
{
	u32 val;
	u32 offset[2];
	int ecc_mode;
	struct atmel_nand_data *board = host->board;
	enum of_gpio_flags flags = 0;

	if (!IS_ENABLED(CONFIG_OFDEVICE))
		return -ENOSYS;

	if (of_property_read_u32(np, "atmel,nand-addr-offset", &val) == 0) {
		if (val >= 32) {
			dev_err(host->dev, "invalid addr-offset %u\n", val);
			return -EINVAL;
		}
		board->ale = val;
	}

	if (of_property_read_u32(np, "atmel,nand-cmd-offset", &val) == 0) {
		if (val >= 32) {
			dev_err(host->dev, "invalid cmd-offset %u\n", val);
			return -EINVAL;
		}
		board->cle = val;
	}

	ecc_mode = of_get_nand_ecc_mode(np);

	board->ecc_mode = ecc_mode < 0 ? NAND_ECC_SOFT : ecc_mode;

	board->on_flash_bbt = of_get_nand_on_flash_bbt(np);

	if (of_get_nand_bus_width(np) == 16)
		board->bus_width_16 = 1;

	board->rdy_pin = of_get_gpio_flags(np, 0, &flags);
	board->enable_pin = of_get_gpio(np, 1);
	board->det_pin = of_get_gpio(np, 2);

	board->has_pmecc = of_property_read_bool(np, "atmel,has-pmecc");

	if (!(board->ecc_mode == NAND_ECC_HW) || !board->has_pmecc)
		return 0;	/* Not using PMECC */

	/* use PMECC, get correction capability, sector size and lookup
	* table offset.
	* If correction bits and sector size are not specified, then
	*   find
	* them from NAND ONFI parameters.
	*/
	if (of_property_read_u32(np, "atmel,pmecc-cap", &val) == 0) {
		if ((val != 2) && (val != 4) && (val != 8) && (val != 12) && (val != 24)) {
			dev_err(host->dev, "Unsupported PMECC correction capability: %d"
					" should be 2, 4, 8, 12 or 24\n", val);
			return -EINVAL;
		}

		board->pmecc_corr_cap = (u8)val;
	}

	if (of_property_read_u32(np, "atmel,pmecc-sector-size", &val) == 0) {
		if ((val != 512) && (val != 1024)) {
				dev_err(host->dev, "Unsupported PMECC sector size: %d"
					" should be 512 or 1024 bytes\n", val);
			return -EINVAL;
		}

		board->pmecc_sector_size = (u16)val;
	}

	if (of_property_read_u32_array(np, "atmel,pmecc-lookup-table-offset", offset, 2) != 0) {
		dev_err(host->dev, "Cannot get PMECC lookup table offset\n");
		return -EINVAL;
	}

	if (!offset[0] && !offset[1]) {
		dev_err(host->dev, "Invalid PMECC lookup table offset\n");
		return -EINVAL;
	}

	board->pmecc_lookup_table_offset = (board->pmecc_sector_size == 512) ? offset[0] : offset[1];

	return 0;
}

static int atmel_hw_nand_init_params(struct device *dev,
					 struct atmel_nand_host *host)
{
	struct resource *iores;
	struct nand_chip *nand_chip = &host->nand_chip;
	struct mtd_info *mtd = nand_to_mtd(nand_chip);

	iores = dev_request_mem_resource(dev, 1);
	if (IS_ERR(iores))
		return PTR_ERR(iores);
	host->ecc = IOMEM(iores->start);

	/* ECC is calculated for the whole page (1 step) */
	nand_chip->ecc.size = mtd->writesize;

	/* set ECC page size and oob layout */
	switch (mtd->writesize) {
	case 512:
		host->ecclayout = &atmel_oobinfo_small;
		mtd_set_ecclayout(mtd, host->ecclayout);
		ecc_writel(host->ecc, MR, ATMEL_ECC_PAGESIZE_528);
		break;
	case 1024:
		host->ecclayout = &atmel_oobinfo_large;
		mtd_set_ecclayout(mtd, host->ecclayout);
		ecc_writel(host->ecc, MR, ATMEL_ECC_PAGESIZE_1056);
		break;
	case 2048:
		host->ecclayout = &atmel_oobinfo_large;
		mtd_set_ecclayout(mtd, host->ecclayout);
		ecc_writel(host->ecc, MR, ATMEL_ECC_PAGESIZE_2112);
		break;
	case 4096:
		host->ecclayout = &atmel_oobinfo_large;
		mtd_set_ecclayout(mtd, host->ecclayout);
		ecc_writel(host->ecc, MR, ATMEL_ECC_PAGESIZE_4224);
		break;
	default:
		/* page size not handled by HW ECC */
		/* switching back to soft ECC */
		nand_chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_SOFT;
		return 0;
	}

	/* set up for HW ECC */
	nand_chip->ecc.calculate = atmel_nand_calculate;
	nand_chip->ecc.correct = atmel_nand_correct;
	nand_chip->ecc.hwctl = atmel_nand_hwctl;
	nand_chip->ecc.read_page = atmel_nand_read_page;
	nand_chip->ecc.bytes = 4;
	nand_chip->ecc.strength = 1;

	return 0;
}

/*
 * Probe for the NAND device.
 */
static int __init atmel_nand_probe(struct device *dev)
{
	struct resource *iores;
	struct atmel_nand_data *pdata = NULL;
	struct atmel_nand_host *host;
	struct mtd_info *mtd;
	struct nand_chip *nand_chip;
	int res = 0;

	/* Allocate memory for the device structure (and zero it) */
	host = kzalloc(sizeof(struct atmel_nand_host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	pdata = kzalloc(sizeof(struct atmel_nand_data), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	iores = dev_request_mem_resource(dev, 0);
	if (IS_ERR(iores))
		return PTR_ERR(iores);
	host->io_base = IOMEM(iores->start);

	nand_chip = &host->nand_chip;
	mtd = nand_to_mtd(nand_chip);
	host->board = pdata;
	host->dev = dev;

	if (dev->of_node) {
		res = atmel_nand_of_init(host, dev->of_node);
		if (res)
			goto err_no_card;
	} else {
		memcpy(host->board, dev->platform_data, sizeof(struct atmel_nand_data));
	}

	nand_chip->priv = host;		/* link the private data structures */
	mtd->dev.parent = dev;

	/* Set address of NAND IO lines */
	nand_chip->legacy.IO_ADDR_R = host->io_base;
	nand_chip->legacy.IO_ADDR_W = host->io_base;
	nand_chip->legacy.cmd_ctrl = atmel_nand_cmd_ctrl;

	if (gpio_is_valid(host->board->rdy_pin)) {
		res = gpio_request(host->board->rdy_pin, "nand_rdy");
		if (res < 0) {
			dev_err(dev, "can't request rdy gpio %d\n",
				host->board->rdy_pin);
			goto err_no_card;
		}

		res = gpio_direction_input(host->board->rdy_pin);
		if (res < 0) {
			dev_err(dev,
				"can't request input direction rdy gpio %d\n",
				host->board->rdy_pin);
			goto err_no_card;
		}

		nand_chip->legacy.dev_ready = atmel_nand_device_ready;
	}

	if (gpio_is_valid(host->board->enable_pin)) {
		res = gpio_request(host->board->enable_pin, "nand_enable");
		if (res < 0) {
			dev_err(dev,
				"can't request enable gpio %d\n",
				host->board->enable_pin);
			goto err_no_card;
		}

		res = gpio_direction_output(host->board->enable_pin, 1);
		if (res < 0) {
			dev_err(dev,
				"can't request output direction enable gpio %d\n",
				host->board->enable_pin);
			goto err_no_card;
		}
	}

	nand_chip->ecc.strength = pdata->ecc_strength ? : 1;
	nand_chip->ecc.size = 1 << (pdata->ecc_size_shift ? : 9);

	if (pdata->ecc_mode == NAND_ECC_SOFT) {
		nand_chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_SOFT;
		nand_chip->ecc.algo = NAND_ECC_ALGO_HAMMING;
	} else {
		nand_chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	}

	nand_chip->legacy.chip_delay = 40;		/* 40us command delay time */

	if (host->board->bus_width_16) {	/* 16-bit bus width */
		nand_chip->options |= NAND_BUSWIDTH_16;
		nand_chip->legacy.read_buf = atmel_read_buf16;
		nand_chip->legacy.write_buf = atmel_write_buf16;
	} else {
		nand_chip->legacy.read_buf = atmel_read_buf;
		nand_chip->legacy.write_buf = atmel_write_buf;
	}

	atmel_nand_enable(host);

	if (gpio_is_valid(host->board->det_pin)) {
		res = gpio_request(host->board->det_pin, "nand_det");
		if (res < 0) {
			dev_err(dev, "can't request det gpio %d\n",
				host->board->det_pin);
			goto err_no_card;
		}

		res = gpio_direction_input(host->board->det_pin);
		if (res < 0) {
			dev_err(dev,
				"can't request input direction det gpio %d\n",
				host->board->det_pin);
			goto err_no_card;
		}

		if (gpio_get_value(host->board->det_pin)) {
			dev_info(dev, "No SmartMedia card inserted.\n");
			res = -ENXIO;
			goto err_no_card;
		}
	}

	if (host->board->on_flash_bbt) {
		dev_info(dev, "Use On Flash BBT\n");
		nand_chip->bbt_options |= NAND_BBT_USE_FLASH;
	}


	/* first scan to find the device and get the page size */
	if (nand_scan_ident(nand_chip, 1, NULL)) {
		res = -ENXIO;
		goto err_scan_ident;
	}

	host->ecc_code = xmalloc(mtd->oobsize);

	if (nand_chip->ecc.engine_type == NAND_ECC_ENGINE_TYPE_ON_HOST) {
		if (IS_ENABLED(CONFIG_NAND_ATMEL_PMECC) && pdata->has_pmecc)
			res = atmel_pmecc_nand_init_params(dev, host);
		else
			res = atmel_hw_nand_init_params(dev, host);

		if (res != 0)
			goto err_hw_ecc;
	}

	/* second phase scan */
	if (nand_scan_tail(nand_chip)) {
		res = -ENXIO;
		goto err_scan_tail;
	}

	add_mtd_nand_device(mtd, "nand");

	if (!res)
		return res;

err_scan_tail:
err_hw_ecc:
err_scan_ident:
err_no_card:
	atmel_nand_disable(host);
	kfree(pdata);
	kfree(host);
	return res;
}

static struct of_device_id atmel_nand_dt_ids[] = {
	{ .compatible = "atmel,at91rm9200-nand" },
	{ /* sentinel */ }
};

static struct driver atmel_nand_driver = {
	.name	= "atmel_nand",
	.probe	= atmel_nand_probe,
	.of_compatible	= DRV_OF_COMPAT(atmel_nand_dt_ids),
};
device_platform_driver(atmel_nand_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rick Bronson");
MODULE_DESCRIPTION("NAND/SmartMedia driver for AT91 / AVR32");
