//
// Created by runsisi on 23-4-4.
//

#pragma once

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

typedef struct KMSGrabContext {
  AVBufferRef *device_ref;
  AVHWDeviceContext *device;
  AVDRMDeviceContext *hwctx;
  int fb2_available;

  AVBufferRef *frames_ref;
  AVHWFramesContext *frames;

  uint32_t plane_id;
  uint32_t drm_format;
  int width;
  int height;

  char device_path[64];
  enum AVPixelFormat format;
  int64_t drm_format_modifier;
  int64_t source_plane;
  int64_t source_crtc;
  AVRational framerate;
} KMSGrabContext;

int kmsgrab_read_frame(KMSGrabContext *ctx, AVFrame *frame);
int kmsgrab_read_header(KMSGrabContext *ctx);
void kmsgrab_read_close(KMSGrabContext *ctx);
