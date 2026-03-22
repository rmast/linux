#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef __user
#define __user
#endif

#ifndef u32
typedef __u32 u32;
#endif

#ifndef s32
typedef __s32 s32;
#endif

#include <linux/atomisp.h>

#define BUF_COUNT 4

struct buffer {
	void *start;
	size_t length;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [/dev/videoX] [interval_ms] [frames] [subdev]\n"
		"  interval_ms: print cadence (default 200)\n"
		"  frames: number of frames to capture before exit (default: infinite)\n"
		"  subdev: optional /dev/v4l-subdevX for vblank/hblank/pixel_rate\n",
		prog);
}

static unsigned long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ULL +
	       (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static int get_ctrl_u32(int fd, __u32 id, __u32 *value)
{
	struct v4l2_control ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = id;
	if (xioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0)
		return -1;
	*value = ctrl.value;
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0";
	const char *subdev = NULL;
	unsigned int interval_ms = 200;
	unsigned int max_frames = 0;
	struct atomisp_parm params;
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct buffer bufs[BUF_COUNT];
	struct pollfd pfd;
	unsigned int i;
	unsigned int frames = 0;
	unsigned long long next_print;
	bool reported_metadata = false;
	int fd;
	int sfd = -1;

	if (argc > 1)
		dev = argv[1];
	if (argc > 2)
		interval_ms = (unsigned int)atoi(argv[2]);
	if (argc > 3)
		max_frames = (unsigned int)atoi(argv[3]);
	if (argc > 4)
		subdev = argv[4];
	if (argc > 5) {
		usage(argv[0]);
		return 1;
	}

	fd = open(dev, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (subdev) {
		sfd = open(subdev, O_RDWR | O_NONBLOCK, 0);
		if (sfd < 0) {
			perror("open subdev");
			close(fd);
			return 1;
		}
	}

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		close(fd);
		return 1;
	}

	{
		unsigned int input = 0;
		if (xioctl(fd, VIDIOC_S_INPUT, &input) < 0)
			perror("VIDIOC_S_INPUT");
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(cap.capabilities & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "Device does not support capture/streaming\n");
		close(fd);
		return 1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		perror("VIDIOC_G_FMT");
		close(fd);
		return 1;
	}

	if (fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0 ||
	    fmt.fmt.pix.pixelformat == 0 ||
	    (fmt.fmt.pix.width != 1280 ||
	     (fmt.fmt.pix.height != 720 && fmt.fmt.pix.height != 960))) {
		fmt.fmt.pix.width = 1280;
		fmt.fmt.pix.height = 720;
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
		fmt.fmt.pix.field = V4L2_FIELD_NONE;
	}

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		close(fd);
		return 1;
	}

	memset(&req, 0, sizeof(req));
	req.count = BUF_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		close(fd);
		return 1;
	}

	if (req.count < BUF_COUNT) {
		fprintf(stderr, "Insufficient buffer memory (got %u)\n", req.count);
		close(fd);
		return 1;
	}

	for (i = 0; i < BUF_COUNT; ++i) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			close(fd);
			return 1;
		}

		bufs[i].length = buf.length;
		bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, buf.m.offset);
		if (bufs[i].start == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return 1;
		}
	}

	for (i = 0; i < BUF_COUNT; ++i) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			close(fd);
			return 1;
		}
	}

	if (xioctl(fd, VIDIOC_STREAMON, &fmt.type) < 0) {
		perror("VIDIOC_STREAMON");
		close(fd);
		return 1;
	}

	pfd.fd = fd;
	pfd.events = POLLIN;
	next_print = now_ms();

	memset(&params, 0, sizeof(params));

	for (;;) {
		struct v4l2_buffer buf;
		int pret;

		pret = poll(&pfd, 1, 1000);
		if (pret < 0) {
			perror("poll");
			break;
		}
		if (pret == 0)
			continue;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				continue;
			perror("VIDIOC_DQBUF");
			break;
		}

		frames++;
		if (now_ms() >= next_print) {
			if (xioctl(fd, ATOMISP_IOC_G_ISP_PARM, &params) == 0) {
				__u32 vblank = 0;
				__u32 hblank = 0;
				__u32 pixel_rate = 0;
				__u32 exposure = 0;
				int cfd = (sfd >= 0) ? sfd : fd;
				bool have_vblank = get_ctrl_u32(cfd, V4L2_CID_VBLANK, &vblank) == 0;
				bool have_hblank = get_ctrl_u32(cfd, V4L2_CID_HBLANK, &hblank) == 0;
				bool have_pixel_rate =
					get_ctrl_u32(cfd, V4L2_CID_PIXEL_RATE, &pixel_rate) == 0;
				bool have_exposure =
					get_ctrl_u32(cfd, V4L2_CID_EXPOSURE, &exposure) == 0;

				if (!reported_metadata) {
					reported_metadata = true;
					printf("metadata_height=%u metadata_stride=%u\n",
					       params.metadata_config.metadata_height,
					       params.metadata_config.metadata_stride);
					if (!params.metadata_config.metadata_height ||
					    !params.metadata_config.metadata_stride) {
						printf("metadata unavailable (embedded data disabled or unsupported)\n");
					}
				}
				printf("frame=%u frame_duration_us=%u",
				       frames,
				       params.metadata_config.frame_duration_us);
				if (have_vblank)
					printf(" vblank=%u", vblank);
				if (have_hblank)
					printf(" hblank=%u", hblank);
				if (have_pixel_rate)
					printf(" pixel_rate=%u", pixel_rate);
				if (have_exposure)
					printf(" exposure=%u", exposure);
				printf("\n");
				fflush(stdout);
			} else {
				perror("ATOMISP_IOC_G_ISP_PARM");
			}
			next_print += interval_ms;
		}

		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			break;
		}

		if (max_frames && frames >= max_frames)
			break;
	}

	xioctl(fd, VIDIOC_STREAMOFF, &fmt.type);

	for (i = 0; i < BUF_COUNT; ++i)
		munmap(bufs[i].start, bufs[i].length);

	if (sfd >= 0)
		close(sfd);
	close(fd);
	return 0;
}
