/*
 * SdioDrv.c - MSM Linux SDIO driver (platform and OS dependent)
 *
 * The lower SDIO driver (BSP) for MSM on Linux OS.
 * Provides all SDIO commands and read/write operation methods.
 *
 * Copyright (c) 1998 - 2008 Texas Instruments Incorporated
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <mach/io.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <mach/hardware.h>
#include <linux/platform_device.h>
#include <mach/board.h>
#include <linux/errno.h>
#include <linux/clk.h>
#include <mach/dma.h>
#include <asm/io.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <asm/mach/mmc.h>
#include <linux/delay.h>

#include "SdioDrvDbg.h"
#include "SdioDrv.h"
#include "msm_sdcc.h"


/* Add by daniel, provide sdio driver for emapi
 */
#define EXPORT_TI_SDIO_DRIVER 1
#if EXPORT_TI_SDIO_DRIVER
struct tisdio_platform_data {
	int (*sdio_connect_bus)(void *fCbFunc,
                            void *hCbArg,
                            unsigned int uBlkSizeShift,
                            unsigned int uSdioThreadPriority);
	int (*sdio_disconnect_bus)(void);
	int (*sdio_execute_cmd)(unsigned int uCmd,
                            unsigned int uArg,
                            unsigned int uRespType,
                            void *       pResponse,
                            unsigned int uLen);
	int (*sdio_read)(unsigned int uFunc,
                            unsigned int uHwAddr,
                            void *       pData,
                            unsigned int uLen,
                            unsigned int bBlkMode,
                            unsigned int bIncAddr,
                            unsigned int bMore);
	int (*sdio_write)(unsigned int uFunc,
                            unsigned int uHwAddr,
                            void *       pData,
                            unsigned int uLen,
                            unsigned int bBlkMode,
                            unsigned int bIncAddr,
                            unsigned int bMore);
	int (*sdio_read_direct_bytes)(unsigned int uFunc,
                            unsigned int  uHwAddr,
                            unsigned char *pData,
                            unsigned int  uLen,
                            unsigned int  bMore);
	int (*sdio_write_direct_bytes)(unsigned int uFunc,
                             unsigned int  uHwAddr,
                             unsigned char *pData,
                             unsigned int  uLen,
                             unsigned int  bMore);
};

struct tisdio_platform_data ti_sdio_dev_data =
{
    sdioDrv_ConnectBus,
    sdioDrv_DisconnectBus,
    sdioDrv_ExecuteCmd,
    sdioDrv_ReadSync,
    sdioDrv_WriteSync,
    sdioDrv_ReadDirectBytes,
    sdioDrv_WriteDirectBytes,
};

static void tisdio_dev_release(struct device *dev)
{
	return;
}

static struct platform_device tisdio_dev = {
	.name           = "ti_sdio_driver",
	.id             = 1,
	.num_resources  = 0,
	.resource       = NULL,
	.dev		= {
		.release = tisdio_dev_release,
	},
};

#endif



int mmc_io_rw_extended(struct mmc_card *card, int write, unsigned fn,
		unsigned addr, int incr_addr, u8 *buf, unsigned blocks, unsigned blksz)
{
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	struct scatterlist sg;

	BUG_ON(!card);
	BUG_ON(fn > 7);
	BUG_ON(blocks == 1 && blksz > 512);
	WARN_ON(blocks == 0);
	WARN_ON(blksz == 0);

	memset(&mrq, 0, sizeof(struct mmc_request));
	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&data, 0, sizeof(struct mmc_data));

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_IO_RW_EXTENDED;
	cmd.arg = write ? 0x80000000 : 0x00000000;
	cmd.arg |= fn << 28;
	cmd.arg |= incr_addr ? 0x04000000 : 0x00000000;
	cmd.arg |= addr << 9;
	if (blocks == 1 && blksz <= 512)
		cmd.arg |= (blksz == 512) ? 0 : blksz;	/* byte mode */
	else
		cmd.arg |= 0x08000000 | blocks;		/* block mode */
	cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;

	data.blksz = blksz;
	data.blocks = blocks;
	data.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, buf, blksz * blocks);

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(card->host, &mrq);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	if (mmc_host_is_spi(card->host)) {
		/* host driver already reported errors */
	} else {
		if (cmd.resp[0] & R5_ERROR)
			return -EIO;
		if (cmd.resp[0] & R5_FUNCTION_NUMBER)
			return -EINVAL;
		if (cmd.resp[0] & R5_OUT_OF_RANGE)
			return -ERANGE;
	}

	return 0;
}

/* This definition is very important. It should match the platform device
   definition. E.g., in msm's devices file, the following has been added
   to the second mmc device:

   static struct platform_device msm_sdc1_device = {
   .name		= "TIWLAN_SDIO",
   .id		= 1,
   .num_resources	= ARRAY_SIZE(msm_sdc1_resources),
   .resource	= msm_sdc1_resources,
   .dev		= {
   .coherent_dma_mask	= 0xffffffff,
   },
   };
   */
#define SDIO_DRIVER_NAME 			"TIWLAN_SDIO"

/* TODO: Platform specific */
#define TIWLAN_MMC_CONTROLLER              1

typedef struct MSM_sdiodrv
{
	struct platform_device *pdev;
	void          (*BusTxnCB)(void* BusTxnHandle, int status);
	void*         BusTxnHandle;
	unsigned int  uBlkSize;
	unsigned int  uBlkSizeShift;
	int (*wlanDrvIf_pm_resume)(void);
	int (*wlanDrvIf_pm_suspend)(void);
	struct device *dev;
} MSM_sdiodrv_t;

MSM_sdiodrv_t g_drv;

module_param(g_sdio_debug_level, int, 0644);
MODULE_PARM_DESC(g_sdio_debug_level, "debug level");
int g_sdio_debug_level = SDIO_DEBUGLEVEL_ERR;
EXPORT_SYMBOL( g_sdio_debug_level);

struct platform_device *sdioDrv_get_platform_device(void)
{
	return g_drv.pdev;
}

int sdioDrv_ExecuteCmd (unsigned int uCmd,
		unsigned int uArg,
		unsigned int uRespType,
		void *       pResponse,
		unsigned int uLen)
{
	int read, i = 0;
	int err;
	struct mmc_command cmd;
	unsigned char* ret = ((unsigned char*)pResponse);

	memset(&cmd, 0, sizeof(struct mmc_command));
	cmd.opcode = uCmd;
	cmd.arg = uArg;
	cmd.flags = uRespType;

	PDEBUG("sdioDrv_ExecuteCmd() starting cmd %02x arg %08x\n", (int)uCmd, (int)uArg);

	err = mmc_wait_for_cmd(mmc_get_drvdata(g_drv.pdev), &cmd, 3);
	if (err) {
		PERR("sdioDrv_ExecuteCmd fail: %d\n", err);
		return 1;
	}

	// Copy the result back to the argument
	i = 0;
	while ((uLen > 0) && (ret))
	{
		read = (uLen < 4) ? uLen : 4;
		memcpy(ret, &cmd.resp[i], uLen);
		ret+= read;
		uLen-= read;
		i++;
	}

	return 0;
}

int sdioDrv_ReadSync (unsigned int uFunc,
		unsigned int uHwAddr,
		void *       pData,
		unsigned int uLen,
		unsigned int bBlkMode,
		unsigned int bIncAddr,
		unsigned int bMore)
{
	int          iStatus;
	struct mmc_card scard;
	int num_blocks;

	if (bBlkMode) {
		memset(&scard, 0, sizeof(struct mmc_card));
		scard.cccr.multi_block = 1;
		scard.type = MMC_TYPE_SDIO;
		scard.host = mmc_get_drvdata(g_drv.pdev);

		num_blocks = uLen / g_drv.uBlkSize;
		iStatus = mmc_io_rw_extended(&scard, 0, uFunc, uHwAddr, bIncAddr, pData, num_blocks, g_drv.uBlkSize);

	} else {
		memset(&scard, 0, sizeof(struct mmc_card));
		scard.cccr.multi_block = 0; //don't use block mode for now
		scard.type = MMC_TYPE_SDIO;
		scard.host = mmc_get_drvdata(g_drv.pdev);

		iStatus = mmc_io_rw_extended(&scard, 0, uFunc, uHwAddr, bIncAddr, pData, 1, uLen);
	}

	if (iStatus != 0) {
		PERR("%s FAILED(%d)!!\n", __func__, iStatus);
	}

	return iStatus;
}


/*--------------------------------------------------------------------------------------*/
int sdioDrv_WriteSync      (unsigned int uFunc,
		unsigned int uHwAddr,
		void *       pData,
		unsigned int uLen,
		unsigned int bBlkMode,
		unsigned int bIncAddr,
		unsigned int bMore)
{
	int          iStatus;
	struct mmc_card scard;
	int num_blocks;

	if (bBlkMode) {
		memset(&scard, 0, sizeof(struct mmc_card));
		scard.cccr.multi_block = 1;
		scard.type = MMC_TYPE_SDIO;
		scard.host = mmc_get_drvdata(g_drv.pdev);

		num_blocks = uLen / g_drv.uBlkSize;
		iStatus = mmc_io_rw_extended(&scard, 1, uFunc, uHwAddr, bIncAddr, pData, num_blocks, g_drv.uBlkSize);

	} else {
		memset(&scard, 0, sizeof(struct mmc_card));
		scard.cccr.multi_block = 0;
		scard.type = MMC_TYPE_SDIO;
		scard.host = mmc_get_drvdata(g_drv.pdev);

		iStatus = mmc_io_rw_extended(&scard, 1, uFunc, uHwAddr, bIncAddr, pData, 1, uLen);
	}

	if (iStatus != 0) {
		PERR("%s FAILED(%d)!!\n", __func__, iStatus);
	}

	return iStatus;
}

/*--------------------------------------------------------------------------------------*/

int sdioDrv_ReadDirectBytes(unsigned int  uFunc,
		unsigned int  uHwAddr,
		unsigned char *pData,
		unsigned int  uLen,
		unsigned int  bMore)
{
	unsigned int i;
	int          iStatus;
	struct mmc_command cmd;

	for (i = 0; i < uLen; i++) {
		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = SD_IO_RW_DIRECT;
		cmd.arg = 0x00000000; //read
		cmd.arg |= uFunc << 28;
		cmd.arg |= 0x00000000; //not write
		cmd.arg |= uHwAddr << 9;
		cmd.arg |= 0; //no in
		cmd.flags = MMC_RSP_PRESENT;

		iStatus = mmc_wait_for_cmd(mmc_get_drvdata(g_drv.pdev), &cmd, 0);
		if (iStatus) {
			PERR("sdioDrv_WriteSyncBytes() SDIO Command error status = %d\n", iStatus);
			return -1;
		}

		*pData = cmd.resp[0] & 0xFF;

		uHwAddr++;
		pData++;
	}

	return 0;
}

/*--------------------------------------------------------------------------------------*/

int sdioDrv_WriteDirectBytes(unsigned int  uFunc,
		unsigned int  uHwAddr,
		unsigned char *pData,
		unsigned int  uLen,
		unsigned int  bMore)
{
	unsigned int i;
	int          iStatus;
	struct mmc_command cmd;

	for (i = 0; i < uLen; i++) {
		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = SD_IO_RW_DIRECT;
		cmd.arg = 0x80000000; //write
		cmd.arg |= uFunc << 28;
		cmd.arg |= 0x00000000; //no out
		cmd.arg |= uHwAddr << 9;
		cmd.arg |= *pData; //in
		cmd.flags = MMC_RSP_PRESENT;

		iStatus = mmc_wait_for_cmd(mmc_get_drvdata(g_drv.pdev), &cmd, 3);
		if (iStatus) {
			PERR("sdioDrv_WriteSyncBytes() SDIO Command error status = %d\n", iStatus);
			return -1;
		}

		uHwAddr++;
		pData++;
	}

	return 0;
}

unsigned int sdioDrv_status(struct device* dev)
{
	return 1;
}

int sdioDrv_ConnectBus (void *       fCbFunc,
                        void *       hCbArg,
                        unsigned int uBlkSizeShift,
                        unsigned int  uSdioThreadPriority)
{
	g_drv.BusTxnCB      = fCbFunc;
	g_drv.BusTxnHandle  = hCbArg;
	g_drv.uBlkSizeShift = uBlkSizeShift;
	g_drv.uBlkSize      = 1 << uBlkSizeShift;

	return 0;
}

int sdioDrv_DisconnectBus (void)
{
	g_drv.BusTxnCB      = NULL;
	g_drv.BusTxnHandle  = 0;
	g_drv.uBlkSizeShift = 0;
	g_drv.uBlkSize      = 0;

	return 0;
}

int sdioDrv_set_clock(unsigned int clock)
{
	struct mmc_ios ios;
	struct mmc_host *mmc = 0;


	mmc = mmc_get_drvdata(g_drv.pdev);
	mmc->index = TIWLAN_MMC_CONTROLLER;

	printk(KERN_INFO "[ %s ] Settin clock %u Hz\n", __func__, clock);
	memset(&ios, 0, sizeof(struct mmc_ios));
	ios.bus_width = TIWLAN_SDIO_BUSWIDE;
	ios.power_mode = MMC_POWER_ON;
	ios.clock =  clock;
	mmc->ops->set_ios(mmc, &ios);
	return 0;
}


static int sdioDrv_probe(struct platform_device *pdev)
{
	int rc;
	struct mmc_ios ios;
	struct mmc_platform_data* plat = pdev->dev.platform_data;
	struct mmc_host *mmc = 0;

	printk(KERN_INFO "[ %s ] BEGIN\n", __func__);
	if (pdev->id != TIWLAN_MMC_CONTROLLER) {
		dev_err(&pdev->dev, "TIWLAN driver will not drive mmc device %d\n", pdev->id);
		return -EBUSY;
	}

	printk(KERN_INFO "[ %s ] Probing the controller\n", __func__);
	plat->ocr_mask = TIWLAN_MMC_CONTROLLER;
	plat->status = sdioDrv_status;
	rc = msmsdcc_probe(pdev);
	if (rc)
		return rc;

	mmc = mmc_get_drvdata(pdev);
	mmc->index = TIWLAN_MMC_CONTROLLER;

	printk(KERN_INFO "[ %s ] Setting bus width to %d bits, power on, clock %dHz\n", __func__,
			(TIWLAN_SDIO_BUSWIDE == MMC_BUS_WIDTH_4 ? 4 : 1), TIWLAN_SDIO_CLOCK);
	memset(&ios, 0, sizeof(struct mmc_ios));
	ios.bus_width = TIWLAN_SDIO_BUSWIDE;
	ios.power_mode = MMC_POWER_UP;
	ios.clock = TIWLAN_SDIO_CLOCK;
	ios.vdd = 1;
	mmc->ops->set_ios(mmc, &ios);

	mdelay(100);

	ios.bus_width = TIWLAN_SDIO_BUSWIDE;
	ios.power_mode = MMC_POWER_ON;
	ios.clock =  TIWLAN_SDIO_CLOCK;
	ios.vdd = 1;
	mmc->ops->set_ios(mmc, &ios);
	/* remember device struct */
	g_drv.pdev = pdev;


#if EXPORT_TI_SDIO_DRIVER
    /* Added by daniel, provide sdio interface for emapi
     */
    tisdio_dev.dev.platform_data = &ti_sdio_dev_data;
    platform_device_register(&tisdio_dev);
#endif


	return rc;
}

static int sdioDrv_remove(struct platform_device *pdev)
{

#if EXPORT_TI_SDIO_DRIVER
    /* Added by daniel, provide sdio interface for emapi
     */
    platform_device_unregister(&tisdio_dev);
#endif

	return msmsdcc_remove(pdev);
}

static int sdioDrv_suspend(struct platform_device *pdev, pm_message_t state)
{

	// printk(KERN_INFO "TISDIO: Asking TIWLAN to suspend\n");

#if 0
	/* Tell WLAN driver to suspend, if a suspension function has been registered */
	if (g_drv.wlanDrvIf_pm_suspend) {
		printk(KERN_INFO "TISDIO: Asking TIWLAN to suspend\n");
		rc = g_drv.wlanDrvIf_pm_suspend();
		if (rc != 0)
			return rc;
	}
#endif

	return msmsdcc_suspend(pdev, state);
}

/* Routine to resume the MMC device */
static int sdioDrv_resume(struct platform_device *pdev)
{
	int rc;

	// printk(KERN_INFO "TISDIO: Asking TIWLAN to resume\n");
	rc = msmsdcc_resume(pdev);
	return rc;
#if 0
	if (rc)
		return rc;

	if (g_drv.wlanDrvIf_pm_resume) {
		printk(KERN_INFO "TISDIO: Asking TIWLAN to resume\n");
		return(g_drv.wlanDrvIf_pm_resume());
	}
	else
		return 0;
#endif
}

static struct platform_driver sdioDrv_struct = {
	.probe		= sdioDrv_probe,
	.remove		= sdioDrv_remove,
	.suspend	= sdioDrv_suspend,
	.resume		= sdioDrv_resume,
	.driver		= {
		.name = SDIO_DRIVER_NAME,
	},
};

void sdioDrv_register_pm(int (*wlanDrvIf_Start)(void),
		int (*wlanDrvIf_Stop)(void))
{
	g_drv.wlanDrvIf_pm_resume = wlanDrvIf_Start;
	g_drv.wlanDrvIf_pm_suspend = wlanDrvIf_Stop;
}

static int __init sdioDrv_init(void)
{
	/* Register the sdio driver */
	return platform_driver_register(&sdioDrv_struct);
}

static void __exit sdioDrv_exit(void)
{
	/* Unregister sdio driver */
	platform_driver_unregister(&sdioDrv_struct);
}

module_init(sdioDrv_init);
module_exit(sdioDrv_exit);

EXPORT_SYMBOL(sdioDrv_ConnectBus);
EXPORT_SYMBOL(sdioDrv_DisconnectBus);
EXPORT_SYMBOL(sdioDrv_ExecuteCmd);
EXPORT_SYMBOL(sdioDrv_ReadSync);
EXPORT_SYMBOL(sdioDrv_WriteSync);
EXPORT_SYMBOL(sdioDrv_ReadDirectBytes);
EXPORT_SYMBOL(sdioDrv_WriteDirectBytes);
EXPORT_SYMBOL(sdioDrv_register_pm);
EXPORT_SYMBOL(sdioDrv_get_platform_device);
EXPORT_SYMBOL(sdioDrv_set_clock);

MODULE_DESCRIPTION("TI WLAN SDIO driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS(SDIO_DRIVER_NAME);
MODULE_AUTHOR("Texas Instruments Inc");
