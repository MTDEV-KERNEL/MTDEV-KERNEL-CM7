/*
 *  linux/drivers/mmc/host/msm_sdcc.c - Qualcomm MSM 7X00A SDCC Driver
 *
 *  Copyright (C) 2007 Google Inc,
 *  Copyright (C) 2003 Deep Blue Solutions, Ltd, All Rights Reserved.
 *  Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Based on mmci.c
 *
 * Author: San Mehat (san@android.com)
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/log2.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>
#include <linux/clk.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/memory.h>
#include <linux/pm_runtime.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>

#include <asm/cacheflush.h>
#include <asm/div64.h>
#include <asm/sizes.h>

#include <asm/mach/mmc.h>
#include <mach/msm_iomap.h>
#include <mach/clk.h>
#include <mach/dma.h>
#include <mach/htc_pwrsink.h>
#include <asm/uaccess.h>


#include "msm_sdcc.h"
#include <mach/gpio.h>  //DIV5-PHONE-JH-POWER SAVING MODE-01+
#if defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
#include "../../../arch/arm/mach-msm/smd_private.h"    //DIV5-CONN-MW-POWER SAVING MODE-01+
#include <linux/workqueue.h>//DIV5-CONN-MW-POWER SAVING MODE-01+
#endif
#define DRIVER_NAME "msm-sdcc"

#define DBG(host, fmt, args...)	\
	pr_debug("%s: %s: " fmt "\n", mmc_hostname(host->mmc), __func__ , args)

#define IRQ_DEBUG 0
#define SUPPORT_SD_REMOVAL_TURNOFF	10

#ifdef CONFIG_DEBUG_FS
#undef CONFIG_DEBUG_FS
#endif

#if defined(CONFIG_DEBUG_FS)
static void msmsdcc_dbg_createhost(struct msmsdcc_host *);
static struct dentry *debugfs_dir;
static struct dentry *debugfs_file;
static int  msmsdcc_dbg_init(void);
#endif

// KD 2011-10-28 external definition for the libra driver flag
// Note: This is initialized to zero (*not* loaded)
static int libra_loaded = 0;
EXPORT_SYMBOL(libra_loaded);

static unsigned int msmsdcc_pwrsave = 1;
static int support_sd_removal_turnoff = 1;

#define DUMMY_52_STATE_NONE		0
#define DUMMY_52_STATE_SENT		1

static struct mmc_command dummy52cmd;
static struct mmc_request dummy52mrq = {
	.cmd = &dummy52cmd,
	.data = NULL,
	.stop = NULL,
};
static struct mmc_command dummy52cmd = {
	.opcode = SD_IO_RW_DIRECT,
	.flags = MMC_RSP_PRESENT,
	.data = NULL,
	.mrq = &dummy52mrq,
};

#define VERBOSE_COMMAND_TIMEOUTS	0

#if IRQ_DEBUG == 1
static char *irq_status_bits[] = { "cmdcrcfail", "datcrcfail", "cmdtimeout",
				   "dattimeout", "txunderrun", "rxoverrun",
				   "cmdrespend", "cmdsent", "dataend", NULL,
				   "datablkend", "cmdactive", "txactive",
				   "rxactive", "txhalfempty", "rxhalffull",
				   "txfifofull", "rxfifofull", "txfifoempty",
				   "rxfifoempty", "txdataavlbl", "rxdataavlbl",
				   "sdiointr", "progdone", "atacmdcompl",
				   "sdiointrope", "ccstimeout", NULL, NULL,
				   NULL, NULL, NULL };

static void
msmsdcc_print_status(struct msmsdcc_host *host, char *hdr, uint32_t status)
{
	int i;

	pr_debug("%s-%s ", mmc_hostname(host->mmc), hdr);
	for (i = 0; i < 32; i++) {
		if (status & (1 << i))
			pr_debug("%s ", irq_status_bits[i]);
	}
	pr_debug("\n");
}
#endif

static void
msmsdcc_start_command(struct msmsdcc_host *host, struct mmc_command *cmd,
		      u32 c);

static void dump_sdcc_regs(struct msmsdcc_host *host)
{
	pr_info("%s: MCI_POWER 	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCIPOWER));
	pr_info("%s: MCI_CLK 	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCICLOCK));
	pr_info("%s: MCI_CMD 	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCICOMMAND));
	pr_info("%s: MCI_DATATIMER 	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCIDATATIMER));
	pr_info("%s: MCI_DATALEN 	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCIDATALENGTH));
	pr_info("%s: MCI_DATACTL	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCIDATACTRL));
	pr_info("%s: MCI_RESP0 	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCIRESPONSE0));
	pr_info("%s: MCI_STATUS	%.8x\n", mmc_hostname(host->mmc), readl(host->base + MMCISTATUS));
}

static void msmsdcc_reset_and_restore(struct msmsdcc_host *host)
{
	u32	mci_clk = 0;
	u32	mci_mask0 = 0;
	int ret;

	/* Save the controller state */
	mci_clk = readl(host->base + MMCICLOCK);
	mci_mask0 = readl(host->base + MMCIMASK0);

	/* Reset the controller */
	ret = clk_reset(host->clk, CLK_RESET_ASSERT);
	if (ret)
		pr_err("%s: Clock assert failed at %u Hz with err %d\n",
				mmc_hostname(host->mmc), host->clk_rate, ret);

	ret = clk_reset(host->clk, CLK_RESET_DEASSERT);
	if (ret)
		pr_err("%s: Clock deassert failed at %u Hz with err %d\n",
				mmc_hostname(host->mmc), host->clk_rate, ret);

	pr_info("%s: Controller has been reset\n", mmc_hostname(host->mmc));

	/* Restore the contoller state */
	writel(host->pwr, host->base + MMCIPOWER);
	writel(mci_clk, host->base + MMCICLOCK);
	writel(mci_mask0, host->base + MMCIMASK0);
	ret = clk_set_rate(host->clk, host->clk_rate);
	if (ret)
		pr_err("%s: Failed to set clk rate %u Hz. err %d\n",
				mmc_hostname(host->mmc), host->clk_rate, ret);
	if (host->mmc->update_stats) {
		pr_info("%s: SDCC registers after reset\n", mmc_hostname(host->mmc));
		dump_sdcc_regs(host);
	}
}

static int
msmsdcc_request_end(struct msmsdcc_host *host, struct mmc_request *mrq)
{
	int retval = 0;

	BUG_ON(host->curr.data);

	host->curr.mrq = NULL;
	host->curr.cmd = NULL;

	if (mrq->data)
		mrq->data->bytes_xfered = host->curr.data_xfered;
	if (mrq->cmd->error == -ETIMEDOUT)
		mdelay(5);

#ifdef CONFIG_MMC_MSM_PROG_DONE_SCAN
	if ((mrq->cmd->opcode == SD_IO_RW_EXTENDED) &&
			(mrq->cmd->arg & 0x80000000)) {
		/* If its a write and a cmd53 set the prog_scan flag. */
		host->prog_scan = 1;
		/* Send STOP to let the SDCC know to stop. */
		writel(MCI_CSPM_MCIABORT, host->base + MMCICOMMAND);
		retval = 1;
	}
	if (mrq->cmd->opcode == SD_IO_RW_DIRECT) {
		/* Ok the cmd52 following a cmd53 is received */
		/* clear all the flags. */
		host->prog_scan = 0;
		host->prog_enable = 0;
	}
#endif
	/*
	 * Need to drop the host lock here; mmc_request_done may call
	 * back into the driver...
	 */
	spin_unlock(&host->lock);
	mmc_request_done(host->mmc, mrq);
	spin_lock(&host->lock);

	return retval;
}

static void
msmsdcc_stop_data(struct msmsdcc_host *host)
{
	host->curr.data = NULL;
	host->curr.got_dataend = 0;
}

static inline uint32_t msmsdcc_fifo_addr(struct msmsdcc_host *host)
{
	return host->memres->start + MMCIFIFO;
}

static inline void msmsdcc_delay(struct msmsdcc_host *host)
{
	udelay(1 + ((3 * USEC_PER_SEC) /
		(host->clk_rate ? host->clk_rate : host->plat->msmsdcc_fmin)));
}

static inline void
msmsdcc_start_command_exec(struct msmsdcc_host *host, u32 arg, u32 c)
{
	struct mmc_host *mmc = host->mmc;
	writel(arg, host->base + MMCIARGUMENT);
	msmsdcc_delay(host);
	writel(c, host->base + MMCICOMMAND);
	if (host->mmc->update_stats)
		mmc->entries[mmc->id].mci_cmd_after_enabling_cpsm = readl(host->base + MMCICOMMAND);
}

static void
msmsdcc_dma_exec_func(struct msm_dmov_cmd *cmd)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)cmd->user;
	struct mmc_host *mmc = host->mmc;

	if (mmc->update_stats)
		mmc->entries[mmc->id].timer_start = ktime_get();

	writel(host->cmd_timeout, host->base + MMCIDATATIMER);
	writel((unsigned int)host->curr.xfer_size, host->base + MMCIDATALENGTH);
	writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) |
	       host->cmd_pio_irqmask, host->base + MMCIMASK0);
	msmsdcc_delay(host);	/* Allow data parms to be applied */
	if (host->mmc->update_stats) {
		mmc->entries[mmc->id].mci_sts_before_datactl = readl(host->base + MMCISTATUS);
		mmc->entries[mmc->id].mci_mask0_before_datactl = readl(host->base + MMCIMASK0);
		mmc->entries[mmc->id].mci_datatimer_before_datactl = readl(host->base + MMCIDATATIMER);
		mmc->entries[mmc->id].mci_datalen_before_datactl = readl(host->base + MMCIDATALENGTH);
		mmc->entries[mmc->id].mci_datactl_before_enabling_datactl = readl(host->base + MMCIDATACTRL);
		mmc->entries[mmc->id].mci_clk_before_datactl = readl(host->base + MMCICLOCK);
		mmc->entries[mmc->id].mci_pwr_before_datactl = readl(host->base + MMCIPOWER);
		mmc->entries[mmc->id].mci_cmd_before_datactl = readl(host->base + MMCICOMMAND);
	}
	writel(host->cmd_datactrl, host->base + MMCIDATACTRL);
	msmsdcc_delay(host);	/* Force delay prior to ADM or command */

	if (host->mmc->update_stats) {
		mmc->entries[mmc->id].mci_data_ctl_after_dpsm = readl(host->base + MMCIDATACTRL);
		mmc->entries[mmc->id].mci_sts_after_enabling_dpsm = readl(host->base + MMCISTATUS);
	}
	if (host->cmd_cmd) {
		msmsdcc_start_command_exec(host,
			(u32)host->cmd_cmd->arg, (u32)host->cmd_c);
	}
}

static void
msmsdcc_dma_complete_tlet(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	unsigned long		flags;
	struct mmc_request	*mrq;
	struct mmc_host *mmc = host->mmc;

	spin_lock_irqsave(&host->lock, flags);
	mrq = host->curr.mrq;
	BUG_ON(!mrq);

	if (!(host->dma.result & DMOV_RSLT_VALID)) {
		pr_err("msmsdcc: Invalid DataMover result\n");
		goto out;
	}

	if (host->dma.result & DMOV_RSLT_DONE) {
		host->curr.data_xfered = host->curr.xfer_size;
	} else {
		/* Error or flush  */
		if (host->dma.result & DMOV_RSLT_ERROR)
			pr_err("%s: DMA error (0x%.8x)\n",
			       mmc_hostname(host->mmc), host->dma.result);
		if (host->dma.result & DMOV_RSLT_FLUSH)
			pr_err("%s: DMA channel flushed (0x%.8x)\n",
			       mmc_hostname(host->mmc), host->dma.result);
		if (host->dma.err) {
			pr_err("Flush data: %.8x %.8x %.8x %.8x %.8x %.8x\n",
			       host->dma.err->flush[0], host->dma.err->flush[1],
			       host->dma.err->flush[2], host->dma.err->flush[3],
			       host->dma.err->flush[4],
			       host->dma.err->flush[5]);
			msmsdcc_reset_and_restore(host);
		}
		if (!mrq->data->error)
			mrq->data->error = -EIO;
	}
	dma_unmap_sg(mmc_dev(host->mmc), host->dma.sg, host->dma.num_ents,
		     host->dma.dir);

	if (host->curr.user_pages) {
		struct scatterlist *sg = host->dma.sg;
		int i;

		for (i = 0; i < host->dma.num_ents; i++, sg++)
			flush_dcache_page(sg_page(sg));
	}

	host->dma.sg = NULL;
	host->dma.busy = 0;

	if (host->curr.got_dataend || mrq->data->error) {

		if (mrq->data->error && !(host->curr.got_dataend)) {
			pr_info("%s: Worked around bug 1535304\n",
			       mmc_hostname(host->mmc));
		}
		/*
		 * If we've already gotten our DATAEND / DATABLKEND
		 * for this request, then complete it through here.
		 */
		msmsdcc_stop_data(host);

		if (!mrq->data->error) {
			host->curr.data_xfered = host->curr.xfer_size;
			if (host->mmc->update_stats) {
				if (mrq->data->flags & MMC_DATA_READ) {
					mmc->entries[mmc->id].read_data_cnt = host->curr.data_xfered;
					mmc->entries[mmc->id].mci_sts_after_reading_alldata = readl(host->base + MMCISTATUS);
				}
			}
		}
		if (!mrq->data->stop || mrq->cmd->error) {
			host->curr.mrq = NULL;
			host->curr.cmd = NULL;
			mrq->data->bytes_xfered = host->curr.data_xfered;

			spin_unlock_irqrestore(&host->lock, flags);

#ifdef CONFIG_MMC_MSM_PROG_DONE_SCAN
			if ((mrq->cmd->opcode == SD_IO_RW_EXTENDED)
				&& (mrq->cmd->arg & 0x80000000)) {
				/* set the prog_scan in a cmd53.*/
				host->prog_scan = 1;
				/* Send STOP to let the SDCC know to stop. */
				writel(MCI_CSPM_MCIABORT,
						host->base + MMCICOMMAND);
			}
#endif
			mmc_request_done(host->mmc, mrq);
			return;
		} else
			msmsdcc_start_command(host, mrq->data->stop, 0);
	}

out:
	spin_unlock_irqrestore(&host->lock, flags);
	return;
}

static void
msmsdcc_dma_complete_func(struct msm_dmov_cmd *cmd,
			  unsigned int result,
			  struct msm_dmov_errdata *err)
{
	struct msmsdcc_dma_data	*dma_data =
		container_of(cmd, struct msmsdcc_dma_data, hdr);
	struct msmsdcc_host *host = dma_data->host;

	dma_data->result = result;
	dma_data->err = err;

	tasklet_schedule(&host->dma_tlet);
}

static int validate_dma(struct msmsdcc_host *host, struct mmc_data *data)
{
	if (host->dma.channel == -1)
		return -ENOENT;

	if ((data->blksz * data->blocks) < MCI_FIFOSIZE)
		return -EINVAL;
	if ((data->blksz * data->blocks) % MCI_FIFOSIZE)
		return -EINVAL;
	return 0;
}

static int msmsdcc_config_dma(struct msmsdcc_host *host, struct mmc_data *data)
{
	struct msmsdcc_nc_dmadata *nc;
	dmov_box *box;
	uint32_t rows;
	uint32_t crci;
	unsigned int n;
	int i, rc;
	struct scatterlist *sg = data->sg;

	rc = validate_dma(host, data);
	if (rc)
		return rc;

	host->dma.sg = data->sg;
	host->dma.num_ents = data->sg_len;

	BUG_ON(host->dma.num_ents > NR_SG); /* Prevent memory corruption */

	nc = host->dma.nc;

	if (host->pdev_id == 1)
		crci = DMOV_SDC1_CRCI;
	else if (host->pdev_id == 2)
		crci = DMOV_SDC2_CRCI;
	else if (host->pdev_id == 3)
		crci = DMOV_SDC3_CRCI;
	else if (host->pdev_id == 4)
		crci = DMOV_SDC4_CRCI;
#ifdef DMOV_SDC5_CRCI
	else if (host->pdev_id == 5)
		crci = DMOV_SDC5_CRCI;
#endif
	else {
		host->dma.sg = NULL;
		host->dma.num_ents = 0;
		return -ENOENT;
	}

	if (data->flags & MMC_DATA_READ)
		host->dma.dir = DMA_FROM_DEVICE;
	else
		host->dma.dir = DMA_TO_DEVICE;

	/* host->curr.user_pages = (data->flags & MMC_DATA_USERPAGE); */
	host->curr.user_pages = 0;
	box = &nc->cmd[0];
	for (i = 0; i < host->dma.num_ents; i++) {
		box->cmd = CMD_MODE_BOX;

		/* Initialize sg dma address */
		sg->dma_address = page_to_dma(mmc_dev(host->mmc), sg_page(sg))
					+ sg->offset;

		if (i == (host->dma.num_ents - 1))
			box->cmd |= CMD_LC;
		rows = (sg_dma_len(sg) % MCI_FIFOSIZE) ?
			(sg_dma_len(sg) / MCI_FIFOSIZE) + 1 :
			(sg_dma_len(sg) / MCI_FIFOSIZE) ;

		if (data->flags & MMC_DATA_READ) {
			box->src_row_addr = msmsdcc_fifo_addr(host);
			box->dst_row_addr = sg_dma_address(sg);

			box->src_dst_len = (MCI_FIFOSIZE << 16) |
					   (MCI_FIFOSIZE);
			box->row_offset = MCI_FIFOSIZE;

			box->num_rows = rows * ((1 << 16) + 1);
			box->cmd |= CMD_SRC_CRCI(crci);
		} else {
			box->src_row_addr = sg_dma_address(sg);
			box->dst_row_addr = msmsdcc_fifo_addr(host);

			box->src_dst_len = (MCI_FIFOSIZE << 16) |
					   (MCI_FIFOSIZE);
			box->row_offset = (MCI_FIFOSIZE << 16);

			box->num_rows = rows * ((1 << 16) + 1);
			box->cmd |= CMD_DST_CRCI(crci);
		}
		box++;
		sg++;
	}

	/* location of command block must be 64 bit aligned */
	BUG_ON(host->dma.cmd_busaddr & 0x07);

	nc->cmdptr = (host->dma.cmd_busaddr >> 3) | CMD_PTR_LP;
	host->dma.hdr.cmdptr = DMOV_CMD_PTR_LIST |
			       DMOV_CMD_ADDR(host->dma.cmdptr_busaddr);
	host->dma.hdr.complete_func = msmsdcc_dma_complete_func;
	host->dma.hdr.crci_mask = msm_dmov_build_crci_mask(1, crci);

	n = dma_map_sg(mmc_dev(host->mmc), host->dma.sg,
			host->dma.num_ents, host->dma.dir);
	/* dsb inside dma_map_sg will write nc out to mem as well */

	if (n != host->dma.num_ents) {
		pr_err("%s: Unable to map in all sg elements\n",
		       mmc_hostname(host->mmc));
		host->dma.sg = NULL;
		host->dma.num_ents = 0;
		return -ENOMEM;
	}

	return 0;
}

static void
msmsdcc_start_command_deferred(struct msmsdcc_host *host,
				struct mmc_command *cmd, u32 *c)
{
	DBG(host, "op %02x arg %08x flags %08x\n",
	    cmd->opcode, cmd->arg, cmd->flags);

	*c |= (cmd->opcode | MCI_CPSM_ENABLE);

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			*c |= MCI_CPSM_LONGRSP;
		*c |= MCI_CPSM_RESPONSE;
	}

	if (/*interrupt*/0)
		*c |= MCI_CPSM_INTERRUPT;

	if ((((cmd->opcode == 17) || (cmd->opcode == 18))  ||
	     ((cmd->opcode == 24) || (cmd->opcode == 25))) ||
	      (cmd->opcode == 53))
		*c |= MCI_CSPM_DATCMD;

	if (host->prog_scan && (cmd->opcode == 12)) {
		*c |= MCI_CPSM_PROGENA;
		host->prog_enable = 1;
	}

#ifdef CONFIG_MMC_MSM_PROG_DONE_SCAN
	if ((cmd->opcode == SD_IO_RW_DIRECT)
			&& (host->prog_scan == 1)) {
		*c |= MCI_CPSM_PROGENA;
		host->prog_enable = 1;
		mod_timer(&host->prog_timer, jiffies + 2*HZ);
	}
#endif

	if (cmd == cmd->mrq->stop)
		*c |= MCI_CSPM_MCIABORT;

	if (host->curr.cmd != NULL) {
		pr_err("%s: Overlapping command requests\n",
		       mmc_hostname(host->mmc));
	}
	host->curr.cmd = cmd;
}

static void
msmsdcc_start_data(struct msmsdcc_host *host, struct mmc_data *data,
			struct mmc_command *cmd, u32 c)
{
	unsigned int datactrl, timeout;
	unsigned long long clks;
	void __iomem *base = host->base;
	unsigned int pio_irqmask = 0;
	struct mmc_host *mmc = host->mmc;

	host->curr.data = data;
	host->curr.xfer_size = data->blksz * data->blocks;
	host->curr.xfer_remain = host->curr.xfer_size;
	host->curr.data_xfered = 0;
	host->curr.got_dataend = 0;

	memset(&host->pio, 0, sizeof(host->pio));

	datactrl = MCI_DPSM_ENABLE | (data->blksz << 4);

	if (!msmsdcc_config_dma(host, data))
		datactrl |= MCI_DPSM_DMAENABLE;
	else {
		host->pio.sg = data->sg;
		host->pio.sg_len = data->sg_len;
		host->pio.sg_off = 0;

		if (data->flags & MMC_DATA_READ) {
			pio_irqmask = MCI_RXFIFOHALFFULLMASK;
			if (host->curr.xfer_remain < MCI_FIFOSIZE)
				pio_irqmask |= MCI_RXDATAAVLBLMASK;
		} else
			pio_irqmask = MCI_TXFIFOHALFEMPTYMASK;
	}

	if (data->flags & MMC_DATA_READ)
		datactrl |= MCI_DPSM_DIRECTION;

	clks = (unsigned long long)data->timeout_ns * host->clk_rate;
	do_div(clks, 1000000000UL);
	timeout = data->timeout_clks + (unsigned int)clks*2 ;

	if (datactrl & MCI_DPSM_DMAENABLE) {
		/* Save parameters for the exec function */
		host->cmd_timeout = timeout;
		host->cmd_pio_irqmask = pio_irqmask;
		host->cmd_datactrl = datactrl;
		host->cmd_cmd = cmd;

		host->dma.hdr.exec_func = msmsdcc_dma_exec_func;
		host->dma.hdr.user = (void *)host;
		host->dma.busy = 1;

		if (cmd) {
			msmsdcc_start_command_deferred(host, cmd, &c);
			host->cmd_c = c;
		}
		dsb();
		msm_dmov_enqueue_cmd_ext(host->dma.channel, &host->dma.hdr);
		if (data->flags & MMC_DATA_WRITE)
			host->prog_scan = 1;
		if (data->flags & MMC_DATA_READ)
			mmc->entries[mmc->id].req_rsize = host->curr.xfer_size;
	} else {
		if (mmc->update_stats)
			mmc->entries[mmc->id].timer_start = ktime_get();

		writel(timeout, base + MMCIDATATIMER);

		writel(host->curr.xfer_size, base + MMCIDATALENGTH);

		writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) |
				pio_irqmask, host->base + MMCIMASK0);
		msmsdcc_delay(host);	/* Allow parms to be applied */
		if (host->mmc->update_stats) {
			mmc->entries[mmc->id].mci_sts_before_datactl = readl(host->base + MMCISTATUS);
			mmc->entries[mmc->id].mci_mask0_before_datactl = readl(host->base + MMCIMASK0);
			mmc->entries[mmc->id].mci_datatimer_before_datactl = readl(host->base + MMCIDATATIMER);
			mmc->entries[mmc->id].mci_datalen_before_datactl = readl(host->base + MMCIDATALENGTH);
			mmc->entries[mmc->id].mci_datactl_before_enabling_datactl = readl(host->base + MMCIDATACTRL);
			mmc->entries[mmc->id].mci_clk_before_datactl = readl(host->base + MMCICLOCK);
			mmc->entries[mmc->id].mci_pwr_before_datactl = readl(host->base + MMCIPOWER);
			mmc->entries[mmc->id].mci_cmd_before_datactl = readl(host->base + MMCICOMMAND);
		}
//sw2-6-1-RH-Wlan_Reset8-00+[
		if (host->mmc->update_stats) {
            int counter = 0;
			while ((readl(host->base + MMCISTATUS) & 0x3000)) {
                if(counter++ == 1000) {
                    pr_info("mmc: oops! status is not cleared after 1000 (0x%x)\n", readl(host->base + MMCISTATUS));
                    break;
                    }
            }
        }
//sw2-6-1-RH-Wlan_Reset8-00+]
		writel(datactrl, base + MMCIDATACTRL);
		if (host->mmc->update_stats) {
			if (data->flags & MMC_DATA_READ)
				while ((0x2000 & readl(host->base + MMCISTATUS)) != 0x2000);
			mmc->entries[mmc->id].mci_data_ctl_after_dpsm = readl(host->base + MMCIDATACTRL);
			mmc->entries[mmc->id].mci_sts_after_enabling_dpsm = readl(host->base + MMCISTATUS);
			if (data->flags & MMC_DATA_READ)
				mmc->entries[mmc->id].req_rsize = host->curr.xfer_size; 
		}

		if (cmd) {
			msmsdcc_delay(host); /* Delay between data/command */
			/* Daisy-chain the command if requested */
			msmsdcc_start_command(host, cmd, c);
		}
	}
}

static void
msmsdcc_start_command(struct msmsdcc_host *host, struct mmc_command *cmd, u32 c)
{
	msmsdcc_start_command_deferred(host, cmd, &c);
	msmsdcc_start_command_exec(host, cmd->arg, c);
}

static void debug_print_stats(struct msmsdcc_host *host, uint32_t status)
{
	int i;

	if (!host->mmc->update_stats)
		return;
	if (host->mmc->dump_done)
		return;

	host->mmc->stats_err = 1;
	host->mmc->dump_done = 1;
	if (host->mmc->caps & MMC_CAP_8_BIT_DATA)
		pr_info("%s: supports 8 bit bus-width\n", mmc_hostname(host->mmc));
	else if (host->mmc->caps & MMC_CAP_4_BIT_DATA)
		pr_info("%s: supports 4 bit bus-width\n", mmc_hostname(host->mmc));

	if (status & MCI_CMDTIMEOUT)
		pr_info("%s: Command timeout\n", mmc_hostname(host->mmc));

	if (status & MCI_DATATIMEOUT)
		pr_info("%s: Data timeout occured at %lld\n", mmc_hostname(host->mmc),
				ktime_to_us(ktime_get()));

	pr_info("%s: SDCC register values when failure is detected\n", mmc_hostname(host->mmc));
	dump_sdcc_regs(host);

	for (i = host->mmc->id; i>=0; i--)
		pr_info("%s: req_start %lld timer_start %lld cmd %d args %.8x irqs_rcvd %.8x\n"
			"req_rsize %d data_read %d pio_simulated %d pio_sim_rxactive %d\n"
			"t_rxavail %lld t_rxactive %lld t_dataend %lld\n"
			"mci_sts_after_reading_alldata %.8x mci_test_input %.8x\n"
			"SDCC registers before enabling DPSM\n"
			"-------------------------------------------------------\n"
			"mci_sts %.8x mci_mask0 %.8x mci_datatimer %.8x mci_datalen %.8x\n"
			"mci_datactl %.8x mci_clk %.8x mci_pwr %.8x mci_cmd %.8x\n"
			"-------------------------------------------------------\n"
			"mci_cmd_after_enabling_cpsm %.8x mci_data_ctl_after_dpsm %.8x mci_resp0 %.8x\n"
			"mci_sts_after_enabling_dpsm %.8x\n"
			"SDCC registers when DATAEND is received\n"
			"-------------------------------------------------------\n"
			"mci_sts %.8x mci_mask0 %.8x mci_datatimer %.8x mci_datalen %.8x\n"
			"mci_datactl %.8x mci_clk %.8x mci_pwr %.8x mci_cmd %.8x\n"
			"-------------------------------------------------------\n\n",
				mmc_hostname(host->mmc),
				ktime_to_us(host->mmc->entries[i].req_start),
				ktime_to_us(host->mmc->entries[i].timer_start),
				host->mmc->entries[i].opcode,
				host->mmc->entries[i].args, host->mmc->entries[i].mci_sts,
				host->mmc->entries[i].req_rsize,
				host->mmc->entries[i].read_data_cnt, host->mmc->entries[i].pio_simulated,
				!host->mmc->entries[i].sim_no_rxactive,
				ktime_to_us(host->mmc->entries[i].t_rxavail),
				ktime_to_us(host->mmc->entries[i].t_rxactive),
				ktime_to_us(host->mmc->entries[i].t_dataend),
				host->mmc->entries[i].mci_sts_after_reading_alldata,
				host->mmc->entries[i].mci_test_input,
				host->mmc->entries[i].mci_sts_before_datactl, host->mmc->entries[i].mci_mask0_before_datactl,
				host->mmc->entries[i].mci_datatimer_before_datactl, host->mmc->entries[i].mci_datalen_before_datactl,
				host->mmc->entries[i].mci_datactl_before_enabling_datactl, host->mmc->entries[i].mci_clk_before_datactl,
				host->mmc->entries[i].mci_pwr_before_datactl, host->mmc->entries[i].mci_cmd_before_datactl,
				host->mmc->entries[i].mci_cmd_after_enabling_cpsm, host->mmc->entries[i].mci_data_ctl_after_dpsm,
				host->mmc->entries[i].mci_resp0,
				host->mmc->entries[i].mci_sts_after_enabling_dpsm,
				host->mmc->entries[i].mci_sts_after_dataend, host->mmc->entries[i].mci_mask0_after_dataend,
				host->mmc->entries[i].mci_datatimer_after_dataend, host->mmc->entries[i].mci_datalen_after_dataend,
				host->mmc->entries[i].mci_datactl_after_dataend, host->mmc->entries[i].mci_clk_after_dataend,
				host->mmc->entries[i].mci_pwr_after_dataend, host->mmc->entries[i].mci_cmd_after_dataend);

	if (!host->mmc->stats_overflow)
		return;

	for (i = 49; i>host->mmc->id; i--)
		pr_info("%s: req_start %lld timer_start %lld cmd %d args %.8x irqs_rcvd %.8x\n"
			"req_rsize %d data_read %d pio_simulated %d pio_sim_rxactive %d\n"
			"t_rxavail %lld t_rxactive %lld t_dataend %lld\n"
			"mci_sts_after_reading_alldata %.8x mci_test_input %.8x\n"
			"SDCC registers before enabling DPSM\n"
			"-------------------------------------------------------\n"
			"mci_sts %.8x mci_mask0 %.8x mci_datatimer %.8x mci_datalen %.8x\n"
			"mci_datactl %.8x mci_clk %.8x mci_pwr %.8x mci_cmd %.8x\n"
			"-------------------------------------------------------\n"
			"mci_cmd_after_enabling_cpsm %.8x mci_data_ctl_after_dpsm %.8x mci_resp0 %.8x\n"
			"mci_sts_after_enabling_dpsm %.8x\n"
			"SDCC registers when DATAEND is received\n"
			"-------------------------------------------------------\n"
			"mci_sts %.8x mci_mask0 %.8x mci_datatimer %.8x mci_datalen %.8x\n"
			"mci_datactl %.8x mci_clk %.8x mci_pwr %.8x mci_cmd %.8x\n"
			"-------------------------------------------------------\n\n",
				mmc_hostname(host->mmc),
				ktime_to_us(host->mmc->entries[i].req_start),
				ktime_to_us(host->mmc->entries[i].timer_start),
				host->mmc->entries[i].opcode,
				host->mmc->entries[i].args, host->mmc->entries[i].mci_sts,
				host->mmc->entries[i].req_rsize,
				host->mmc->entries[i].read_data_cnt, host->mmc->entries[i].pio_simulated,
				!host->mmc->entries[i].sim_no_rxactive,
				ktime_to_us(host->mmc->entries[i].t_rxavail),
				ktime_to_us(host->mmc->entries[i].t_rxactive),
				ktime_to_us(host->mmc->entries[i].t_dataend),
				host->mmc->entries[i].mci_sts_after_reading_alldata,
				host->mmc->entries[i].mci_test_input,
				host->mmc->entries[i].mci_sts_before_datactl, host->mmc->entries[i].mci_mask0_before_datactl,
				host->mmc->entries[i].mci_datatimer_before_datactl, host->mmc->entries[i].mci_datalen_before_datactl,
				host->mmc->entries[i].mci_datactl_before_enabling_datactl, host->mmc->entries[i].mci_clk_before_datactl,
				host->mmc->entries[i].mci_pwr_before_datactl, host->mmc->entries[i].mci_cmd_before_datactl,
				host->mmc->entries[i].mci_cmd_after_enabling_cpsm, host->mmc->entries[i].mci_data_ctl_after_dpsm,
				host->mmc->entries[i].mci_resp0,
				host->mmc->entries[i].mci_sts_after_enabling_dpsm,
				host->mmc->entries[i].mci_sts_after_dataend, host->mmc->entries[i].mci_mask0_after_dataend,
				host->mmc->entries[i].mci_datatimer_after_dataend, host->mmc->entries[i].mci_datalen_after_dataend,
				host->mmc->entries[i].mci_datactl_after_dataend, host->mmc->entries[i].mci_clk_after_dataend,
				host->mmc->entries[i].mci_pwr_after_dataend, host->mmc->entries[i].mci_cmd_after_dataend);
}

void debug_print_stats_mmc(struct mmc_host *mmc)
{
    struct msmsdcc_host *host = mmc_priv(mmc);
    debug_print_stats(host, 0);
}

EXPORT_SYMBOL(debug_print_stats_mmc);


static void
msmsdcc_data_err(struct msmsdcc_host *host, struct mmc_data *data,
		 unsigned int status)
{
	if (status & MCI_DATACRCFAIL) {
		pr_err("%s: Data CRC error\n",
		       mmc_hostname(host->mmc));
		pr_err("%s: opcode 0x%.8x\n", __func__,
		       data->mrq->cmd->opcode);
		pr_err("%s: blksz %d, blocks %d\n", __func__,
		       data->blksz, data->blocks);
		data->error = -EILSEQ;
	} else if (status & MCI_DATATIMEOUT) {
		/* CRC is optional for the bus test commands, not all
		 * cards respond back with CRC. However controller
		 * waits for the CRC and times out. Hence ignore the
		 * data timeouts during the Bustest.
		 */
		if (!(data->mrq->cmd->opcode == MMC_BUSTEST_W
			|| data->mrq->cmd->opcode == MMC_BUSTEST_R)) {
			pr_err("%s: Data timeout\n",
				 mmc_hostname(host->mmc));
			data->error = -ETIMEDOUT;
		}

	} else if (status & MCI_RXOVERRUN) {
		pr_err("%s: RX overrun\n", mmc_hostname(host->mmc));
		data->error = -EIO;
	} else if (status & MCI_TXUNDERRUN) {
		pr_err("%s: TX underrun\n", mmc_hostname(host->mmc));
		data->error = -EIO;
	} else {
		pr_err("%s: Unknown error (0x%.8x)\n",
		      mmc_hostname(host->mmc), status);
		data->error = -EIO;
	}
}


static int
msmsdcc_pio_read(struct msmsdcc_host *host, char *buffer, unsigned int remain)
{
	void __iomem	*base = host->base;
	uint32_t	*ptr = (uint32_t *) buffer;
	int		count = 0;

	if (remain % 4)
		remain = ((remain >> 2) + 1) << 2;

	while (readl(base + MMCISTATUS) & MCI_RXDATAAVLBL) {

		*ptr = readl(base + MMCIFIFO + (count % MCI_FIFOSIZE));
		ptr++;
		count += sizeof(uint32_t);

		remain -=  sizeof(uint32_t);
		if (remain == 0) {
			if (host->mmc->update_stats)
				host->mmc->entries[host->mmc->id].mci_sts_after_reading_alldata = readl(base + MMCISTATUS);
			break;
		}
	}
	if (host->mmc->update_stats)
		host->mmc->entries[host->mmc->id].read_data_cnt += count;
	return count;
}

static int
msmsdcc_pio_write(struct msmsdcc_host *host, char *buffer,
		  unsigned int remain, u32 status)
{
	void __iomem *base = host->base;
	char *ptr = buffer;

	do {
		unsigned int count, maxcnt, sz;

		maxcnt = status & MCI_TXFIFOEMPTY ? MCI_FIFOSIZE :
						    MCI_FIFOHALFSIZE;
		count = min(remain, maxcnt);

		sz = count % 4 ? (count >> 2) + 1 : (count >> 2);
		writesl(base + MMCIFIFO, ptr, sz);
		ptr += count;
		remain -= count;

		if (remain == 0)
			break;

		status = readl(base + MMCISTATUS);
	} while (status & MCI_TXFIFOHALFEMPTY);

	return ptr - buffer;
}

static irqreturn_t
msmsdcc_pio_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;
	void __iomem		*base = host->base;
	uint32_t		status;

	status = readl(base + MMCISTATUS);
	if (((readl(host->base + MMCIMASK0) & status) & (MCI_IRQ_PIO)) == 0)
		return IRQ_NONE;

	if (host->mmc->update_stats) {
		host->mmc->entries[host->mmc->id].mci_sts |= status;
	}
#if IRQ_DEBUG
	msmsdcc_print_status(host, "irq1-r", status);
#endif

	spin_lock(&host->lock);

	do {
		unsigned long flags;
		unsigned int remain, len;
		char *buffer;

		if (!(status & (MCI_TXFIFOHALFEMPTY | MCI_RXDATAAVLBL)))
			break;

		/* Map the current scatter buffer */
		local_irq_save(flags);
		buffer = kmap_atomic(sg_page(host->pio.sg),
				     KM_BIO_SRC_IRQ) + host->pio.sg->offset;
		buffer += host->pio.sg_off;
		remain = host->pio.sg->length - host->pio.sg_off;

		len = 0;
		if (status & MCI_RXACTIVE)
			len = msmsdcc_pio_read(host, buffer, remain);
		else if (host->mmc->entries[host->mmc->id].pio_simulated)
			host->mmc->entries[host->mmc->id].sim_no_rxactive = 1;
		if (status & MCI_TXACTIVE)
			len = msmsdcc_pio_write(host, buffer, remain, status);

		/* Unmap the buffer */
		kunmap_atomic(buffer, KM_BIO_SRC_IRQ);
		local_irq_restore(flags);

		host->pio.sg_off += len;
		host->curr.xfer_remain -= len;
		host->curr.data_xfered += len;
		remain -= len;

		if (remain) /* Done with this page? */
			break; /* Nope */

		if (status & MCI_RXACTIVE && host->curr.user_pages)
			flush_dcache_page(sg_page(host->pio.sg));

		if (!--host->pio.sg_len) {
			memset(&host->pio, 0, sizeof(host->pio));
			break;
		}

		/* Advance to next sg */
		host->pio.sg++;
		host->pio.sg_off = 0;

		status = readl(base + MMCISTATUS);
	} while (1);

	if (status & MCI_RXACTIVE && host->curr.xfer_remain < MCI_FIFOSIZE) {
		writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) |
				MCI_RXDATAAVLBLMASK, host->base + MMCIMASK0);
		if (!host->curr.xfer_remain) {
			/* Delay needed (same port was just written) */
			msmsdcc_delay(host);
			writel((readl(host->base + MMCIMASK0) &
				(~(MCI_IRQ_PIO))) | 0, host->base + MMCIMASK0);
		}
	} else if (!host->curr.xfer_remain)
		writel((readl(host->base + MMCIMASK0) & (~(MCI_IRQ_PIO))) | 0,
				host->base + MMCIMASK0);

	spin_unlock(&host->lock);

	return IRQ_HANDLED;
}

static void
msmsdcc_request_start(struct msmsdcc_host *host, struct mmc_request *mrq);

static irqreturn_t
msmsdcc_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;
	struct mmc_host *mmc = host->mmc;
	void __iomem		*base = host->base;
	u32			status;
	int			ret = 0;
	int			timer = 0;
	ktime_t t;

	spin_lock(&host->lock);

	do {
		struct mmc_command *cmd;
		struct mmc_data *data;

		if (timer) {
			timer = 0;
			msmsdcc_delay(host);
		}

		if (!host->clks_on) {
			pr_info("%s: irq received when clocks are off\n",
					__func__);
			host->mmc->ios.clock = host->clk_rate;
			host->mmc->ops->set_ios(host->mmc, &host->mmc->ios);
			/* only ansyc interrupt can come when clocks are off */
			writel(MCI_SDIOINTMASK, host->base + MMCICLEAR);
		}

		status = readl(host->base + MMCISTATUS);
		if (mmc->update_stats) {
			t = ktime_get();
			if (status & MCI_DATAEND)
				mmc->entries[mmc->id].t_dataend = t;
			if (status & MCI_RXACTIVE)
				mmc->entries[mmc->id].t_rxactive = t;
			if (status & MCI_RXDATAAVLBL) {
				mmc->entries[mmc->id].mci_test_input = readl(base + MMCITESTINPUT);
				mmc->entries[mmc->id].t_rxavail = t;
			}
		}

		if (((readl(host->base + MMCIMASK0) & status) &
						(~(MCI_IRQ_PIO))) == 0)
			break;

#if IRQ_DEBUG
		msmsdcc_print_status(host, "irq0-r", status);
#endif
		status &= readl(host->base + MMCIMASK0);
		if (host->mmc->update_stats)
			mmc->entries[mmc->id].mci_sts |= status;

		writel(status, host->base + MMCICLEAR);
#if IRQ_DEBUG
		msmsdcc_print_status(host, "irq0-p", status);
#endif

		if ((host->plat->dummy52_required) &&
		    (host->dummy_52_state == DUMMY_52_STATE_SENT)) {
			if (status & MCI_PROGDONE) {
				host->dummy_52_state = DUMMY_52_STATE_NONE;
				host->curr.cmd = NULL;
				spin_unlock(&host->lock);
				msmsdcc_request_start(host, host->curr.mrq);
				return IRQ_HANDLED;
			}
			break;
		}

		data = host->curr.data;
#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
		if (status & MCI_SDIOINTROPE) {
			if (host->sdcc_suspending)
				wake_lock(&host->sdio_suspend_wlock);
			mmc_signal_sdio_irq(host->mmc);
		} else {
                        wake_unlock(&host->sdio_suspend_wlock);
                }
#endif
		/*
		 * Check for proper command response
		 */
		cmd = host->curr.cmd;
		if ((status & (MCI_CMDSENT | MCI_CMDRESPEND | MCI_CMDCRCFAIL |
			      MCI_CMDTIMEOUT | MCI_PROGDONE)) && cmd) {

			host->curr.cmd = NULL;

			cmd->resp[0] = readl(base + MMCIRESPONSE0);
			cmd->resp[1] = readl(base + MMCIRESPONSE1);
			cmd->resp[2] = readl(base + MMCIRESPONSE2);
			cmd->resp[3] = readl(base + MMCIRESPONSE3);

			if(host->mmc->update_stats)
				mmc->entries[mmc->id].mci_resp0 = cmd->resp[0];
			if (status & MCI_CMDTIMEOUT) {
#if VERBOSE_COMMAND_TIMEOUTS
				pr_err("%s: Command timeout\n",
				       mmc_hostname(host->mmc));
#endif
				cmd->error = -ETIMEDOUT;
			} else if (status & MCI_CMDCRCFAIL &&
				   cmd->flags & MMC_RSP_CRC) {
				pr_err("%s: Command CRC error\n",
				       mmc_hostname(host->mmc));
				cmd->error = -EILSEQ;
			}
			if (cmd->error)
				debug_print_stats(host, status);

			if (!cmd->data || cmd->error) {
				if (host->curr.data && host->dma.sg)
					msm_dmov_stop_cmd(host->dma.channel,
							  &host->dma.hdr, 0);
				else if (host->curr.data) { /* Non DMA */
					msmsdcc_reset_and_restore(host);
					msmsdcc_stop_data(host);
					timer |= msmsdcc_request_end(host,
							cmd->mrq);
				} else { /* host->data == NULL */
					if (!cmd->error && host->prog_enable) {
						if (status & MCI_PROGDONE) {
							host->prog_scan = 0;
							host->prog_enable = 0;
							del_timer_sync(&host->prog_timer);
							timer |=
							 msmsdcc_request_end(
								host, cmd->mrq);
						} else
							host->curr.cmd = cmd;
					} else {
						if (host->prog_enable) {
							host->prog_scan = 0;
							host->prog_enable = 0;
							del_timer_sync(&host->prog_timer);
						}
//sw2-6-1-RH-Wlan_Reset8-00+[
						if (cmd->error)
							msmsdcc_reset_and_restore(host);
//sw2-6-1-RH-Wlan_Reset8-00+]
						timer |=
							msmsdcc_request_end(
							 host, cmd->mrq);
					}
				}
			} else if (cmd->data) {
				if (!(cmd->data->flags & MMC_DATA_READ))
					msmsdcc_start_data(host, cmd->data,
								NULL, 0);
			}
		}

		if (data) {
			/* Check for data errors */
			if (status & (MCI_DATACRCFAIL|MCI_DATATIMEOUT|
				      MCI_TXUNDERRUN|MCI_RXOVERRUN)) {
				msmsdcc_data_err(host, data, status);
				debug_print_stats(host, status);
				host->curr.data_xfered = 0;
				if (host->dma.sg)
					msm_dmov_stop_cmd(host->dma.channel,
							  &host->dma.hdr, 0);
				else {
					msmsdcc_reset_and_restore(host);
					if (host->curr.data)
						msmsdcc_stop_data(host);
					if (!data->stop)
						timer |=
						 msmsdcc_request_end(host,
								    data->mrq);
					else {
						msmsdcc_start_command(host,
								     data->stop,
								     0);
						timer = 1;
					}
				}
//sw2-6-1-RH-Patch_from_v7033_SDowner-00+[
				if (((readl(host->base + MMCIMASK0) & status) &
						(~(MCI_IRQ_PIO))) == 0)
			break;
//sw2-6-1-RH-Patch_from_v7033_SDowner-00+]
			}

			/* Check for data done */
			if (!host->curr.got_dataend && (status & MCI_DATAEND)) {
				host->curr.got_dataend = 1;
				if (host->mmc->update_stats) {
					mmc->entries[mmc->id].mci_sts_after_dataend = readl(host->base + MMCISTATUS);
					mmc->entries[mmc->id].mci_mask0_after_dataend = readl(host->base + MMCIMASK0);
					mmc->entries[mmc->id].mci_datatimer_after_dataend = readl(host->base + MMCIDATATIMER);
					mmc->entries[mmc->id].mci_datalen_after_dataend = readl(host->base + MMCIDATALENGTH);
					mmc->entries[mmc->id].mci_datactl_after_dataend = readl(host->base + MMCIDATACTRL);
					mmc->entries[mmc->id].mci_clk_after_dataend = readl(host->base + MMCICLOCK);
					mmc->entries[mmc->id].mci_pwr_after_dataend = readl(host->base + MMCIPOWER);
					mmc->entries[mmc->id].mci_cmd_after_dataend = readl(host->base + MMCICOMMAND);
				}
			}

			if (host->curr.got_dataend) {
				/*
				 * If DMA is still in progress, we complete
				 * via the completion handler
				 */
				if (!host->dma.busy) {
					/*
					 * There appears to be an issue in the
					 * controller where if you request a
					 * small block transfer (< fifo size),
					 * you may get your DATAEND/DATABLKEND
					 * irq without the PIO data irq.
					 *
					 * Check to see if theres still data
					 * to be read, and simulate a PIO irq.
					 */
					if (readl(host->base + MMCISTATUS) &
						       MCI_RXDATAAVLBL) {
						spin_unlock(&host->lock);
						if (host->mmc->update_stats)
							mmc->entries[mmc->id].pio_simulated = 1;
						msmsdcc_pio_irq(1, host);
						spin_lock(&host->lock);
					}
					msmsdcc_stop_data(host);
					if (!data->error) {
						host->curr.data_xfered =
							host->curr.xfer_size;
						if (host->mmc->update_stats) {
							if (data->flags & MMC_DATA_READ) {
								mmc->entries[mmc->id].read_data_cnt = host->curr.data_xfered;
								mmc->entries[mmc->id].mci_sts_after_reading_alldata = readl(base + MMCISTATUS);
							}
						}
					}

					if (!data->stop)
						timer |= msmsdcc_request_end(
							  host, data->mrq);
					else {
						msmsdcc_start_command(host,
							      data->stop, 0);
						timer = 1;
					}
				}
			}
		}

		ret = 1;
	} while (status);

	spin_unlock(&host->lock);

	return IRQ_RETVAL(ret);
}


/* This is a dirty and ugly hack to allow libra driver to cleanup when the
 + * Libra card doesnt respond back */ 
void msmsdcc_libra_recovery_helper(struct mmc_host *mmc)
{
    struct msmsdcc_host *host = mmc_priv(mmc);
    struct mmc_command *cmd;
    struct mmc_data *data;
    unsigned long flags;

    spin_lock_irqsave(&host->lock, flags);

    cmd = host->curr.cmd;
    data = host->curr.data;


    if (data && host->dma.sg)
        msm_dmov_stop_cmd(host->dma.channel, &host->dma.hdr, 0);
    else {
        if (host->curr.data) {
            msmsdcc_stop_data(host);
        }
    }

    host->curr.mrq = NULL;
    host->curr.cmd = NULL;

    msmsdcc_reset_and_restore(host);

    spin_unlock_irqrestore(&host->lock, flags);
}

EXPORT_SYMBOL(msmsdcc_libra_recovery_helper);

static void
msmsdcc_request_start(struct msmsdcc_host *host, struct mmc_request *mrq)
{
	struct mmc_host *mmc = host->mmc;
	if (mmc->update_stats) {
		mmc->entries[mmc->id].req_start = ktime_get();
		mmc->entries[mmc->id].opcode = mrq->cmd->opcode;
		mmc->entries[mmc->id].args = mrq->cmd->arg;
	}

	if (mrq->data && mrq->data->flags & MMC_DATA_READ) {
		/* Queue/read data, daisy-chain command when data starts */
		msmsdcc_start_data(host, mrq->data, mrq->cmd, 0);
	} else {
		msmsdcc_start_command(host, mrq->cmd, 0);
	}
}

static void
msmsdcc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long		flags;

	WARN_ON(host->curr.mrq != NULL);

        WARN_ON(host->pwr == 0);

	spin_lock_irqsave(&host->lock, flags);

	if (host->eject) {
		if (mrq->data && !(mrq->data->flags & MMC_DATA_READ)) {
			mrq->cmd->error = 0;
			mrq->data->bytes_xfered = mrq->data->blksz *
						  mrq->data->blocks;
		} else
			mrq->cmd->error = -ENOMEDIUM;

		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(mmc, mrq);
		return;
	}

    if (host->curr.mrq != NULL) {
        mrq->cmd->error = -EBUSY;
        spin_unlock_irqrestore(&host->lock, flags);
        mmc_request_done(mmc, mrq);
        return;
    }

	host->curr.mrq = mrq;

	if (host->plat->dummy52_required) {
		if (host->dummy_52_needed) {
			if (mrq->data) {
				host->dummy_52_state = DUMMY_52_STATE_SENT;
				msmsdcc_start_command(host, &dummy52cmd,
						      MCI_CPSM_PROGENA);
				spin_unlock_irqrestore(&host->lock, flags);
				return;
			}
			host->dummy_52_needed = 0;
		}
		if ((mrq->cmd->opcode == SD_IO_RW_EXTENDED) && (mrq->data))
			host->dummy_52_needed = 1;
	}

	msmsdcc_request_start(host, mrq);
	spin_unlock_irqrestore(&host->lock, flags);
}
//DIV5-PHONE-JH-InterruptMode-01+[
#if defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
static int is_wimax_slot(struct msmsdcc_host *host)
{
    int rc = 0;

    if(host->pdev_id == 3)
        rc = 1;

    return rc;
}
#endif
//DIV5-PHONE-JH-InterruptMode-01+]

static inline int msmsdcc_is_pwrsave(struct msmsdcc_host *host)
{
	if (host->clk_rate > 400000 && msmsdcc_pwrsave)
		return 1;
	return 0;
}

static void
msmsdcc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	u32 clk = 0, pwr = 0;
	int rc;

	DBG(host, "ios->clock = %u\n", ios->clock);

	if (ios->clock) {

		if (!host->clks_on) {
			if (!IS_ERR(host->pclk))
				clk_enable(host->pclk);
			clk_enable(host->clk);
			host->clks_on = 1;
			if (mmc->card && mmc->card->type == MMC_TYPE_SDIO &&
					!host->plat->sdiowakeup_irq) {
				writel(host->mci_irqenable,
					host->base + MMCIMASK0);
				disable_irq_wake(host->irqres->start);
			}
		}

		if ((ios->clock < host->plat->msmsdcc_fmax) &&
				(ios->clock > host->plat->msmsdcc_fmid))
			ios->clock = host->plat->msmsdcc_fmid;

		if (ios->clock != host->clk_rate) {
			rc = clk_set_rate(host->clk, ios->clock);
			WARN_ON(rc < 0);
			host->clk_rate = ios->clock;
		}
		/*
		 * give atleast 2 MCLK cycles delay for clocks
		 * and SDCC core to stabilize
		 */
		msmsdcc_delay(host);
		clk |= MCI_CLK_ENABLE;
	}

	if (ios->bus_width == MMC_BUS_WIDTH_8)
		clk |= MCI_CLK_WIDEBUS_8;
	else if (ios->bus_width == MMC_BUS_WIDTH_4)
		clk |= MCI_CLK_WIDEBUS_4;
	else
		clk |= MCI_CLK_WIDEBUS_1;

	if (msmsdcc_is_pwrsave(host))
//DIV5-PHONE-JH-InterruptMode-01+[
#if defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
        if(!is_wimax_slot(host))
#endif
//DIV5-PHONE-JH-InterruptMode-01+]	
		clk |= MCI_CLK_PWRSAVE;

	clk |= MCI_CLK_FLOWENA;
	clk |= MCI_CLK_SELECTIN; /* feedback clock */

if (host->pdev_id != 3) {		
	if (host->plat->translate_vdd)
		pwr |= host->plat->translate_vdd(mmc_dev(mmc), ios->vdd);
}

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		htc_pwrsink_set(PWRSINK_SDCARD, 0);
		break;
	case MMC_POWER_UP:
		pwr |= MCI_PWR_UP;
		break;
	case MMC_POWER_ON:
		htc_pwrsink_set(PWRSINK_SDCARD, 100);
		pwr |= MCI_PWR_ON;
		break;
	}

	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		pwr |= MCI_OD;

	writel(clk, host->base + MMCICLOCK);

	udelay(50);

	if (host->pwr != pwr) {
		host->pwr = pwr;
		writel(pwr, host->base + MMCIPOWER);
	}

	if (!(clk & MCI_CLK_ENABLE) && host->clks_on) {
		if (mmc->card && mmc->card->type == MMC_TYPE_SDIO &&
				!host->plat->sdiowakeup_irq) {
			writel(MCI_SDIOINTMASK, host->base + MMCIMASK0);
			enable_irq_wake(host->irqres->start);
		}
		clk_disable(host->clk);
		if (!IS_ERR(host->pclk))
			clk_disable(host->pclk);
		host->clks_on = 0;
	}
}

int msmsdcc_set_pwrsave(struct mmc_host *mmc, int pwrsave)
{
	struct msmsdcc_host *host = mmc_priv(mmc);
	u32 clk;

	clk = readl(host->base + MMCICLOCK);
	pr_debug("Changing to pwr_save=%d", pwrsave);
	if (pwrsave && msmsdcc_is_pwrsave(host))
		clk |= MCI_CLK_PWRSAVE;
	else
		clk &= ~MCI_CLK_PWRSAVE;
	writel(clk, host->base + MMCICLOCK);

	return 0;
}

static int msmsdcc_get_ro(struct mmc_host *mmc)
{
	int wpswitch_status = -ENOSYS;
	struct msmsdcc_host *host = mmc_priv(mmc);

	if (host->plat->wpswitch) {
		wpswitch_status = host->plat->wpswitch(mmc_dev(mmc));
		if (wpswitch_status < 0)
			wpswitch_status = -ENOSYS;
	}
	pr_debug("%s: Card read-only status %d\n", __func__, wpswitch_status);
	return wpswitch_status;
}

#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
static void msmsdcc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct msmsdcc_host *host = mmc_priv(mmc);

	if (enable) {
		host->mci_irqenable |= MCI_SDIOINTOPERMASK;
		writel(readl(host->base + MMCIMASK0) | MCI_SDIOINTOPERMASK,
			       host->base + MMCIMASK0);
	} else {
		host->mci_irqenable &= ~MCI_SDIOINTOPERMASK;
		writel(readl(host->base + MMCIMASK0) & ~MCI_SDIOINTOPERMASK,
		       host->base + MMCIMASK0);
	}
}
#endif /* CONFIG_MMC_MSM_SDIO_SUPPORT */

static int msmsdcc_enable(struct mmc_host *mmc)
{
	int rc;

	rc = pm_runtime_get_sync(mmc->parent);

	if (rc < 0) {
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, rc);
		return rc;
	}
	return 0;
}

static int msmsdcc_disable(struct mmc_host *mmc, int lazy)
{
	int rc;

	if (mmc->card && mmc->card->type == MMC_TYPE_SDIO)
		return -ENOTSUPP;

	rc = pm_runtime_put_sync(mmc->parent);

	if (rc < 0)
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, rc);
	return rc;
}

static const struct mmc_host_ops msmsdcc_ops = {
	.enable		= msmsdcc_enable,
	.disable	= msmsdcc_disable,
	.request	= msmsdcc_request,
	.set_ios	= msmsdcc_set_ios,
	.get_ro		= msmsdcc_get_ro,
#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
	.enable_sdio_irq = msmsdcc_enable_sdio_irq,
#endif
};

static void
msmsdcc_check_status(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *)data;
	unsigned int status;

	if (!host->plat->status) {
		mmc_detect_change(host->mmc, 0);
	} else if(!host->plat->status_irq) {
		mmc_detect_change(host->mmc, 0);
	} else {
		status = host->plat->status(mmc_dev(host->mmc));
		if(host->plat->status_irq)
		{
			host->mmc->card_status = host->plat->status(mmc_dev(host->mmc));
		}
		host->eject = !status;
		if (status ^ host->oldstat) {
			pr_info("%s: Slot status change detected (%d -> %d)\n",
			       mmc_hostname(host->mmc), host->oldstat, status);
// +++ FB0.B-22 reboot device after micro sd remove, chihchia 2010.8.10
#ifdef CONFIG_REMOVED_SD_POWEROFF
#ifndef CONFIG_FIH_FTM
			if(support_sd_removal_turnoff && host->oldstat == 1 && status == 0)
			{
				printk("%s: Slot status change detected (%d -> %d), shutdown system\n",
					   mmc_hostname(host->mmc), host->oldstat, status);			       

				emergency_sync();
				kernel_power_off();
			}
#endif
#endif
// --- FB0.B-22			
			mmc_detect_change(host->mmc, 0);
		}
		host->oldstat = status;
	}
}

static irqreturn_t
msmsdcc_platform_status_irq(int irq, void *dev_id)
{
	struct msmsdcc_host *host = dev_id;

	pr_debug("%s: %d\n", __func__, irq);
	msmsdcc_check_status((unsigned long) host);
	return IRQ_HANDLED;
}

static irqreturn_t
msmsdcc_platform_sdiowakeup_irq(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;

	pr_info("%s: SDIO Wake up IRQ : %d\n", __func__, irq);
	spin_lock(&host->lock);
	if (!host->sdio_irq_disabled) {
		wake_lock(&host->sdio_wlock);
		disable_irq_nosync(irq);
		disable_irq_wake(irq);
		host->sdio_irq_disabled = 1;
	}
	spin_unlock(&host->lock);
	return IRQ_HANDLED;
}

static void
msmsdcc_status_notify_cb(int card_present, void *dev_id)
{
	struct msmsdcc_host *host = dev_id;

	pr_debug("%s: card_present %d\n", mmc_hostname(host->mmc),
	       card_present);
	msmsdcc_check_status((unsigned long) host);
}

static int
msmsdcc_init_dma(struct msmsdcc_host *host)
{
	memset(&host->dma, 0, sizeof(struct msmsdcc_dma_data));
	host->dma.host = host;
	host->dma.channel = -1;

	if (!host->dmares)
		return -ENODEV;

	host->dma.nc = dma_alloc_coherent(NULL,
					  sizeof(struct msmsdcc_nc_dmadata),
					  &host->dma.nc_busaddr,
					  GFP_KERNEL);
	if (host->dma.nc == NULL) {
		pr_err("Unable to allocate DMA buffer\n");
		return -ENOMEM;
	}
	memset(host->dma.nc, 0x00, sizeof(struct msmsdcc_nc_dmadata));
	host->dma.cmd_busaddr = host->dma.nc_busaddr;
	host->dma.cmdptr_busaddr = host->dma.nc_busaddr +
				offsetof(struct msmsdcc_nc_dmadata, cmdptr);
	host->dma.channel = host->dmares->start;

	return 0;
}

static ssize_t
show_polling(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int poll;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	poll = !!(mmc->caps & MMC_CAP_NEEDS_POLL);
	spin_unlock_irqrestore(&host->lock, flags);

	return snprintf(buf, PAGE_SIZE, "%d\n", poll);
}

static ssize_t
set_polling(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int value;
	unsigned long flags;

	sscanf(buf, "%d", &value);

	spin_lock_irqsave(&host->lock, flags);
	if (value) {
		mmc->caps |= MMC_CAP_NEEDS_POLL;
		mmc_detect_change(host->mmc, 0);
	} else {
		mmc->caps &= ~MMC_CAP_NEEDS_POLL;
	}
#ifdef CONFIG_HAS_EARLYSUSPEND
	host->polling_enabled = mmc->caps & MMC_CAP_NEEDS_POLL;
#endif
	spin_unlock_irqrestore(&host->lock, flags);
	return count;
}

static DEVICE_ATTR(polling, S_IRUGO | S_IWUSR,
		show_polling, set_polling);
static struct attribute *dev_attrs[] = {
	&dev_attr_polling.attr,
	NULL,
};
static struct attribute_group dev_attr_grp = {
	.attrs = dev_attrs,
};
//DIV5-CONN-MW-POWER SAVING MODE-01+[
#if  defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
static void wimax_work(struct work_struct *work)
{   
       int wimax_on;
       int H_WAKEUP;
       int W_WAKEUP;
	   
        wimax_on = gpio_get_value(WiMAX_V3P8_FET_CTRL_N);
        printk(KERN_INFO "%s: Get WiMAX_V3P8_FET_CTRL_N = %d\n", __func__, wimax_on);
	   

        H_WAKEUP = gpio_get_value(WiMAX_WAKEUP_HOST_N);		
        printk(KERN_INFO "%s: Get gpio 142(WiMAX_WAKEUP_HOST_N) = %d\n", __func__, H_WAKEUP);

        W_WAKEUP = gpio_get_value(PM8058_GPIO_PM_TO_SYS(PMIC_HOST_WAKEUP_WiMAX_N));
        printk(KERN_INFO "%s: Get gpio 06(PMIC_HOST_WAKEUP_WiMAX_N) = %d\n", __func__, W_WAKEUP);
        if(wimax_on == 1) //if wimax on
        {

            if (H_WAKEUP == 0) //H_WAKEUP is HIGH
            {

                    if (W_WAKEUP == 1) //H_WAKEUP is HIGH,,W_WAKEUP is HIGH
                    {
		 
                         printk(KERN_INFO "%s: wake unlock by wimax , system suspend...\n", __func__);
                    }
                    else//H_WAKEUP is HIGH,W_WAKEUP is LOW
                    {
                         printk(KERN_INFO "%s: H_WAKEUP is HIGH,W_WAKEUP is LOW ...) \n", __func__);

                    }
			
            }
            else   //H_WAKEUP is LOW
            {

                    
                    if (W_WAKEUP == 1) //H_WAKEUP is LOW,,W_WAKEUP is HIGH
                    {

                         printk(KERN_INFO "%s: wake-up by wimax !!!!!!!!) \n", __func__);
                    }
                    else//H_WAKEUP is LOW,,W_WAKEUP is LOW
                    {
                         printk(KERN_INFO "%s: H_WAKEUP is LOW,W_WAKEUP is LOW) \n", __func__);

                    }             
               
            }

         }
		

}

static irqreturn_t WIMAX_wakeup_host_isr(int irq, void *dev_id)
{
	struct msmsdcc_host	*host = dev_id;
        int wimax_on;

        wimax_on = gpio_get_value(WiMAX_V3P8_FET_CTRL_N);
        printk(KERN_INFO "%s: Get WiMAX_V3P8_FET_CTRL_N = %d\n", __func__, wimax_on);

       if(wake_lock_active(&host->wimax_wakelock)) 
       {
		wake_unlock(&host->wimax_wakelock);    	
		printk(KERN_INFO "%s: wake unlock by wimax , system suspend...\n", __func__);
       }		
	   
      schedule_work(&host->work);
	  
	return IRQ_HANDLED;
}
#endif
//DIV5-CONN-MW-POWER SAVING MODE-01+]

#ifdef CONFIG_HAS_EARLYSUSPEND
static void msmsdcc_early_suspend(struct early_suspend *h)
{
	struct msmsdcc_host *host =
		container_of(h, struct msmsdcc_host, early_suspend);
	unsigned long flags;
//DIV5-CONN-MW-POWER SAVING MODE-01+[
#if  defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
        int test_sus,wimax_on=0;       
        if( (fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
        {
            wimax_on = gpio_get_value(WiMAX_V3P8_FET_CTRL_N);
            printk(KERN_INFO "%s: Get WiMAX_V3P8_FET_CTRL_N = %d\n", __func__, wimax_on);
            if(wimax_on == 1) //if wimax on
            {
                test_sus = gpio_get_value(WiMAX_WAKEUP_HOST_N);
                printk(KERN_INFO "%s: Get gpio 142(WiMAX_WAKEUP_HOST_N) = %d\n", __func__, test_sus);
                if (test_sus == 1)
                {
                   if(wake_lock_active(&host->wimax_wakelock))
                            wake_lock(&host->wimax_wakelock);
                    printk(KERN_INFO "%s: wake lock by wimax...) \n", __func__);
                    gpio_set_value(PM8058_GPIO_PM_TO_SYS(PMIC_HOST_WAKEUP_WiMAX_N), 1);
  
                    printk(KERN_INFO "%s: Set PMIC gpio 16 output HIGH, IRQF_TRIGGER_FALLING.\n", __func__);
                }
                else
                {
                if(wake_lock_active(&host->wimax_wakelock))
                    wake_unlock(&host->wimax_wakelock);
                    printk(KERN_INFO "%s:wake_unlock.\n", __func__);		
                }	
    
             }//end of if(wimax_on == 1) //if wimax on
        }//end of    if( (fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
#endif
//DIV5-CONN-MW-POWER SAVING MODE-01+]
//KD 2010-10-26 - Hold wakelock on the Wlan interface while asleep
	if (libra_loaded) {
		wake_lock(&host->sdio_wlan_lock);
		printk("%s: [msm_sdcc] wake_lock WLAN\n", mmc_hostname(host->mmc));
	} else {
		printk("%s: [msm_sdcc] NO wake_lock WLAN NOT loaded\n", mmc_hostname(host->mmc));
	}	
	spin_lock_irqsave(&host->lock, flags);
	host->polling_enabled = host->mmc->caps & MMC_CAP_NEEDS_POLL;
	host->mmc->caps &= ~MMC_CAP_NEEDS_POLL;
	spin_unlock_irqrestore(&host->lock, flags);
};
static void msmsdcc_late_resume(struct early_suspend *h)
{
	struct msmsdcc_host *host =
		container_of(h, struct msmsdcc_host, early_suspend);
	unsigned long flags;
	unsigned int status;
//DIV5-CONN-MW-POWER SAVING MODE-01+[
#if  defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
        int wimax_on=0;
        if( (fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
        {
            wimax_on = gpio_get_value(WiMAX_V3P8_FET_CTRL_N);
            if(wimax_on == 1) //if wimax on
            {
		gpio_set_value(PM8058_GPIO_PM_TO_SYS(PMIC_HOST_WAKEUP_WiMAX_N), 0);
		printk(KERN_INFO "%s: Set PMIC gpio 16 output LOW.\n", __func__);  

             }//end of if(wimax_on == 1)
        }//end of   if ((fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
#endif
//DIV5-CONN-MW-POWER SAVING MODE-01+]
	if(host->plat->status && host->plat->status_irq && host->pdev_id == 4)
	{
		status = host->plat->status(mmc_dev(host->mmc));
		printk("%s: [msm_sdcc] host->oldstat:%d, status:%d\n", mmc_hostname(host->mmc), host->oldstat, status);
#ifdef CONFIG_REMOVED_SD_POWEROFF
		if(support_sd_removal_turnoff && host->oldstat == 1 && status == 0)
		{
			printk("%s: [msm_sdcc] Slot status change detected (%d -> %d), shutdown system\n",
					   mmc_hostname(host->mmc), host->oldstat, status);			       

			emergency_sync();
			kernel_power_off();
		}
#endif		
		if(host->oldstat != status )
		{
			printk("%s: [msm_sdcc] force msmsdcc_check_status\n", mmc_hostname(host->mmc));
			msmsdcc_check_status((unsigned long) host);
		}
	}

//KD 2010-10-26 - Release wakelock on the Wlan interface when resuming
	if (libra_loaded) {
		wake_unlock(&host->sdio_wlan_lock);
		printk("%s: [msm_sdcc] wake_unlock WLAN\n", mmc_hostname(host->mmc));
	} else {
		printk("%s: [msm_sdcc] NO wake_unlock WLAN NOT loaded\n", mmc_hostname(host->mmc));
	}
	if (host->polling_enabled) {
		spin_lock_irqsave(&host->lock, flags);
		host->mmc->caps |= MMC_CAP_NEEDS_POLL;
		mmc_detect_change(host->mmc, 0);
		spin_unlock_irqrestore(&host->lock, flags);
	}
};
#endif

static void msmsdcc_prog_timer_expired(unsigned long data)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *) data;
	struct mmc_command *cmd;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	cmd = host->curr.cmd;
	pr_info("%s: %s\n", mmc_hostname(host->mmc), __func__);
//sw2-6-1-RH-Wlan_Reset8-00+[
	cmd->error = -ETIMEDOUT;
	msmsdcc_reset_and_restore(host);
//sw2-6-1-RH-Wlan_Reset8-00+]
	msmsdcc_request_end(host, cmd->mrq);
	spin_unlock_irqrestore(&host->lock, flags);
}

static int
msmsdcc_probe(struct platform_device *pdev)
{
	struct mmc_platform_data *plat = pdev->dev.platform_data;
	struct msmsdcc_host *host;
	struct mmc_host *mmc;
	struct resource *irqres = NULL;
	struct resource *memres = NULL;
	struct resource *dmares = NULL;
	int ret;
	int i;

	/* must have platform data */
	if (!plat) {
		pr_err("%s: Platform data not available\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	if (pdev->id < 1 || pdev->id > 5)
		return -EINVAL;

	if (pdev->resource == NULL || pdev->num_resources < 3) {
		pr_err("%s: Invalid resource\n", __func__);
		return -ENXIO;
	}

	for (i = 0; i < pdev->num_resources; i++) {
		if (pdev->resource[i].flags & IORESOURCE_MEM)
			memres = &pdev->resource[i];
		if (pdev->resource[i].flags & IORESOURCE_IRQ)
			irqres = &pdev->resource[i];
		if (pdev->resource[i].flags & IORESOURCE_DMA)
			dmares = &pdev->resource[i];
	}
	if (!irqres || !memres) {
		pr_err("%s: Invalid resource\n", __func__);
		return -ENXIO;
	}

	/*
	 * Setup our host structure
	 */

	mmc = mmc_alloc_host(sizeof(struct msmsdcc_host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto out;
	}

	host = mmc_priv(mmc);
	host->pdev_id = pdev->id;
	host->plat = plat;
	host->mmc = mmc;
	host->curr.cmd = NULL;

	host->base = ioremap(memres->start, PAGE_SIZE);
	if (!host->base) {
		ret = -ENOMEM;
		goto host_free;
	}

	host->irqres = irqres;
	host->memres = memres;
	host->dmares = dmares;
	spin_lock_init(&host->lock);

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	if (plat->embedded_sdio)
		mmc_set_embedded_sdio_data(mmc,
					   &plat->embedded_sdio->cis,
					   &plat->embedded_sdio->cccr,
					   plat->embedded_sdio->funcs,
					   plat->embedded_sdio->num_funcs);
#endif

	tasklet_init(&host->dma_tlet, msmsdcc_dma_complete_tlet,
			(unsigned long)host);

	/*
	 * Setup DMA
	 */
	ret = msmsdcc_init_dma(host);
	if (ret)
		goto ioremap_free;

	/*
	 * Setup main peripheral bus clock
	 */
	host->pclk = clk_get(&pdev->dev, "sdc_pclk");
	if (!IS_ERR(host->pclk)) {
		ret = clk_enable(host->pclk);
		if (ret)
			goto pclk_put;

		host->pclk_rate = clk_get_rate(host->pclk);
	}

	/*
	 * Setup SDC MMC clock
	 */
	host->clk = clk_get(&pdev->dev, "sdc_clk");
	if (IS_ERR(host->clk)) {
		ret = PTR_ERR(host->clk);
		goto pclk_disable;
	}

	ret = clk_set_rate(host->clk, plat->msmsdcc_fmin);
	if (ret) {
		pr_err("%s: Clock rate set failed (%d)\n", __func__, ret);
		goto clk_put;
	}

	ret = clk_enable(host->clk);
	if (ret)
		goto clk_put;

	host->clk_rate = clk_get_rate(host->clk);

	host->clks_on = 1;

	/*
	 * Setup MMC host structure
	 */
	mmc->ops = &msmsdcc_ops;
	mmc->f_min = plat->msmsdcc_fmin;
	mmc->f_max = plat->msmsdcc_fmax;
	mmc->ocr_avail = plat->ocr_mask;
	mmc->pm_caps |= MMC_PM_KEEP_POWER;
	mmc->caps |= plat->mmc_bus_width;
//sw2-6-1-RH-Patch_from_v7531_SDowner-00+[
	mmc->card_status = 1;
//sw2-6-1-RH-Patch_from_v7531_SDowner-00+]

	mmc->caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED;

	if (plat->nonremovable)
		mmc->caps |= MMC_CAP_NONREMOVABLE;
#ifdef CONFIG_MMC_MSM_SDIO_SUPPORT
	mmc->caps |= MMC_CAP_SDIO_IRQ;
#endif

	mmc->max_phys_segs = NR_SG;
	mmc->max_hw_segs = NR_SG;
	mmc->max_blk_size = 4096;	/* MCI_DATA_CTL BLOCKSIZE up to 4096 */
	mmc->max_blk_count = 65536;

	mmc->max_req_size = 33554432;	/* MCI_DATA_LENGTH is 25 bits */
	mmc->max_seg_size = mmc->max_req_size;

	writel(0, host->base + MMCIMASK0);
	writel(MCI_CLEAR_STATIC_MASK, host->base + MMCICLEAR);

	/* Delay needed (MMCIMASK0 was just written above) */
	msmsdcc_delay(host);
	writel(MCI_IRQENABLE, host->base + MMCIMASK0);
	host->mci_irqenable = MCI_IRQENABLE;

	ret = request_irq(irqres->start, msmsdcc_irq, IRQF_SHARED,
			  DRIVER_NAME " (cmd)", host);
	if (ret)
		goto clk_disable;

	ret = request_irq(irqres->start, msmsdcc_pio_irq, IRQF_SHARED,
			  DRIVER_NAME " (pio)", host);
	if (ret)
		goto irq_free;

	if (plat->sdiowakeup_irq) {
		ret = request_irq(plat->sdiowakeup_irq,
			msmsdcc_platform_sdiowakeup_irq,
			IRQF_SHARED | IRQF_TRIGGER_LOW,
			DRIVER_NAME "sdiowakeup", host);
		if (ret) {
			pr_err("Unable to get sdio wakeup IRQ %d (%d)\n",
				plat->sdiowakeup_irq, ret);
			goto pio_irq_free;
		} else {
			mmc->pm_caps |= MMC_PM_WAKE_SDIO_IRQ;
			disable_irq(plat->sdiowakeup_irq);
			wake_lock_init(&host->sdio_wlock, WAKE_LOCK_SUSPEND,
					mmc_hostname(mmc));
		}
	}

	wake_lock_init(&host->sdio_suspend_wlock, WAKE_LOCK_SUSPEND,
			mmc_hostname(mmc));
// KD 2010-10-26 - Init wake lock for the WLAN interface
	wake_lock_init(&host->sdio_wlan_lock, WAKE_LOCK_SUSPEND,
					mmc_hostname(mmc));

//DIV5-CONN-MW-POWER SAVING MODE-01+[
        #if defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
        if( (fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
        {
            ret = request_irq(gpio_to_irq(WiMAX_WAKEUP_HOST_N),
                          WIMAX_wakeup_host_isr,
                          (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING),
                           "wimax_wakeup_host", host);

            wake_lock_init(&host->wimax_wakelock, WAKE_LOCK_SUSPEND,"wimax");
            INIT_WORK(&host->work, wimax_work);
        }//end of if( (fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
        #endif
//DIV5-CONN-MW-POWER SAVING MODE-01+]

	/*
	 * Setup card detect change
	 */

	if (plat->status) {
		host->oldstat = plat->status(mmc_dev(host->mmc));
		host->eject = !host->oldstat;
	}

	if (plat->status_irq) {
		ret = request_threaded_irq(plat->status_irq, NULL,
				  msmsdcc_platform_status_irq,
				  plat->irq_flags,
				  DRIVER_NAME " (slot)",
				  host);
		if (ret) {
			pr_err("Unable to get slot IRQ %d (%d)\n",
			       plat->status_irq, ret);
			goto sdiowakeup_irq_free;
		}
// +++ FB0.B-22 , enable irq as a wakeup source, chihchia 2010.8.10
#ifndef CONFIG_FIH_FTM
		enable_irq_wake(plat->status_irq);
#endif			
// --- FB0.B-22
	} else if (plat->register_status_notify) {
		plat->register_status_notify(msmsdcc_status_notify_cb, host);
	} else if (!plat->status)
		pr_err("%s: No card detect facilities available\n",
		       mmc_hostname(mmc));

	mmc_set_drvdata(pdev, mmc);

	ret = pm_runtime_set_active(&(pdev)->dev);
	if (ret < 0)
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, ret);
	/*
	 * There is no notion of suspend/resume for SD/MMC/SDIO
	 * cards. So host can be suspended/resumed with out
	 * worrying about its children.
	 */
	pm_suspend_ignore_children(&(pdev)->dev, true);

	/*
	 * MMC/SD/SDIO bus suspend/resume operations are defined
	 * only for the slots that will be used for non-removable
	 * media or for all slots when CONFIG_MMC_UNSAFE_RESUME is
	 * defined. Otherwise, they simply become card removal and
	 * insertion events during suspend and resume respectively.
	 * Hence, enable run-time PM only for slots for which bus
	 * suspend/resume operations are defined.
	 */
#ifdef CONFIG_MMC_UNSAFE_RESUME
	/*
	 * If this capability is set, MMC core will enable/disable host
	 * for every claim/release operation on a host. We use this
	 * notification to increment/decrement runtime pm usage count.
	 */
	mmc->caps |= MMC_CAP_DISABLE;
	pm_runtime_enable(&(pdev)->dev);
#else
	if (mmc->caps & MMC_CAP_NONREMOVABLE) {
		mmc->caps |= MMC_CAP_DISABLE;
		pm_runtime_enable(&(pdev)->dev);
	}
#endif
	init_timer(&host->prog_timer);
	host->prog_timer.data = (unsigned long)host;
	host->prog_timer.function = msmsdcc_prog_timer_expired;

	mmc_add_host(mmc);

#ifdef CONFIG_HAS_EARLYSUSPEND
	host->early_suspend.suspend = msmsdcc_early_suspend;
	host->early_suspend.resume  = msmsdcc_late_resume;
	host->early_suspend.level   = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	register_early_suspend(&host->early_suspend);
#endif

	pr_info("%s: Qualcomm MSM SDCC at 0x%016llx irq %d,%d dma %d\n",
	       mmc_hostname(mmc), (unsigned long long)memres->start,
	       (unsigned int) irqres->start,
	       (unsigned int) plat->status_irq, host->dma.channel);

	pr_info("%s: 8 bit data mode %s\n", mmc_hostname(mmc),
		(mmc->caps & MMC_CAP_8_BIT_DATA ? "enabled" : "disabled"));
	pr_info("%s: 4 bit data mode %s\n", mmc_hostname(mmc),
	       (mmc->caps & MMC_CAP_4_BIT_DATA ? "enabled" : "disabled"));
	pr_info("%s: polling status mode %s\n", mmc_hostname(mmc),
	       (mmc->caps & MMC_CAP_NEEDS_POLL ? "enabled" : "disabled"));
	pr_info("%s: MMC clock %u -> %u Hz, PCLK %u Hz\n",
	       mmc_hostname(mmc), plat->msmsdcc_fmin, plat->msmsdcc_fmax,
							host->pclk_rate);
	pr_info("%s: Slot eject status = %d\n", mmc_hostname(mmc),
	       host->eject);
	pr_info("%s: Power save feature enable = %d\n",
	       mmc_hostname(mmc), msmsdcc_pwrsave);

	if (host->dma.channel != -1) {
		pr_info("%s: DM non-cached buffer at %p, dma_addr 0x%.8x\n",
		       mmc_hostname(mmc), host->dma.nc, host->dma.nc_busaddr);
		pr_info("%s: DM cmd busaddr 0x%.8x, cmdptr busaddr 0x%.8x\n",
		       mmc_hostname(mmc), host->dma.cmd_busaddr,
		       host->dma.cmdptr_busaddr);
	} else
		pr_info("%s: PIO transfer enabled\n", mmc_hostname(mmc));

#if defined(CONFIG_DEBUG_FS)
	msmsdcc_dbg_createhost(host);
#endif
	if (!plat->status_irq) {
		ret = sysfs_create_group(&pdev->dev.kobj, &dev_attr_grp);
		if (ret)
			goto platform_irq_free;
	}
	return 0;

 platform_irq_free:
	pm_runtime_disable(&(pdev)->dev);
	pm_runtime_set_suspended(&(pdev)->dev);

	if (plat->status_irq)
		free_irq(plat->status_irq, host);
 sdiowakeup_irq_free:
	wake_lock_destroy(&host->sdio_suspend_wlock);
	if (plat->sdiowakeup_irq) {
		wake_lock_destroy(&host->sdio_wlock);
		free_irq(plat->sdiowakeup_irq, host);
	}
// KD 2010-10-26 Destroy wake lock on the WLAN interface on the way out
	wake_lock_destroy(&host->sdio_wlan_lock);
 pio_irq_free:
	free_irq(irqres->start, host);
 irq_free:
	free_irq(irqres->start, host);
 clk_disable:
	clk_disable(host->clk);
 clk_put:
	clk_put(host->clk);
 pclk_disable:
	if (!IS_ERR(host->pclk))
		clk_disable(host->pclk);
 pclk_put:
	if (!IS_ERR(host->pclk))
		clk_put(host->pclk);

	dma_free_coherent(NULL, sizeof(struct msmsdcc_nc_dmadata),
			host->dma.nc, host->dma.nc_busaddr);
 ioremap_free:
	iounmap(host->base);
 host_free:
	mmc_free_host(mmc);
 out:
	return ret;
}

static int msmsdcc_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = mmc_get_drvdata(pdev);
	struct mmc_platform_data *plat;
	struct msmsdcc_host *host;

	if (!mmc)
		return -ENXIO;

	if (pm_runtime_suspended(&(pdev)->dev))
		pm_runtime_resume(&(pdev)->dev);

	host = mmc_priv(mmc);

	DBG(host, "Removing SDCC2 device = %d\n", pdev->id);
	plat = host->plat;

	if (!plat->status_irq)
		sysfs_remove_group(&pdev->dev.kobj, &dev_attr_grp);

	tasklet_kill(&host->dma_tlet);
	mmc_remove_host(mmc);

	if (plat->status_irq)
		free_irq(plat->status_irq, host);

	wake_lock_destroy(&host->sdio_suspend_wlock);
	if (plat->sdiowakeup_irq) {
		wake_lock_destroy(&host->sdio_wlock);
		set_irq_wake(plat->sdiowakeup_irq, 0);
		free_irq(plat->sdiowakeup_irq, host);
	}

	free_irq(host->irqres->start, host);
	free_irq(host->irqres->start, host);

//DIV5-CONN-MW-POWER SAVING MODE-01+[
        #if defined(CONFIG_FIH_PROJECT_SF4Y6) && defined(CONFIG_FIH_WIMAX_GCT_SDIO)
        if( (fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
        {
	    wake_lock_destroy(&host->wimax_wakelock);
	    set_irq_wake(gpio_to_irq(WiMAX_WAKEUP_HOST_N), 0);		
	    free_irq(gpio_to_irq(WiMAX_WAKEUP_HOST_N), host);
       	} //end of if( (fih_get_product_id() == Product_SF6) &&  (fih_get_product_phase() >= Product_PR3))
       	cancel_work_sync(&host->work);
        #endif
//DIV5-CONN-MW-POWER SAVING MODE-01+]

	clk_put(host->clk);
	if (!IS_ERR(host->pclk))
		clk_put(host->pclk);

	dma_free_coherent(NULL, sizeof(struct msmsdcc_nc_dmadata),
			host->dma.nc, host->dma.nc_busaddr);
	iounmap(host->base);
	mmc_free_host(mmc);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&host->early_suspend);
#endif
	pm_runtime_disable(&(pdev)->dev);
	pm_runtime_set_suspended(&(pdev)->dev);

	return 0;
}

#ifdef CONFIG_PM
static int
msmsdcc_runtime_suspend(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int rc = 0;

	if (mmc) {
		host->sdcc_suspending = 1;

		if (!mmc->card || (host->plat->sdiowakeup_irq &&
				mmc->card->type == MMC_TYPE_SDIO) ||
				mmc->card->type != MMC_TYPE_SDIO) {
			/*
			 * MMC core thinks that host is disabled by now since
			 * runtime suspend is scheduled after msmsdcc_disable()
			 * is called. Thus, MMC core will try to enable the host
			 * while suspending it. This results in a synchronous
			 * runtime resume request while in runtime suspending
			 * context and hence inorder to complete this resume
			 * requet, it will wait for suspend to be complete,
			 * but runtime suspend also can not proceed further
			 * until the host is resumed. Thus, it leads to a hang.
			 * Hence, increase the pm usage count before suspending
			 * the host so that any resume requests after this will
			 * simple become pm usage counter increment operations.
			 */
			pm_runtime_get_noresume(dev);
			rc = mmc_suspend_host(mmc, PMSG_SUSPEND);
			pm_runtime_put_noidle(dev);
		}

		if (!rc) {
			/*
			 * If MMC core level suspend is not supported, turn
			 * off clocks to allow deep sleep (TCXO shutdown).
			 */
			spin_lock_irqsave(&host->lock, flags);
			if (host->clks_on) {
				writel(0, host->base + MMCIMASK0);
				mmc->ios.clock = 0;
				mmc->ops->set_ios(host->mmc, &host->mmc->ios);
			}
			spin_unlock_irqrestore(&host->lock, flags);
		}

		if ((mmc->pm_flags & MMC_PM_WAKE_SDIO_IRQ) && mmc->card &&
				mmc->card->type == MMC_TYPE_SDIO) {
			host->sdio_irq_disabled = 0;
			enable_irq_wake(host->plat->sdiowakeup_irq);
			enable_irq(host->plat->sdiowakeup_irq);
		}
		host->sdcc_suspending = 0;
	}
	return rc;
}

static int msmsdcc_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;	
	void __user *argp = (void __user *)arg;	
	int value;
	
	switch (cmd) 
	{
		case SUPPORT_SD_REMOVAL_TURNOFF:
		{
			if(copy_from_user(&value, argp, sizeof(value)))
				return -1; 			
			
			if(value ==0 || value == 1)
				support_sd_removal_turnoff = value;
		}
		break;
		default:
			break;
	}

	return ret;
}

static int
msmsdcc_runtime_resume(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int release_lock = 0;

	if (mmc) {
		spin_lock_irqsave(&host->lock, flags);
		if (!host->clks_on) {
			mmc->ios.clock = host->clk_rate;
			mmc->ops->set_ios(host->mmc, &host->mmc->ios);
		}

		writel(host->mci_irqenable, host->base + MMCIMASK0);

		if ((mmc->pm_flags & MMC_PM_WAKE_SDIO_IRQ) &&
				!host->sdio_irq_disabled) {
			if (mmc->card && mmc->card->type == MMC_TYPE_SDIO) {
				disable_irq_nosync(host->plat->sdiowakeup_irq);
				disable_irq_wake(host->plat->sdiowakeup_irq);
				host->sdio_irq_disabled = 1;
			}
		} else {
			release_lock = 1;
		}

		spin_unlock_irqrestore(&host->lock, flags);
		if (!mmc->card || (host->plat->sdiowakeup_irq &&
				mmc->card->type == MMC_TYPE_SDIO) ||
				mmc->card->type != MMC_TYPE_SDIO)
			mmc_resume_host(mmc);

		/*
		 * After resuming the host wait for sometime so that
		 * the SDIO work will be processed.
		 */
		if ((mmc->pm_flags & MMC_PM_WAKE_SDIO_IRQ) && release_lock)
			wake_lock_timeout(&host->sdio_wlock, 1);
	}
	wake_unlock(&host->sdio_suspend_wlock);
	return 0;
}

static int msmsdcc_runtime_idle(struct device *dev)
{
	/* Idle timeout is not configurable for now */
	pm_schedule_suspend(dev, MSM_MMC_IDLE_TIMEOUT);

	return -EAGAIN;
}

static int msmsdcc_pm_suspend(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;

	if (host->plat->status_irq)
	{
		if(host->pdev_id == 4) 
		{
			printk("[SD] detect pin status:%d\n", host->plat->status(mmc_dev(host->mmc)));
			if(host->plat->status(mmc_dev(host->mmc)))
				set_irq_type(host->plat->status_irq, IRQF_TRIGGER_RISING);
			else
				set_irq_type(host->plat->status_irq, IRQF_TRIGGER_FALLING);
		}
		disable_irq(host->plat->status_irq);
	}

	if (!pm_runtime_suspended(dev))
		rc = msmsdcc_runtime_suspend(dev);

	return rc;
}

static int msmsdcc_pm_resume(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct msmsdcc_host *host = mmc_priv(mmc);
	int rc = 0;

	rc = msmsdcc_runtime_resume(dev);
	if (host->plat->status_irq)
	{
		if(host->pdev_id == 4) 				
			set_irq_type(host->plat->status_irq, IRQF_TRIGGER_RISING);

		enable_irq(host->plat->status_irq);
	}

	/* Update the run-time PM status */
	pm_runtime_disable(dev);
	rc = pm_runtime_set_active(dev);
	if (rc < 0)
		pr_info("%s: %s: failed with error %d", mmc_hostname(mmc),
				__func__, rc);
	pm_runtime_enable(dev);

	return rc;
}

#else
#define msmsdcc_runtime_suspend NULL
#define msmsdcc_runtime_resume NULL
#define msmsdcc_runtime_idle NULL
#define msmsdcc_pm_suspend NULL
#define msmsdcc_pm_resume NULL
#endif

static const struct dev_pm_ops msmsdcc_dev_pm_ops = {
	.runtime_suspend = msmsdcc_runtime_suspend,
	.runtime_resume  = msmsdcc_runtime_resume,
	.runtime_idle    = msmsdcc_runtime_idle,
	.suspend 	 = msmsdcc_pm_suspend,
	.resume		 = msmsdcc_pm_resume,
};

static struct platform_driver msmsdcc_driver = {
	.probe		= msmsdcc_probe,
	.remove		= msmsdcc_remove,
	.driver		= {
		.name	= "msm_sdcc",
		.pm	= &msmsdcc_dev_pm_ops,
	},
};

static struct file_operations msmsdcc_fops = {
	.owner =   THIS_MODULE,
	.ioctl =   msmsdcc_ioctl,
};

static struct miscdevice msmsdcc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "msmsdcc",
	.fops = &msmsdcc_fops,
};
static int __init msmsdcc_init(void)
{
#if defined(CONFIG_DEBUG_FS)
	int ret = 0;
	ret = msmsdcc_dbg_init();
	if (ret) {
		pr_err("Failed to create debug fs dir \n");
		return ret;
	}
#endif
	misc_register(&msmsdcc_device);
	return platform_driver_register(&msmsdcc_driver);
}

static void __exit msmsdcc_exit(void)
{
	platform_driver_unregister(&msmsdcc_driver);
	misc_deregister(&msmsdcc_device);

#if defined(CONFIG_DEBUG_FS)
	debugfs_remove(debugfs_file);
	debugfs_remove(debugfs_dir);
#endif
}

module_init(msmsdcc_init);
module_exit(msmsdcc_exit);

MODULE_DESCRIPTION("Qualcomm Multimedia Card Interface driver");
MODULE_LICENSE("GPL");

#if defined(CONFIG_DEBUG_FS)

static int
msmsdcc_dbg_state_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t
msmsdcc_dbg_state_read(struct file *file, char __user *ubuf,
		       size_t count, loff_t *ppos)
{
	struct msmsdcc_host *host = (struct msmsdcc_host *) file->private_data;
	char buf[1024];
	int max, i;

	i = 0;
	max = sizeof(buf) - 1;

	i += scnprintf(buf + i, max - i, "STAT: %p %p %p\n", host->curr.mrq,
		       host->curr.cmd, host->curr.data);
	if (host->curr.cmd) {
		struct mmc_command *cmd = host->curr.cmd;

		i += scnprintf(buf + i, max - i, "CMD : %.8x %.8x %.8x\n",
			      cmd->opcode, cmd->arg, cmd->flags);
	}
	if (host->curr.data) {
		struct mmc_data *data = host->curr.data;
		i += scnprintf(buf + i, max - i,
			      "DAT0: %.8x %.8x %.8x %.8x %.8x %.8x\n",
			      data->timeout_ns, data->timeout_clks,
			      data->blksz, data->blocks, data->error,
			      data->flags);
		i += scnprintf(buf + i, max - i, "DAT1: %.8x %.8x %.8x %p\n",
			      host->curr.xfer_size, host->curr.xfer_remain,
			      host->curr.data_xfered, host->dma.sg);
	}

	return simple_read_from_buffer(ubuf, count, ppos, buf, i);
}

static const struct file_operations msmsdcc_dbg_state_ops = {
	.read	= msmsdcc_dbg_state_read,
	.open	= msmsdcc_dbg_state_open,
};

static void msmsdcc_dbg_createhost(struct msmsdcc_host *host)
{
	if (debugfs_dir) {
		debugfs_file = debugfs_create_file(mmc_hostname(host->mmc),
							0644, debugfs_dir, host,
							&msmsdcc_dbg_state_ops);
	}
}

static int __init msmsdcc_dbg_init(void)
{
	int err;

	debugfs_dir = debugfs_create_dir("msmsdcc", 0);
	if (IS_ERR(debugfs_dir)) {
		err = PTR_ERR(debugfs_dir);
		debugfs_dir = NULL;
		return err;
	}

	return 0;
}
#endif
