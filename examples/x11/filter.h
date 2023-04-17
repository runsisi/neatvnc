#pragma once

#include <stdint.h>
#include <libavutil/pixfmt.h>

typedef struct AVBufferRef AVBufferRef;
typedef struct AVFilterGraph AVFilterGraph;
typedef struct AVFilterContext AVFilterContext;

struct nvnc_fb;

struct pixel_filter {
	int width;
	int height;
	enum AVPixelFormat av_pixel_format;

	/* type: AVHWDeviceContext */
	AVBufferRef* hw_device_ctx;

	/* type: AVHWFramesContext */
	AVBufferRef* hw_frames_ctx;

	AVFilterGraph* filter_graph;
	AVFilterContext* filter_in;
	AVFilterContext* filter_out;
};

int fb_map(struct nvnc_fb* fb, void* context);
void fb_unmap(struct nvnc_fb* fb);

struct pixel_filter* pixel_filter_create(int width, int height,
		int format, AVBufferRef* hw_device_ref, AVBufferRef* hw_frames_ref);
void pixel_filter_destroy(struct pixel_filter* self);
