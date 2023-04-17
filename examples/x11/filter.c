#include <assert.h>

#include <libdrm/drm_fourcc.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>

#include "fb.h"
#include "filter.h"


static enum AVPixelFormat drm_to_av_pixel_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return AV_PIX_FMT_BGR0;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return AV_PIX_FMT_RGB0;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		return AV_PIX_FMT_0BGR;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		return AV_PIX_FMT_0RGB;
	}

	return AV_PIX_FMT_NONE;
}

static int pixel_filter__init_buffersrc(struct pixel_filter* self)
{
	int rc;

	/* Placeholder values are used to pacify input checking and the real
	 * values are set below.
	 */
	rc = avfilter_graph_create_filter(&self->filter_in,
		avfilter_get_by_name("buffer"), "in",
		"width=1:height=1:pix_fmt=drm_prime:time_base=1/1", NULL,
		self->filter_graph);
	if (rc != 0)
		return -1;

	AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
	if (!params)
		return -1;

	params->format = AV_PIX_FMT_DRM_PRIME;
	params->width = self->width;
	params->height = self->height;
	params->hw_frames_ctx = self->hw_frames_ctx;

	rc = av_buffersrc_parameters_set(self->filter_in, params);
	assert(rc == 0);

	av_free(params);
	return 0;
}

static int pixel_filter__init_filters(struct pixel_filter* self)
{
	int rc;

	self->filter_graph = avfilter_graph_alloc();
	if (!self->filter_graph)
		return -1;

	rc = pixel_filter__init_buffersrc(self);
	if (rc != 0)
		goto failure;

	rc = avfilter_graph_create_filter(&self->filter_out,
		avfilter_get_by_name("buffersink"), "out", NULL,
		NULL, self->filter_graph);
	if (rc != 0)
		goto failure;

	AVFilterInOut* inputs = avfilter_inout_alloc();
	if (!inputs)
		goto failure;

	inputs->name = av_strdup("in");
	inputs->filter_ctx = self->filter_in;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	AVFilterInOut* outputs = avfilter_inout_alloc();
	if (!outputs) {
		avfilter_inout_free(&inputs);
		goto failure;
	}

	outputs->name = av_strdup("out");
	outputs->filter_ctx = self->filter_out;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	char filter[128] = "";
	const char* fmt = "hwmap=mode=direct:derive_device=vaapi"
					  ",hwdownload,format=pix_fmts=%s";
	snprintf(filter, sizeof(filter), fmt,
		av_get_pix_fmt_name(self->av_pixel_format));

	rc = avfilter_graph_parse(self->filter_graph,
		filter,
		outputs, inputs, NULL);
	if (rc != 0)
		goto failure;

	assert(self->hw_device_ctx);

	for (unsigned int i = 0; i < self->filter_graph->nb_filters; ++i) {
		self->filter_graph->filters[i]->hw_device_ctx =
			av_buffer_ref(self->hw_device_ctx);
	}

	rc = avfilter_graph_config(self->filter_graph, NULL);
	if (rc != 0)
		goto failure;

	return 0;

failure:
	avfilter_graph_free(&self->filter_graph);
	return -1;
}

struct pixel_filter* pixel_filter_create(int width, int height,
		int drm_format, AVBufferRef* hw_device_ref, AVBufferRef* hw_frames_ref)
{
	struct pixel_filter* self = calloc(1, sizeof(*self));
	if (!self) {
		return NULL;
	}

	self->hw_device_ctx = av_buffer_ref(hw_device_ref);
	self->hw_frames_ctx = av_buffer_ref(hw_frames_ref);

	self->width = width;
	self->height = height;
	self->av_pixel_format = drm_to_av_pixel_format(drm_format);
	if (self->av_pixel_format == AV_PIX_FMT_NONE) {
		goto pix_fmt_failure;
	}

	if (pixel_filter__init_filters(self) < 0) {
		goto pix_fmt_failure;
	}

	return self;

pix_fmt_failure:
	av_buffer_unref(&self->hw_frames_ctx);
	av_buffer_unref(&self->hw_device_ctx);
	free(self);
	return NULL;
}

void pixel_filter_destroy(struct pixel_filter* self)
{
	av_buffer_unref(&self->hw_frames_ctx);
	av_buffer_unref(&self->hw_device_ctx);
	avfilter_graph_free(&self->filter_graph);
	free(self);
}

int fb_map(struct nvnc_fb* fb, void* context)
{
	assert(fb->type == NVNC_FB_AVFRAME);

	if (fb->addr) {
		return 0;
	}

	struct pixel_filter* self = context;
	AVFrame* frame_in = fb->frame;

	int r = av_buffersrc_add_frame_flags(self->filter_in, frame_in,
		AV_BUFFERSRC_FLAG_KEEP_REF);
	if (r < 0) {
		return r;
	}

	AVFrame* filtered_frame = av_frame_alloc();
	if (!filtered_frame) {
		return AVERROR(ENOMEM);
	}

	r = av_buffersink_get_frame(self->filter_out, filtered_frame);
	if (r < 0) {
		goto free_frame;
	}

	int size = av_image_get_buffer_size(filtered_frame->format, filtered_frame->width,
		filtered_frame->height, 1);
	fb->addr = av_malloc(size);
	r = av_image_copy_to_buffer(fb->addr, size,
		(const uint8_t * const *)filtered_frame->data,
		(const int *)filtered_frame->linesize, filtered_frame->format,
		filtered_frame->width, filtered_frame->height, 1);
	if (r < 0) {
		goto free_data;
	}

	// single planar format
	fb->stride = filtered_frame->linesize[0] / nvnc_fb_get_pixel_size(fb);
	av_frame_free(&filtered_frame);
	return 0;

free_data:
	av_freep(&fb->addr);
	fb->addr = NULL;
free_frame:
	av_frame_free(&filtered_frame);
	return r;
}

void fb_unmap(struct nvnc_fb* fb)
{
	assert(fb->type == NVNC_FB_AVFRAME);

	if (!fb->addr) {
		return;
	}

	av_freep(&fb->addr);
	fb->addr = NULL;
	fb->stride = 0;
}
