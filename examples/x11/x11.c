#include "neatvnc.h"
#include "common.h"
#include "display.h"
#include "fb.h"

#include "sys/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <tgmath.h>
#include <aml.h>
#include <signal.h>
#include <assert.h>
#include <pixman.h>
#include <sys/param.h>

#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>

#include "kmsgrab.h"


static void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
							 enum nvnc_button_mask buttons)
{
	if (!(buttons & NVNC_BUTTON_LEFT))
		return;

	struct nvnc* server = nvnc_client_get_server(client);
	assert(server);
}

static int find_control_node(char* node, size_t maxlen)
{
	int r = -ENOENT;
	drmDevice* devices[64];

	int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < n; ++i) {
		drmDevice* dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_PRIMARY)))
			continue;

		strncpy(node, dev->nodes[DRM_NODE_PRIMARY], maxlen);
		node[maxlen - 1] = '\0';
		r = 0;
		break;
	}

	strncpy(node, "/dev/dri/card0", maxlen);
	node[maxlen - 1] = '\0';

	drmFreeDevices(devices, n);
	return r;
}

static struct nvnc_fb* nvnc_fb_from_avframe(AVFrame* frame, uint32_t format)
{
	struct nvnc_fb* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	fb->type = NVNC_FB_AVFRAME;
	fb->ref = 1;
	fb->is_external = true;
	fb->width = frame->width;
	fb->height = frame->height;
	fb->fourcc_format = format;
	fb->frame = frame;
	fb->transform = NVNC_TRANSFORM_NORMAL;
	fb->pts = NVNC_NO_PTS;

	return fb;
}

static void on_sigint()
{
	aml_exit(aml_get_default());
}

static void on_tick(void* obj)
{
	struct nvnc* server = aml_get_userdata(obj);

	static KMSGrabContext* kms = NULL;
	if (!kms) {
		kms = calloc(1, sizeof(*kms));
		kms->format = AV_PIX_FMT_NONE;
		kms->drm_format_modifier = DRM_FORMAT_MOD_INVALID;

		int r = find_control_node(kms->device_path, sizeof(kms->device_path));
		if (r < 0) {
			goto free_kms;
		}
		r = kmsgrab_read_header(kms);
		if (r < 0) {
			goto free_kms;
		}
	}

	AVFrame* frame = av_frame_alloc();
	int r = kmsgrab_read_frame(kms, frame);
	if (r < 0) {
		// assume EIO and reopen
		goto close_kms;
	}

	struct nvnc_fb* fb = nvnc_fb_from_avframe(frame, kms->drm_format);
	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, fb->width, fb->height);
	nvnc_display_feed_buffer(server->display, fb, &damage);
	pixman_region_fini(&damage);
	nvnc_fb_unref(fb);

	return;

close_kms:
	av_frame_free(&frame);
	kmsgrab_read_close(kms);
	kms = NULL;
free_kms:
	free(kms);
	kms = NULL;
}

int main()
{
	struct aml* aml = aml_new();
	aml_set_default(aml);

	Display* dpy = XOpenDisplay(NULL);
	if (!dpy) {
		exit(1);
	}

	int screen = DefaultScreen(dpy);
	Window root = RootWindow(dpy, screen);

	int major, minor;
	XCompositeQueryVersion(dpy, &major, &minor);
	if (major > 0 || minor >= 2) {
		printf("XComposite available: %d.%d\n", major, minor);
	}

	struct nvnc* server = nvnc_open("0.0.0.0", 5900);
	assert(server);

	struct nvnc_display* display = nvnc_display_new(0, 0);
	assert(display);

	nvnc_add_display(server, display);

	nvnc_set_name(server, "Draw");
	nvnc_set_pointer_fn(server, on_pointer_event);

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	aml_start(aml_get_default(), sig);
	aml_unref(sig);

	struct aml_ticker* tick = aml_ticker_new(1000000 / 60, on_tick, server, NULL);
	aml_start(aml_get_default(), tick);
	aml_unref(tick);

	aml_run(aml);

	nvnc_close(server);
	nvnc_display_unref(display);
	aml_unref(aml);
}
