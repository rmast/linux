#include <fcntl.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [/dev/videoX] [interval_ms]\n", prog);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0";
	unsigned int interval_ms = 200;
	struct atomisp_parm p;
	int fd;

	if (argc > 1)
		dev = argv[1];
	if (argc > 2)
		interval_ms = (unsigned int)atoi(argv[2]);
	if (argc > 3) {
		usage(argv[0]);
		return 1;
	}

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	memset(&p, 0, sizeof(p));

	for (;;) {
		if (ioctl(fd, ATOMISP_IOC_G_ISP_PARM, &p) == 0) {
			printf("frame_duration_us=%u\n",
			       p.metadata_config.frame_duration_us);
			fflush(stdout);
		} else {
			perror("ioctl");
			break;
		}
		usleep(interval_ms * 1000U);
	}

	close(fd);
	return 0;
}
