/*
 * FFMPEG DRM/KMS example application
 * Jorge Ramirez-Ortiz <jramirez@baylibre.com>
 *
 * Main file of the application
 *      Based on code from:
 *      	2001 Fabrice Bellard (FFMPEG/doc/examples/decode_video.c
 *      	2018 Stanimir Varbanov (v4l2-decode/src/drm.c)
 *
 * This code has been tested on Linaro's Dragonboard 820c
 *      kernel v4.14.15, venus decoder
 *      ffmpeg 4.0 + lrusacks ffmpeg/DRM support + review
 *      	https://github.com/ldts/ffmpeg  branch lrusak/v4l2-drmprime
 *
 * Copyright (c) 2018 Baylibre
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <sys/time.h>
#include <getopt.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>

#define ALIGN(x, a)		((x) + (a - 1)) & (~(a - 1))
#define DRM_ALIGN(val, align)	((val + (align - 1)) & ~(align - 1))

#define INBUF_SIZE 4096

struct drm_buffer {
	unsigned int fourcc;
	uint32_t handles[AV_DRM_MAX_PLANES];
	unsigned int fb_handle;
	uint32_t pitches[AV_DRM_MAX_PLANES];
	uint32_t offsets[AV_DRM_MAX_PLANES];
	uint64_t modifiers[AV_DRM_MAX_PLANES];
	uint32_t bo_handles[AV_DRM_MAX_PLANES];
};

struct drm_dev {
	int fd;
	uint32_t conn_id, enc_id, crtc_id, plane_id, crtc_idx;
	uint32_t width, height;
	uint32_t pitch, size, handle;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	drmModeAtomicReq *req;
	drmEventContext drm_event_ctx;
	uint32_t count_props;
	drmModePropertyPtr props[128];
	struct drm_dev *next;
	struct drm_buffer *bufs[2]; // double buffering
};

static struct drm_dev *pdev;
static unsigned int drm_format;
static int disable_plane_id = 0;

#define DBG_TAG "  ffmpeg-drm"

#define print(msg, ...)							\
	do {								\
			struct timeval tv;				\
			gettimeofday(&tv, NULL);			\
			fprintf(stderr, "%08u:%08u :" msg,		\
				(uint32_t)tv.tv_sec,			\
				(uint32_t)tv.tv_usec, ##__VA_ARGS__);	\
	} while (0)

#define err(msg, ...)  print("error: " msg "\n", ##__VA_ARGS__)
#define info(msg, ...) print(msg "\n", ##__VA_ARGS__)
#define dbg(msg, ...)  print(DBG_TAG ": " msg "\n", ##__VA_ARGS__)

uint32_t get_property_id(const char *name)
{
	uint32_t i;

	for (i = 0; i < pdev->count_props; ++i)
		if (!strcmp(pdev->props[i]->name, name))
			return pdev->props[i]->prop_id;

	return 0;
}

void set_plane_transparent(int plane_id)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	uint32_t fb;
	int ret;
	void *map;
	int handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = pdev->width;
	creq.height = pdev->height;
	creq.bpp = 32;
	ret = drmIoctl(pdev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		printf("DRM_IOCTL_MODE_CREATE_DUMB fail\n");
		return;
	}
	/* creq.pitch, creq.handle and creq.size are filled by this ioctl with
	 * the requested values and can be used now. */

	/* the framebuffer "fb" can now used for scanout with KMS */

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = creq.handle;
	ret = drmIoctl(pdev->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		printf("DRM_IOCTL_MODE_MAP_DUMB fail\n");
		return;
	}
	/* mreq.offset now contains the new offset that can be used with mmap() */

	/* perform actual memory mapping */
	map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, pdev->fd, mreq.offset);
	if (map == MAP_FAILED) {
		printf("mmap fail\n");
		return;
	}

	/* clear the framebuffer to 0 (= full transparency in ARGB8888) */
	memset(map, 0, creq.size);
	munmap(map, creq.size);

	printf("size = %llu ; pitch = %u\n", creq.size, creq.pitch);

	handles[0] = creq.handle;
	pitches[0] = creq.pitch;
	offsets[0] = 0;
	/* create framebuffer object for the dumb-buffer */
	ret = drmModeAddFB2(pdev->fd, pdev->width, pdev->height, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb, 0);
	if (ret) {
		printf("drmModeAddFB fail\n");
		return;
	}

	printf("Setting FB_ID %u; width %u; height %u; plane %u\n", fb, pdev->width, pdev->height, plane_id);

	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("FB_ID"), fb);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("SRC_X"), 0);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("SRC_Y"), 0);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("SRC_W"), pdev->width << 16);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("SRC_H"), pdev->height << 16);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("CRTC_X"), 0);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("CRTC_Y"), 0);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("CRTC_W"), pdev->width);
	ret = drmModeAtomicAddProperty(pdev->req, plane_id, get_property_id("CRTC_H"), pdev->height);
}

static void page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
			      unsigned int tv_usec, void *user_data)
{
	// Nothing to do.
}

int drm_get_plane_props(int fd, uint32_t id)
{
	uint32_t i;

	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
	if (!props) {
		err("drmModeObjectGetProperties failed\n");
		return -1;
	}
	printf("Found %u props\n", props->count_props);
	pdev->count_props = props->count_props;
	for (i = 0; i < props->count_props; i++) {
		pdev->props[i] = drmModeGetProperty(fd, props->props[i]);
		print("Added prop %u:%s\n", pdev->props[i]->prop_id, pdev->props[i]->name);
	}
	drmModeFreeObjectProperties(props);

	return 0;
}

int drm_add_property(const char *name, uint64_t value)
{
	int ret;
	uint32_t prop_id = get_property_id(name);

	if (!prop_id) {
		err("Couldn't find prop %s\n", name);
		return -1;
	}

	ret = drmModeAtomicAddProperty(pdev->req, pdev->plane_id, get_property_id(name), value);
	if (ret < 0) {
		err("drmModeAtomicAddProperty (%s:%lu) failed: %d\n", name, value, ret);
		return ret;
	}

	return 0;
}

int drm_dmabuf_set_plane(struct drm_buffer *buf, uint32_t width,
			 uint32_t height, int fullscreen, AVRational sar)
{
	int ret;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t crtc_x = 0;
	uint32_t crtc_y = 0;
	double ratio_w;
	double ratio_h;

	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(pdev->fd, &fds);

	crtc_w = (width * sar.num) / sar.den;
	crtc_h = height;
	ratio_w = (double)pdev->width / crtc_w;
	ratio_h = (double)pdev->height / crtc_h;

	if (ratio_w > ratio_h) {
		crtc_w *= ratio_h;
		crtc_h *= ratio_h;
		crtc_x = (pdev->width - crtc_w) / 2;
	} else {
		crtc_w *= ratio_w;
		crtc_h *= ratio_w;
		crtc_y = (pdev->height - crtc_h) / 2;
	}

	printf("crtc_x: %u; crtc_y:%u; crtc_w: %u; crtc_h: %u\n", crtc_x, crtc_y, crtc_w, crtc_h);

	drm_add_property("FB_ID", buf->fb_handle);
	drm_add_property("CRTC_ID", pdev->crtc_id);
	drm_add_property("SRC_X", 0);
	drm_add_property("SRC_Y", 0);
	drm_add_property("SRC_W", width << 16);
	drm_add_property("SRC_H", height << 16);
	drm_add_property("CRTC_X", crtc_x);
	drm_add_property("CRTC_Y", crtc_y);
	drm_add_property("CRTC_W", crtc_w);
	drm_add_property("CRTC_H", crtc_h);

	if (disable_plane_id) {
		set_plane_transparent(disable_plane_id);
		disable_plane_id = 0;
	}

	ret = drmModeAtomicCommit(pdev->fd, pdev->req, DRM_MODE_PAGE_FLIP_EVENT  | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret) {
		err("drmModeAtomicCommit failed: %s\n", strerror(errno));
		return ret;
	}

	do {
		ret = select(pdev->fd + 1, &fds, NULL, NULL, NULL);
	} while (ret == -1 && errno == EINTR);

	if (FD_ISSET(pdev->fd, &fds))
		drmHandleEvent(pdev->fd, &pdev->drm_event_ctx);

	drmModeAtomicFree(pdev->req);
	pdev->req = drmModeAtomicAlloc();

	return 0;
}

static int drm_dmabuf_addfb(struct drm_buffer *buf, uint32_t width, uint32_t height)
{
	int ret;

	width = ALIGN(width, 8);

	ret = drmModeAddFB2WithModifiers(pdev->fd, width, height, buf->fourcc, buf->handles,
			    buf->pitches, buf->offsets, buf->modifiers, &buf->fb_handle, 0);
	if (ret) {
		err("drmModeAddFB2 failed: %d (%s). width=%u, height=%u\n", ret, strerror(errno), width, height);
		return ret;
	}

	return 0;
}

static int find_plane(int fd, unsigned int fourcc, uint32_t *plane_id,
			uint32_t crtc_id, uint32_t crtc_idx)
{
	drmModePlaneResPtr planes;
	drmModePlanePtr plane;
	unsigned int i;
	unsigned int j;
	int ret = 0;
	unsigned int format = fourcc;

	planes = drmModeGetPlaneResources(fd);
	if (!planes) {
		err("drmModeGetPlaneResources failed\n");
		return -1;
	}

	info("drm: found planes %u", planes->count_planes);

	for (i = 0; i < planes->count_planes; ++i) {
		plane = drmModeGetPlane(fd, planes->planes[i]);
		if (!plane) {
			err("drmModeGetPlane failed: %s\n", strerror(errno));
			break;
		}

		if (!(plane->possible_crtcs & (1 << crtc_idx))) {
			drmModeFreePlane(plane);
			continue;
		}

		for (j = 0; j < plane->count_formats; ++j) {
			if (plane->formats[j] == format)
				break;
		}

		if (j == plane->count_formats) {
			drmModeFreePlane(plane);
			continue;
		}

		*plane_id = plane->plane_id;
		drmModeFreePlane(plane);
		break;
	}

	if (i == planes->count_planes)
		ret = -1;

	drmModeFreePlaneResources(planes);

	return ret;
}

static struct drm_dev *drm_find_dev(int fd)
{
	int i;
	struct drm_dev *dev = NULL, *dev_head = NULL;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	drmModeCrtc *crtc = NULL;

	if ((res = drmModeGetResources(fd)) == NULL) {
		err("drmModeGetResources() failed");
		return NULL;
	}

	if (res->count_crtcs <= 0) {
		err("no Crtcs");
		goto free_res;
	}

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn) {
			if (conn->connection == DRM_MODE_CONNECTED) {
				dbg("drm: connector: connected");
			} else if (conn->connection == DRM_MODE_DISCONNECTED) {
				dbg("drm: connector: disconnected");
			} else if (conn->connection == DRM_MODE_UNKNOWNCONNECTION) {
				dbg("drm: connector: unknownconnection");
			} else {
				dbg("drm: connector: unknown");
			}
		}

		if (conn != NULL && conn->connection == DRM_MODE_CONNECTED
		    && conn->count_modes > 0) {
			dev = (struct drm_dev *) malloc(sizeof(struct drm_dev));
			memset(dev, 0, sizeof(struct drm_dev));

			dev->conn_id = conn->connector_id;
			dev->enc_id = conn->encoder_id;
			dev->next = NULL;

			memcpy(&dev->mode, &conn->modes[0], sizeof(drmModeModeInfo));
			dev->width = conn->modes[0].hdisplay;
			dev->height = conn->modes[0].vdisplay;

			if (conn->encoder_id) {
				enc = drmModeGetEncoder(fd, conn->encoder_id);
				if (!enc) {
					err("drmModeGetEncoder() faild");
					goto free_res;
				}
				if (enc->crtc_id) {
					crtc = drmModeGetCrtc(fd, enc->crtc_id);
					if (crtc) {
						dev->crtc_id = enc->crtc_id;
						drmModeFreeCrtc(crtc);
					}
				}
				drmModeFreeEncoder(enc);
			}

			dev->saved_crtc = NULL;

			/* create dev list */
			dev->next = dev_head;
			dev_head = dev;
		}
		drmModeFreeConnector(conn);
	}

	dev->crtc_idx = -1;

	for (i = 0; i < res->count_crtcs; ++i) {
		if (dev->crtc_id == res->crtcs[i]) {
			dev->crtc_idx = i;
			break;
		}
	}

	if (dev->crtc_idx == -1)
		err( "drm: CRTC not found\n");

free_res:
	drmModeFreeResources(res);

	return dev_head;
}

static int drm_open(const char *path)
{
	int fd, flags;
	uint64_t has_dumb;
	int ret;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		err("cannot open \"%s\"\n", path);
		return -1;
	}

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0 ||
	     fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		err("fcntl FD_CLOEXEC failed\n");
		goto err;
	}

	/* check capability */
	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
	if (ret < 0 || has_dumb == 0) {
		err("drmGetCap DRM_CAP_DUMB_BUFFER failed or doesn't have dumb "
		    "buffer\n");
		goto err;
	}

	return fd;
err:
	close(fd);
	return -1;
}

static int drm_init(unsigned int fourcc, const char *device)
{
	struct drm_dev *dev_head, *dev;
	drmModeAtomicReq *req ;
	int fd;
	int ret;

	fd = drm_open(device);
	if (fd < 0)
		return -1;

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		err("No atomic modesetting support: %s", strerror(errno));
		goto err;
	}

	req = drmModeAtomicAlloc();

	dev_head = drm_find_dev(fd);
	if (dev_head == NULL) {
		err("available drm devices not found\n");
		goto err;
	}

	dbg("available connector(s)");

	for (dev = dev_head; dev != NULL; dev = dev->next) {
		dbg("connector id:%d", dev->conn_id);
		dbg("\tencoder id:%d crtc id:%d", dev->enc_id,
		    dev->crtc_id);
		dbg("\twidth:%d height:%d", dev->width, dev->height);
	}

	/* FIXME: use first drm_dev */
	dev = dev_head;
	dev->fd = fd;
	dev->req = req;
	pdev = dev;

	ret = find_plane(fd, fourcc, &dev->plane_id, dev->crtc_id, dev->crtc_idx);
	if (ret) {
		err("Cannot find plane\n");
		goto err;
	}

	ret = drm_get_plane_props(fd, dev->plane_id);
	pdev->drm_event_ctx.version = DRM_EVENT_CONTEXT_VERSION;
	pdev->drm_event_ctx.page_flip_handler = page_flip_handler;

	info("drm: Found %c%c%c%c plane_id: %u",
		(fourcc>>0)&0xff, (fourcc>>8)&0xff, (fourcc>>16)&0xff, (fourcc>>24)&0xff,
		dev->plane_id);

	return 0;

err:
	close(fd);
	pdev = NULL;
	return -1;
}

static void drm_remove_fb(struct drm_buffer *drm_buf)
{
	struct drm_gem_close gem_close;
	int i;

	if (drmModeRmFB(pdev->fd, drm_buf->fb_handle))
		err("cant remove fb %d\n", drm_buf->fb_handle);

	for (i = 0; i < AV_DRM_MAX_PLANES; i++) {
		if (drm_buf->bo_handles[i]) {
			memset(&gem_close, 0, sizeof gem_close);
			gem_close.handle = drm_buf->bo_handles[i];
			if (drmIoctl(pdev->fd, DRM_IOCTL_GEM_CLOSE, &gem_close) < 0)
				err("cant close gem: %s\n", strerror(errno));
		}
	}
	free(drm_buf);
}

static int display(struct drm_buffer *drm_buf, int width, int height, AVRational sar)
{
        int ret;

	ret = drm_dmabuf_addfb(drm_buf, width, height);
	if (ret) {
		err("cannot add framebuffer %d\n", ret);
		return -EFAULT;
	}

	drm_dmabuf_set_plane(drm_buf, width, height, 1, sar);

	if (pdev->bufs[1])
		drm_remove_fb(pdev->bufs[1]);

	pdev->bufs[1] = pdev->bufs[0];
	pdev->bufs[0] = drm_buf;

	return 0;
}

static void decode_and_display(AVCodecContext *dec_ctx, AVFrame *frame,
			AVPacket *pkt, const char *device)
{
	AVDRMFrameDescriptor *desc;
	AVDRMLayerDescriptor* layer;
	struct drm_buffer *drm_buf;
	int ret;

	ret = avcodec_send_packet(dec_ctx, pkt);
	if (ret < 0) {
		err("Error sending a packet for decoding\n");
		exit(1);
	}

	while (ret >= 0) {
		ret = avcodec_receive_frame(dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return;
		else if (ret < 0) {
			err("Error during decoding\n");
			exit(1);
		}

		desc = (AVDRMFrameDescriptor *) frame->data[0];
		layer = &desc->layers[0];

                if (!pdev) {
                    /* initialize DRM with the format returned in the frame */
                    ret = drm_init(layer->format, device);
                    if (ret) {
                        err("Error initializing drm\n");
                        exit(1);
                    }

                    /* remember the format */
                    drm_format = layer->format;
                }

		drm_buf = calloc(1, sizeof(*drm_buf));
		// convert Prime FD to GEM handle
		for (int i = 0; i < desc->nb_objects; i++) {
			ret = drmPrimeFDToHandle(pdev->fd, desc->objects[i].fd, &drm_buf->bo_handles[i]);
			if (ret < 0) {
				err("Failed FDToHandle\n");
				return;
			}
		}

		for (int i = 0; i < layer->nb_planes && i < AV_DRM_MAX_PLANES; i++) {
			int object = layer->planes[i].object_index;
			uint32_t handle = drm_buf->bo_handles[object];
			if (handle && layer->planes[i].pitch) {
				drm_buf->handles[i] = handle;
				drm_buf->pitches[i] = layer->planes[i].pitch;
				drm_buf->offsets[i] = layer->planes[i].offset;
				drm_buf->modifiers[i] = desc->objects[object].format_modifier;
			}
		}

		/* pass the format in the buffer */
		drm_buf->fourcc = drm_format;
		ret = display(drm_buf, frame->width, frame->height, frame->sample_aspect_ratio);
		if (ret < 0)
			return;
    }
}

static const struct option options[] = {
	{
#define help_opt	0
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
	},
	{
#define video_opt	1
		.name = "video",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define codec_opt	2
		.name = "codec",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define height_opt	3
		.name = "height",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define width_opt	4
		.name = "width",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define device_opt	5
		.name = "device",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define disable_plane_opt	6
		.name = "disable-plane",
		.has_arg = 1,
		.flag = NULL,
	},
	{
		.name = NULL,
	},
};

static void usage(void)
{
	fprintf(stderr, "usage: ffmpeg-drm <options>, with:\n");
	fprintf(stderr, "--help            display this menu\n");
	fprintf(stderr, "--video=<name>    video to display\n");
	fprintf(stderr, "--codec=<name>    ffmpeg codec: ie h264_v4l2m2m\n");
	fprintf(stderr, "--width=<value>   frame width\n");
	fprintf(stderr, "--height=<value>  frame height\n");
	fprintf(stderr, "--device=<value>  dri device to use\n");
	fprintf(stderr, "\n");
}

int main(int argc, char *argv[])
{
	uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
	AVCodecParserContext *parser;
	AVCodecContext *c = NULL;
	const AVCodec *codec;
	AVFrame *frame;
	AVPacket *pkt;
	size_t data_size;
	uint8_t *data;
	FILE *f;
	int ret;
	int lindex, opt;
	unsigned int frame_width = 0, frame_height = 0;
	char *codec_name = NULL, *video_name = NULL;
	char *device_name = "/dev/dri/card0";
	AVDictionary *opts = NULL;

	for (;;) {
		lindex = -1;

		opt = getopt_long_only(argc, argv, "", options, &lindex);
		if (opt == EOF)
			break;

		switch (lindex) {
		case help_opt:
			usage();
			exit(0);
		case video_opt:
			video_name = optarg;
			break;
		case codec_opt:
			codec_name = optarg;
			break;
		case width_opt:
			frame_width = atoi(optarg);
			break;
		case height_opt:
			frame_height = atoi(optarg);
			break;
		case device_opt:
			device_name = optarg;
			break;
		case disable_plane_opt:
			disable_plane_id = atoi(optarg);
			break;
		default:
			usage();
			exit(1);
		}
	}

	if (!frame_width || !frame_height || !codec_name || !video_name) {
		usage();
		exit(0);
	}

	pkt = av_packet_alloc();
	if (!pkt) {
		err("Error allocating packet\n");
		exit(1);
	}

	/* set end of buffer to 0 (this ensures that no overreading happens for
	   damaged MPEG streams) */
	memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

	/* find the video decoder: ie: h264_v4l2m2m */
	codec = avcodec_find_decoder_by_name(codec_name);
	if (!codec) {
		err("Codec not found\n");
		exit(1);
	}

	parser = av_parser_init(codec->id);
	if (!parser) {
		err("parser not found\n");
		exit(1);
	}

	c = avcodec_alloc_context3(codec);
	if (!c) {
		err("Could not allocate video codec context\n");
		exit(1);
	}

	/* For some codecs, such as msmpeg4 and mpeg4, width and height
	   MUST be initialized before opening the ffmpeg codec (ie, before
	   calling avcodec_open2) because this information is not available in
	   the bitstream). */
	c->pix_fmt = AV_PIX_FMT_DRM_PRIME;   /* request a DRM frame */
	c->coded_height = frame_height;
	c->coded_width = frame_width;

	av_dict_set(&opts, "num_capture_buffers", "16", 0);

	/* open it */
	if (avcodec_open2(c, codec, &opts) < 0) {
		err("Could not open codec\n");
		exit(1);
	}

	f = fopen(video_name, "rb");
	if (!f) {
		err("Could not open %s\n", video_name);
		exit(1);
	}

	frame = av_frame_alloc();
	if (!frame) {
		err("Could not allocate video frame\n");
		exit(1);
	}

	while (!feof(f)) {
		/* read raw data from the input file */
		data_size = fread(inbuf, 1, INBUF_SIZE, f);
		if (!data_size)
			break;

		/* use the parser to split the data into frames */
		data = inbuf;
		while (data_size > 0) {
			ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
				data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			if (ret < 0) {
				err("Error while parsing\n");
				exit(1);
			}

			data += ret;
			data_size -= ret;

			if (pkt->size)
				decode_and_display(c, frame, pkt, device_name);
		}
	}
	fclose(f);

	decode_and_display(c, frame, NULL, device_name);
	if (pdev->bufs[0])
		drm_remove_fb(pdev->bufs[0]);
	if (pdev->bufs[1])
		drm_remove_fb(pdev->bufs[1]);

	av_parser_close(parser);
	avcodec_free_context(&c);
	av_frame_free(&frame);
	av_packet_free(&pkt);

	return 0;
}
