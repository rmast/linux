/*
 * MT9M114 Low-Light Control Example
 * 
 * This program demonstrates how to use the new low-light controls
 * added by the MT9M114 patch for Asus T100 (Bay Trail).
 *
 * Compile: gcc -o mt9m114_lowlight_control mt9m114_lowlight_control.c
 * Usage: ./mt9m114_lowlight_control /dev/v4l-subdev2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <errno.h>

/* Custom control IDs (must match the kernel driver) */
#define V4L2_CID_MT9M114_AE_METERING_WEIGHTS	(V4L2_CID_USER_BASE + 0x1100)
#define V4L2_CID_MT9M114_AE_TRACK_SPEED		(V4L2_CID_USER_BASE + 0x1101)

/* Preset metering patterns */
static const unsigned char pattern_center_weighted[25] = {
	0x02, 0x04, 0x08, 0x04, 0x02,
	0x04, 0x08, 0x0c, 0x08, 0x04,
	0x08, 0x0c, 0x0f, 0x0c, 0x08,
	0x04, 0x08, 0x0c, 0x08, 0x04,
	0x02, 0x04, 0x08, 0x04, 0x02
};

static const unsigned char pattern_uniform[25] = {
	0x08, 0x08, 0x08, 0x08, 0x08,
	0x08, 0x08, 0x08, 0x08, 0x08,
	0x08, 0x08, 0x08, 0x08, 0x08,
	0x08, 0x08, 0x08, 0x08, 0x08,
	0x08, 0x08, 0x08, 0x08, 0x08
};

static const unsigned char pattern_backlit_portrait[25] = {
	0x00, 0x00, 0x00, 0x00, 0x00,  /* Ignore top (bright window) */
	0x02, 0x08, 0x0c, 0x08, 0x02,
	0x04, 0x0c, 0x0f, 0x0c, 0x04,  /* Strong center (face) */
	0x02, 0x08, 0x0c, 0x08, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00   /* Ignore bottom */
};

static const unsigned char pattern_spot_center[25] = {
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x04, 0x02, 0x00,
	0x00, 0x04, 0x0f, 0x04, 0x00,  /* Only center matters */
	0x00, 0x02, 0x04, 0x02, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00
};

void print_usage(const char *prog)
{
	printf("Usage: %s <device> <command> [options]\n\n", prog);
	printf("Commands:\n");
	printf("  status                  - Show current settings\n");
	printf("  lowlight <mode>         - Configure for low-light\n");
	printf("                            mode: normal, low, verylow\n");
	printf("  metering <pattern>      - Set AE metering pattern\n");
	printf("                            pattern: center, uniform, backlit, spot\n");
	printf("  exposure <lines>        - Set exposure time (in lines)\n");
	printf("  vblank <lines>          - Set vertical blanking\n");
	printf("\nExamples:\n");
	printf("  %s /dev/v4l-subdev2 status\n", prog);
	printf("  %s /dev/v4l-subdev2 lowlight low\n", prog);
	printf("  %s /dev/v4l-subdev2 metering backlit\n", prog);
	printf("  %s /dev/v4l-subdev2 exposure 3000\n", prog);
	printf("  %s /dev/v4l-subdev2 vblank 10000\n", prog);
}

int set_metering_pattern(int fd, const unsigned char *pattern)
{
	struct v4l2_ext_control ctrl = {0};
	struct v4l2_ext_controls ctrls = {0};
	unsigned char buffer[25];

	memcpy(buffer, pattern, 25);

	ctrl.id = V4L2_CID_MT9M114_AE_METERING_WEIGHTS;
	ctrl.size = 25;
	ctrl.ptr = buffer;

	ctrls.count = 1;
	ctrls.controls = &ctrl;

	if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
		perror("Failed to set metering weights");
		return -1;
	}

	printf("✓ Metering pattern updated\n");
	return 0;
}

int set_ae_track_speed(int fd, int speed)
{
	struct v4l2_control ctrl = {0};

	ctrl.id = V4L2_CID_MT9M114_AE_TRACK_SPEED;
	ctrl.value = speed;

	if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		perror("Failed to set AE tracking speed");
		return -1;
	}

	printf("✓ AE tracking speed set to 0x%02x\n", speed);
	return 0;
}

int set_simple_ctrl(int fd, unsigned int id, int value, const char *name)
{
	struct v4l2_control ctrl = {0};

	ctrl.id = id;
	ctrl.value = value;

	if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		perror(name);
		return -1;
	}

	printf("✓ %s set to %d\n", name, value);
	return 0;
}

int get_simple_ctrl(int fd, unsigned int id, const char *name)
{
	struct v4l2_control ctrl = {0};

	ctrl.id = id;

	if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
		/* Control may not exist, that's ok */
		return -1;
	}

	printf("  %-25s: %d\n", name, ctrl.value);
	return ctrl.value;
}

void show_status(int fd)
{
	struct v4l2_queryctrl qctrl = {0};
	int exposure, vblank, hblank, gain, ae_speed;

	printf("MT9M114 Current Settings:\n");
	printf("========================\n\n");

	exposure = get_simple_ctrl(fd, V4L2_CID_EXPOSURE, "Exposure");
	vblank = get_simple_ctrl(fd, V4L2_CID_VBLANK, "Vertical Blanking");
	hblank = get_simple_ctrl(fd, V4L2_CID_HBLANK, "Horizontal Blanking");
	gain = get_simple_ctrl(fd, V4L2_CID_ANALOGUE_GAIN, "Analog Gain");
	
	ae_speed = get_simple_ctrl(fd, V4L2_CID_MT9M114_AE_TRACK_SPEED, 
	                           "AE Tracking Speed");
	
	if (ae_speed >= 0) {
		printf("\n");
		if (ae_speed == 0x00)
			printf("  AE Mode: NORMAL (good lighting)\n");
		else if (ae_speed <= 0x02)
			printf("  AE Mode: BALANCED (low light)\n");
		else
			printf("  AE Mode: FAST (very low light)\n");
	}

	if (exposure >= 0 && vblank >= 0) {
		int frame_length = 976 + vblank;  /* Assuming 976 height */
		float frame_time_ms = (frame_length * 33.0) / 1000.0;  /* Approx line time 33µs */
		float fps = 1000.0 / frame_time_ms;
		
		printf("\n");
		printf("  Frame Length: %d lines\n", frame_length);
		printf("  Estimated FPS: %.1f\n", fps);
		printf("  Estimated frame time: %.1f ms\n", frame_time_ms);
		
		if (exposure > 1000) {
			printf("\n  ⚠ Long exposure active (VTS extended)\n");
		}
	}

	printf("\n");

	/* Check exposure range */
	qctrl.id = V4L2_CID_EXPOSURE;
	if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
		printf("  Exposure range: %d to %d\n", qctrl.minimum, qctrl.maximum);
		
		if (qctrl.maximum > 5000)
			printf("  ✓ Extended exposure range available (low-light capable)\n");
	}
}

int main(int argc, char **argv)
{
	int fd;
	const char *device;
	const char *command;

	if (argc < 3) {
		print_usage(argv[0]);
		return 1;
	}

	device = argv[1];
	command = argv[2];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("Failed to open device");
		return 1;
	}

	if (strcmp(command, "status") == 0) {
		show_status(fd);
	}
	else if (strcmp(command, "lowlight") == 0) {
		if (argc < 4) {
			printf("Error: lowlight requires mode argument\n");
			print_usage(argv[0]);
			close(fd);
			return 1;
		}

		const char *mode = argv[3];
		
		if (strcmp(mode, "normal") == 0) {
			printf("Configuring for normal lighting...\n");
			set_ae_track_speed(fd, 0x00);
			set_simple_ctrl(fd, V4L2_CID_VBLANK, 21, "VBLANK");
		}
		else if (strcmp(mode, "low") == 0) {
			printf("Configuring for low light...\n");
			set_ae_track_speed(fd, 0x03);
			set_simple_ctrl(fd, V4L2_CID_VBLANK, 5000, "VBLANK");
			set_metering_pattern(fd, pattern_center_weighted);
		}
		else if (strcmp(mode, "verylow") == 0) {
			printf("Configuring for very low light...\n");
			set_ae_track_speed(fd, 0x05);
			set_simple_ctrl(fd, V4L2_CID_VBLANK, 15000, "VBLANK");
			set_metering_pattern(fd, pattern_spot_center);
		}
		else {
			printf("Error: unknown mode '%s'\n", mode);
			printf("Valid modes: normal, low, verylow\n");
			close(fd);
			return 1;
		}
	}
	else if (strcmp(command, "metering") == 0) {
		if (argc < 4) {
			printf("Error: metering requires pattern argument\n");
			print_usage(argv[0]);
			close(fd);
			return 1;
		}

		const char *pattern = argv[3];
		
		if (strcmp(pattern, "center") == 0) {
			printf("Setting center-weighted metering...\n");
			set_metering_pattern(fd, pattern_center_weighted);
		}
		else if (strcmp(pattern, "uniform") == 0) {
			printf("Setting uniform metering...\n");
			set_metering_pattern(fd, pattern_uniform);
		}
		else if (strcmp(pattern, "backlit") == 0) {
			printf("Setting backlit portrait metering...\n");
			set_metering_pattern(fd, pattern_backlit_portrait);
		}
		else if (strcmp(pattern, "spot") == 0) {
			printf("Setting spot center metering...\n");
			set_metering_pattern(fd, pattern_spot_center);
		}
		else {
			printf("Error: unknown pattern '%s'\n", pattern);
			printf("Valid patterns: center, uniform, backlit, spot\n");
			close(fd);
			return 1;
		}
	}
	else if (strcmp(command, "exposure") == 0) {
		if (argc < 4) {
			printf("Error: exposure requires value argument\n");
			print_usage(argv[0]);
			close(fd);
			return 1;
		}

		int value = atoi(argv[3]);
		printf("Setting exposure to %d lines...\n", value);
		set_simple_ctrl(fd, V4L2_CID_EXPOSURE, value, "Exposure");
		
		printf("\nNote: If exposure > frame_length, VTS will auto-adjust\n");
		printf("      (FPS will drop to accommodate long exposure)\n");
	}
	else if (strcmp(command, "vblank") == 0) {
		if (argc < 4) {
			printf("Error: vblank requires value argument\n");
			print_usage(argv[0]);
			close(fd);
			return 1;
		}

		int value = atoi(argv[3]);
		printf("Setting vertical blanking to %d lines...\n", value);
		set_simple_ctrl(fd, V4L2_CID_VBLANK, value, "Vertical Blanking");
	}
	else {
		printf("Error: unknown command '%s'\n", command);
		print_usage(argv[0]);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
