// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Medifield PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2010 Intel Corporation. All Rights Reserved.
 *
 * Copyright (c) 2010 Silicon Hive www.siliconhive.com.
 */
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/pm_runtime.h>
#include <linux/timer.h>

#include <asm/iosf_mbi.h>

#include <media/v4l2-event.h>

#define CREATE_TRACE_POINTS
#include "atomisp_trace_event.h"

#include "atomisp_cmd.h"
#include "atomisp_common.h"
#include "atomisp_fops.h"
#include "atomisp_internal.h"
#include "atomisp-regs.h"
#include "atomisp_compat.h"
#include "atomisp_subdev.h"
#include "atomisp_dfs_tables.h"

#include <hmm/hmm.h>

#include "sh_css_hrt.h"
#include "sh_css_defs.h"
#include "system_global.h"
#include "sh_css_internal.h"
#include "sh_css_sp.h"
#include "gp_device.h"
#include "device_access.h"
#include "irq.h"

#include "ia_css_types.h"
#include "ia_css_stream.h"
#include "ia_css_debug.h"
#include "bits.h"

union host {
	struct {
		void *kernel_ptr;
		void __user *user_ptr;
		int size;
	} scalar;
	struct {
		void *hmm_ptr;
	} ptr;
};

/*
 * get sensor:dis71430/ov2720 related info from v4l2_subdev->priv data field.
 * subdev->priv is set in mrst.c
 */
struct camera_mipi_info *atomisp_to_sensor_mipi_info(struct v4l2_subdev *sd)
{
	return (struct camera_mipi_info *)v4l2_get_subdev_hostdata(sd);
}

/*
 * get struct atomisp_video_pipe from v4l2 video_device
 */
struct atomisp_video_pipe *atomisp_to_video_pipe(struct video_device *dev)
{
	return (struct atomisp_video_pipe *)
	       container_of(dev, struct atomisp_video_pipe, vdev);
}

static unsigned short atomisp_get_sensor_fps(struct atomisp_sub_device *asd)
{
	struct v4l2_subdev_frame_interval fi = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct atomisp_device *isp = asd->isp;

	unsigned short fps = 0;
	int ret;

	ret = v4l2_subdev_call_state_active(isp->inputs[asd->input_curr].csi_remote_source,
					    pad, get_frame_interval, &fi);

	if (!ret && fi.interval.numerator)
		fps = fi.interval.denominator / fi.interval.numerator;

	return fps;
}

static unsigned int atomisp_hflip_bayer_order(unsigned int bayer_order)
{
	switch (bayer_order) {
	case IA_CSS_BAYER_ORDER_RGGB:
		return IA_CSS_BAYER_ORDER_GRBG;
	case IA_CSS_BAYER_ORDER_BGGR:
		return IA_CSS_BAYER_ORDER_GBRG;
	case IA_CSS_BAYER_ORDER_GRBG:
		return IA_CSS_BAYER_ORDER_RGGB;
	case IA_CSS_BAYER_ORDER_GBRG:
		return IA_CSS_BAYER_ORDER_BGGR;
	default:
		return bayer_order;
	}
}

static unsigned int atomisp_vflip_bayer_order(unsigned int bayer_order)
{
	switch (bayer_order) {
	case IA_CSS_BAYER_ORDER_RGGB:
		return IA_CSS_BAYER_ORDER_GBRG;
	case IA_CSS_BAYER_ORDER_BGGR:
		return IA_CSS_BAYER_ORDER_GRBG;
	case IA_CSS_BAYER_ORDER_GRBG:
		return IA_CSS_BAYER_ORDER_BGGR;
	case IA_CSS_BAYER_ORDER_GBRG:
		return IA_CSS_BAYER_ORDER_RGGB;
	default:
		return bayer_order;
	}
}

static unsigned int
atomisp_get_effective_bayer_order(struct atomisp_input_subdev *input,
				  unsigned int bayer_order)
{
	struct v4l2_control ctrl = { 0 };
	bool hflip = false;
	bool vflip = false;

	if (!input->sensor || !input->sensor->ctrl_handler)
		return bayer_order;

	ctrl.id = V4L2_CID_HFLIP;
	if (!v4l2_g_ctrl(input->sensor->ctrl_handler, &ctrl))
		hflip = !!ctrl.value;

	ctrl.id = V4L2_CID_VFLIP;
	if (!v4l2_g_ctrl(input->sensor->ctrl_handler, &ctrl))
		vflip = !!ctrl.value;

	if (hflip)
		bayer_order = atomisp_hflip_bayer_order(bayer_order);

	if (vflip)
		bayer_order = atomisp_vflip_bayer_order(bayer_order);

	return bayer_order;
}

/*
 * DFS progress is shown as follows:
 * 1. Target frequency is calculated according to FPS/Resolution/ISP running
 *    mode.
 * 2. Ratio is calculated using formula: 2 * HPLL / target frequency - 1
 *    with proper rounding.
 * 3. Set ratio to ISPFREQ40, 1 to FREQVALID and ISPFREQGUAR40
 *    to 200MHz in ISPSSPM1.
 * 4. Wait for FREQVALID to be cleared by P-Unit.
 * 5. Wait for field ISPFREQSTAT40 in ISPSSPM1 turn to ratio set in 3.
 */
static int write_target_freq_to_hw(struct atomisp_device *isp,
				   unsigned int new_freq)
{
	unsigned int ratio, timeout, guar_ratio;
	u32 isp_sspm1 = 0;
	int i;

	if (!isp->hpll_freq) {
		dev_err(isp->dev, "failed to get hpll_freq. no change to freq\n");
		return -EINVAL;
	}

	iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
	if (isp_sspm1 & ISP_FREQ_VALID_MASK) {
		dev_dbg(isp->dev, "clearing ISPSSPM1 valid bit.\n");
		iosf_mbi_write(BT_MBI_UNIT_PMC, MBI_REG_WRITE, ISPSSPM1,
			       isp_sspm1 & ~(1 << ISP_FREQ_VALID_OFFSET));
	}

	ratio = (2 * isp->hpll_freq + new_freq / 2) / new_freq - 1;
	guar_ratio = (2 * isp->hpll_freq + 200 / 2) / 200 - 1;

	iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
	isp_sspm1 &= ~(0x1F << ISP_REQ_FREQ_OFFSET);

	for (i = 0; i < ISP_DFS_TRY_TIMES; i++) {
		iosf_mbi_write(BT_MBI_UNIT_PMC, MBI_REG_WRITE, ISPSSPM1,
			       isp_sspm1
			       | ratio << ISP_REQ_FREQ_OFFSET
			       | 1 << ISP_FREQ_VALID_OFFSET
			       | guar_ratio << ISP_REQ_GUAR_FREQ_OFFSET);

		iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
		timeout = 20;
		while ((isp_sspm1 & ISP_FREQ_VALID_MASK) && timeout) {
			iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
			dev_dbg(isp->dev, "waiting for ISPSSPM1 valid bit to be 0.\n");
			udelay(100);
			timeout--;
		}

		if (timeout != 0)
			break;
	}

	if (timeout == 0) {
		dev_err(isp->dev, "DFS failed due to HW error.\n");
		return -EINVAL;
	}

	iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
	timeout = 10;
	while (((isp_sspm1 >> ISP_FREQ_STAT_OFFSET) != ratio) && timeout) {
		iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
		dev_dbg(isp->dev, "waiting for ISPSSPM1 status bit to be 0x%x.\n",
			new_freq);
		udelay(100);
		timeout--;
	}
	if (timeout == 0) {
		dev_err(isp->dev, "DFS target freq is rejected by HW.\n");
		return -EINVAL;
	}

	return 0;
}

int atomisp_freq_scaling(struct atomisp_device *isp,
			 enum atomisp_dfs_mode mode,
			 bool force)
{
	const struct atomisp_dfs_config *dfs;
	unsigned int new_freq;
	struct atomisp_freq_scaling_rule curr_rules;
	int i, ret;
	unsigned short fps = 0;

	dfs = isp->dfs;

	if (dfs->lowest_freq == 0 || dfs->max_freq_at_vmin == 0 ||
	    dfs->highest_freq == 0 || dfs->dfs_table_size == 0 ||
	    !dfs->dfs_table) {
		dev_err(isp->dev, "DFS configuration is invalid.\n");
		return -EINVAL;
	}

	if (mode == ATOMISP_DFS_MODE_LOW) {
		new_freq = dfs->lowest_freq;
		goto done;
	}

	if (mode == ATOMISP_DFS_MODE_MAX) {
		new_freq = dfs->highest_freq;
		goto done;
	}

	fps = atomisp_get_sensor_fps(&isp->asd);
	if (fps == 0) {
		dev_info(isp->dev,
			 "Sensor didn't report FPS. Using DFS max mode.\n");
		new_freq = dfs->highest_freq;
		goto done;
	}

	curr_rules.width = isp->asd.fmt[ATOMISP_SUBDEV_PAD_SOURCE].fmt.width;
	curr_rules.height = isp->asd.fmt[ATOMISP_SUBDEV_PAD_SOURCE].fmt.height;
	curr_rules.fps = fps;
	curr_rules.run_mode = isp->asd.run_mode->val;

	/* search for the target frequency by looping freq rules*/
	for (i = 0; i < dfs->dfs_table_size; i++) {
		if (curr_rules.width != dfs->dfs_table[i].width &&
		    dfs->dfs_table[i].width != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.height != dfs->dfs_table[i].height &&
		    dfs->dfs_table[i].height != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.fps != dfs->dfs_table[i].fps &&
		    dfs->dfs_table[i].fps != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.run_mode != dfs->dfs_table[i].run_mode &&
		    dfs->dfs_table[i].run_mode != ISP_FREQ_RULE_ANY)
			continue;
		break;
	}

	if (i == dfs->dfs_table_size)
		new_freq = dfs->max_freq_at_vmin;
	else
		new_freq = dfs->dfs_table[i].isp_freq;

done:
	dev_dbg(isp->dev, "DFS target frequency=%d.\n", new_freq);

	if ((new_freq == isp->running_freq) && !force)
		return 0;

	dev_dbg(isp->dev, "Programming DFS frequency to %d\n", new_freq);

	ret = write_target_freq_to_hw(isp, new_freq);
	if (!ret) {
		isp->running_freq = new_freq;
		trace_ipu_pstate(new_freq, -1);
	}
	return ret;
}

/*
 * reset and restore ISP
 */
int atomisp_reset(struct atomisp_device *isp)
{
	/* Reset ISP by power-cycling it */
	int ret = 0;

	dev_dbg(isp->dev, "%s\n", __func__);

	ret = atomisp_power_off(isp->dev);
	if (ret < 0)
		dev_err(isp->dev, "atomisp_power_off failed, %d\n", ret);

	ret = atomisp_power_on(isp->dev);
	if (ret < 0) {
		dev_err(isp->dev, "atomisp_power_on failed, %d\n", ret);
		isp->isp_fatal_error = true;
	}

	return ret;
}

/*
 * interrupt disable functions
 */
static void disable_isp_irq(enum hrt_isp_css_irq irq)
{
	irq_disable_channel(IRQ0_ID, irq);

	if (irq != hrt_isp_css_irq_sp)
		return;

	cnd_sp_irq_enable(SP0_ID, false);
}

/*
 * interrupt clean function
 */
static void clear_isp_irq(enum hrt_isp_css_irq irq)
{
	irq_clear_all(IRQ0_ID);
}

void atomisp_msi_irq_init(struct atomisp_device *isp)
{
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	u32 msg32;
	u16 msg16;

	pci_read_config_dword(pdev, PCI_MSI_CAPID, &msg32);
	msg32 |= 1 << MSI_ENABLE_BIT;
	pci_write_config_dword(pdev, PCI_MSI_CAPID, msg32);

	msg32 = (1 << INTR_IER) | (1 << INTR_IIR);
	pci_write_config_dword(pdev, PCI_INTERRUPT_CTRL, msg32);

	pci_read_config_word(pdev, PCI_COMMAND, &msg16);
	msg16 |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
		  PCI_COMMAND_INTX_DISABLE);
	pci_write_config_word(pdev, PCI_COMMAND, msg16);
}

void atomisp_msi_irq_uninit(struct atomisp_device *isp)
{
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	u32 msg32;
	u16 msg16;

	pci_read_config_dword(pdev, PCI_MSI_CAPID, &msg32);
	msg32 &=  ~(1 << MSI_ENABLE_BIT);
	pci_write_config_dword(pdev, PCI_MSI_CAPID, msg32);

	msg32 = 0x0;
	pci_write_config_dword(pdev, PCI_INTERRUPT_CTRL, msg32);

	pci_read_config_word(pdev, PCI_COMMAND, &msg16);
	msg16 &= ~(PCI_COMMAND_MASTER);
	pci_write_config_word(pdev, PCI_COMMAND, msg16);
}

static void atomisp_sof_event(struct atomisp_sub_device *asd)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_FRAME_SYNC;
	event.u.frame_sync.frame_sequence = atomic_read(&asd->sof_count);

	v4l2_event_queue(asd->subdev.devnode, &event);
}

void atomisp_eof_event(struct atomisp_sub_device *asd, uint8_t exp_id)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_FRAME_END;
	event.u.frame_sync.frame_sequence = exp_id;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_3a_stats_ready_event(struct atomisp_sub_device *asd,
	uint8_t exp_id)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_3A_STATS_READY;
	event.u.frame_sync.frame_sequence = exp_id;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_metadata_ready_event(struct atomisp_sub_device *asd,
	enum atomisp_metadata_type md_type)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_METADATA_READY;
	event.u.data[0] = md_type;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_reset_event(struct atomisp_sub_device *asd)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_CSS_RESET;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void print_csi_rx_errors(enum mipi_port_id port,
				struct atomisp_device *isp)
{
	u32 infos = 0;

	atomisp_css_rx_get_irq_info(port, &infos);

	dev_err(isp->dev, "CSI Receiver port %d errors:\n", port);
	if (infos & IA_CSS_RX_IRQ_INFO_BUFFER_OVERRUN)
		dev_err(isp->dev, "  buffer overrun");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_SOT)
		dev_err(isp->dev, "  start-of-transmission error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_SOT_SYNC)
		dev_err(isp->dev, "  start-of-transmission sync error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_CONTROL)
		dev_err(isp->dev, "  control error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_ECC_DOUBLE)
		dev_err(isp->dev, "  2 or more ECC errors");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_CRC)
		dev_err(isp->dev, "  CRC mismatch");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_UNKNOWN_ID)
		dev_err(isp->dev, "  unknown error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_FRAME_SYNC)
		dev_err(isp->dev, "  frame sync error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_FRAME_DATA)
		dev_err(isp->dev, "  frame data error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_DATA_TIMEOUT)
		dev_err(isp->dev, "  data timeout");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_UNKNOWN_ESC)
		dev_err(isp->dev, "  unknown escape command entry");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_LINE_SYNC)
		dev_err(isp->dev, "  line sync error");
}

/* Clear irq reg */
static void clear_irq_reg(struct atomisp_device *isp)
{
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	u32 msg_ret;

	pci_read_config_dword(pdev, PCI_INTERRUPT_CTRL, &msg_ret);
	msg_ret |= 1 << INTR_IIR;
	pci_write_config_dword(pdev, PCI_INTERRUPT_CTRL, msg_ret);
}

/* interrupt handling function*/
irqreturn_t atomisp_isr(int irq, void *dev)
{
	struct atomisp_device *isp = (struct atomisp_device *)dev;
	struct atomisp_css_event eof_event;
	unsigned int irq_infos = 0;
	unsigned long flags;
	int err;

	spin_lock_irqsave(&isp->lock, flags);

	if (!isp->css_initialized) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_HANDLED;
	}
	err = atomisp_css_irq_translate(isp, &irq_infos);
	if (err) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_NONE;
	}

	clear_irq_reg(isp);

	if (!isp->asd.streaming)
		goto out_nowake;

	if (irq_infos & IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF) {
		atomic_inc(&isp->asd.sof_count);
		atomisp_sof_event(&isp->asd);

		/*
		 * If sequence_temp and sequence are the same there where no frames
		 * lost so we can increase sequence_temp.
		 * If not then processing of frame is still in progress and driver
		 * needs to keep old sequence_temp value.
		 * NOTE: There is assumption here that ISP will not start processing
		 * next frame from sensor before old one is completely done.
		 */
		if (atomic_read(&isp->asd.sequence) == atomic_read(&isp->asd.sequence_temp))
			atomic_set(&isp->asd.sequence_temp, atomic_read(&isp->asd.sof_count));

		dev_dbg_ratelimited(isp->dev, "irq:0x%x (SOF)\n", irq_infos);
		irq_infos &= ~IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF;
	}

	if (irq_infos & IA_CSS_IRQ_INFO_EVENTS_READY)
		atomic_set(&isp->asd.sequence, atomic_read(&isp->asd.sequence_temp));

	if ((irq_infos & IA_CSS_IRQ_INFO_INPUT_SYSTEM_ERROR) ||
	    (irq_infos & IA_CSS_IRQ_INFO_IF_ERROR)) {
		/* handle mipi receiver error */
		u32 rx_infos;
		enum mipi_port_id port;

		for (port = MIPI_PORT0_ID; port <= MIPI_PORT2_ID;
		     port++) {
			print_csi_rx_errors(port, isp);
			atomisp_css_rx_get_irq_info(port, &rx_infos);
			atomisp_css_rx_clear_irq_info(port, rx_infos);
		}
	}

	if (irq_infos & IA_CSS_IRQ_INFO_ISYS_EVENTS_READY) {
		while (ia_css_dequeue_isys_event(&eof_event.event) == 0) {
			atomisp_eof_event(&isp->asd, eof_event.event.exp_id);
			dev_dbg_ratelimited(isp->dev, "ISYS event: EOF exp_id %d\n",
					    eof_event.event.exp_id);
		}

		irq_infos &= ~IA_CSS_IRQ_INFO_ISYS_EVENTS_READY;
		if (irq_infos == 0)
			goto out_nowake;
	}

	spin_unlock_irqrestore(&isp->lock, flags);

	dev_dbg_ratelimited(isp->dev, "irq:0x%x (unhandled)\n", irq_infos);

	return IRQ_WAKE_THREAD;

out_nowake:
	spin_unlock_irqrestore(&isp->lock, flags);

	if (irq_infos)
		dev_dbg_ratelimited(isp->dev, "irq:0x%x (ignored, as not streaming anymore)\n",
				    irq_infos);

	return IRQ_HANDLED;
}

void atomisp_clear_css_buffer_counters(struct atomisp_sub_device *asd)
{
	int i;

	memset(asd->s3a_bufs_in_css, 0, sizeof(asd->s3a_bufs_in_css));
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++)
		memset(asd->metadata_bufs_in_css[i], 0,
		       sizeof(asd->metadata_bufs_in_css[i]));
	asd->dis_bufs_in_css = 0;
}

/* 0x100000 is the start of dmem inside SP */
#define SP_DMEM_BASE	0x100000

void dump_sp_dmem(struct atomisp_device *isp, unsigned int addr,
		  unsigned int size)
{
	unsigned int data = 0;
	unsigned int size32 = DIV_ROUND_UP(size, sizeof(u32));

	dev_dbg(isp->dev, "atomisp mmio base: %p\n", isp->base);
	dev_dbg(isp->dev, "%s, addr:0x%x, size: %d, size32: %d\n", __func__,
		addr, size, size32);
	if (size32 * 4 + addr > 0x4000) {
		dev_err(isp->dev, "illegal size (%d) or addr (0x%x)\n",
			size32, addr);
		return;
	}
	addr += SP_DMEM_BASE;
	addr &= 0x003FFFFF;
	do {
		data = readl(isp->base + addr);
		dev_dbg(isp->dev, "%s, \t [0x%x]:0x%x\n", __func__, addr, data);
		addr += sizeof(u32);
	} while (--size32);
}

int atomisp_buffers_in_css(struct atomisp_video_pipe *pipe)
{
	unsigned long irqflags;
	struct list_head *pos;
	int buffers_in_css = 0;

	spin_lock_irqsave(&pipe->irq_lock, irqflags);

	list_for_each(pos, &pipe->buffers_in_css)
		buffers_in_css++;

	spin_unlock_irqrestore(&pipe->irq_lock, irqflags);

	return buffers_in_css;
}

void atomisp_buffer_done(struct ia_css_frame *frame, enum vb2_buffer_state state)
{
	struct atomisp_video_pipe *pipe = vb_to_pipe(&frame->vb.vb2_buf);

	lockdep_assert_held(&pipe->irq_lock);

	frame->vb.vb2_buf.timestamp = ktime_get_ns();
	frame->vb.field = pipe->pix.field;
	frame->vb.sequence = atomic_read(&pipe->asd->sequence);
	dev_info(pipe->isp->dev,
		 "ATOMISP_GEOM vb2_done vb=%u state=%u seq=%u exp=%u isp_cfg=%u dynq=%d frame=%ux%u padded=%u data_bytes=%u pix=%ux%u bpl=%u size=%u payload=%u\n",
		 frame->vb.vb2_buf.index, state, frame->vb.sequence,
		 frame->exp_id, frame->isp_config_id, frame->dynamic_queue_id,
		 frame->frame_info.res.width, frame->frame_info.res.height,
		 frame->frame_info.padded_width, frame->data_bytes,
		 pipe->pix.width, pipe->pix.height,
		 pipe->pix.bytesperline, pipe->pix.sizeimage,
		 state == VB2_BUF_STATE_DONE ? pipe->pix.sizeimage : 0);
	list_del(&frame->queue);
	if (state == VB2_BUF_STATE_DONE)
		vb2_set_plane_payload(&frame->vb.vb2_buf, 0, pipe->pix.sizeimage);
	vb2_buffer_done(&frame->vb.vb2_buf, state);
}

void atomisp_flush_video_pipe(struct atomisp_video_pipe *pipe, enum vb2_buffer_state state,
			      bool warn_on_css_frames)
{
	struct ia_css_frame *frame, *_frame;
	unsigned long irqflags;

	spin_lock_irqsave(&pipe->irq_lock, irqflags);

	list_for_each_entry_safe(frame, _frame, &pipe->buffers_in_css, queue) {
		if (warn_on_css_frames)
			dev_warn(pipe->isp->dev, "Warning: CSS frames queued on flush\n");
		atomisp_buffer_done(frame, state);
	}

	list_for_each_entry_safe(frame, _frame, &pipe->activeq, queue)
		atomisp_buffer_done(frame, state);

	list_for_each_entry_safe(frame, _frame, &pipe->buffers_waiting_for_param, queue) {
		pipe->frame_request_config_id[frame->vb.vb2_buf.index] = 0;
		atomisp_buffer_done(frame, state);
	}

	spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
}

/* clean out the parameters that did not apply */
void atomisp_flush_params_queue(struct atomisp_video_pipe *pipe)
{
	struct atomisp_css_params_with_list *param;

	while (!list_empty(&pipe->per_frame_params)) {
		param = list_entry(pipe->per_frame_params.next,
				   struct atomisp_css_params_with_list, list);
		list_del(&param->list);
		atomisp_free_css_parameters(&param->params);
		kvfree(param);
	}
}

/* Re-queue per-frame parameters */
static void atomisp_recover_params_queue(struct atomisp_video_pipe *pipe)
{
	struct atomisp_css_params_with_list *param;
	int i;

	for (i = 0; i < VIDEO_MAX_FRAME; i++) {
		param = pipe->frame_params[i];
		if (param)
			list_add_tail(&param->list, &pipe->per_frame_params);
		pipe->frame_params[i] = NULL;
	}
	atomisp_handle_parameter_and_buffer(pipe);
}

void atomisp_buf_done(struct atomisp_sub_device *asd, int error,
		      enum ia_css_buffer_type buf_type,
		      enum ia_css_pipe_id css_pipe_id,
		      bool q_buffers, enum atomisp_input_stream_id stream_id)
{
	struct atomisp_video_pipe *pipe = NULL;
	struct atomisp_css_buffer buffer;
	bool requeue = false;
	unsigned long irqflags;
	struct ia_css_frame *frame = NULL;
	struct atomisp_s3a_buf *s3a_buf = NULL, *_s3a_buf_tmp, *s3a_iter;
	struct atomisp_dis_buf *dis_buf = NULL, *_dis_buf_tmp, *dis_iter;
	struct atomisp_metadata_buf *md_buf = NULL, *_md_buf_tmp, *md_iter;
	enum atomisp_metadata_type md_type;
	struct atomisp_device *isp = asd->isp;
	int i, err;

	lockdep_assert_held(&isp->mutex);

	if (
	    buf_type != IA_CSS_BUFFER_TYPE_METADATA &&
	    buf_type != IA_CSS_BUFFER_TYPE_3A_STATISTICS &&
	    buf_type != IA_CSS_BUFFER_TYPE_DIS_STATISTICS &&
	    buf_type != IA_CSS_BUFFER_TYPE_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_RAW_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_VF_OUTPUT_FRAME) {
		dev_err(isp->dev, "%s, unsupported buffer type: %d\n",
			__func__, buf_type);
		return;
	}

	memset(&buffer, 0, sizeof(struct atomisp_css_buffer));
	buffer.css_buffer.type = buf_type;
	err = atomisp_css_dequeue_buffer(asd, stream_id, css_pipe_id,
					 buf_type, &buffer);
	if (err) {
		dev_err(isp->dev,
			"atomisp_css_dequeue_buffer failed: 0x%x\n", err);
		return;
	}

	switch (buf_type) {
	case IA_CSS_BUFFER_TYPE_3A_STATISTICS:
		list_for_each_entry_safe(s3a_iter, _s3a_buf_tmp,
					 &asd->s3a_stats_in_css, list) {
			if (s3a_iter->s3a_data ==
			    buffer.css_buffer.data.stats_3a) {
				list_del_init(&s3a_iter->list);
				list_add_tail(&s3a_iter->list,
					      &asd->s3a_stats_ready);
				s3a_buf = s3a_iter;
				break;
			}
		}

		asd->s3a_bufs_in_css[css_pipe_id]--;
		atomisp_3a_stats_ready_event(asd, buffer.css_buffer.exp_id);
		if (s3a_buf)
			dev_dbg(isp->dev, "%s: s3a stat with exp_id %d is ready\n",
				__func__, s3a_buf->s3a_data->exp_id);
		else
			dev_dbg(isp->dev, "%s: s3a stat is ready with no exp_id found\n",
				__func__);
		break;
	case IA_CSS_BUFFER_TYPE_METADATA:
		if (error)
			break;

		md_type = ATOMISP_MAIN_METADATA;
		list_for_each_entry_safe(md_iter, _md_buf_tmp,
					 &asd->metadata_in_css[md_type], list) {
			if (md_iter->metadata ==
			    buffer.css_buffer.data.metadata) {
				list_del_init(&md_iter->list);
				list_add_tail(&md_iter->list,
					      &asd->metadata_ready[md_type]);
				md_buf = md_iter;
				break;
			}
		}
		asd->metadata_bufs_in_css[stream_id][css_pipe_id]--;
		atomisp_metadata_ready_event(asd, md_type);
		if (md_buf)
			dev_dbg(isp->dev, "%s: metadata with exp_id %d is ready\n",
				__func__, md_buf->metadata->exp_id);
		else
			dev_dbg(isp->dev, "%s: metadata is ready with no exp_id found\n",
				__func__);
		break;
	case IA_CSS_BUFFER_TYPE_DIS_STATISTICS:
		list_for_each_entry_safe(dis_iter, _dis_buf_tmp,
					 &asd->dis_stats_in_css, list) {
			if (dis_iter->dis_data ==
			    buffer.css_buffer.data.stats_dvs) {
				spin_lock_irqsave(&asd->dis_stats_lock,
						  irqflags);
				list_del_init(&dis_iter->list);
				list_add(&dis_iter->list, &asd->dis_stats);
				asd->params.dis_proj_data_valid = true;
				spin_unlock_irqrestore(&asd->dis_stats_lock,
						       irqflags);
				dis_buf = dis_iter;
				break;
			}
		}
		asd->dis_bufs_in_css--;
		if (dis_buf)
			dev_dbg(isp->dev, "%s: dis stat with exp_id %d is ready\n",
				__func__, dis_buf->dis_data->exp_id);
		else
			dev_dbg(isp->dev, "%s: dis stat is ready with no exp_id found\n",
				__func__);
		break;
	case IA_CSS_BUFFER_TYPE_VF_OUTPUT_FRAME:
	case IA_CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME:
		frame = buffer.css_buffer.data.frame;
		if (!frame) {
			WARN_ON(1);
			break;
		}
		if (!frame->valid)
			error = true;

		pipe = vb_to_pipe(&frame->vb.vb2_buf);

		dev_dbg(isp->dev, "%s: vf frame with exp_id %d is ready\n",
			__func__, frame->exp_id);
		pipe->frame_config_id[frame->vb.vb2_buf.index] = frame->isp_config_id;
		break;
	case IA_CSS_BUFFER_TYPE_OUTPUT_FRAME:
	case IA_CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME:
		frame = buffer.css_buffer.data.frame;
		if (!frame) {
			WARN_ON(1);
			break;
		}

		if (!frame->valid)
			error = true;

		pipe = vb_to_pipe(&frame->vb.vb2_buf);

		dev_dbg(isp->dev, "%s: main frame with exp_id %d is ready\n",
			__func__, frame->exp_id);
		dev_info(isp->dev,
			 "ATOMISP_GEOM css_done stream=%u pipe=%u type=%u error=%d vb=%u exp=%u isp_cfg=%u dynq=%d valid=%d frame=%ux%u padded=%u data_bytes=%u pix=%ux%u bpl=%u size=%u\n",
			 stream_id, css_pipe_id, buf_type, error,
			 frame->vb.vb2_buf.index, frame->exp_id, frame->isp_config_id,
			 frame->dynamic_queue_id, frame->valid,
			 frame->frame_info.res.width, frame->frame_info.res.height,
			 frame->frame_info.padded_width, frame->data_bytes,
			 pipe->pix.width, pipe->pix.height,
			 pipe->pix.bytesperline, pipe->pix.sizeimage);

		i = frame->vb.vb2_buf.index;

		/* free the parameters */
		if (pipe->frame_params[i]) {
			if (asd->params.dvs_6axis == pipe->frame_params[i]->params.dvs_6axis)
				asd->params.dvs_6axis = NULL;
			atomisp_free_css_parameters(&pipe->frame_params[i]->params);
			kvfree(pipe->frame_params[i]);
			pipe->frame_params[i] = NULL;
		}

		pipe->frame_config_id[i] = frame->isp_config_id;

		if (asd->params.css_update_params_needed) {
			atomisp_apply_css_parameters(asd,
						     &asd->params.css_param);
			if (asd->params.css_param.update_flag.dz_config)
				asd->params.config.dz_config = &asd->params.css_param.dz_config;
			/* New global dvs 6axis config should be blocked
			 * here if there's a buffer with per-frame parameters
			 * pending in CSS frame buffer queue.
			 * This is to aviod zooming vibration since global
			 * parameters take effect immediately while
			 * per-frame parameters are taken after previous
			 * buffers in CSS got processed.
			 */
			if (asd->params.dvs_6axis)
				atomisp_css_set_dvs_6axis(asd,
							  asd->params.dvs_6axis);
			else
				asd->params.css_update_params_needed = false;
			/* The update flag should not be cleaned here
			 * since it is still going to be used to make up
			 * following per-frame parameters.
			 * This will introduce more copy work since each
			 * time when updating global parameters, the whole
			 * parameter set are applied.
			 * FIXME: A new set of parameter copy functions can
			 * be added to make up per-frame parameters based on
			 * solid structures stored in asd->params.css_param
			 * instead of using shadow pointers in update flag.
			 */
			atomisp_css_update_isp_params(asd);
		}
		break;
	default:
		break;
	}
	if (frame) {
		spin_lock_irqsave(&pipe->irq_lock, irqflags);
		atomisp_buffer_done(frame, error ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
	}

	/*
	 * Requeue should only be done for 3a and dis buffers.
	 * Queue/dequeue order will change if driver recycles image buffers.
	 */
	if (requeue) {
		err = atomisp_css_queue_buffer(asd,
					       stream_id, css_pipe_id,
					       buf_type, &buffer);
		if (err)
			dev_err(isp->dev, "%s, q to css fails: %d\n",
				__func__, err);
		return;
	}
	if (!error && q_buffers)
		atomisp_qbuffers_to_css(asd);
}

void atomisp_assert_recovery_work(struct work_struct *work)
{
	struct atomisp_device *isp = container_of(work, struct atomisp_device,
						  assert_recovery_work);
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	unsigned long flags;
	int ret;

	mutex_lock(&isp->mutex);

	if (!isp->asd.streaming)
		goto out_unlock;

	atomisp_css_irq_enable(isp, IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF, false);

	spin_lock_irqsave(&isp->lock, flags);
	isp->asd.streaming = false;
	spin_unlock_irqrestore(&isp->lock, flags);

	/* stream off sensor */
	ret = v4l2_subdev_call(isp->inputs[isp->asd.input_curr].csi_remote_source,
			       video, s_stream, 0);
	if (ret)
		dev_warn(isp->dev, "Stopping sensor stream failed: %d\n", ret);

	atomisp_clear_css_buffer_counters(&isp->asd);

	atomisp_css_stop(&isp->asd, true);

	isp->asd.preview_exp_id = 1;
	isp->asd.postview_exp_id = 1;
	/* notify HAL the CSS reset */
	dev_dbg(isp->dev, "send reset event to %s\n", isp->asd.subdev.devnode->name);
	atomisp_reset_event(&isp->asd);

	/* clear irq */
	disable_isp_irq(hrt_isp_css_irq_sp);
	clear_isp_irq(hrt_isp_css_irq_sp);

	/* Set the SRSE to 3 before resetting */
	pci_write_config_dword(pdev, PCI_I_CONTROL,
			       isp->saved_regs.i_control | MRFLD_PCI_I_CONTROL_SRSE_RESET_MASK);

	/* reset ISP and restore its state */
	atomisp_reset(isp);

	atomisp_css_input_set_mode(&isp->asd, IA_CSS_INPUT_MODE_BUFFERED_SENSOR);

	/* Recreate streams destroyed by atomisp_css_stop() */
	atomisp_create_pipes_stream(&isp->asd);

	/* Invalidate caches. FIXME: should flush only necessary buffers */
	wbinvd();

	if (atomisp_css_start(&isp->asd)) {
		dev_warn(isp->dev, "start SP failed, so do not set streaming to be enable!\n");
	} else {
		spin_lock_irqsave(&isp->lock, flags);
		isp->asd.streaming = true;
		spin_unlock_irqrestore(&isp->lock, flags);
	}

	atomisp_csi2_configure(&isp->asd);

	atomisp_css_irq_enable(isp, IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF,
			       atomisp_css_valid_sof(isp));

	if (atomisp_freq_scaling(isp, ATOMISP_DFS_MODE_AUTO, true) < 0)
		dev_dbg(isp->dev, "DFS auto failed while recovering!\n");

	/* Dequeueing buffers is not needed, CSS will recycle buffers that it has */
	atomisp_flush_video_pipe(&isp->asd.video_out, VB2_BUF_STATE_ERROR, false);

	/* Requeue unprocessed per-frame parameters. */
	atomisp_recover_params_queue(&isp->asd.video_out);

	ret = v4l2_subdev_call(isp->inputs[isp->asd.input_curr].csi_remote_source,
			       video, s_stream, 1);
	if (ret)
		dev_err(isp->dev, "Starting sensor stream failed: %d\n", ret);

out_unlock:
	mutex_unlock(&isp->mutex);
}

irqreturn_t atomisp_isr_thread(int irq, void *isp_ptr)
{
	struct atomisp_device *isp = isp_ptr;
	unsigned long flags;
	bool streaming;

	spin_lock_irqsave(&isp->lock, flags);
	streaming = isp->asd.streaming;
	spin_unlock_irqrestore(&isp->lock, flags);

	if (!streaming)
		return IRQ_HANDLED;

	/*
	 * The standard CSS2.0 API tells the following calling sequence of
	 * dequeue ready buffers:
	 * while (ia_css_dequeue_psys_event(...)) {
	 *	switch (event.type) {
	 *	...
	 *	ia_css_pipe_dequeue_buffer()
	 *	}
	 * }
	 * That is, dequeue event and buffer are one after another.
	 *
	 * But the following implementation is to first deuque all the event
	 * to a FIFO, then process the event in the FIFO.
	 * This will not have issue in single stream mode, but it do have some
	 * issue in multiple stream case. The issue is that
	 * ia_css_pipe_dequeue_buffer() will not return the corrent buffer in
	 * a specific pipe.
	 *
	 * This is due to ia_css_pipe_dequeue_buffer() does not take the
	 * ia_css_pipe parameter.
	 *
	 * So:
	 * For CSS2.0: we change the way to not dequeue all the event at one
	 * time, instead, dequue one and process one, then another
	 */
	mutex_lock(&isp->mutex);
	atomisp_css_isr_thread(isp);
	mutex_unlock(&isp->mutex);

	return IRQ_HANDLED;
}

/*
 * raw format match between SH format and V4L2 format
 */
static int raw_output_format_match_input(u32 input, u32 output)
{
	if ((input == ATOMISP_INPUT_FORMAT_RAW_12) &&
	    ((output == V4L2_PIX_FMT_SRGGB12) ||
	     (output == V4L2_PIX_FMT_SGRBG12) ||
	     (output == V4L2_PIX_FMT_SBGGR12) ||
	     (output == V4L2_PIX_FMT_SGBRG12)))
		return 0;

	if ((input == ATOMISP_INPUT_FORMAT_RAW_10) &&
	    ((output == V4L2_PIX_FMT_SRGGB10) ||
	     (output == V4L2_PIX_FMT_SGRBG10) ||
	     (output == V4L2_PIX_FMT_SBGGR10) ||
	     (output == V4L2_PIX_FMT_SGBRG10)))
		return 0;

	if ((input == ATOMISP_INPUT_FORMAT_RAW_8) &&
	    ((output == V4L2_PIX_FMT_SRGGB8) ||
	     (output == V4L2_PIX_FMT_SGRBG8) ||
	     (output == V4L2_PIX_FMT_SBGGR8) ||
	     (output == V4L2_PIX_FMT_SGBRG8)))
		return 0;

	if ((input == ATOMISP_INPUT_FORMAT_RAW_16) && (output == V4L2_PIX_FMT_SBGGR16))
		return 0;

	return -EINVAL;
}

u32 atomisp_get_pixel_depth(u32 pixelformat)
{
	switch (pixelformat) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_YVU420:
		return 12;
	case V4L2_PIX_FMT_YUV422P:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_SBGGR16:
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
		return 16;
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_YUV444:
		return 24;
	case V4L2_PIX_FMT_RGBX32:
		return 32;
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_CUSTOM_M10MO_RAW:
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		return 8;
	default:
		return 8 * 2;	/* raw type now */
	}
}

bool atomisp_is_mbuscode_raw(uint32_t code)
{
	return code >= 0x3000 && code < 0x4000;
}

/*
 * ISP features control function
 */

/*
 * Set ISP capture mode based on current settings
 */
static void atomisp_update_capture_mode(struct atomisp_sub_device *asd)
{
	if (asd->params.gdc_cac_en)
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_ADVANCED);
	else if (asd->params.low_light)
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_LOW_LIGHT);
	else if (asd->video_out.sh_fmt == IA_CSS_FRAME_FORMAT_RAW)
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_RAW);
	else
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_PRIMARY);
}

void atomisp_free_internal_buffers(struct atomisp_sub_device *asd)
{
	atomisp_free_css_parameters(&asd->params.css_param);
}

static void atomisp_update_grid_info(struct atomisp_sub_device *asd,
				     enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	int err;

	if (atomisp_css_get_grid_info(asd, pipe_id))
		return;

	/* We must free all buffers because they no longer match
	   the grid size. */
	atomisp_css_free_stat_buffers(asd);

	err = atomisp_alloc_css_stat_bufs(asd, ATOMISP_INPUT_STREAM_GENERAL);
	if (err) {
		dev_err(isp->dev, "stat_buf allocate error\n");
		goto err;
	}

	if (atomisp_alloc_3a_output_buf(asd)) {
		/* Failure for 3A buffers does not influence DIS buffers */
		if (asd->params.s3a_output_bytes != 0) {
			/* For SOC sensor happens s3a_output_bytes == 0,
			 * using if condition to exclude false error log */
			dev_err(isp->dev, "Failed to allocate memory for 3A statistics\n");
		}
		goto err;
	}

	if (atomisp_alloc_dis_coef_buf(asd)) {
		dev_err(isp->dev,
			"Failed to allocate memory for DIS statistics\n");
		goto err;
	}

	if (atomisp_alloc_metadata_output_buf(asd)) {
		dev_err(isp->dev, "Failed to allocate memory for metadata\n");
		goto err;
	}

	return;

err:
	atomisp_css_free_stat_buffers(asd);
	return;
}

static void atomisp_curr_user_grid_info(struct atomisp_sub_device *asd,
					struct atomisp_grid_info *info)
{
	memcpy(info, &asd->params.curr_grid_info.s3a_grid,
	       sizeof(struct ia_css_3a_grid_info));
}

int atomisp_compare_grid(struct atomisp_sub_device *asd,
			 struct atomisp_grid_info *atomgrid)
{
	struct atomisp_grid_info tmp = {0};

	atomisp_curr_user_grid_info(asd, &tmp);
	return memcmp(atomgrid, &tmp, sizeof(tmp));
}

int atomisp_set_dis_vector(struct atomisp_sub_device *asd,
			   struct atomisp_dis_vector *vector)
{
	atomisp_css_video_set_dis_vector(asd, vector);

	asd->params.dis_proj_data_valid = false;
	asd->params.css_update_params_needed = true;
	return 0;
}

int atomisp_set_dis_coefs(struct atomisp_sub_device *asd,
			  struct atomisp_dis_coefficients *coefs)
{
	return atomisp_css_set_dis_coefs(asd, coefs);
}

/*
 * Function to calculate real zoom region for every pipe
 */
int atomisp_calculate_real_zoom_region(struct atomisp_sub_device *asd,
				       struct ia_css_dz_config   *dz_config,
				       enum ia_css_pipe_id css_pipe_id)

{
	struct atomisp_stream_env *stream_env =
		    &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	struct atomisp_resolution  eff_res, out_res;
	int w_offset, h_offset;

	memset(&eff_res, 0, sizeof(eff_res));
	memset(&out_res, 0, sizeof(out_res));

	if (dz_config->dx || dz_config->dy)
		return 0;

	if (css_pipe_id != IA_CSS_PIPE_ID_PREVIEW
	    && css_pipe_id != IA_CSS_PIPE_ID_CAPTURE) {
		dev_err(asd->isp->dev, "%s the set pipe no support crop region"
			, __func__);
		return -EINVAL;
	}

	eff_res.width =
	    stream_env->stream_config.input_config.effective_res.width;
	eff_res.height =
	    stream_env->stream_config.input_config.effective_res.height;
	if (eff_res.width == 0 || eff_res.height == 0) {
		dev_err(asd->isp->dev, "%s err effective resolution"
			, __func__);
		return -EINVAL;
	}

	if (dz_config->zoom_region.resolution.width
	    == asd->sensor_array_res.width
	    || dz_config->zoom_region.resolution.height
	    == asd->sensor_array_res.height) {
		/*no need crop region*/
		dz_config->zoom_region.origin.x = 0;
		dz_config->zoom_region.origin.y = 0;
		dz_config->zoom_region.resolution.width = eff_res.width;
		dz_config->zoom_region.resolution.height = eff_res.height;
		return 0;
	}

	/* FIXME:
	 * This is not the correct implementation with Google's definition, due
	 * to firmware limitation.
	 * map real crop region base on above calculating base max crop region.
	 */

	if (!IS_ISP2401) {
		dz_config->zoom_region.origin.x = dz_config->zoom_region.origin.x
						  * eff_res.width
						  / asd->sensor_array_res.width;
		dz_config->zoom_region.origin.y = dz_config->zoom_region.origin.y
						  * eff_res.height
						  / asd->sensor_array_res.height;
		dz_config->zoom_region.resolution.width = dz_config->zoom_region.resolution.width
							  * eff_res.width
							  / asd->sensor_array_res.width;
		dz_config->zoom_region.resolution.height = dz_config->zoom_region.resolution.height
							  * eff_res.height
							  / asd->sensor_array_res.height;
		/*
		 * Set same ratio of crop region resolution and current pipe output
		 * resolution
		 */
		out_res.width = stream_env->pipe_configs[css_pipe_id].output_info[0].res.width;
		out_res.height = stream_env->pipe_configs[css_pipe_id].output_info[0].res.height;
		if (out_res.width == 0 || out_res.height == 0) {
			dev_err(asd->isp->dev, "%s err current pipe output resolution"
				, __func__);
			return -EINVAL;
		}
	} else {
		out_res.width = stream_env->pipe_configs[css_pipe_id].output_info[0].res.width;
		out_res.height = stream_env->pipe_configs[css_pipe_id].output_info[0].res.height;
		if (out_res.width == 0 || out_res.height == 0) {
			dev_err(asd->isp->dev, "%s err current pipe output resolution"
				, __func__);
			return -EINVAL;
		}

		if (asd->sensor_array_res.width * out_res.height
		    < out_res.width * asd->sensor_array_res.height) {
			h_offset = asd->sensor_array_res.height
				   - asd->sensor_array_res.width
				   * out_res.height / out_res.width;
			h_offset = h_offset / 2;
			if (dz_config->zoom_region.origin.y < h_offset)
				dz_config->zoom_region.origin.y = 0;
			else
				dz_config->zoom_region.origin.y = dz_config->zoom_region.origin.y - h_offset;
			w_offset = 0;
		} else {
			w_offset = asd->sensor_array_res.width
				   - asd->sensor_array_res.height
				   * out_res.width / out_res.height;
			w_offset = w_offset / 2;
			if (dz_config->zoom_region.origin.x < w_offset)
				dz_config->zoom_region.origin.x = 0;
			else
				dz_config->zoom_region.origin.x = dz_config->zoom_region.origin.x - w_offset;
			h_offset = 0;
		}
		dz_config->zoom_region.origin.x = dz_config->zoom_region.origin.x
						  * eff_res.width
						  / (asd->sensor_array_res.width - 2 * w_offset);
		dz_config->zoom_region.origin.y = dz_config->zoom_region.origin.y
						  * eff_res.height
						  / (asd->sensor_array_res.height - 2 * h_offset);
		dz_config->zoom_region.resolution.width = dz_config->zoom_region.resolution.width
						  * eff_res.width
						  / (asd->sensor_array_res.width - 2 * w_offset);
		dz_config->zoom_region.resolution.height = dz_config->zoom_region.resolution.height
						  * eff_res.height
						  / (asd->sensor_array_res.height - 2 * h_offset);
	}

	if (out_res.width * dz_config->zoom_region.resolution.height
	    > dz_config->zoom_region.resolution.width * out_res.height) {
		dz_config->zoom_region.resolution.height =
		    dz_config->zoom_region.resolution.width
		    * out_res.height / out_res.width;
	} else {
		dz_config->zoom_region.resolution.width =
		    dz_config->zoom_region.resolution.height
		    * out_res.width / out_res.height;
	}
	dev_dbg(asd->isp->dev,
		"%s crop region:(%d,%d),(%d,%d) eff_res(%d, %d) array_size(%d,%d) out_res(%d, %d)\n",
		__func__, dz_config->zoom_region.origin.x,
		dz_config->zoom_region.origin.y,
		dz_config->zoom_region.resolution.width,
		dz_config->zoom_region.resolution.height,
		eff_res.width, eff_res.height,
		asd->sensor_array_res.width,
		asd->sensor_array_res.height,
		out_res.width, out_res.height);

	if ((dz_config->zoom_region.origin.x +
	     dz_config->zoom_region.resolution.width
	     > eff_res.width) ||
	    (dz_config->zoom_region.origin.y +
	     dz_config->zoom_region.resolution.height
	     > eff_res.height))
		return -EINVAL;

	return 0;
}

/*
 * Function to check the zoom region whether is effective
 */
static bool atomisp_check_zoom_region(
    struct atomisp_sub_device *asd,
    struct ia_css_dz_config *dz_config)
{
	struct atomisp_resolution  config;
	bool flag = false;
	unsigned int w, h;

	memset(&config, 0, sizeof(struct atomisp_resolution));

	if (dz_config->dx && dz_config->dy)
		return true;

	config.width = asd->sensor_array_res.width;
	config.height = asd->sensor_array_res.height;
	w = dz_config->zoom_region.origin.x +
	    dz_config->zoom_region.resolution.width;
	h = dz_config->zoom_region.origin.y +
	    dz_config->zoom_region.resolution.height;

	if ((w <= config.width) && (h <= config.height) && w > 0 && h > 0)
		flag = true;
	else
		/* setting error zoom region */
		dev_err(asd->isp->dev,
			"%s zoom region ERROR:dz_config:(%d,%d),(%d,%d)array_res(%d, %d)\n",
			__func__, dz_config->zoom_region.origin.x,
			dz_config->zoom_region.origin.y,
			dz_config->zoom_region.resolution.width,
			dz_config->zoom_region.resolution.height,
			config.width, config.height);

	return flag;
}

void atomisp_apply_css_parameters(
    struct atomisp_sub_device *asd,
    struct atomisp_css_params *css_param)
{
	if (css_param->update_flag.wb_config)
		asd->params.config.wb_config = &css_param->wb_config;

	if (css_param->update_flag.ob_config)
		asd->params.config.ob_config = &css_param->ob_config;

	if (css_param->update_flag.dp_config)
		asd->params.config.dp_config = &css_param->dp_config;

	if (css_param->update_flag.nr_config)
		asd->params.config.nr_config = &css_param->nr_config;

	if (css_param->update_flag.ee_config)
		asd->params.config.ee_config = &css_param->ee_config;

	if (css_param->update_flag.tnr_config)
		asd->params.config.tnr_config = &css_param->tnr_config;

	if (css_param->update_flag.a3a_config)
		asd->params.config.s3a_config = &css_param->s3a_config;

	if (css_param->update_flag.ctc_config)
		asd->params.config.ctc_config = &css_param->ctc_config;

	if (css_param->update_flag.cnr_config)
		asd->params.config.cnr_config = &css_param->cnr_config;

	if (css_param->update_flag.ecd_config)
		asd->params.config.ecd_config = &css_param->ecd_config;

	if (css_param->update_flag.ynr_config)
		asd->params.config.ynr_config = &css_param->ynr_config;

	if (css_param->update_flag.fc_config)
		asd->params.config.fc_config = &css_param->fc_config;

	if (css_param->update_flag.macc_config)
		asd->params.config.macc_config = &css_param->macc_config;

	if (css_param->update_flag.aa_config)
		asd->params.config.aa_config = &css_param->aa_config;

	if (css_param->update_flag.anr_config)
		asd->params.config.anr_config = &css_param->anr_config;

	if (css_param->update_flag.xnr_config)
		asd->params.config.xnr_config = &css_param->xnr_config;

	if (css_param->update_flag.yuv2rgb_cc_config)
		asd->params.config.yuv2rgb_cc_config = &css_param->yuv2rgb_cc_config;

	if (css_param->update_flag.rgb2yuv_cc_config)
		asd->params.config.rgb2yuv_cc_config = &css_param->rgb2yuv_cc_config;

	if (css_param->update_flag.macc_table)
		asd->params.config.macc_table = &css_param->macc_table;

	if (css_param->update_flag.xnr_table)
		asd->params.config.xnr_table = &css_param->xnr_table;

	if (css_param->update_flag.r_gamma_table)
		asd->params.config.r_gamma_table = &css_param->r_gamma_table;

	if (css_param->update_flag.g_gamma_table)
		asd->params.config.g_gamma_table = &css_param->g_gamma_table;

	if (css_param->update_flag.b_gamma_table)
		asd->params.config.b_gamma_table = &css_param->b_gamma_table;

	if (css_param->update_flag.anr_thres)
		atomisp_css_set_anr_thres(asd, &css_param->anr_thres);

	if (css_param->update_flag.shading_table)
		asd->params.config.shading_table = css_param->shading_table;

	if (css_param->update_flag.morph_table && asd->params.gdc_cac_en)
		asd->params.config.morph_table = css_param->morph_table;

	if (css_param->update_flag.dvs2_coefs) {
		struct ia_css_dvs_grid_info *dvs_grid_info =
		    atomisp_css_get_dvs_grid_info(
			&asd->params.curr_grid_info);

		if (dvs_grid_info && dvs_grid_info->enable)
			atomisp_css_set_dvs2_coefs(asd, css_param->dvs2_coeff);
	}

	if (css_param->update_flag.dvs_6axis_config)
		atomisp_css_set_dvs_6axis(asd, css_param->dvs_6axis);

	atomisp_css_set_isp_config_id(asd, css_param->isp_config_id);
	/*
	 * These configurations are on used by ISP1.x, not for ISP2.x,
	 * so do not handle them. see comments of ia_css_isp_config.
	 * 1 cc_config
	 * 2 ce_config
	 * 3 de_config
	 * 4 gc_config
	 * 5 gamma_table
	 * 6 ctc_table
	 * 7 dvs_coefs
	 */
}

static unsigned int long copy_from_compatible(void *to, const void *from,
	unsigned long n, bool from_user)
{
	if (from_user)
		return copy_from_user(to, (void __user *)from, n);
	else
		memcpy(to, from, n);
	return 0;
}

int atomisp_cp_general_isp_parameters(struct atomisp_sub_device *asd,
				      struct atomisp_parameters *arg,
				      struct atomisp_css_params *css_param,
				      bool from_user)
{
	struct atomisp_parameters *cur_config = &css_param->update_flag;

	if (!arg || !asd || !css_param)
		return -EINVAL;

	if (arg->wb_config && (from_user || !cur_config->wb_config)) {
		if (copy_from_compatible(&css_param->wb_config, arg->wb_config,
					 sizeof(struct ia_css_wb_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.wb_config =
		    (struct atomisp_wb_config *)&css_param->wb_config;
	}

	if (arg->ob_config && (from_user || !cur_config->ob_config)) {
		if (copy_from_compatible(&css_param->ob_config, arg->ob_config,
					 sizeof(struct ia_css_ob_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ob_config =
		    (struct atomisp_ob_config *)&css_param->ob_config;
	}

	if (arg->dp_config && (from_user || !cur_config->dp_config)) {
		if (copy_from_compatible(&css_param->dp_config, arg->dp_config,
					 sizeof(struct ia_css_dp_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.dp_config =
		    (struct atomisp_dp_config *)&css_param->dp_config;
	}

	if (asd->run_mode->val != ATOMISP_RUN_MODE_VIDEO) {
		if (arg->dz_config && (from_user || !cur_config->dz_config)) {
			if (copy_from_compatible(&css_param->dz_config,
						 arg->dz_config,
						 sizeof(struct ia_css_dz_config),
						 from_user))
				return -EFAULT;
			if (!atomisp_check_zoom_region(asd,
						       &css_param->dz_config)) {
				dev_err(asd->isp->dev, "crop region error!");
				return -EINVAL;
			}
			css_param->update_flag.dz_config =
			    (struct atomisp_dz_config *)
			    &css_param->dz_config;
		}
	}

	if (arg->nr_config && (from_user || !cur_config->nr_config)) {
		if (copy_from_compatible(&css_param->nr_config, arg->nr_config,
					 sizeof(struct ia_css_nr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.nr_config =
		    (struct atomisp_nr_config *)&css_param->nr_config;
	}

	if (arg->ee_config && (from_user || !cur_config->ee_config)) {
		if (copy_from_compatible(&css_param->ee_config, arg->ee_config,
					 sizeof(struct ia_css_ee_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ee_config =
		    (struct atomisp_ee_config *)&css_param->ee_config;
	}

	if (arg->tnr_config && (from_user || !cur_config->tnr_config)) {
		if (copy_from_compatible(&css_param->tnr_config,
					 arg->tnr_config,
					 sizeof(struct ia_css_tnr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.tnr_config =
		    (struct atomisp_tnr_config *)
		    &css_param->tnr_config;
	}

	if (arg->a3a_config && (from_user || !cur_config->a3a_config)) {
		if (copy_from_compatible(&css_param->s3a_config,
					 arg->a3a_config,
					 sizeof(struct ia_css_3a_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.a3a_config =
		    (struct atomisp_3a_config *)&css_param->s3a_config;
	}

	if (arg->ctc_config && (from_user || !cur_config->ctc_config)) {
		if (copy_from_compatible(&css_param->ctc_config,
					 arg->ctc_config,
					 sizeof(struct ia_css_ctc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ctc_config =
		    (struct atomisp_ctc_config *)
		    &css_param->ctc_config;
	}

	if (arg->cnr_config && (from_user || !cur_config->cnr_config)) {
		if (copy_from_compatible(&css_param->cnr_config,
					 arg->cnr_config,
					 sizeof(struct ia_css_cnr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.cnr_config =
		    (struct atomisp_cnr_config *)
		    &css_param->cnr_config;
	}

	if (arg->ecd_config && (from_user || !cur_config->ecd_config)) {
		if (copy_from_compatible(&css_param->ecd_config,
					 arg->ecd_config,
					 sizeof(struct ia_css_ecd_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ecd_config =
		    (struct atomisp_ecd_config *)
		    &css_param->ecd_config;
	}

	if (arg->ynr_config && (from_user || !cur_config->ynr_config)) {
		if (copy_from_compatible(&css_param->ynr_config,
					 arg->ynr_config,
					 sizeof(struct ia_css_ynr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ynr_config =
		    (struct atomisp_ynr_config *)
		    &css_param->ynr_config;
	}

	if (arg->fc_config && (from_user || !cur_config->fc_config)) {
		if (copy_from_compatible(&css_param->fc_config,
					 arg->fc_config,
					 sizeof(struct ia_css_fc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.fc_config =
		    (struct atomisp_fc_config *)&css_param->fc_config;
	}

	if (arg->macc_config && (from_user || !cur_config->macc_config)) {
		if (copy_from_compatible(&css_param->macc_config,
					 arg->macc_config,
					 sizeof(struct ia_css_macc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.macc_config =
		    (struct atomisp_macc_config *)
		    &css_param->macc_config;
	}

	if (arg->aa_config && (from_user || !cur_config->aa_config)) {
		if (copy_from_compatible(&css_param->aa_config, arg->aa_config,
					 sizeof(struct ia_css_aa_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.aa_config =
		    (struct atomisp_aa_config *)&css_param->aa_config;
	}

	if (arg->anr_config && (from_user || !cur_config->anr_config)) {
		if (copy_from_compatible(&css_param->anr_config,
					 arg->anr_config,
					 sizeof(struct ia_css_anr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.anr_config =
		    (struct atomisp_anr_config *)
		    &css_param->anr_config;
	}

	if (arg->xnr_config && (from_user || !cur_config->xnr_config)) {
		if (copy_from_compatible(&css_param->xnr_config,
					 arg->xnr_config,
					 sizeof(struct ia_css_xnr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.xnr_config =
		    (struct atomisp_xnr_config *)
		    &css_param->xnr_config;
	}

	if (arg->yuv2rgb_cc_config &&
	    (from_user || !cur_config->yuv2rgb_cc_config)) {
		if (copy_from_compatible(&css_param->yuv2rgb_cc_config,
					 arg->yuv2rgb_cc_config,
					 sizeof(struct ia_css_cc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.yuv2rgb_cc_config =
		    (struct atomisp_cc_config *)
		    &css_param->yuv2rgb_cc_config;
	}

	if (arg->rgb2yuv_cc_config &&
	    (from_user || !cur_config->rgb2yuv_cc_config)) {
		if (copy_from_compatible(&css_param->rgb2yuv_cc_config,
					 arg->rgb2yuv_cc_config,
					 sizeof(struct ia_css_cc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.rgb2yuv_cc_config =
		    (struct atomisp_cc_config *)
		    &css_param->rgb2yuv_cc_config;
	}

	if (arg->macc_table && (from_user || !cur_config->macc_table)) {
		if (copy_from_compatible(&css_param->macc_table,
					 arg->macc_table,
					 sizeof(struct ia_css_macc_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.macc_table =
		    (struct atomisp_macc_table *)
		    &css_param->macc_table;
	}

	if (arg->xnr_table && (from_user || !cur_config->xnr_table)) {
		if (copy_from_compatible(&css_param->xnr_table,
					 arg->xnr_table,
					 sizeof(struct ia_css_xnr_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.xnr_table =
		    (struct atomisp_xnr_table *)&css_param->xnr_table;
	}

	if (arg->r_gamma_table && (from_user || !cur_config->r_gamma_table)) {
		if (copy_from_compatible(&css_param->r_gamma_table,
					 arg->r_gamma_table,
					 sizeof(struct ia_css_rgb_gamma_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.r_gamma_table =
		    (struct atomisp_rgb_gamma_table *)
		    &css_param->r_gamma_table;
	}

	if (arg->g_gamma_table && (from_user || !cur_config->g_gamma_table)) {
		if (copy_from_compatible(&css_param->g_gamma_table,
					 arg->g_gamma_table,
					 sizeof(struct ia_css_rgb_gamma_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.g_gamma_table =
		    (struct atomisp_rgb_gamma_table *)
		    &css_param->g_gamma_table;
	}

	if (arg->b_gamma_table && (from_user || !cur_config->b_gamma_table)) {
		if (copy_from_compatible(&css_param->b_gamma_table,
					 arg->b_gamma_table,
					 sizeof(struct ia_css_rgb_gamma_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.b_gamma_table =
		    (struct atomisp_rgb_gamma_table *)
		    &css_param->b_gamma_table;
	}

	if (arg->anr_thres && (from_user || !cur_config->anr_thres)) {
		if (copy_from_compatible(&css_param->anr_thres, arg->anr_thres,
					 sizeof(struct ia_css_anr_thres),
					 from_user))
			return -EFAULT;
		css_param->update_flag.anr_thres =
		    (struct atomisp_anr_thres *)&css_param->anr_thres;
	}

	if (from_user)
		css_param->isp_config_id = arg->isp_config_id;
	/*
	 * These configurations are on used by ISP1.x, not for ISP2.x,
	 * so do not handle them. see comments of ia_css_isp_config.
	 * 1 cc_config
	 * 2 ce_config
	 * 3 de_config
	 * 4 gc_config
	 * 5 gamma_table
	 * 6 ctc_table
	 * 7 dvs_coefs
	 */
	return 0;
}

int atomisp_cp_lsc_table(struct atomisp_sub_device *asd,
			 struct atomisp_shading_table *source_st,
			 struct atomisp_css_params *css_param,
			 bool from_user)
{
	unsigned int i;
	unsigned int len_table;
	struct ia_css_shading_table *shading_table;
	struct ia_css_shading_table *old_table;
	struct atomisp_shading_table *st, dest_st;

	if (!source_st)
		return 0;

	if (!css_param)
		return -EINVAL;

	if (!from_user && css_param->update_flag.shading_table)
		return 0;

	if (IS_ISP2401) {
		if (copy_from_compatible(&dest_st, source_st,
					sizeof(struct atomisp_shading_table),
					from_user)) {
			dev_err(asd->isp->dev, "copy shading table failed!");
			return -EFAULT;
		}
		st = &dest_st;
	} else {
		st = source_st;
	}

	old_table = css_param->shading_table;

	/* user config is to disable the shading table. */
	if (!st->enable) {
		/* Generate a minimum table with enable = 0. */
		shading_table = atomisp_css_shading_table_alloc(1, 1);
		if (!shading_table)
			return -ENOMEM;
		shading_table->enable = 0;
		goto set_lsc;
	}

	/* Setting a new table. Validate first - all tables must be set */
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (!st->data[i]) {
			dev_err(asd->isp->dev, "shading table validate failed");
			return -EINVAL;
		}
	}

	/* Shading table size per color */
	if (st->width > SH_CSS_MAX_SCTBL_WIDTH_PER_COLOR ||
	    st->height > SH_CSS_MAX_SCTBL_HEIGHT_PER_COLOR) {
		dev_err(asd->isp->dev, "shading table w/h validate failed!");
		return -EINVAL;
	}

	shading_table = atomisp_css_shading_table_alloc(st->width, st->height);
	if (!shading_table)
		return -ENOMEM;

	len_table = st->width * st->height * ATOMISP_SC_TYPE_SIZE;
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (copy_from_compatible(shading_table->data[i],
					 st->data[i], len_table, from_user)) {
			atomisp_css_shading_table_free(shading_table);
			return -EFAULT;
		}
	}
	shading_table->sensor_width = st->sensor_width;
	shading_table->sensor_height = st->sensor_height;
	shading_table->fraction_bits = st->fraction_bits;
	shading_table->enable = st->enable;

	/* No need to update shading table if it is the same */
	if (old_table &&
	    old_table->sensor_width == shading_table->sensor_width &&
	    old_table->sensor_height == shading_table->sensor_height &&
	    old_table->width == shading_table->width &&
	    old_table->height == shading_table->height &&
	    old_table->fraction_bits == shading_table->fraction_bits &&
	    old_table->enable == shading_table->enable) {
		bool data_is_same = true;

		for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
			if (memcmp(shading_table->data[i], old_table->data[i],
				   len_table) != 0) {
				data_is_same = false;
				break;
			}
		}

		if (data_is_same) {
			atomisp_css_shading_table_free(shading_table);
			return 0;
		}
	}

set_lsc:
	/* set LSC to CSS */
	css_param->shading_table = shading_table;
	css_param->update_flag.shading_table = (struct atomisp_shading_table *)shading_table;
	asd->params.sc_en = shading_table;

	if (old_table)
		atomisp_css_shading_table_free(old_table);

	return 0;
}

int atomisp_css_cp_dvs2_coefs(struct atomisp_sub_device *asd,
			      struct ia_css_dvs2_coefficients *coefs,
			      struct atomisp_css_params *css_param,
			      bool from_user)
{
	struct ia_css_dvs_grid_info *cur =
	    atomisp_css_get_dvs_grid_info(&asd->params.curr_grid_info);
	int dvs_hor_coef_bytes, dvs_ver_coef_bytes;
	struct ia_css_dvs2_coefficients dvs2_coefs;

	if (!coefs || !cur)
		return 0;

	if (!from_user && css_param->update_flag.dvs2_coefs)
		return 0;

	if (!IS_ISP2401) {
		if (sizeof(*cur) != sizeof(coefs->grid) ||
		    memcmp(&coefs->grid, cur, sizeof(coefs->grid))) {
			dev_err(asd->isp->dev, "dvs grid mismatch!\n");
			/* If the grid info in the argument differs from the current
			grid info, we tell the caller to reset the grid size and
			try again. */
			return -EAGAIN;
		}

		if (!coefs->hor_coefs.odd_real ||
		    !coefs->hor_coefs.odd_imag ||
		    !coefs->hor_coefs.even_real ||
		    !coefs->hor_coefs.even_imag ||
		    !coefs->ver_coefs.odd_real ||
		    !coefs->ver_coefs.odd_imag ||
		    !coefs->ver_coefs.even_real ||
		    !coefs->ver_coefs.even_imag)
			return -EINVAL;

		if (!css_param->dvs2_coeff) {
			/* DIS coefficients. */
			css_param->dvs2_coeff = ia_css_dvs2_coefficients_allocate(cur);
			if (!css_param->dvs2_coeff)
				return -ENOMEM;
		}

		dvs_hor_coef_bytes = asd->params.dvs_hor_coef_bytes;
		dvs_ver_coef_bytes = asd->params.dvs_ver_coef_bytes;
		if (copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_real,
					coefs->hor_coefs.odd_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_imag,
					coefs->hor_coefs.odd_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_real,
					coefs->hor_coefs.even_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_imag,
					coefs->hor_coefs.even_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_real,
					coefs->ver_coefs.odd_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_imag,
					coefs->ver_coefs.odd_imag, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_real,
					coefs->ver_coefs.even_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_imag,
					coefs->ver_coefs.even_imag, dvs_ver_coef_bytes, from_user)) {
			ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
			css_param->dvs2_coeff = NULL;
			return -EFAULT;
		}
	} else {
		if (copy_from_compatible(&dvs2_coefs, coefs,
					sizeof(struct ia_css_dvs2_coefficients),
					from_user)) {
			dev_err(asd->isp->dev, "copy dvs2 coef failed");
			return -EFAULT;
		}

		if (sizeof(*cur) != sizeof(dvs2_coefs.grid) ||
		    memcmp(&dvs2_coefs.grid, cur, sizeof(dvs2_coefs.grid))) {
			dev_err(asd->isp->dev, "dvs grid mismatch!\n");
			/* If the grid info in the argument differs from the current
			grid info, we tell the caller to reset the grid size and
			try again. */
			return -EAGAIN;
		}

		if (!dvs2_coefs.hor_coefs.odd_real ||
		    !dvs2_coefs.hor_coefs.odd_imag ||
		    !dvs2_coefs.hor_coefs.even_real ||
		    !dvs2_coefs.hor_coefs.even_imag ||
		    !dvs2_coefs.ver_coefs.odd_real ||
		    !dvs2_coefs.ver_coefs.odd_imag ||
		    !dvs2_coefs.ver_coefs.even_real ||
		    !dvs2_coefs.ver_coefs.even_imag)
			return -EINVAL;

		if (!css_param->dvs2_coeff) {
			/* DIS coefficients. */
			css_param->dvs2_coeff = ia_css_dvs2_coefficients_allocate(cur);
			if (!css_param->dvs2_coeff)
				return -ENOMEM;
		}

		dvs_hor_coef_bytes = asd->params.dvs_hor_coef_bytes;
		dvs_ver_coef_bytes = asd->params.dvs_ver_coef_bytes;
		if (copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_real,
					dvs2_coefs.hor_coefs.odd_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_imag,
					dvs2_coefs.hor_coefs.odd_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_real,
					dvs2_coefs.hor_coefs.even_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_imag,
					dvs2_coefs.hor_coefs.even_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_real,
					dvs2_coefs.ver_coefs.odd_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_imag,
					dvs2_coefs.ver_coefs.odd_imag, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_real,
					dvs2_coefs.ver_coefs.even_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_imag,
					dvs2_coefs.ver_coefs.even_imag, dvs_ver_coef_bytes, from_user)) {
			ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
			css_param->dvs2_coeff = NULL;
			return -EFAULT;
		}
	}

	css_param->update_flag.dvs2_coefs =
	    (struct atomisp_dis_coefficients *)css_param->dvs2_coeff;
	return 0;
}

int atomisp_cp_dvs_6axis_config(struct atomisp_sub_device *asd,
				struct atomisp_dvs_6axis_config *source_6axis_config,
				struct atomisp_css_params *css_param,
				bool from_user)
{
	struct ia_css_dvs_6axis_config *dvs_6axis_config;
	struct ia_css_dvs_6axis_config *old_6axis_config;
	struct ia_css_stream *stream =
		    asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream;
	struct ia_css_dvs_grid_info *dvs_grid_info =
	    atomisp_css_get_dvs_grid_info(&asd->params.curr_grid_info);
	int ret = -EFAULT;

	if (!stream) {
		dev_err(asd->isp->dev, "%s: internal error!", __func__);
		return -EINVAL;
	}

	if (!source_6axis_config || !dvs_grid_info)
		return 0;

	if (!dvs_grid_info->enable)
		return 0;

	if (!from_user && css_param->update_flag.dvs_6axis_config)
		return 0;

	/* check whether need to reallocate for 6 axis config */
	old_6axis_config = css_param->dvs_6axis;
	dvs_6axis_config = old_6axis_config;

	if (IS_ISP2401) {
		struct ia_css_dvs_6axis_config t_6axis_config;

		if (copy_from_compatible(&t_6axis_config, source_6axis_config,
					sizeof(struct atomisp_dvs_6axis_config),
					from_user)) {
			dev_err(asd->isp->dev, "copy morph table failed!");
			return -EFAULT;
		}

		if (old_6axis_config &&
		    (old_6axis_config->width_y != t_6axis_config.width_y ||
		    old_6axis_config->height_y != t_6axis_config.height_y ||
		    old_6axis_config->width_uv != t_6axis_config.width_uv ||
		    old_6axis_config->height_uv != t_6axis_config.height_uv)) {
			ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
			css_param->dvs_6axis = NULL;

			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config)
				return -ENOMEM;
		} else if (!dvs_6axis_config) {
			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config)
				return -ENOMEM;
		}

		dvs_6axis_config->exp_id = t_6axis_config.exp_id;

		if (copy_from_compatible(dvs_6axis_config->xcoords_y,
					t_6axis_config.xcoords_y,
					t_6axis_config.width_y *
					t_6axis_config.height_y *
					sizeof(*dvs_6axis_config->xcoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_y,
					t_6axis_config.ycoords_y,
					t_6axis_config.width_y *
					t_6axis_config.height_y *
					sizeof(*dvs_6axis_config->ycoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->xcoords_uv,
					t_6axis_config.xcoords_uv,
					t_6axis_config.width_uv *
					t_6axis_config.height_uv *
					sizeof(*dvs_6axis_config->xcoords_uv),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_uv,
					t_6axis_config.ycoords_uv,
					t_6axis_config.width_uv *
					t_6axis_config.height_uv *
					sizeof(*dvs_6axis_config->ycoords_uv),
					from_user))
			goto error;
	} else {
		if (old_6axis_config &&
		    (old_6axis_config->width_y != source_6axis_config->width_y ||
		    old_6axis_config->height_y != source_6axis_config->height_y ||
		    old_6axis_config->width_uv != source_6axis_config->width_uv ||
		    old_6axis_config->height_uv != source_6axis_config->height_uv)) {
			ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
			css_param->dvs_6axis = NULL;

			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config) {
				ret = -ENOMEM;
				goto error;
			}
		} else if (!dvs_6axis_config) {
			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config) {
				ret = -ENOMEM;
				goto error;
			}
		}

		dvs_6axis_config->exp_id = source_6axis_config->exp_id;

		if (copy_from_compatible(dvs_6axis_config->xcoords_y,
					source_6axis_config->xcoords_y,
					source_6axis_config->width_y *
					source_6axis_config->height_y *
					sizeof(*source_6axis_config->xcoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_y,
					source_6axis_config->ycoords_y,
					source_6axis_config->width_y *
					source_6axis_config->height_y *
					sizeof(*source_6axis_config->ycoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->xcoords_uv,
					source_6axis_config->xcoords_uv,
					source_6axis_config->width_uv *
					source_6axis_config->height_uv *
					sizeof(*source_6axis_config->xcoords_uv),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_uv,
					source_6axis_config->ycoords_uv,
					source_6axis_config->width_uv *
					source_6axis_config->height_uv *
					sizeof(*source_6axis_config->ycoords_uv),
					from_user))
			goto error;
	}
	css_param->dvs_6axis = dvs_6axis_config;
	css_param->update_flag.dvs_6axis_config =
	    (struct atomisp_dvs_6axis_config *)dvs_6axis_config;
	return 0;

error:
	if (dvs_6axis_config)
		ia_css_dvs2_6axis_config_free(dvs_6axis_config);
	return ret;
}

int atomisp_cp_morph_table(struct atomisp_sub_device *asd,
			   struct atomisp_morph_table *source_morph_table,
			   struct atomisp_css_params *css_param,
			   bool from_user)
{
	int ret = -EFAULT;
	unsigned int i;
	struct ia_css_morph_table *morph_table;
	struct ia_css_morph_table *old_morph_table;

	if (!source_morph_table)
		return 0;

	if (!from_user && css_param->update_flag.morph_table)
		return 0;

	old_morph_table = css_param->morph_table;

	if (IS_ISP2401) {
		struct ia_css_morph_table mtbl;

		if (copy_from_compatible(&mtbl, source_morph_table,
				sizeof(struct atomisp_morph_table),
				from_user)) {
			dev_err(asd->isp->dev, "copy morph table failed!");
			return -EFAULT;
		}

		morph_table = atomisp_css_morph_table_allocate(
				mtbl.width,
				mtbl.height);
		if (!morph_table)
			return -ENOMEM;

		for (i = 0; i < IA_CSS_MORPH_TABLE_NUM_PLANES; i++) {
			if (copy_from_compatible(morph_table->coordinates_x[i],
						(__force void *)source_morph_table->coordinates_x[i],
						mtbl.height * mtbl.width *
						sizeof(*morph_table->coordinates_x[i]),
						from_user))
				goto error;

			if (copy_from_compatible(morph_table->coordinates_y[i],
						(__force void *)source_morph_table->coordinates_y[i],
						mtbl.height * mtbl.width *
						sizeof(*morph_table->coordinates_y[i]),
						from_user))
				goto error;
		}
	} else {
		morph_table = atomisp_css_morph_table_allocate(
				source_morph_table->width,
				source_morph_table->height);
		if (!morph_table) {
			ret = -ENOMEM;
			goto error;
		}

		for (i = 0; i < IA_CSS_MORPH_TABLE_NUM_PLANES; i++) {
			if (copy_from_compatible(morph_table->coordinates_x[i],
						(__force void *)source_morph_table->coordinates_x[i],
						source_morph_table->height * source_morph_table->width *
						sizeof(*source_morph_table->coordinates_x[i]),
						from_user))
				goto error;

			if (copy_from_compatible(morph_table->coordinates_y[i],
						(__force void *)source_morph_table->coordinates_y[i],
						source_morph_table->height * source_morph_table->width *
						sizeof(*source_morph_table->coordinates_y[i]),
						from_user))
				goto error;
		}
	}

	css_param->morph_table = morph_table;
	if (old_morph_table)
		atomisp_css_morph_table_free(old_morph_table);
	css_param->update_flag.morph_table =
	    (struct atomisp_morph_table *)morph_table;
	return 0;

error:
	if (morph_table)
		atomisp_css_morph_table_free(morph_table);
	return ret;
}

int atomisp_makeup_css_parameters(struct atomisp_sub_device *asd,
				  struct atomisp_parameters *arg,
				  struct atomisp_css_params *css_param)
{
	int ret;

	ret = atomisp_cp_general_isp_parameters(asd, arg, css_param, false);
	if (ret)
		return ret;
	ret = atomisp_cp_lsc_table(asd, arg->shading_table, css_param, false);
	if (ret)
		return ret;
	ret = atomisp_cp_morph_table(asd, arg->morph_table, css_param, false);
	if (ret)
		return ret;
	ret = atomisp_css_cp_dvs2_coefs(asd,
					(struct ia_css_dvs2_coefficients *)arg->dvs2_coefs,
					css_param, false);
	if (ret)
		return ret;
	ret = atomisp_cp_dvs_6axis_config(asd, arg->dvs_6axis_config,
					  css_param, false);
	return ret;
}

void atomisp_free_css_parameters(struct atomisp_css_params *css_param)
{
	if (css_param->dvs_6axis) {
		ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
		css_param->dvs_6axis = NULL;
	}
	if (css_param->dvs2_coeff) {
		ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
		css_param->dvs2_coeff = NULL;
	}
	if (css_param->shading_table) {
		ia_css_shading_table_free(css_param->shading_table);
		css_param->shading_table = NULL;
	}
	if (css_param->morph_table) {
		ia_css_morph_table_free(css_param->morph_table);
		css_param->morph_table = NULL;
	}
}

static void atomisp_move_frame_to_activeq(struct ia_css_frame *frame,
					  struct atomisp_css_params_with_list *param)
{
	struct atomisp_video_pipe *pipe = vb_to_pipe(&frame->vb.vb2_buf);
	unsigned long irqflags;

	pipe->frame_params[frame->vb.vb2_buf.index] = param;
	spin_lock_irqsave(&pipe->irq_lock, irqflags);
	list_move_tail(&frame->queue, &pipe->activeq);
	spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
}

/*
 * Check parameter queue list and buffer queue list to find out if matched items
 * and then set parameter to CSS and enqueue buffer to CSS.
 * Of course, if the buffer in buffer waiting list is not bound to a per-frame
 * parameter, it will be enqueued into CSS as long as the per-frame setting
 * buffers before it get enqueued.
 */
void atomisp_handle_parameter_and_buffer(struct atomisp_video_pipe *pipe)
{
	struct atomisp_sub_device *asd = pipe->asd;
	struct ia_css_frame *frame = NULL, *frame_tmp;
	struct atomisp_css_params_with_list *param = NULL, *param_tmp;
	bool need_to_enqueue_buffer = false;
	int i;

	lockdep_assert_held(&asd->isp->mutex);

	/*
	 * CSS/FW requires set parameter and enqueue buffer happen after ISP
	 * is streamon.
	 */
	if (!asd->streaming)
		return;

	if (list_empty(&pipe->per_frame_params) ||
	    list_empty(&pipe->buffers_waiting_for_param))
		return;

	list_for_each_entry_safe(frame, frame_tmp,
				 &pipe->buffers_waiting_for_param, queue) {
		i = frame->vb.vb2_buf.index;
		if (pipe->frame_request_config_id[i]) {
			list_for_each_entry_safe(param, param_tmp,
						 &pipe->per_frame_params, list) {
				if (pipe->frame_request_config_id[i] != param->params.isp_config_id)
					continue;

				list_del(&param->list);

				/*
				 * clear the request config id as the buffer
				 * will be handled and enqueued into CSS soon
				 */
				pipe->frame_request_config_id[i] = 0;
				atomisp_move_frame_to_activeq(frame, param);
				need_to_enqueue_buffer = true;
				break;
			}

			/* If this is the end, stop further loop */
			if (list_entry_is_head(param, &pipe->per_frame_params, list))
				break;
		} else {
			atomisp_move_frame_to_activeq(frame, NULL);
			need_to_enqueue_buffer = true;
		}
	}

	if (!need_to_enqueue_buffer)
		return;

	atomisp_qbuffers_to_css(asd);
}

static void atomisp_fill_pix_format(struct v4l2_pix_format *f,
				    u32 width, u32 height,
				    const struct atomisp_format_bridge *br_fmt)
{
	u32 bytes;

	f->width = width;
	f->height = height;
	f->pixelformat = br_fmt->pixelformat;

	/* Adding padding to width for bytesperline calculation */
	width = ia_css_frame_pad_width(width, br_fmt->sh_fmt);
	bytes = BITS_TO_BYTES(br_fmt->depth * width);

	if (br_fmt->planar)
		f->bytesperline = width;
	else
		f->bytesperline = bytes;

	f->sizeimage = PAGE_ALIGN(height * bytes);

	if (f->field == V4L2_FIELD_ANY)
		f->field = V4L2_FIELD_NONE;

	/*
	 * FIXME: do we need to set this up differently, depending on the
	 * sensor or the pipeline?
	 */
	f->colorspace = V4L2_COLORSPACE_REC709;
	f->ycbcr_enc = V4L2_YCBCR_ENC_709;
	f->xfer_func = V4L2_XFER_FUNC_709;
}

/* Get sensor padding values for the non padded width x height resolution */
void atomisp_get_padding(struct atomisp_device *isp, u32 width, u32 height,
			 u32 *padding_w, u32 *padding_h)
{
	struct atomisp_input_subdev *input = &isp->inputs[isp->asd.input_curr];
	struct v4l2_rect native_rect = input->native_rect;
	const struct atomisp_in_fmt_conv *fc = NULL;
	unsigned int bayer_order;
	u32 min_pad_w = ISP2400_MIN_PAD_W;
	u32 min_pad_h = ISP2400_MIN_PAD_H;
	struct v4l2_mbus_framefmt *sink;

	if (!input->crop_support) {
		*padding_w = pad_w;
		*padding_h = pad_h;
		return;
	}

	width = min(width, input->native_rect.width);
	height = min(height, input->native_rect.height);

	if (input->binning_support && width <= (input->active_rect.width / 2) &&
				      height <= (input->active_rect.height / 2)) {
		native_rect.width /= 2;
		native_rect.height /= 2;
	}

	*padding_w = min_t(u32, (native_rect.width - width) & ~1, pad_w);
	*padding_h = min_t(u32, (native_rect.height - height) & ~1, pad_h);

	/* The below minimum padding requirements are for BYT / ISP2400 only */
	if (IS_ISP2401)
		return;

	/* Embedded ISP capture already carries its own border, so do not
	 * force the larger raw-sensor padding floor on the YUV path.
	 * For ISP2400, the preview binary has top_cropping=12 baked in, so
	 * effective_res + 12 must fit within the sensor input_res; keep the
	 * ISP2400_MIN_PAD floor (12) even for sensor_isp to ensure this.
	 */
	if (input->sensor_isp && IS_ISP2401)
		min_pad_w = min_pad_h = 8;

	sink = atomisp_subdev_get_ffmt(&isp->asd.subdev, NULL, V4L2_SUBDEV_FORMAT_ACTIVE,
				       ATOMISP_SUBDEV_PAD_SINK);
	if (sink)
		fc = atomisp_find_in_fmt_conv(sink->code);
	if (!fc) {
		dev_warn(isp->dev, "%s: Could not get sensor format\n", __func__);
		goto apply_min_padding;
	}

	/*
	 * Bayer-order padding is only meaningful for RAW Bayer inputs.
	 * Non-Bayer (YUV) sensors have bayer_order=0 which coincides with
	 * IA_CSS_BAYER_ORDER_GRBG and would accidentally pass the checks
	 * below without triggering extra padding, but rely on this implicitly.
	 * Skip explicitly to make the non-Bayer path self-documenting.
	 */
	if (!atomisp_is_mbuscode_raw(sink->code))
		goto apply_min_padding;

	/*
	 * The ISP only supports GRBG for other bayer-orders additional padding
	 * is used so that the raw sensor data can be cropped to fix the order.
	 */
	bayer_order = atomisp_get_effective_bayer_order(input, fc->bayer_order);

	if (bayer_order == IA_CSS_BAYER_ORDER_RGGB ||
	    bayer_order == IA_CSS_BAYER_ORDER_GBRG)
		min_pad_w += 2;

	if (bayer_order == IA_CSS_BAYER_ORDER_BGGR ||
	    bayer_order == IA_CSS_BAYER_ORDER_GBRG)
		min_pad_h += 2;

apply_min_padding:
	*padding_w = max_t(u32, *padding_w, min_pad_w);
	*padding_h = max_t(u32, *padding_h, min_pad_h);
}

int atomisp_s_sensor_power(struct atomisp_device *isp, unsigned int input, bool on)
{
	int ret;

	if (isp->inputs[input].sensor_on == on)
		return 0;

	ret = v4l2_subdev_call(isp->inputs[input].sensor, core, s_power, on);
	if (ret && ret != -ENOIOCTLCMD) {
		dev_err(isp->dev, "Error setting sensor power %d: %d\n", on, ret);
		return ret;
	}

	isp->inputs[input].sensor_on = on;
	return 0;
}

int atomisp_select_input(struct atomisp_device *isp, unsigned int input)
{
	unsigned int input_orig = isp->asd.input_curr;
	int ret;

	/* Power on new sensor */
	ret = atomisp_s_sensor_power(isp, input, 1);
	if (ret)
		return ret;

	isp->asd.input_curr = input;

	/* Power off previous sensor */
	if (input != input_orig)
		atomisp_s_sensor_power(isp, input_orig, 0);

	atomisp_setup_input_links(isp);
	return 0;
}

/*
 * Ensure the CSI-receiver -> ISP link for input_curr is marked as enabled and
 * the other CSI-receiver -> ISP links are disabled.
 */
void atomisp_setup_input_links(struct atomisp_device *isp)
{
	struct media_link *link;

	lockdep_assert_held(&isp->media_dev.graph_mutex);

	for (int i = 0; i < ATOMISP_CAMERA_NR_PORTS; i++) {
		link = media_entity_find_link(
				&isp->csi2_port[i].subdev.entity.pads[CSI2_PAD_SOURCE],
				&isp->asd.subdev.entity.pads[ATOMISP_SUBDEV_PAD_SINK]);
		if (!link) {
			dev_err(isp->dev, "Error cannot find CSI2-port[%d] -> ISP link\n", i);
			continue; /* Should never happen */
		}

		/*
		 * Modify the flags directly, calling media_entity_setup_link()
		 * will end up calling atomisp_link_setup() which calls this
		 * function again leading to endless recursion.
		 */
		if (isp->sensor_subdevs[i] == isp->inputs[isp->asd.input_curr].csi_remote_source)
			link->flags |= MEDIA_LNK_FL_ENABLED;
		else
			link->flags &= ~MEDIA_LNK_FL_ENABLED;

		link->reverse->flags = link->flags;
	}
}

static int atomisp_set_sensor_crop_and_fmt(struct atomisp_device *isp,
					   struct v4l2_mbus_framefmt *ffmt,
					   int which,
					   unsigned int requested_width,
					   unsigned int requested_height)
{
	struct atomisp_input_subdev *input = &isp->inputs[isp->asd.input_curr];
	struct v4l2_subdev_selection sel = {
		.which = which,
		.target = V4L2_SEL_TGT_CROP,
		.r.width = ffmt->width,
		.r.height = ffmt->height,
	};
	struct v4l2_subdev_format format = {
		.which = which,
		.format = *ffmt,
	};
	/* input->sensor (the PA) always forces its own RAW10 code onto
	 * format.format below; keep the originally requested code around
	 * to re-apply it when talking to the sensor ISP's source pad.
	 */
	u32 requested_code = ffmt->code;
	struct v4l2_mbus_framefmt try_saved_format;
	struct v4l2_rect try_saved_crop;
	bool restore_try_state = false;
	struct v4l2_subdev_state *sd_state;
	int ret = 0;

	if (!input->sensor)
		return -EINVAL;

	/*
	 * When there is a separate sensor ISP (e.g. the mt9m114 IFP), its
	 * source pad crops a demosaicing border off the sink size before the
	 * compose/scale step can run. Make sure the PA (input->sensor) is
	 * asked for extra margin here too, independent of what the caller
	 * already added to ffmt, otherwise that border eats into the
	 * requested size and there is nothing left for compose to scale
	 * from, silently returning a size a few pixels short on each side.
	 */
	if (input->sensor_isp) {
		u32 margin_w, margin_h;

		atomisp_get_padding(isp, requested_width, requested_height, &margin_w, &margin_h);
		sel.r.width = requested_width + margin_w;
		sel.r.height = requested_height + margin_h;
		format.format.width = sel.r.width;
		format.format.height = sel.r.height;
		ffmt->width = sel.r.width;
		ffmt->height = sel.r.height;
	}

	/*
	 * Some old sensor drivers already write the registers on set_fmt
	 * instead of on stream on, power on the sensor now (on newer
	 * sensor drivers the s_power op is a no-op).
	 */
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = atomisp_s_sensor_power(isp, isp->asd.input_curr, 1);
		if (ret)
			return ret;
	}

	sd_state = (which == V4L2_SUBDEV_FORMAT_TRY) ? input->try_sd_state :
						       input->sensor->active_state;
	if (sd_state)
		v4l2_subdev_lock_state(sd_state);

	if (which == V4L2_SUBDEV_FORMAT_TRY && sd_state) {
		struct v4l2_mbus_framefmt *state_fmt;
		struct v4l2_rect *state_crop;

		state_fmt = v4l2_subdev_state_get_format(sd_state, 0);
		state_crop = v4l2_subdev_state_get_crop(sd_state, 0);
		if (state_fmt && state_crop) {
			try_saved_format = *state_fmt;
			try_saved_crop = *state_crop;
			restore_try_state = true;
		}
	}

	if (!input->crop_support)
		goto set_fmt;

	/* Cropping is done before binning, when binning double the crop rect */
	if (input->binning_support && sel.r.width <= (input->native_rect.width / 2) &&
				      sel.r.height <= (input->native_rect.height / 2)) {
		sel.r.width *= 2;
		sel.r.height *= 2;
	}

	/* Clamp to avoid top/left calculations overflowing */
	sel.r.width = min(sel.r.width, input->native_rect.width);
	sel.r.height = min(sel.r.height, input->native_rect.height);

	sel.r.left = ((input->native_rect.width - sel.r.width) / 2) & ~1;
	sel.r.top = ((input->native_rect.height - sel.r.height) / 2) & ~1;

	ret = v4l2_subdev_call(input->sensor, pad, set_selection, sd_state, &sel);
	if (ret)
		dev_err(isp->dev, "Error setting crop to (%d,%d)/%ux%u: %d\n",
			sel.r.left, sel.r.top, sel.r.width, sel.r.height, ret);

set_fmt:
	if (ret == 0) {
		ret = v4l2_subdev_call(input->sensor, pad, set_fmt, sd_state, &format);
		dev_dbg(isp->dev, "Set sensor format ret: %d size %dx%d\n",
			ret, format.format.width, format.format.height);
	}

	if (sd_state)
		v4l2_subdev_unlock_state(sd_state);

	if (which == V4L2_SUBDEV_FORMAT_TRY && restore_try_state && sd_state) {
		struct v4l2_mbus_framefmt *state_fmt;
		struct v4l2_rect *state_crop;

		v4l2_subdev_lock_state(sd_state);
		state_fmt = v4l2_subdev_state_get_format(sd_state, 0);
		state_crop = v4l2_subdev_state_get_crop(sd_state, 0);
		if (state_fmt)
			*state_fmt = try_saved_format;
		if (state_crop)
			*state_crop = try_saved_crop;
		v4l2_subdev_unlock_state(sd_state);
	}

	/*
	 * Propagate new fmt to sensor ISP. This also runs for TRY so that the
	 * pix format atomisp_try_fmt() reports to userspace already reflects
	 * the IFP scaler's compose size instead of only the sink crop size.
	 */
	if (ret == 0 && input->sensor_isp &&
	    (which == V4L2_SUBDEV_FORMAT_ACTIVE || input->try_sd_state_isp)) {
		if (which == V4L2_SUBDEV_FORMAT_TRY) {
			sd_state = input->try_sd_state_isp;
			v4l2_subdev_lock_state(sd_state);
		} else {
			sd_state = v4l2_subdev_lock_and_get_active_state(input->sensor_isp);
		}

		format.pad = SENSOR_ISP_PAD_SINK;
		ret = v4l2_subdev_call(input->sensor_isp, pad, set_fmt, sd_state, &format);
		dev_dbg(isp->dev, "Set sensor ISP sink format ret: %d size %dx%d\n",
			ret, format.format.width, format.format.height);

		/*
		 * Set the source pad's media bus code *before* asking for compose,
		 * since e.g. the mt9m114 IFP bypasses crop/compose entirely while
		 * its source pad is still set to RAW10 (the sink-pad default).
		 */
		if (ret == 0) {
			format.pad = SENSOR_ISP_PAD_SOURCE;
			format.format.code = requested_code;
			ret = v4l2_subdev_call(input->sensor_isp, pad, set_fmt, sd_state, &format);
			dev_dbg(isp->dev, "Set sensor ISP source format (pre-compose) ret: %d size %dx%d\n",
				ret, format.format.width, format.format.height);
		}

		/*
		 * Ask the sensor ISP's scaler (e.g. the mt9m114 IFP) to downscale to the
		 * originally requested size instead of always passing through its full
		 * sink crop. Sensors without scaler-on-sink support (or that can only
		 * bypass it, like RAW10 passthrough) will reject or clamp this; that is
		 * not fatal, it just means no downscale happens for this format/sensor.
		 */
		if (ret == 0 && (requested_width < format.format.width ||
				 requested_height < format.format.height)) {
			/* requested_* are the true user request, without the pad_w/pad_h margin. */
			struct v4l2_subdev_selection crop_sel = {
				.which = which,
				.pad = SENSOR_ISP_PAD_SINK,
				.target = V4L2_SEL_TGT_CROP,
			};
			struct v4l2_subdev_selection compose_sel = {
				.which = which,
				.pad = SENSOR_ISP_PAD_SINK,
				.target = V4L2_SEL_TGT_COMPOSE,
				.r.width = requested_width,
				.r.height = requested_height,
			};
			int compose_ret;

			v4l2_subdev_call(input->sensor_isp, pad, get_selection, sd_state, &crop_sel);
			dev_dbg(isp->dev, "DEBUG sensor ISP sink crop before compose: %ux%u src_code=0x%x\n",
				crop_sel.r.width, crop_sel.r.height, format.format.code);

			compose_ret = v4l2_subdev_call(input->sensor_isp, pad, set_selection,
						       sd_state, &compose_sel);
			dev_dbg(isp->dev, "Set sensor ISP compose ret: %d size %dx%d\n",
				compose_ret, compose_sel.r.width, compose_sel.r.height);
		}

		if (ret == 0) {
			format.pad = SENSOR_ISP_PAD_SOURCE;
			format.format.code = requested_code;
			ret = v4l2_subdev_call(input->sensor_isp, pad, set_fmt, sd_state, &format);
			dev_dbg(isp->dev, "Set sensor ISP source format ret: %d size %dx%d\n",
				ret, format.format.width, format.format.height);
		}

		if (sd_state)
			v4l2_subdev_unlock_state(sd_state);
	}

	/* Propagate new fmt to CSI port */
	if (ret == 0 && which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		format.pad = CSI2_PAD_SINK;
		ret = v4l2_subdev_call(input->csi_port, pad, set_fmt, NULL, &format);
		if (ret)
			return ret;
	}

	*ffmt = format.format;
	return ret;
}

/* This function looks up the closest available resolution. */
int atomisp_try_fmt(struct atomisp_device *isp, struct v4l2_pix_format *f,
		    const struct atomisp_format_bridge **fmt_ret,
		    const struct atomisp_format_bridge **snr_fmt_ret)
{
	const struct atomisp_format_bridge *fmt, *snr_fmt;
	struct atomisp_sub_device *asd = &isp->asd;
	struct v4l2_mbus_framefmt ffmt = { };
	u32 padding_w, padding_h;
	int ret;

	fmt = atomisp_get_format_bridge(f->pixelformat);
	/* Currently, raw formats are broken!!! */
	if (!fmt || fmt->sh_fmt == IA_CSS_FRAME_FORMAT_RAW) {
		f->pixelformat = V4L2_PIX_FMT_YUV420;

		fmt = atomisp_get_format_bridge(f->pixelformat);
		if (!fmt)
			return -EINVAL;
	}

	/*
	 * The preview pipeline does not support width > 1920. Also limit height
	 * to avoid sensor drivers still picking a too wide resolution.
	 */
	if (asd->run_mode->val == ATOMISP_RUN_MODE_PREVIEW) {
		f->width = min(f->width, 1920U);
		f->height = min(f->height, 1440U);
	}

	/*
	 * atomisp_set_fmt() will set the sensor resolution to the requested
	 * resolution + padding. Add padding here and remove it again after
	 * the set_fmt call, like atomisp_set_fmt_to_snr() does.
	 */
	atomisp_get_padding(isp, f->width, f->height, &padding_w, &padding_h);
	v4l2_fill_mbus_format(&ffmt, f, fmt->mbus_code);
	ffmt.width += padding_w;
	ffmt.height += padding_h;

	dev_dbg(isp->dev, "try_mbus_fmt: try %ux%u\n", ffmt.width, ffmt.height);

	{
	unsigned int req_width = f->width, req_height = f->height;

	ret = atomisp_set_sensor_crop_and_fmt(isp, &ffmt, V4L2_SUBDEV_FORMAT_TRY,
					      f->width, f->height);
	if (ret)
		return ret;

	dev_dbg(isp->dev, "try_mbus_fmt: got %ux%u\n", ffmt.width, ffmt.height);

	snr_fmt = atomisp_get_format_bridge_from_mbus(ffmt.code);
	if (!snr_fmt) {
		dev_err(isp->dev, "unknown sensor format 0x%8.8x\n",
			ffmt.code);
		return -EINVAL;
	}

	/*
	 * Recalculate padding for the sensor-selected size to keep TRY probing
	 * deterministic across repeated calls with the same user request.
	 * If the sensor ISP's scaler already delivered exactly the requested
	 * size (e.g. via the compose path in atomisp_set_sensor_crop_and_fmt()),
	 * there is no padding left to strip back off.
	 */
	if (ffmt.width == req_width && ffmt.height == req_height) {
		f->width = ffmt.width;
		f->height = ffmt.height;
	} else {
		atomisp_get_padding(isp, ffmt.width, ffmt.height, &padding_w, &padding_h);
		f->width = ffmt.width > padding_w ? ffmt.width - padding_w : ffmt.width;
		f->height = ffmt.height > padding_h ? ffmt.height - padding_h : ffmt.height;
	}
	}

	/*
	 * If the format is jpeg or custom RAW, then the width and height will
	 * not satisfy the normal atomisp requirements and no need to check
	 * the below conditions. So just assign to what is being returned from
	 * the sensor driver.
	 */
	if (f->pixelformat == V4L2_PIX_FMT_JPEG ||
	    f->pixelformat == V4L2_PIX_FMT_CUSTOM_M10MO_RAW)
		goto out_fill_pix_format;

	/* app vs isp */
	f->width = rounddown(clamp_t(u32, f->width, ATOM_ISP_MIN_WIDTH,
				     ATOM_ISP_MAX_WIDTH), ATOM_ISP_STEP_WIDTH);
	f->height = rounddown(clamp_t(u32, f->height, ATOM_ISP_MIN_HEIGHT,
				      ATOM_ISP_MAX_HEIGHT), ATOM_ISP_STEP_HEIGHT);

out_fill_pix_format:
	atomisp_fill_pix_format(f, f->width, f->height, fmt);

	if (fmt_ret)
		*fmt_ret = fmt;

	if (snr_fmt_ret)
		*snr_fmt_ret = snr_fmt;

	return 0;
}

enum mipi_port_id atomisp_port_to_mipi_port(struct atomisp_device *isp,
					    enum atomisp_camera_port port)
{
	switch (port) {
	case ATOMISP_CAMERA_PORT_PRIMARY:
		return MIPI_PORT0_ID;
	case ATOMISP_CAMERA_PORT_SECONDARY:
		return MIPI_PORT1_ID;
	case ATOMISP_CAMERA_PORT_TERTIARY:
		return MIPI_PORT2_ID;
	default:
		dev_err(isp->dev, "unsupported port: %d\n", port);
		return MIPI_PORT0_ID;
	}
}

static inline int atomisp_set_sensor_mipi_to_isp(
    struct atomisp_sub_device *asd,
    enum atomisp_input_stream_id stream_id,
    struct camera_mipi_info *mipi_info)
{
	struct v4l2_control ctrl;
	struct atomisp_device *isp = asd->isp;
	struct atomisp_input_subdev *input = &isp->inputs[asd->input_curr];
	const struct atomisp_in_fmt_conv *fc;
	int mipi_freq = 0;
	unsigned int input_format, bayer_order;
	enum atomisp_input_format metadata_format = ATOMISP_INPUT_FORMAT_EMBEDDED;
	u32 mipi_port, metadata_width = 0, metadata_height = 0;

	ctrl.id = V4L2_CID_LINK_FREQ;
	if (v4l2_g_ctrl(input->sensor->ctrl_handler, &ctrl) == 0)
		mipi_freq = ctrl.value;

	if (asd->stream_env[stream_id].isys_configs == 1) {
		input_format =
		    asd->stream_env[stream_id].isys_info[0].input_format;
		atomisp_css_isys_set_format(asd, stream_id,
					    input_format, IA_CSS_STREAM_DEFAULT_ISYS_STREAM_IDX);
	} else if (asd->stream_env[stream_id].isys_configs == 2) {
		atomisp_css_isys_two_stream_cfg_update_stream1(
		    asd, stream_id,
		    asd->stream_env[stream_id].isys_info[0].input_format,
		    asd->stream_env[stream_id].isys_info[0].width,
		    asd->stream_env[stream_id].isys_info[0].height);

		atomisp_css_isys_two_stream_cfg_update_stream2(
		    asd, stream_id,
		    asd->stream_env[stream_id].isys_info[1].input_format,
		    asd->stream_env[stream_id].isys_info[1].width,
		    asd->stream_env[stream_id].isys_info[1].height);
	}

	/*
	 * Prefer the active sink pad format, as it reflects the actual media
	 * graph configuration selected at runtime. Fall back to mipi_info for
	 * legacy sensors that don't expose a usable mbus code.
	 */
	{
		struct v4l2_mbus_framefmt *sink;

		sink = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
					       V4L2_SUBDEV_FORMAT_ACTIVE,
					       ATOMISP_SUBDEV_PAD_SINK);
		fc = atomisp_find_in_fmt_conv(sink->code);
	}

	if (fc) {
		input_format = fc->atomisp_in_fmt;
		bayer_order = fc->bayer_order;
	} else if (mipi_info && mipi_info->input_format != -1) {
		bayer_order = mipi_info->raw_bayer_order;
		fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(
			 mipi_info->input_format);
		if (!fc)
			return -EINVAL;
		input_format = fc->atomisp_in_fmt;
	} else {
		return -EINVAL;
	}

	if (mipi_info) {
		metadata_format = mipi_info->metadata_format;
		metadata_width = mipi_info->metadata_width;
		metadata_height = mipi_info->metadata_height;
	}

	bayer_order = atomisp_get_effective_bayer_order(input, bayer_order);

	atomisp_css_input_set_format(asd, stream_id, input_format);
	atomisp_css_input_set_bayer_order(asd, stream_id, bayer_order);

	fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(metadata_format);
	if (!fc)
		return -EINVAL;

	input_format = fc->atomisp_in_fmt;
	mipi_port = atomisp_port_to_mipi_port(isp, input->port);
	atomisp_css_input_configure_port(asd, mipi_port,
					 isp->sensor_lanes[mipi_port],
					 0xffff4, mipi_freq,
					 input_format,
					 metadata_width, metadata_height);
	return 0;
}

static int configure_pp_input_nop(struct atomisp_sub_device *asd,
				  unsigned int width, unsigned int height)
{
	return 0;
}

static int configure_output_nop(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum ia_css_frame_format sh_fmt)
{
	return 0;
}

static int get_frame_info_nop(struct atomisp_sub_device *asd,
			      struct ia_css_frame_info *finfo)
{
	return 0;
}

/*
 * Resets CSS parameters that depend on input resolution.
 *
 * Update params like CSS RAW binning, 2ppc mode and pp_input
 * which depend on input size, but are not automatically
 * handled in CSS when the input resolution is changed.
 */
static int css_input_resolution_changed(struct atomisp_sub_device *asd,
					struct v4l2_mbus_framefmt *ffmt)
{
	struct atomisp_metadata_buf *md_buf = NULL, *_md_buf;
	unsigned int i;

	dev_dbg(asd->isp->dev, "css_input_resolution_changed to %ux%u\n",
		ffmt->width, ffmt->height);

	if (IS_ISP2401)
		atomisp_css_input_set_two_pixels_per_clock(asd, false);
	else
		atomisp_css_input_set_two_pixels_per_clock(asd, true);

	/*
	 * If sensor input changed, which means metadata resolution changed
	 * together. Release all metadata buffers here to let it re-allocated
	 * next time in reqbufs.
	 */
	for (i = 0; i < ATOMISP_METADATA_TYPE_NUM; i++) {
		list_for_each_entry_safe(md_buf, _md_buf, &asd->metadata[i],
					 list) {
			atomisp_css_free_metadata_buffer(md_buf);
			list_del(&md_buf->list);
			kfree(md_buf);
		}
	}
	return 0;

	/*
	 * TODO: atomisp_css_preview_configure_pp_input() not
	 *       reset due to CSS bug tracked as PSI BZ 115124
	 */
}

static int atomisp_set_fmt_to_isp(struct video_device *vdev,
				  struct ia_css_frame_info *output_info,
				  const struct v4l2_pix_format *pix)
{
	struct camera_mipi_info *mipi_info;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_input_subdev *input = &isp->inputs[asd->input_curr];
	const struct atomisp_format_bridge *format;
	struct v4l2_rect *isp_sink_crop;
	enum ia_css_pipe_id pipe_id;
	int (*configure_output)(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum ia_css_frame_format sh_fmt) =
				    configure_output_nop;
	int (*get_frame_info)(struct atomisp_sub_device *asd,
			      struct ia_css_frame_info *finfo) =
				  get_frame_info_nop;
	int (*configure_pp_input)(struct atomisp_sub_device *asd,
				  unsigned int width, unsigned int height) =
				      configure_pp_input_nop;
	const struct atomisp_in_fmt_conv *fc = NULL;
	struct v4l2_mbus_framefmt *ffmt;
	int ret, i;

	isp_sink_crop = atomisp_subdev_get_rect(
			    &asd->subdev, NULL, V4L2_SUBDEV_FORMAT_ACTIVE,
			    ATOMISP_SUBDEV_PAD_SINK, V4L2_SEL_TGT_CROP);

	format = atomisp_get_format_bridge(pix->pixelformat);
	if (!format)
		return -EINVAL;

	mipi_info = atomisp_to_sensor_mipi_info(input->sensor);

	if (atomisp_set_sensor_mipi_to_isp(asd, ATOMISP_INPUT_STREAM_GENERAL,
					   mipi_info))
		return -EINVAL;

	if (mipi_info)
		fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(mipi_info->input_format);
	if (!fc) {
		ffmt = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
					       V4L2_SUBDEV_FORMAT_ACTIVE,
					       ATOMISP_SUBDEV_PAD_SINK);
		fc = atomisp_find_in_fmt_conv(ffmt->code);
	}
	if (!fc)
		return -EINVAL;

	if (format->sh_fmt == IA_CSS_FRAME_FORMAT_RAW &&
	    raw_output_format_match_input(fc->atomisp_in_fmt, pix->pixelformat))
		return -EINVAL;

	/*
	 * Configure viewfinder also when vfpp is disabled: the
	 * CSS still requires viewfinder configuration.
	 */
	{
		u32 width, height;

		if (pix->width < 640 || pix->height < 480) {
			width = pix->width;
			height = pix->height;
		} else {
			width = 640;
			height = 480;
		}

		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO ||
		    asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER) {
			atomisp_css_video_configure_viewfinder(asd, width, height, 0,
							       IA_CSS_FRAME_FORMAT_NV12);
		} else if (asd->run_mode->val == ATOMISP_RUN_MODE_PREVIEW) {
			atomisp_css_preview_configure_viewfinder(asd, width, height, 0,
							 IA_CSS_FRAME_FORMAT_NV12);
		} else if (asd->run_mode->val == ATOMISP_RUN_MODE_STILL_CAPTURE ||
			   asd->vfpp->val == ATOMISP_VFPP_DISABLE_LOWLAT) {
			atomisp_css_capture_configure_viewfinder(asd, width, height, 0,
								 IA_CSS_FRAME_FORMAT_NV12);
		}
	}

	atomisp_css_input_set_mode(asd, IA_CSS_INPUT_MODE_BUFFERED_SENSOR);

	for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].pipe_extra_configs[i].disable_vf_pp = asd->vfpp->val != ATOMISP_VFPP_ENABLE;

	/* ISP2401 new input system need to use copy pipe */
	if (asd->copy_mode) {
		pipe_id = IA_CSS_PIPE_ID_COPY;
		atomisp_css_capture_enable_online(asd, ATOMISP_INPUT_STREAM_GENERAL, false);
	} else if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER) {
		/* video same in continuouscapture and online modes */
		configure_output = atomisp_css_video_configure_output;
		get_frame_info = atomisp_css_video_get_output_frame_info;
		pipe_id = IA_CSS_PIPE_ID_VIDEO;
	} else if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		configure_output = atomisp_css_video_configure_output;
		get_frame_info = atomisp_css_video_get_output_frame_info;
		pipe_id = IA_CSS_PIPE_ID_VIDEO;
	} else if (asd->run_mode->val == ATOMISP_RUN_MODE_PREVIEW) {
		configure_output = atomisp_css_preview_configure_output;
		get_frame_info = atomisp_css_preview_get_output_frame_info;
		configure_pp_input = atomisp_css_preview_configure_pp_input;
		pipe_id = IA_CSS_PIPE_ID_PREVIEW;
	} else {
		if (format->sh_fmt == IA_CSS_FRAME_FORMAT_RAW) {
			atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_RAW);
			atomisp_css_enable_dz(asd, false);
		} else {
			atomisp_update_capture_mode(asd);
		}

		/* in case of ANR, force capture pipe to offline mode */
		atomisp_css_capture_enable_online(asd, ATOMISP_INPUT_STREAM_GENERAL,
						  !asd->params.low_light);

		configure_output = atomisp_css_capture_configure_output;
		get_frame_info = atomisp_css_capture_get_output_frame_info;
		configure_pp_input = atomisp_css_capture_configure_pp_input;
		pipe_id = IA_CSS_PIPE_ID_CAPTURE;

		if (asd->run_mode->val != ATOMISP_RUN_MODE_STILL_CAPTURE) {
			dev_err(isp->dev,
				"Need to set the running mode first\n");
			asd->run_mode->val = ATOMISP_RUN_MODE_STILL_CAPTURE;
		}
	}

	if (asd->copy_mode)
		ret = atomisp_css_copy_configure_output(asd, ATOMISP_INPUT_STREAM_GENERAL,
							pix->width, pix->height,
							format->planar ? pix->bytesperline :
							pix->bytesperline * 8 / format->depth,
							format->sh_fmt);
	else
		ret = configure_output(asd, pix->width, pix->height,
				       format->planar ? pix->bytesperline :
				       pix->bytesperline * 8 / format->depth,
				       format->sh_fmt);
	if (ret) {
		dev_err(isp->dev, "configure_output %ux%u, format %8.8x\n",
			pix->width, pix->height, format->sh_fmt);
		return -EINVAL;
	}

	ret = configure_pp_input(asd, isp_sink_crop->width, isp_sink_crop->height);
	if (ret) {
		dev_err(isp->dev, "configure_pp_input %ux%u\n",
			isp_sink_crop->width,
			isp_sink_crop->height);
		return -EINVAL;
	}
	if (asd->copy_mode)
		ret = atomisp_css_copy_get_output_frame_info(asd,
							     ATOMISP_INPUT_STREAM_GENERAL,
							     output_info);
	else
		ret = get_frame_info(asd, output_info);
	if (ret) {
		dev_err(isp->dev, "__get_frame_info %ux%u (padded to %u) returned %d\n",
			pix->width, pix->height, pix->bytesperline, ret);
		return ret;
	}

	atomisp_update_grid_info(asd, pipe_id);
	return 0;
}

static void atomisp_get_dis_envelop(struct atomisp_sub_device *asd,
				    unsigned int width, unsigned int height,
				    unsigned int *dvs_env_w, unsigned int *dvs_env_h)
{
	if (asd->params.video_dis_en &&
	    asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		/* envelope is 20% of the output resolution */
		/*
		 * dvs envelope cannot be round up.
		 * it would cause ISP timeout and color switch issue
		 */
		*dvs_env_w = rounddown(width / 5, ATOM_ISP_STEP_WIDTH);
		*dvs_env_h = rounddown(height / 5, ATOM_ISP_STEP_HEIGHT);
	}

	asd->params.dis_proj_data_valid = false;
	asd->params.css_update_params_needed = true;
}

static void atomisp_check_copy_mode(struct atomisp_sub_device *asd,
				    const struct v4l2_pix_format *f)
{
	struct v4l2_mbus_framefmt *sink, *src;
	const struct atomisp_format_bridge *format;

	if (!IS_ISP2401) {
		/* Only used for the new input system */
		asd->copy_mode = false;
		return;
	}

	/*
	 * Restrict copy mode to RAW passthrough. For processed outputs
	 * (for example YUV preview), forcing copy mode can select the copy
	 * binary and fail stream start.
	 */
	format = atomisp_get_format_bridge(f->pixelformat);
	if (!format || format->sh_fmt != IA_CSS_FRAME_FORMAT_RAW) {
		asd->copy_mode = false;
		dev_dbg(asd->isp->dev,
			"copy_mode: 0 (non-RAW output format %8.8x)\n",
			f->pixelformat);
		return;
	}

	sink = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				       V4L2_SUBDEV_FORMAT_ACTIVE, ATOMISP_SUBDEV_PAD_SINK);
	src = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				      V4L2_SUBDEV_FORMAT_ACTIVE, ATOMISP_SUBDEV_PAD_SOURCE);

	if (sink->code == src->code && sink->width == f->width && sink->height == f->height)
		asd->copy_mode = true;
	else
		asd->copy_mode = false;

	dev_dbg(asd->isp->dev, "copy_mode: %d\n", asd->copy_mode);
}

static int atomisp_set_fmt_to_snr(struct video_device *vdev, const struct v4l2_pix_format *f,
				  unsigned int dvs_env_w, unsigned int dvs_env_h)
{
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	struct atomisp_device *isp = asd->isp;
	const struct atomisp_format_bridge *format;
	struct v4l2_mbus_framefmt req_ffmt, ffmt = { };
	int ret;

	format = atomisp_get_format_bridge(f->pixelformat);
	if (!format)
		return -EINVAL;

	v4l2_fill_mbus_format(&ffmt, f, format->mbus_code);
	ffmt.height += asd->sink_pad_padding_h + dvs_env_h;
	ffmt.width += asd->sink_pad_padding_w + dvs_env_w;

	dev_dbg(isp->dev, "s_mbus_fmt: ask %ux%u (padding %ux%u, dvs %ux%u)\n",
		ffmt.width, ffmt.height, asd->sink_pad_padding_w, asd->sink_pad_padding_h,
		dvs_env_w, dvs_env_h);

	req_ffmt = ffmt;

	/* Disable dvs if resolution can't be supported by sensor */
	if (asd->params.video_dis_en && asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		ret = atomisp_set_sensor_crop_and_fmt(isp, &ffmt, V4L2_SUBDEV_FORMAT_TRY,
						      f->width, f->height);
		if (ret)
			return ret;

		dev_dbg(isp->dev, "video dis: sensor width: %d, height: %d\n",
			ffmt.width, ffmt.height);

		if (ffmt.width < req_ffmt.width ||
		    ffmt.height < req_ffmt.height) {
			req_ffmt.height -= dvs_env_h;
			req_ffmt.width -= dvs_env_w;
			ffmt = req_ffmt;
			dev_warn(isp->dev,
				 "can not enable video dis due to sensor limitation.");
			asd->params.video_dis_en = false;
		}
	}

	ret = atomisp_set_sensor_crop_and_fmt(isp, &ffmt, V4L2_SUBDEV_FORMAT_ACTIVE,
					      f->width, f->height);
	if (ret)
		return ret;

	dev_dbg(isp->dev, "sensor width: %d, height: %d\n",
		ffmt.width, ffmt.height);

	if (ffmt.width < ATOM_ISP_STEP_WIDTH ||
	    ffmt.height < ATOM_ISP_STEP_HEIGHT)
		return -EINVAL;

	if (asd->params.video_dis_en && asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO &&
	    (ffmt.width < req_ffmt.width || ffmt.height < req_ffmt.height)) {
		dev_warn(isp->dev,
			 "can not enable video dis due to sensor limitation.");
		asd->params.video_dis_en = false;
	}

	/*
	 * The sensor may return a size that differs from what we asked for
	 * (e.g. 656x496 when we requested 652x492).  The sink-pad crop is
	 * later calculated as sensor_size - sink_pad_padding, so recompute
	 * the padding here to reflect the actual sensor delta.  Without this,
	 * the ISP sink crop overshoots the user-requested dimensions and
	 * libcamera's pipeline will see a mismatched source size.
	 */
	if (atomisp_subdev_format_conversion(asd) &&
	    ffmt.width >= f->width + dvs_env_w &&
	    ffmt.height >= f->height + dvs_env_h) {
		asd->sink_pad_padding_w = ffmt.width - f->width - dvs_env_w;
		asd->sink_pad_padding_h = ffmt.height - f->height - dvs_env_h;
		/*
		 * ISP2400 CSS preview binaries have DIS (digital image
		 * stabilisation) enabled, which forces a minimum DVS envelope
		 * of SH_CSS_MIN_DVS_ENVELOPE (12) pixels.  The IFMTR check
		 * requires binary->in_frame_info (= effective_res + 12) to fit
		 * within the sensor input_res.  Clamp the padding to at least
		 * ISP2400_MIN_PAD_W/H so that effective_res is small enough
		 * for the binary's in_frame_info to stay within sensor bounds.
		 */
		if (!IS_ISP2401) {
			asd->sink_pad_padding_w = max_t(u32, asd->sink_pad_padding_w,
							ISP2400_MIN_PAD_W);
			asd->sink_pad_padding_h = max_t(u32, asd->sink_pad_padding_h,
							ISP2400_MIN_PAD_H);
		}
		dev_dbg(isp->dev, "adjusted sink padding to %ux%u after sensor set_fmt\n",
			asd->sink_pad_padding_w, asd->sink_pad_padding_h);
	}

	atomisp_subdev_set_ffmt(&asd->subdev, NULL,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK, &ffmt);

	return css_input_resolution_changed(asd, &ffmt);
}

int atomisp_set_fmt(struct video_device *vdev, struct v4l2_format *f)
{
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	const struct atomisp_format_bridge *format_bridge;
	const struct atomisp_format_bridge *snr_format_bridge;
	struct ia_css_frame_info output_info;
	unsigned int dvs_env_w = 0, dvs_env_h = 0;
	struct v4l2_mbus_framefmt isp_source_fmt = {0};
	struct v4l2_rect isp_sink_crop;
	int ret;

	ret = atomisp_pipe_check(pipe, true);
	if (ret)
		return ret;

	dev_dbg(isp->dev,
		"setting resolution %ux%u bytesperline %u\n",
		f->fmt.pix.width, f->fmt.pix.height, f->fmt.pix.bytesperline);

	/* Ensure that the resolution is equal or below the maximum supported */
	ret = atomisp_try_fmt(isp, &f->fmt.pix, &format_bridge, &snr_format_bridge);
	if (ret)
		return ret;

	pipe->sh_fmt = format_bridge->sh_fmt;
	pipe->pix.pixelformat = format_bridge->pixelformat;

	atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK)->code =
				    snr_format_bridge->mbus_code;

	isp_source_fmt.code = format_bridge->mbus_code;
	atomisp_subdev_set_ffmt(&asd->subdev, NULL,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SOURCE, &isp_source_fmt);

	if (atomisp_subdev_format_conversion(asd)) {
		atomisp_get_padding(isp, f->fmt.pix.width, f->fmt.pix.height,
				    &asd->sink_pad_padding_w, &asd->sink_pad_padding_h);
	} else {
		asd->sink_pad_padding_w = 0;
		asd->sink_pad_padding_h = 0;
	}

	atomisp_get_dis_envelop(asd, f->fmt.pix.width, f->fmt.pix.height,
				&dvs_env_w, &dvs_env_h);

	ret = atomisp_set_fmt_to_snr(vdev, &f->fmt.pix, dvs_env_w, dvs_env_h);
	if (ret) {
		dev_warn(isp->dev,
			 "Set format to sensor failed with %d\n", ret);
		return -EINVAL;
	}

	atomisp_csi_lane_config(isp);

	atomisp_check_copy_mode(asd, &f->fmt.pix);

	isp_sink_crop = *atomisp_subdev_get_rect(&asd->subdev, NULL,
			V4L2_SUBDEV_FORMAT_ACTIVE,
			ATOMISP_SUBDEV_PAD_SINK,
			V4L2_SEL_TGT_CROP);

	/* Try to enable YUV downscaling if ISP input is 10 % (either
	 * width or height) bigger than the desired result. */
	if (!IS_MOFD ||
	    isp_sink_crop.width * 9 / 10 < f->fmt.pix.width ||
	    isp_sink_crop.height * 9 / 10 < f->fmt.pix.height ||
	    (atomisp_subdev_format_conversion(asd) &&
	     (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO ||
	      asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER))) {
		isp_sink_crop.width = f->fmt.pix.width;
		isp_sink_crop.height = f->fmt.pix.height;

		atomisp_subdev_set_selection(&asd->subdev, NULL,
					     V4L2_SUBDEV_FORMAT_ACTIVE,
					     ATOMISP_SUBDEV_PAD_SOURCE, V4L2_SEL_TGT_COMPOSE,
					     0, &isp_sink_crop);
	} else {
		struct v4l2_rect main_compose = {0};

		main_compose.width = isp_sink_crop.width;
		main_compose.height =
		    DIV_ROUND_UP(main_compose.width * f->fmt.pix.height,
				 f->fmt.pix.width);
		if (main_compose.height > isp_sink_crop.height) {
			main_compose.height = isp_sink_crop.height;
			main_compose.width =
			    DIV_ROUND_UP(main_compose.height *
					 f->fmt.pix.width,
					 f->fmt.pix.height);
		}

		atomisp_subdev_set_selection(&asd->subdev, NULL,
					     V4L2_SUBDEV_FORMAT_ACTIVE,
					     ATOMISP_SUBDEV_PAD_SOURCE,
					     V4L2_SEL_TGT_COMPOSE, 0,
					     &main_compose);
	}

	ret = atomisp_set_fmt_to_isp(vdev, &output_info, &f->fmt.pix);
	if (ret) {
		dev_warn(isp->dev, "Can't set format on ISP. Error %d\n", ret);
		return -EINVAL;
	}

	atomisp_fill_pix_format(&pipe->pix, f->fmt.pix.width, f->fmt.pix.height, format_bridge);

	f->fmt.pix = pipe->pix;

	dev_dbg(isp->dev, "%s: %dx%d, image size: %d, %d bytes per line\n",
		__func__,
		f->fmt.pix.width, f->fmt.pix.height,
		f->fmt.pix.sizeimage, f->fmt.pix.bytesperline);

	return 0;
}

void atomisp_init_raw_buffer_bitmap(struct atomisp_sub_device *asd)
{
	unsigned long flags;

	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	memset(asd->raw_buffer_bitmap, 0, sizeof(asd->raw_buffer_bitmap));
	asd->raw_buffer_locked_count = 0;
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);
}

