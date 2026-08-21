/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Medifield PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2010 Intel Corporation. All Rights Reserved.
 *
 * Copyright (c) 2010 Silicon Hive www.siliconhive.com.
 */

#ifndef	__ATOMISP_CMD_H__
#define	__ATOMISP_CMD_H__

#include "../../include/linux/atomisp.h"
#include <linux/interrupt.h>
#include <linux/videodev2.h>

#include <media/v4l2-subdev.h>

#include "atomisp_internal.h"

#include "ia_css_types.h"
#include "ia_css.h"

struct atomisp_device;
struct ia_css_frame;

extern const struct v4l2_ioctl_ops atomisp_ioctl_ops;

extern const struct atomisp_format_bridge atomisp_output_fmts[];
extern const size_t atomisp_output_fmts_size;

const struct atomisp_format_bridge *atomisp_get_format_bridge(unsigned int pixelformat);
const struct atomisp_format_bridge *atomisp_get_format_bridge_from_mbus(u32 mbus_code);

int atomisp_pipe_check(struct atomisp_video_pipe *pipe, bool streaming_ok);
int atomisp_alloc_css_stat_bufs(struct atomisp_sub_device *asd, uint16_t stream_id);
int atomisp_start_streaming(struct vb2_queue *vq, unsigned int count);
void atomisp_stop_streaming(struct vb2_queue *vq);

#define MSI_ENABLE_BIT		16
#define INTR_DISABLE_BIT	10
#define BUS_MASTER_ENABLE	2
#define MEMORY_SPACE_ENABLE	1
#define INTR_IER		24
#define INTR_IIR		16

/* Helper function */
void dump_sp_dmem(struct atomisp_device *isp, unsigned int addr,
		  unsigned int size);
struct camera_mipi_info *atomisp_to_sensor_mipi_info(struct v4l2_subdev *sd);
struct atomisp_video_pipe *atomisp_to_video_pipe(struct video_device *dev);
int atomisp_reset(struct atomisp_device *isp);
int atomisp_buffers_in_css(struct atomisp_video_pipe *pipe);
void atomisp_buffer_done(struct ia_css_frame *frame, enum vb2_buffer_state state);
void atomisp_flush_video_pipe(struct atomisp_video_pipe *pipe, enum vb2_buffer_state state,
			      bool warn_on_css_frames);
void atomisp_clear_css_buffer_counters(struct atomisp_sub_device *asd);

/* Interrupt functions */
void atomisp_msi_irq_init(struct atomisp_device *isp);
void atomisp_msi_irq_uninit(struct atomisp_device *isp);
void atomisp_assert_recovery_work(struct work_struct *work);
irqreturn_t atomisp_isr(int irq, void *dev);
irqreturn_t atomisp_isr_thread(int irq, void *isp_ptr);
const struct atomisp_format_bridge *get_atomisp_format_bridge_from_mbus(
    u32 mbus_code);
bool atomisp_is_mbuscode_raw(uint32_t code);

/* Get internal fmt according to V4L2 fmt */
bool atomisp_is_viewfinder_support(struct atomisp_device *isp);

/* ISP features control function */

/*
 * Function to enable/disable lens geometry distortion correction (GDC) and
 * chromatic aberration correction (CAC)
 */
int atomisp_gdc_cac(struct atomisp_sub_device *asd, int flag,
		    __s32 *value);

/* Function to enable/disable low light mode (including ANR) */
int atomisp_low_light(struct atomisp_sub_device *asd, int flag,
		      __s32 *value);

/* Function to set the DIS coefficients. */
int atomisp_set_dis_coefs(struct atomisp_sub_device *asd,
			  struct atomisp_dis_coefficients *coefs);

/* Function to set the DIS motion vector. */
int atomisp_set_dis_vector(struct atomisp_sub_device *asd,
			   struct atomisp_dis_vector *vector);

/* Function to configure color effect of the image */
int atomisp_color_effect(struct atomisp_sub_device *asd, int flag,
			 __s32 *effect);

/* Function to configure bad pixel correction */
int atomisp_bad_pixel(struct atomisp_sub_device *asd, int flag,
		      __s32 *value);

/* Function to enable/disable video image stablization */
int atomisp_video_stable(struct atomisp_sub_device *asd, int flag,
			 __s32 *value);

/* Function to configure fixed pattern noise */
int atomisp_fixed_pattern(struct atomisp_sub_device *asd, int flag,
			  __s32 *value);

/* Function to configure false color correction */
int atomisp_false_color(struct atomisp_sub_device *asd, int flag,
			__s32 *value);

/* Function to setup digital zoom */
int atomisp_digital_zoom(struct atomisp_sub_device *asd, int flag,
			 __s32 *value);

/* Function to calculate real zoom region for every pipe */
int atomisp_calculate_real_zoom_region(struct atomisp_sub_device *asd,
				       struct ia_css_dz_config   *dz_config,
				       enum ia_css_pipe_id css_pipe_id);

int atomisp_cp_general_isp_parameters(struct atomisp_sub_device *asd,
				      struct atomisp_parameters *arg,
				      struct atomisp_css_params *css_param,
				      bool from_user);

int atomisp_cp_lsc_table(struct atomisp_sub_device *asd,
			 struct atomisp_shading_table *source_st,
			 struct atomisp_css_params *css_param,
			 bool from_user);

int atomisp_css_cp_dvs2_coefs(struct atomisp_sub_device *asd,
			      struct ia_css_dvs2_coefficients *coefs,
			      struct atomisp_css_params *css_param,
			      bool from_user);

int atomisp_cp_morph_table(struct atomisp_sub_device *asd,
			   struct atomisp_morph_table *source_morph_table,
			   struct atomisp_css_params *css_param,
			   bool from_user);

int atomisp_cp_dvs_6axis_config(struct atomisp_sub_device *asd,
				struct atomisp_dvs_6axis_config *user_6axis_config,
				struct atomisp_css_params *css_param,
				bool from_user);

int atomisp_makeup_css_parameters(struct atomisp_sub_device *asd,
				  struct atomisp_parameters *arg,
				  struct atomisp_css_params *css_param);

int atomisp_compare_grid(struct atomisp_sub_device *asd,
			 struct atomisp_grid_info *atomgrid);

/* Get sensor padding values for the non padded width x height resolution */
void atomisp_get_padding(struct atomisp_device *isp, u32 width, u32 height,
			 u32 *padding_w, u32 *padding_h);

/* Set sensor power (no-op if already on/off) */
int atomisp_s_sensor_power(struct atomisp_device *isp, unsigned int input, bool on);

/* Select which sensor to use, must be called with a valid input */
int atomisp_select_input(struct atomisp_device *isp, unsigned int input);

/* Setup media-controller links to reflect input_curr setting */
void atomisp_setup_input_links(struct atomisp_device *isp);

/* This function looks up the closest available resolution. */
int atomisp_try_fmt(struct atomisp_device *isp, struct v4l2_pix_format *f,
		    const struct atomisp_format_bridge **fmt_ret,
		    const struct atomisp_format_bridge **snr_fmt_ret);

int atomisp_set_fmt(struct video_device *vdev, struct v4l2_format *f);

void atomisp_free_internal_buffers(struct atomisp_sub_device *asd);

int atomisp_freq_scaling(struct atomisp_device *vdev,
			 enum atomisp_dfs_mode mode,
			 bool force);

void atomisp_buf_done(struct atomisp_sub_device *asd, int error,
		      enum ia_css_buffer_type buf_type,
		      enum ia_css_pipe_id css_pipe_id,
		      bool q_buffers, enum atomisp_input_stream_id stream_id);

/* Events. Only one event has to be exported for now. */
void atomisp_eof_event(struct atomisp_sub_device *asd, uint8_t exp_id);

enum mipi_port_id atomisp_port_to_mipi_port(struct atomisp_device *isp,
					    enum atomisp_camera_port port);

void atomisp_apply_css_parameters(
    struct atomisp_sub_device *asd,
    struct atomisp_css_params *css_param);
void atomisp_free_css_parameters(struct atomisp_css_params *css_param);

void atomisp_handle_parameter_and_buffer(struct atomisp_video_pipe *pipe);

void atomisp_flush_params_queue(struct atomisp_video_pipe *asd);

void atomisp_init_raw_buffer_bitmap(struct atomisp_sub_device *asd);

u32 atomisp_get_pixel_depth(u32 pixelformat);

/*
 * Function for HAL to query how many invalid frames at the beginning of ISP
 * pipeline output
 */
int atomisp_get_invalid_frame_num(struct video_device *vdev,
				  int *invalid_frame_num);

int atomisp_power_off(struct device *dev);
int atomisp_power_on(struct device *dev);
#endif /* __ATOMISP_CMD_H__ */
