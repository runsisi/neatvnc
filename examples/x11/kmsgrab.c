//
// Created by runsisi on 23-4-4.
//

#include <fcntl.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libavutil/pixdesc.h>

#include "kmsgrab.h"


static void kmsgrab_free_desc(void */*opaque*/, uint8_t *data) {
  AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
  int i;

  for (i = 0; i < desc->nb_objects; i++)
    close(desc->objects[i].fd);

  av_free(desc);
}

static void kmsgrab_free_frame(void */*opaque*/, uint8_t *data) {
  AVFrame *frame = (AVFrame *)data;

  av_frame_free(&frame);
}

static int kmsgrab_get_fb(KMSGrabContext *ctx,
    drmModePlane *plane,
    AVDRMFrameDescriptor *desc) {
  // KMSGrabContext *ctx = avctx->priv_data;
  drmModeFB *fb = NULL;
  int err, fd;

  fb = drmModeGetFB(ctx->hwctx->fd, plane->fb_id);
  if (!fb) {
    err = errno;
    av_log(NULL, AV_LOG_ERROR, "Failed to get framebuffer "
                               "%u: %s.\n", plane->fb_id, strerror(err));
    err = AVERROR(err);
    goto fail;
  }
  if (fb->width != ctx->width || fb->height != ctx->height) {
    av_log(NULL, AV_LOG_ERROR, "Plane %u framebuffer "
                               "dimensions changed: now %ux%u.\n",
        ctx->plane_id, fb->width, fb->height);
    err = AVERROR(EIO);
    goto fail;
  }
  if (!fb->handle) {
    av_log(NULL, AV_LOG_ERROR, "No handle set on framebuffer.\n");
    err = AVERROR(EIO);
    goto fail;
  }

  err = drmPrimeHandleToFD(ctx->hwctx->fd, fb->handle, O_RDONLY, &fd);
  if (err < 0) {
    err = errno;
    av_log(NULL, AV_LOG_ERROR, "Failed to get PRIME fd from "
                               "framebuffer handle: %s.\n", strerror(err));
    err = AVERROR(err);
    goto fail;
  }

  *desc = (AVDRMFrameDescriptor){
      .nb_objects = 1,
      .objects[0] = {
          .fd               = fd,
          .size             = fb->height * fb->pitch,
          .format_modifier  = (uint64_t)ctx->drm_format_modifier,
      },
      .nb_layers = 1,
      .layers[0] = {
          .format           = ctx->drm_format,
          .nb_planes        = 1,
          .planes[0] = {
              .object_index = 0,
              .offset       = 0,
              .pitch        = fb->pitch,
          },
      },
  };

  err = 0;
fail:
  drmModeFreeFB(fb);
  return err;
}

#if HAVE_LIBDRM_GETFB2

static int kmsgrab_get_fb2(KMSGrabContext *ctx,
    drmModePlane *plane,
    AVDRMFrameDescriptor *desc) {
  // KMSGrabContext *ctx = avctx->priv_data;
  drmModeFB2 *fb;
  int err, i, nb_objects;
  uint64_t modifier = ctx->drm_format_modifier;

  fb = drmModeGetFB2(ctx->hwctx->fd, plane->fb_id);
  if (!fb) {
    err = errno;
    av_log(NULL, AV_LOG_ERROR, "Failed to get framebuffer "
                               "%u: %s.\n", plane->fb_id, strerror(err));
    return AVERROR(err);
  }
  if (fb->pixel_format != ctx->drm_format) {
    av_log(NULL, AV_LOG_ERROR, "Plane %u framebuffer "
                               "format changed: now %u.\n",
        ctx->plane_id, fb->pixel_format);
    err = AVERROR(EIO);
    goto fail;
  }
  if (fb->width != ctx->width || fb->height != ctx->height) {
    av_log(NULL, AV_LOG_ERROR, "Plane %u framebuffer "
                               "dimensions changed: now %ux%u.\n",
        ctx->plane_id, fb->width, fb->height);
    err = AVERROR(EIO);
    goto fail;
  }
  if (!fb->handles[0]) {
    av_log(NULL, AV_LOG_ERROR, "No handle set on framebuffer.\n");
    err = AVERROR(EIO);
    goto fail;
  }

  if (fb->flags & DRM_MODE_FB_MODIFIERS)
    modifier = fb->modifier;

  *desc = (AVDRMFrameDescriptor){
      .nb_layers = 1,
      .layers[0] = {
          .format = ctx->drm_format,
      },
  };

  nb_objects = 0;
  for (i = 0; i < 4 && fb->handles[i]; i++) {
    size_t size;
    int dup = 0, j, obj;

    size = fb->offsets[i] + fb->height * fb->pitches[i];

    for (j = 0; j < i; j++) {
      if (fb->handles[i] == fb->handles[j]) {
        dup = 1;
        break;
      }
    }
    if (dup) {
      obj = desc->layers[0].planes[j].object_index;

      if (desc->objects[j].size < size)
        desc->objects[j].size = size;

      desc->layers[0].planes[i] = (AVDRMPlaneDescriptor){
          .object_index = obj,
          .offset       = fb->offsets[i],
          .pitch        = fb->pitches[i],
      };

    } else {
      int fd;
      err = drmPrimeHandleToFD(ctx->hwctx->fd, fb->handles[i],
          O_RDONLY, &fd);
      if (err < 0) {
        err = errno;
        av_log(NULL, AV_LOG_ERROR, "Failed to get PRIME fd from "
                                   "framebuffer handle: %s.\n", strerror(err));
        err = AVERROR(err);
        goto fail;
      }

      obj = nb_objects++;
      desc->objects[obj] = (AVDRMObjectDescriptor){
          .fd              = fd,
          .size            = size,
          .format_modifier = modifier,
      };
      desc->layers[0].planes[i] = (AVDRMPlaneDescriptor){
          .object_index = obj,
          .offset       = fb->offsets[i],
          .pitch        = fb->pitches[i],
      };
    }
  }
  desc->nb_objects = nb_objects;
  desc->layers[0].nb_planes = i;

  err = 0;
fail:
  drmModeFreeFB2(fb);
  return err;
}

#endif

int kmsgrab_read_frame(KMSGrabContext *ctx, AVFrame *frame) {
  // KMSGrabContext *ctx = avctx->priv_data;
  drmModePlane *plane = NULL;
  AVDRMFrameDescriptor *desc = NULL;
  int err;

  plane = drmModeGetPlane(ctx->hwctx->fd, ctx->plane_id);
  if (!plane) {
    err = errno;
    av_log(NULL, AV_LOG_ERROR, "Failed to get plane "
                               "%u: %s.\n", ctx->plane_id, strerror(err));
    err = AVERROR(err);
    goto fail;
  }
  if (!plane->fb_id) {
    av_log(NULL, AV_LOG_ERROR, "Plane %u no longer has "
                               "an associated framebuffer.\n", ctx->plane_id);
    err = AVERROR(EIO);
    goto fail;
  }

  desc = (AVDRMFrameDescriptor *)av_mallocz(sizeof(*desc));
  if (!desc) {
    err = AVERROR(ENOMEM);
    goto fail;
  }

#if HAVE_LIBDRM_GETFB2
  if (ctx->fb2_available)
    err = kmsgrab_get_fb2(ctx, plane, desc);
  else
#endif
    err = kmsgrab_get_fb(ctx, plane, desc);
  if (err < 0)
    goto fail;

  frame->hw_frames_ctx = av_buffer_ref(ctx->frames_ref);
  if (!frame->hw_frames_ctx) {
    err = AVERROR(ENOMEM);
    goto fail;
  }

  frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc),
      &kmsgrab_free_desc, NULL, 0);
  if (!frame->buf[0]) {
    err = AVERROR(ENOMEM);
    goto fail;
  }

  frame->data[0] = (uint8_t *)desc;
  frame->format = AV_PIX_FMT_DRM_PRIME;
  frame->width = ctx->width;
  frame->height = ctx->height;

  drmModeFreePlane(plane);

  return 0;

fail:
  drmModeFreePlane(plane);
  av_freep(&desc);
  return err;
}

static const struct {
  enum AVPixelFormat pixfmt;
  uint32_t drm_format;
} kmsgrab_formats[] = {
    // Monochrome.
#ifdef DRM_FORMAT_R8
    {AV_PIX_FMT_GRAY8, DRM_FORMAT_R8},
#endif
#ifdef DRM_FORMAT_R16
    {AV_PIX_FMT_GRAY16LE, DRM_FORMAT_R16},
    {AV_PIX_FMT_GRAY16BE, DRM_FORMAT_R16 | DRM_FORMAT_BIG_ENDIAN},
#endif
    // <8-bit RGB.
    {AV_PIX_FMT_BGR8, DRM_FORMAT_BGR233},
    {AV_PIX_FMT_RGB555LE, DRM_FORMAT_XRGB1555},
    {AV_PIX_FMT_RGB555BE, DRM_FORMAT_XRGB1555 | DRM_FORMAT_BIG_ENDIAN},
    {AV_PIX_FMT_BGR555LE, DRM_FORMAT_XBGR1555},
    {AV_PIX_FMT_BGR555BE, DRM_FORMAT_XBGR1555 | DRM_FORMAT_BIG_ENDIAN},
    {AV_PIX_FMT_RGB565LE, DRM_FORMAT_RGB565},
    {AV_PIX_FMT_RGB565BE, DRM_FORMAT_RGB565 | DRM_FORMAT_BIG_ENDIAN},
    {AV_PIX_FMT_BGR565LE, DRM_FORMAT_BGR565},
    {AV_PIX_FMT_BGR565BE, DRM_FORMAT_BGR565 | DRM_FORMAT_BIG_ENDIAN},
    // 8-bit RGB.
    {AV_PIX_FMT_RGB24, DRM_FORMAT_RGB888},
    {AV_PIX_FMT_BGR24, DRM_FORMAT_BGR888},
    {AV_PIX_FMT_0RGB, DRM_FORMAT_BGRX8888},
    {AV_PIX_FMT_0BGR, DRM_FORMAT_RGBX8888},
    {AV_PIX_FMT_RGB0, DRM_FORMAT_XBGR8888},
    {AV_PIX_FMT_BGR0, DRM_FORMAT_XRGB8888},
    {AV_PIX_FMT_ARGB, DRM_FORMAT_BGRA8888},
    {AV_PIX_FMT_ABGR, DRM_FORMAT_RGBA8888},
    {AV_PIX_FMT_RGBA, DRM_FORMAT_ABGR8888},
    {AV_PIX_FMT_BGRA, DRM_FORMAT_ARGB8888},
    // 10-bit RGB.
    {AV_PIX_FMT_X2RGB10LE, DRM_FORMAT_XRGB2101010},
    {AV_PIX_FMT_X2RGB10BE, DRM_FORMAT_XRGB2101010 | DRM_FORMAT_BIG_ENDIAN},
    // 8-bit YUV 4:2:0.
    {AV_PIX_FMT_NV12, DRM_FORMAT_NV12},
    // 8-bit YUV 4:2:2.
    {AV_PIX_FMT_YUYV422, DRM_FORMAT_YUYV},
    {AV_PIX_FMT_YVYU422, DRM_FORMAT_YVYU},
    {AV_PIX_FMT_UYVY422, DRM_FORMAT_UYVY},
};

int kmsgrab_read_header(KMSGrabContext *ctx) {
  // KMSGrabContext *ctx = avctx->priv_data;
  drmModePlaneRes *plane_res = NULL;
  drmModePlane *plane = NULL;
  drmModeFB *fb = NULL;
#if HAVE_LIBDRM_GETFB2
  drmModeFB2 *fb2 = NULL;
#endif
  int err, i;

  err = av_hwdevice_ctx_create(&ctx->device_ref, AV_HWDEVICE_TYPE_DRM,
      ctx->device_path, NULL, 0);
  if (err < 0) {
    av_log(NULL, AV_LOG_ERROR, "Failed to open DRM device.\n");
    return err;
  }
  ctx->device = (AVHWDeviceContext *)ctx->device_ref->data;
  ctx->hwctx = (AVDRMDeviceContext *)ctx->device->hwctx;

  err = drmSetClientCap(ctx->hwctx->fd,
      DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (err < 0) {
    av_log(NULL, AV_LOG_WARNING, "Failed to set universal planes "
                                 "capability: primary planes will not be usable.\n");
  }

  if (ctx->source_plane > 0) {
    plane = drmModeGetPlane(ctx->hwctx->fd, ctx->source_plane);
    if (!plane) {
      err = errno;
      av_log(NULL, AV_LOG_ERROR, "Failed to get plane %ld: "
                                 "%s.\n", ctx->source_plane, strerror(err));
      err = AVERROR(err);
      goto fail;
    }

    if (plane->fb_id == 0) {
      av_log(NULL, AV_LOG_ERROR, "Plane %ld does not have "
                                 "an attached framebuffer.\n", ctx->source_plane);
      err = AVERROR(EINVAL);
      goto fail;
    }
  } else {
    plane_res = drmModeGetPlaneResources(ctx->hwctx->fd);
    if (!plane_res) {
      err = errno;
      av_log(NULL, AV_LOG_ERROR, "Failed to get plane "
                                 "resources: %s.\n", strerror(err));
      err = AVERROR(err);
      goto fail;
    }

    for (i = 0; i < plane_res->count_planes; i++) {
      plane = drmModeGetPlane(ctx->hwctx->fd,
          plane_res->planes[i]);
      if (!plane) {
        err = errno;
        av_log(NULL, AV_LOG_VERBOSE, "Failed to get "
                                     "plane %u: %s.\n",
            plane_res->planes[i], strerror(err));
        continue;
      }

      av_log(NULL, AV_LOG_DEBUG, "Plane %u: "
                                 "CRTC %u FB %u.\n",
          plane->plane_id, plane->crtc_id, plane->fb_id);

      if ((ctx->source_crtc > 0 &&
          plane->crtc_id != ctx->source_crtc) ||
          plane->fb_id == 0) {
        // Either not connected to the target source CRTC
        // or not active.
        drmModeFreePlane(plane);
        plane = NULL;
        continue;
      }

      break;
    }

    if (i == plane_res->count_planes) {
      if (ctx->source_crtc > 0) {
        av_log(NULL, AV_LOG_ERROR, "No usable planes found on "
                                   "CRTC %ld.\n", ctx->source_crtc);
      } else {
        av_log(NULL, AV_LOG_ERROR, "No usable planes found.\n");
      }
      err = AVERROR(EINVAL);
      goto fail;
    }

    av_log(NULL, AV_LOG_INFO, "Using plane %u to "
                              "locate framebuffers.\n", plane->plane_id);
  }

  ctx->plane_id = plane->plane_id;

#if HAVE_LIBDRM_GETFB2
  fb2 = drmModeGetFB2(ctx->hwctx->fd, plane->fb_id);
  if (!fb2 && errno == ENOSYS) {
    av_log(NULL, AV_LOG_INFO, "GETFB2 not supported, "
                              "will try to use GETFB instead.\n");
  } else if (!fb2) {
    err = errno;
    av_log(NULL, AV_LOG_ERROR, "Failed to get "
                               "framebuffer %u: %s.\n",
        plane->fb_id, strerror(err));
    err = AVERROR(err);
    goto fail;
  } else {
    av_log(NULL, AV_LOG_INFO, "Template framebuffer is "
                              "%u: %ux%u "
                              "format %u modifier %lx flags %u.\n",
        fb2->fb_id, fb2->width, fb2->height,
        fb2->pixel_format, fb2->modifier, fb2->flags);

    ctx->width = fb2->width;
    ctx->height = fb2->height;

    if (!fb2->handles[0]) {
      av_log(NULL, AV_LOG_ERROR, "No handle set on framebuffer: "
                                 "maybe you need some additional capabilities?\n");
      err = AVERROR(EINVAL);
      goto fail;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(kmsgrab_formats); i++) {
      if (kmsgrab_formats[i].drm_format == fb2->pixel_format) {
        if (ctx->format != AV_PIX_FMT_NONE &&
            ctx->format != kmsgrab_formats[i].pixfmt) {
          av_log(NULL, AV_LOG_ERROR, "Framebuffer pixel format "
                                     "%u does not match expected format.\n",
              fb2->pixel_format);
          err = AVERROR(EINVAL);
          goto fail;
        }
        ctx->drm_format = fb2->pixel_format;
        ctx->format = kmsgrab_formats[i].pixfmt;
        break;
      }
    }
    if (i == FF_ARRAY_ELEMS(kmsgrab_formats)) {
      av_log(NULL, AV_LOG_ERROR, "Framebuffer pixel format "
                                 "%u is not a known supported format.\n",
          fb2->pixel_format);
      err = AVERROR(EINVAL);
      goto fail;
    }

    if (fb2->flags & DRM_MODE_FB_MODIFIERS) {
      if (ctx->drm_format_modifier != DRM_FORMAT_MOD_INVALID &&
          ctx->drm_format_modifier != fb2->modifier) {
        av_log(NULL, AV_LOG_ERROR, "Framebuffer format modifier "
                                   "%lx does not match expected modifier.\n",
            fb2->modifier);
        err = AVERROR(EINVAL);
        goto fail;
      } else {
        ctx->drm_format_modifier = fb2->modifier;
      }
    }
    av_log(NULL, AV_LOG_VERBOSE, "Format is %s, from "
                                 "DRM format %u modifier %lx.\n",
        av_get_pix_fmt_name(ctx->format),
        ctx->drm_format, ctx->drm_format_modifier);

    ctx->fb2_available = 1;
  }
#endif

  if (!ctx->fb2_available) {
    if (ctx->format == AV_PIX_FMT_NONE) {
      // Backward compatibility: assume BGR0 if no format supplied.
      ctx->format = AV_PIX_FMT_BGR0;
    }
    for (i = 0; i < FF_ARRAY_ELEMS(kmsgrab_formats); i++) {
      if (kmsgrab_formats[i].pixfmt == ctx->format) {
        ctx->drm_format = kmsgrab_formats[i].drm_format;
        break;
      }
    }
    if (i >= FF_ARRAY_ELEMS(kmsgrab_formats)) {
      av_log(NULL, AV_LOG_ERROR, "Unsupported format %s.\n",
          av_get_pix_fmt_name(ctx->format));
      return AVERROR(EINVAL);
    }

    fb = drmModeGetFB(ctx->hwctx->fd, plane->fb_id);
    if (!fb) {
      err = errno;
      av_log(NULL, AV_LOG_ERROR, "Failed to get "
                                 "framebuffer %u: %s.\n",
          plane->fb_id, strerror(err));
      err = AVERROR(err);
      goto fail;
    }

    av_log(NULL, AV_LOG_INFO, "Template framebuffer is %u: "
                              "%ux%u %ubpp %ub depth.\n",
        fb->fb_id, fb->width, fb->height, fb->bpp, fb->depth);

    ctx->width = fb->width;
    ctx->height = fb->height;

    if (!fb->handle) {
      av_log(NULL, AV_LOG_ERROR, "No handle set on framebuffer: "
                                 "maybe you need some additional capabilities?\n");
      err = AVERROR(EINVAL);
      goto fail;
    }
  }

  ctx->frames_ref = av_hwframe_ctx_alloc(ctx->device_ref);
  if (!ctx->frames_ref) {
    err = AVERROR(ENOMEM);
    goto fail;
  }
  ctx->frames = (AVHWFramesContext *)ctx->frames_ref->data;

  ctx->frames->format = AV_PIX_FMT_DRM_PRIME;
  ctx->frames->sw_format = ctx->format,
  ctx->frames->width = ctx->width;
  ctx->frames->height = ctx->height;

  err = av_hwframe_ctx_init(ctx->frames_ref);
  if (err < 0) {
    av_log(NULL, AV_LOG_ERROR, "Failed to initialise "
                               "hardware frames context: %d.\n", err);
    goto fail;
  }

  err = 0;
fail:
  drmModeFreePlaneResources(plane_res);
  drmModeFreePlane(plane);
  drmModeFreeFB(fb);
#if HAVE_LIBDRM_GETFB2
  drmModeFreeFB2(fb2);
#endif
  return err;
}

void kmsgrab_read_close(KMSGrabContext *ctx) {
  av_buffer_unref(&ctx->frames_ref);
  av_buffer_unref(&ctx->device_ref);
}
