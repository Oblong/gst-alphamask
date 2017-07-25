/* GStreamer AlphaMask plugin
 * Copyright (C) 2016 Oblong Industries
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-alphamask
 *
 * The alphamask element combines a video and an alpha stream to produce
 * transparent videos in A420, ARGB or AYUV formats. The alpha channel is
 * made by reinterpreting a GRAY8 auxiliary video stream as an alpha mask.
 *
 * Sample pipeline:
 * |[
 * gst-launch-1.0 videotestsrc pattern=2 ! queue ! mixer.sink_0 \
 *   videotestsrc pattern=18 ! queue ! am.alpha_sink \
 *   videotestsrc ! queue ! alphamask name=am ! queue ! mixer.sink_1 \
 *   videomixer name=mixer sink_0::zorder=0 sink_1::zorder=1 ! queue ! \
 *   glimagesink
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstalphamask.h"
#include <string.h>             /* for memcpy */

#if !GST_CHECK_VERSION (1,8,0)
static inline void
gst_element_class_add_static_pad_template (GstElementClass * klass,
    GstStaticPadTemplate * static_templ)
{
  gst_element_class_add_pad_template (klass,
      gst_static_pad_template_get (static_templ));
}
#endif

GST_DEBUG_CATEGORY (alphamask_debug);
#define GST_CAT_DEFAULT alphamask_debug

enum
{
  PROP_0,
  PROP_LAST
};

#define FORMATS " { AYUV, A420, I420, YV12, NV12, NV21, BGRA, ARGB, RGBA, "\
                "   ABGR, Y444, Y42B, YUY2, UYVY, YVYU, Y41B, RGB, BGR, "\
                "   xRGB, xBGR, RGBx, BGRx } "

static GstStaticPadTemplate vsink_factory =
GST_STATIC_PAD_TEMPLATE ("video_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (FORMATS))
    );

static GstStaticPadTemplate asink_factory =
GST_STATIC_PAD_TEMPLATE ("alpha_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ GRAY8, I420, NV12, NV21 }"))
    );

static GstStaticPadTemplate src_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ A420, ARGB, AYUV }"))
    );

#define DEFAULT_FORMAT GST_VIDEO_FORMAT_A420

#define gst_alpha_mask_parent_class parent_class
G_DEFINE_TYPE (GstAlphaMask, gst_alpha_mask, GST_TYPE_ELEMENT);

#define GST_ALPHA_MASK_GET_LOCK(o) (&GST_ALPHA_MASK (o)->lock)
#define GST_ALPHA_MASK_GET_COND(o) (&GST_ALPHA_MASK (o)->cond)
#define GST_ALPHA_MASK_LOCK(o)     (g_mutex_lock (GST_ALPHA_MASK_GET_LOCK (o)))
#define GST_ALPHA_MASK_UNLOCK(o)   (g_mutex_unlock (GST_ALPHA_MASK_GET_LOCK (o)))
#define GST_ALPHA_MASK_WAIT(o)     (g_cond_wait (GST_ALPHA_MASK_GET_COND (o), GST_ALPHA_MASK_GET_LOCK (o)))
#define GST_ALPHA_MASK_SIGNAL(o)   (g_cond_signal (GST_ALPHA_MASK_GET_COND (o)))
#define GST_ALPHA_MASK_BROADCAST(o)(g_cond_broadcast (GST_ALPHA_MASK_GET_COND (o)))

static void
copy_alpha_packed_u1 (guint8 * dst, guint dstride, guint8 * src, guint sstride,
    guint width, guint height)
{
  guint i, j;

  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++)
      dst[j * 4] = src[j];

    dst += dstride;
    src += sstride;
  }
}

static void
copy_alpha_packed_u4 (guint8 * dst, guint dstride, guint8 * src, guint sstride,
    guint width, guint height)
{
  guint i, j;
  guint w = width >> 2;

  for (i = 0; i < height; i++) {
    guint32 *sd = (guint32 *) src;
    guint8 *dd = dst;
    for (j = 0; j < w; j++) {
      guint32 c = sd[j];
      dd[0] = c & 0xff;
      c >>= 8;
      dd[4] = c & 0xff;
      c >>= 8;
      dd[8] = c & 0xff;
      c >>= 8;
      dd[12] = c & 0xff;
      dd += 16;
    }

    dst += dstride;
    src += sstride;
  }
}

static void
copy_alpha_packed_u8 (guint8 * dst, guint dstride, guint8 * src, guint sstride,
    guint width, guint height)
{
  guint i, j;
  guint w = width >> 3;

  for (i = 0; i < height; i++) {
    guint64 *sd = (guint64 *) src;
    guint8 *dd = dst;
    for (j = 0; j < w; j++) {
      guint64 c = sd[j];
      dd[0] = c & 0xff;
      c >>= 8;
      dd[4] = c & 0xff;
      c >>= 8;
      dd[8] = c & 0xff;
      c >>= 8;
      dd[12] = c & 0xff;
      c >>= 8;
      dd[16] = c & 0xff;
      c >>= 8;
      dd[20] = c & 0xff;
      c >>= 8;
      dd[24] = c & 0xff;
      c >>= 8;
      dd[28] = c & 0xff;
      dd += 32;
    }

    dst += dstride;
    src += sstride;
  }
}

static void
copy_alpha_packed (GstVideoFrame * aframe, GstVideoFrame * oframe)
{
  const GstVideoInfo *ainfo;
  const GstVideoInfo *oinfo;
  guint8 *sp, *dp;
  guint w, h;
  guint ss, ds;

  ainfo = &aframe->info;
  oinfo = &oframe->info;

  sp = aframe->data[0];
  dp = oframe->data[0];

  w = GST_VIDEO_FRAME_COMP_WIDTH (aframe, 0);
  h = GST_VIDEO_FRAME_COMP_HEIGHT (aframe, 0);

  ss = GST_VIDEO_INFO_PLANE_STRIDE (ainfo, 0);
  ds = GST_VIDEO_INFO_PLANE_STRIDE (oinfo, 0);

  if (w & 3)
    copy_alpha_packed_u1 (dp, ds, sp, ss, w, h);
  else if (w & 7)
    copy_alpha_packed_u4 (dp, ds, sp, ss, w, h);
  else
    copy_alpha_packed_u8 (dp, ds, sp, ss, w, h);
}

static void
copy_alpha_planar (GstVideoFrame * aframe, GstVideoFrame * oframe, guint plane)
{
  const GstVideoInfo *ainfo;
  const GstVideoInfo *oinfo;
  guint8 *sp, *dp;
  guint w, h;
  guint ss, ds;

  ainfo = &aframe->info;
  oinfo = &oframe->info;

  sp = aframe->data[0];
  dp = oframe->data[plane];

  w = GST_VIDEO_FRAME_COMP_WIDTH (aframe, 0);
  h = GST_VIDEO_FRAME_COMP_HEIGHT (aframe, 0);

  ss = GST_VIDEO_INFO_PLANE_STRIDE (ainfo, 0);
  ds = GST_VIDEO_INFO_PLANE_STRIDE (oinfo, plane);

  if (ss == ds) {
    memcpy (dp, sp, h * ss);
  } else {
    guint j;
    for (j = 0; j < h; j++) {
      memcpy (dp, sp, w);
      dp += ds;
      sp += ss;
    }
  }
}

static GstBuffer *
gst_alpha_mask_convert (GstAlphaMask * thiz, GstBuffer * ibuf)
{
  GstVideoFrame aframe, iframe, oframe;
  GstBuffer *obuf;
  static GstAllocationParams params = { 0, 15, 0, 0, };
  gint size = GST_VIDEO_INFO_SIZE (&thiz->oinfo);

  obuf = gst_buffer_new_allocate (NULL, size, &params);
  gst_buffer_copy_into (obuf, ibuf, GST_BUFFER_COPY_FLAGS |
      GST_BUFFER_COPY_TIMESTAMPS, 0, -1);

  /* Convert the frame into output format */
  if (!gst_video_frame_map (&iframe, &thiz->iinfo, ibuf, GST_MAP_READ))
    goto invalid_in_frame;

  if (!gst_video_frame_map (&oframe, &thiz->oinfo, obuf, GST_MAP_READWRITE))
    goto invalid_out_frame;

  gst_video_converter_frame (thiz->convert, &iframe, &oframe);
  gst_video_frame_unmap (&iframe);
  gst_buffer_unref (ibuf);

  if (!thiz->alpha_buffer)
    goto beach;

  if (!gst_video_frame_map (&aframe, &thiz->ainfo, thiz->alpha_buffer,
          GST_MAP_READ)) {
    GST_DEBUG_OBJECT (thiz, "received invalid buffer");
  } else {
    if (thiz->oformat == GST_VIDEO_FORMAT_A420)
      copy_alpha_planar (&aframe, &oframe, 3);
    else
      copy_alpha_packed (&aframe, &oframe);
    gst_video_frame_unmap (&aframe);
  }

beach:
  gst_video_frame_unmap (&oframe);

  return obuf;

  /* ERRORS */
invalid_in_frame:
  {
    gst_buffer_unref (ibuf);
    GST_DEBUG_OBJECT (thiz, "received invalid buffer");
    return NULL;
  }

invalid_out_frame:
  {
    gst_video_frame_unmap (&iframe);
    gst_buffer_unref (ibuf);
    gst_buffer_unref (obuf);
    GST_DEBUG_OBJECT (thiz, "invalid output buffer");
    return NULL;
  }
}

static gboolean
gst_alpha_mask_negotiate (GstAlphaMask * thiz, GstCaps * caps)
{
  GstCaps *output_caps = NULL;
  GstCaps *template_caps;
  GstCaps *allowed_caps = NULL;
  GstVideoFormat format = DEFAULT_FORMAT;
  GstVideoInfo info;
  gboolean ret;

  GST_DEBUG_OBJECT (thiz, "performing negotiation");

  /* Clear any pending reconfigure to avoid negotiating twice */
  gst_pad_check_reconfigure (thiz->srcpad);

  if (!caps || gst_caps_is_empty (caps))
    return FALSE;

  template_caps = gst_static_pad_template_get_caps (&src_factory);
  allowed_caps = gst_pad_get_allowed_caps (thiz->srcpad);

  /* If downstream has ANY caps just go for our preferred format */
  if (allowed_caps == template_caps) {
    GST_INFO_OBJECT (thiz, "downstream has ANY caps");
  } else if (allowed_caps) {
    if (gst_caps_is_empty (allowed_caps)) {
      gst_caps_unref (allowed_caps);
      gst_caps_unref (template_caps);
      return FALSE;
    }

    allowed_caps = gst_caps_make_writable (allowed_caps);
    allowed_caps = gst_caps_fixate (allowed_caps);
    gst_video_info_from_caps (&info, allowed_caps);
    format = GST_VIDEO_INFO_FORMAT (&info);
  }
  gst_caps_unref (allowed_caps);
  gst_caps_unref (template_caps);

  gst_video_info_set_format (&info, format, thiz->width, thiz->height);
  info.par_n = thiz->iinfo.par_n;
  info.par_d = thiz->iinfo.par_d;
  info.fps_n = thiz->iinfo.fps_n;
  info.fps_d = thiz->iinfo.fps_d;

  GST_DEBUG_OBJECT (thiz, "Converting video from %d to %d",
      GST_VIDEO_INFO_FORMAT (&thiz->iinfo), GST_VIDEO_INFO_FORMAT (&info));

  if (thiz->convert)
    gst_video_converter_free (thiz->convert);

  thiz->convert = gst_video_converter_new (&thiz->iinfo, &info, NULL);
  if (!thiz->convert) {
    GST_ERROR_OBJECT (thiz, "Video cannot be converted");
    return FALSE;
  }

  thiz->oinfo = info;
  thiz->oformat = format;
  output_caps = gst_video_info_to_caps (&info);

  GST_DEBUG_OBJECT (thiz, "output video caps %" GST_PTR_FORMAT, output_caps);
  ret = gst_pad_set_caps (thiz->srcpad, output_caps);
  if (!ret) {
    GST_DEBUG_OBJECT (thiz, "negotiation failed, schedule reconfigure");
    gst_pad_mark_reconfigure (thiz->srcpad);
  }

  gst_caps_unref (output_caps);

  return ret;
}

static GstFlowReturn
gst_alpha_mask_push_frame (GstAlphaMask * thiz, GstBuffer * ibuffer)
{
  GstBuffer *obuffer = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  switch (thiz->oformat) {
    case GST_VIDEO_FORMAT_A420:
      if (thiz->iformat == GST_VIDEO_FORMAT_GRAY8) {
        GstMemory *mem = gst_buffer_peek_memory (thiz->alpha_buffer, 0);
        obuffer = gst_buffer_make_writable (ibuffer);
        gst_buffer_append_memory (obuffer, gst_memory_ref (mem));
      } else {
        obuffer = gst_alpha_mask_convert (thiz, ibuffer);
      }
      break;
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_AYUV:
      obuffer = gst_alpha_mask_convert (thiz, ibuffer);
      break;
    default:
      break;
  }

  if (obuffer)
    ret = gst_pad_push (thiz->srcpad, obuffer);

  return ret;
}

/* Called with lock held */
static void
gst_alpha_mask_pop_alpha (GstAlphaMask * thiz)
{
  g_return_if_fail (GST_IS_ALPHA_MASK (thiz));

  if (thiz->alpha_buffer) {
    GST_DEBUG_OBJECT (thiz, "releasing alpha buffer %p", thiz->alpha_buffer);
    gst_buffer_unref (thiz->alpha_buffer);
    thiz->alpha_buffer = NULL;
  }

  /* Let the task know we used that buffer */
  GST_ALPHA_MASK_BROADCAST (thiz);
}

static GstFlowReturn
gst_alpha_mask_video_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstAlphaMask *thiz;
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean in_seg = FALSE;
  guint64 start, stop, clip_start = 0, clip_stop = 0;

  thiz = GST_ALPHA_MASK (parent);

  if (!GST_BUFFER_TIMESTAMP_IS_VALID (buffer))
    goto missing_timestamp;

  /* ignore buffers that are outside of the current segment */
  start = GST_BUFFER_TIMESTAMP (buffer);

  if (!GST_BUFFER_DURATION_IS_VALID (buffer)) {
    stop = GST_CLOCK_TIME_NONE;
  } else {
    stop = start + GST_BUFFER_DURATION (buffer);
  }

  GST_LOG_OBJECT (thiz, "%" GST_SEGMENT_FORMAT "  BUFFER: ts=%"
      GST_TIME_FORMAT ", end=%" GST_TIME_FORMAT, &thiz->segment,
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

  /* segment_clip() will adjust start unconditionally to segment_start if
   * no stop time is provided, so handle this ourselves */
  if (stop == GST_CLOCK_TIME_NONE && start < thiz->segment.start)
    goto out_of_segment;

  in_seg = gst_segment_clip (&thiz->segment, GST_FORMAT_TIME, start, stop,
      &clip_start, &clip_stop);

  if (!in_seg)
    goto out_of_segment;

  /* if the buffer is only partially in the segment, fix up stamps */
  if (clip_start != start || (stop != -1 && clip_stop != stop)) {
    GST_DEBUG_OBJECT (thiz, "clipping buffer timestamp/duration to segment");
    buffer = gst_buffer_make_writable (buffer);
    GST_BUFFER_TIMESTAMP (buffer) = clip_start;
    if (stop != -1)
      GST_BUFFER_DURATION (buffer) = clip_stop - clip_start;
  }

  /* now, after we've done the clipping, fix up end time if there's no
   * duration (we only use those estimated values internally though, we
   * don't want to set bogus values on the buffer itself) */
  if (stop == -1) {
    if (thiz->iinfo.fps_n && thiz->iinfo.fps_d) {
      GST_DEBUG_OBJECT (thiz, "estimating duration based on framerate");
      stop = start + gst_util_uint64_scale_int (GST_SECOND,
          thiz->iinfo.fps_d, thiz->iinfo.fps_n);
    } else {
      GST_LOG_OBJECT (thiz, "no duration, assuming minimal duration");
      stop = start + 1;         /* we need to assume some interval */
    }
  }

  gst_object_sync_values (GST_OBJECT (thiz), GST_BUFFER_TIMESTAMP (buffer));

wait_for_alpha_buf:

  GST_ALPHA_MASK_LOCK (thiz);

  if (thiz->video_flushing)
    goto flushing;

  if (thiz->video_eos)
    goto have_eos;

  /* Check if we have a alpha buffer queued */
  if (thiz->alpha_buffer) {
    gboolean pop_alpha = FALSE, valid_alpha_time = TRUE;
    GstClockTime alpha_start = GST_CLOCK_TIME_NONE;
    GstClockTime alpha_end = GST_CLOCK_TIME_NONE;
    GstClockTime alpha_running_time = GST_CLOCK_TIME_NONE;
    GstClockTime alpha_running_time_end = GST_CLOCK_TIME_NONE;
    GstClockTime vid_running_time, vid_running_time_end;

    /* if the alpha buffer isn't stamped right, pop it off the
     * queue and display it for the current video frame only */
    if (!GST_BUFFER_TIMESTAMP_IS_VALID (thiz->alpha_buffer) ||
        !GST_BUFFER_DURATION_IS_VALID (thiz->alpha_buffer)) {
      GST_WARNING_OBJECT (thiz,
          "Got alpha buffer with invalid timestamp or duration");
      pop_alpha = TRUE;
      valid_alpha_time = FALSE;
    } else {
      alpha_start = GST_BUFFER_TIMESTAMP (thiz->alpha_buffer);
      alpha_end = alpha_start + GST_BUFFER_DURATION (thiz->alpha_buffer);
    }

    vid_running_time =
        gst_segment_to_running_time (&thiz->segment, GST_FORMAT_TIME, start);
    vid_running_time_end =
        gst_segment_to_running_time (&thiz->segment, GST_FORMAT_TIME, stop);

    /* If timestamp and duration are valid */
    if (valid_alpha_time) {
      alpha_running_time =
          gst_segment_to_running_time (&thiz->alpha_segment,
          GST_FORMAT_TIME, alpha_start);
      alpha_running_time_end =
          gst_segment_to_running_time (&thiz->alpha_segment,
          GST_FORMAT_TIME, alpha_end);
    }

    GST_LOG_OBJECT (thiz, "A: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
        GST_TIME_ARGS (alpha_running_time),
        GST_TIME_ARGS (alpha_running_time_end));
    GST_LOG_OBJECT (thiz, "V: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
        GST_TIME_ARGS (vid_running_time),
        GST_TIME_ARGS (vid_running_time_end));

    /* Alpha too old or in the future */
    if (valid_alpha_time && alpha_running_time_end <= vid_running_time) {
      /* alpha buffer too old, get rid of it and do nothing  */
      GST_LOG_OBJECT (thiz, "alpha buffer too old, popping");
      pop_alpha = FALSE;
      gst_alpha_mask_pop_alpha (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
      goto wait_for_alpha_buf;
    } else if (valid_alpha_time && vid_running_time_end <= alpha_running_time) {
      GST_WARNING_OBJECT (thiz, "alpha in future, dropping video buffer");
      GST_ALPHA_MASK_UNLOCK (thiz);
      /* Drop the video frame */
      gst_buffer_unref (buffer);
      ret = GST_FLOW_OK;
    } else {
      GST_ALPHA_MASK_UNLOCK (thiz);
      ret = gst_alpha_mask_push_frame (thiz, buffer);

      if (valid_alpha_time && alpha_running_time_end <= vid_running_time_end) {
        GST_LOG_OBJECT (thiz, "alpha buffer not needed any longer");
        pop_alpha = TRUE;
      }
    }
    if (pop_alpha) {
      GST_ALPHA_MASK_LOCK (thiz);
      gst_alpha_mask_pop_alpha (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
    }
  } else {
    gboolean wait_for_alpha_buf = TRUE;

    if (thiz->alpha_eos || thiz->alpha_segment_done)
      wait_for_alpha_buf = FALSE;

    /* Alpha pad linked, but no alpha buffer available - what now? */
    if (thiz->alpha_segment.format == GST_FORMAT_TIME) {
      GstClockTime alpha_start_running_time, alpha_position_running_time;
      GstClockTime vid_running_time;

      vid_running_time =
          gst_segment_to_running_time (&thiz->segment, GST_FORMAT_TIME,
          GST_BUFFER_TIMESTAMP (buffer));
      alpha_start_running_time =
          gst_segment_to_running_time (&thiz->alpha_segment,
          GST_FORMAT_TIME, thiz->alpha_segment.start);
      alpha_position_running_time =
          gst_segment_to_running_time (&thiz->alpha_segment,
          GST_FORMAT_TIME, thiz->alpha_segment.position);

      if ((GST_CLOCK_TIME_IS_VALID (alpha_start_running_time) &&
              vid_running_time < alpha_start_running_time) ||
          (GST_CLOCK_TIME_IS_VALID (alpha_position_running_time) &&
              vid_running_time < alpha_position_running_time)) {
        wait_for_alpha_buf = FALSE;
      }
    }

    if (wait_for_alpha_buf) {
      GST_DEBUG_OBJECT (thiz, "no alpha buffer, need to wait for one");
      GST_ALPHA_MASK_WAIT (thiz);
      GST_DEBUG_OBJECT (thiz, "resuming");
      GST_ALPHA_MASK_UNLOCK (thiz);
      goto wait_for_alpha_buf;
    } else {
      GST_ALPHA_MASK_UNLOCK (thiz);
      GST_LOG_OBJECT (thiz, "no need to wait for a alpha buffer");
      ret = gst_pad_push (thiz->srcpad, buffer);
    }
  }

  /* Update position */
  thiz->segment.position = clip_start;

  return ret;

missing_timestamp:
  {
    GST_WARNING_OBJECT (thiz, "buffer without timestamp, discarding");
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }

flushing:
  {
    GST_ALPHA_MASK_UNLOCK (thiz);
    GST_DEBUG_OBJECT (thiz, "flushing, discarding buffer");
    gst_buffer_unref (buffer);
    return GST_FLOW_FLUSHING;
  }
have_eos:
  {
    GST_ALPHA_MASK_UNLOCK (thiz);
    GST_DEBUG_OBJECT (thiz, "eos, discarding buffer");
    gst_buffer_unref (buffer);
    return GST_FLOW_EOS;
  }
out_of_segment:
  {
    GST_DEBUG_OBJECT (thiz, "buffer out of segment, discarding");
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }
}

static gboolean
gst_alpha_mask_video_setcaps (GstAlphaMask * thiz, GstCaps * caps)
{
  GstVideoInfo info;
  gboolean ret = FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_caps;

  GST_DEBUG_OBJECT (thiz, "received video caps %" GST_PTR_FORMAT, caps);

  thiz->iinfo = info;
  thiz->width = GST_VIDEO_INFO_WIDTH (&info);
  thiz->height = GST_VIDEO_INFO_HEIGHT (&info);
  thiz->iformat = GST_VIDEO_INFO_FORMAT (&info);

  ret = gst_alpha_mask_negotiate (thiz, caps);

  return ret;

  /* ERRORS */
invalid_caps:
  {
    GST_DEBUG_OBJECT (thiz, "could not parse caps");
    return FALSE;
  }
}

static gboolean
gst_alpha_mask_video_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret = FALSE;
  GstAlphaMask *thiz = NULL;

  thiz = GST_ALPHA_MASK (parent);

  GST_DEBUG_OBJECT (pad, "received event %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_alpha_mask_video_setcaps (thiz, caps);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *segment;

      GST_DEBUG_OBJECT (thiz, "received new segment");

      GST_ALPHA_MASK_LOCK (thiz);
      thiz->video_eos = FALSE;
      thiz->video_segment_done = FALSE;
      GST_ALPHA_MASK_UNLOCK (thiz);

      gst_event_parse_segment (event, &segment);

      if (segment->format == GST_FORMAT_TIME) {
        gst_segment_copy_into (segment, &thiz->segment);
        GST_INFO_OBJECT (thiz, "VIDEO SEGMENT now: %" GST_SEGMENT_FORMAT,
            &thiz->segment);
      } else {
        GST_ELEMENT_WARNING (thiz, STREAM, MUX, (NULL),
            ("received non-TIME newsegment event on video input"));
      }

      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    case GST_EVENT_EOS:
      GST_ALPHA_MASK_LOCK (thiz);
      GST_INFO_OBJECT (thiz, "video EOS");
      thiz->video_eos = TRUE;
      GST_ALPHA_MASK_UNLOCK (thiz);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    case GST_EVENT_SEGMENT_DONE:
      GST_ALPHA_MASK_LOCK (thiz);
      GST_INFO_OBJECT (thiz, "video segment-done");
      thiz->video_segment_done = TRUE;
      GST_ALPHA_MASK_UNLOCK (thiz);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    case GST_EVENT_FLUSH_START:
      GST_ALPHA_MASK_LOCK (thiz);
      GST_INFO_OBJECT (thiz, "video flush start");
      thiz->video_flushing = TRUE;
      GST_ALPHA_MASK_BROADCAST (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    case GST_EVENT_FLUSH_STOP:
      GST_ALPHA_MASK_LOCK (thiz);
      GST_INFO_OBJECT (thiz, "video flush stop");
      thiz->video_flushing = FALSE;
      thiz->video_eos = FALSE;
      thiz->video_segment_done = FALSE;
      gst_segment_init (&thiz->segment, GST_FORMAT_TIME);
      GST_ALPHA_MASK_UNLOCK (thiz);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

/* We receive alpha buffers here. If they are out of segment we just ignore them.
   If the buffer is in our segment we keep it internally except if another one
   is already waiting here, in that case we wait that it gets kicked out */
static GstFlowReturn
gst_alpha_mask_alpha_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstAlphaMask *thiz = NULL;
  gboolean in_seg = FALSE;
  guint64 clip_start = 0, clip_stop = 0;

  thiz = GST_ALPHA_MASK (parent);

  GST_ALPHA_MASK_LOCK (thiz);

  if (thiz->alpha_flushing) {
    GST_ALPHA_MASK_UNLOCK (thiz);
    ret = GST_FLOW_FLUSHING;
    GST_LOG_OBJECT (thiz, "alpha flushing");
    goto beach;
  }

  if (thiz->alpha_eos) {
    GST_ALPHA_MASK_UNLOCK (thiz);
    ret = GST_FLOW_EOS;
    GST_LOG_OBJECT (thiz, "alpha EOS");
    goto beach;
  }

  GST_LOG_OBJECT (thiz, "%" GST_SEGMENT_FORMAT "  BUFFER: ts=%"
      GST_TIME_FORMAT ", end=%" GST_TIME_FORMAT, &thiz->segment,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer) +
          GST_BUFFER_DURATION (buffer)));

  if (G_LIKELY (GST_BUFFER_TIMESTAMP_IS_VALID (buffer))) {
    GstClockTime stop;

    if (G_LIKELY (GST_BUFFER_DURATION_IS_VALID (buffer)))
      stop = GST_BUFFER_TIMESTAMP (buffer) + GST_BUFFER_DURATION (buffer);
    else
      stop = GST_CLOCK_TIME_NONE;

    in_seg = gst_segment_clip (&thiz->alpha_segment, GST_FORMAT_TIME,
        GST_BUFFER_TIMESTAMP (buffer), stop, &clip_start, &clip_stop);
  } else {
    in_seg = TRUE;
  }

  if (in_seg) {
    /* about to change metadata */
    buffer = gst_buffer_make_writable (buffer);
    if (GST_BUFFER_TIMESTAMP_IS_VALID (buffer))
      GST_BUFFER_TIMESTAMP (buffer) = clip_start;
    if (GST_BUFFER_DURATION_IS_VALID (buffer))
      GST_BUFFER_DURATION (buffer) = clip_stop - clip_start;

    /* Wait for the previous buffer to go away */
    while (thiz->alpha_buffer != NULL) {
      GST_DEBUG ("Pad %s:%s has a buffer queued, waiting",
          GST_DEBUG_PAD_NAME (pad));
      GST_ALPHA_MASK_WAIT (thiz);
      GST_DEBUG ("Pad %s:%s resuming", GST_DEBUG_PAD_NAME (pad));
      if (thiz->alpha_flushing) {
        GST_ALPHA_MASK_UNLOCK (thiz);
        ret = GST_FLOW_FLUSHING;
        goto beach;
      }
    }

    if (GST_BUFFER_TIMESTAMP_IS_VALID (buffer))
      thiz->alpha_segment.position = clip_start;

    thiz->alpha_buffer = buffer;        /* pass ownership of @buffer */
    buffer = NULL;

    /* in case the video chain is waiting for a alpha buffer, wake it up */
    GST_ALPHA_MASK_BROADCAST (thiz);
  }

  GST_ALPHA_MASK_UNLOCK (thiz);

beach:
  if (buffer)
    gst_buffer_unref (buffer);

  return ret;
}

static gboolean
gst_alpha_mask_alpha_setcaps (GstAlphaMask * thiz, GstCaps * caps)
{
  GstVideoInfo info;

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_caps;

  GST_DEBUG_OBJECT (thiz, "received alpha caps %" GST_PTR_FORMAT, caps);
  thiz->ainfo = info;

  return TRUE;

  /* ERRORS */
invalid_caps:
  {
    GST_DEBUG_OBJECT (thiz, "could not parse caps");
    return FALSE;
  }
}

static gboolean
gst_alpha_mask_alpha_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret = FALSE;
  GstAlphaMask *thiz = NULL;

  thiz = GST_ALPHA_MASK (parent);

  GST_LOG_OBJECT (pad, "received event %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_alpha_mask_alpha_setcaps (thiz, caps);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *segment;

      GST_ALPHA_MASK_LOCK (thiz);
      thiz->alpha_eos = FALSE;
      thiz->alpha_segment_done = FALSE;
      gst_alpha_mask_pop_alpha (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);

      gst_event_parse_segment (event, &segment);

      if (segment->format == GST_FORMAT_TIME) {
        GST_ALPHA_MASK_LOCK (thiz);
        gst_segment_copy_into (segment, &thiz->alpha_segment);
        GST_INFO_OBJECT (thiz, "ALPHA SEGMENT now: %" GST_SEGMENT_FORMAT,
            &thiz->alpha_segment);
        GST_ALPHA_MASK_UNLOCK (thiz);
      } else {
        GST_ELEMENT_WARNING (thiz, STREAM, MUX, (NULL),
            ("received non-TIME newsegment event on alpha input"));
      }

      gst_event_unref (event);
      ret = TRUE;

      /* wake up the video chain, it might be waiting for a alpha buffer or
       * a alpha segment update */
      GST_ALPHA_MASK_LOCK (thiz);
      GST_ALPHA_MASK_BROADCAST (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
      break;
    }
    case GST_EVENT_GAP:
    {
      GstClockTime start, duration;

      gst_event_parse_gap (event, &start, &duration);
      if (GST_CLOCK_TIME_IS_VALID (duration))
        start += duration;
      /* we do not expect another buffer until after gap,
       * so that is our position now */
      thiz->alpha_segment.position = start;

      /* wake up the video chain, it might be waiting for a alpha buffer or
       * a alpha segment update */
      GST_ALPHA_MASK_LOCK (thiz);
      GST_ALPHA_MASK_BROADCAST (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);

      gst_event_unref (event);
      ret = TRUE;
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      GST_ALPHA_MASK_LOCK (thiz);
      GST_INFO_OBJECT (thiz, "alpha flush stop");
      thiz->alpha_flushing = FALSE;
      thiz->alpha_eos = FALSE;
      thiz->alpha_segment_done = FALSE;
      gst_alpha_mask_pop_alpha (thiz);
      gst_segment_init (&thiz->alpha_segment, GST_FORMAT_TIME);
      GST_ALPHA_MASK_UNLOCK (thiz);
      gst_event_unref (event);
      ret = TRUE;
      break;
    case GST_EVENT_FLUSH_START:
      GST_ALPHA_MASK_LOCK (thiz);
      GST_INFO_OBJECT (thiz, "alpha flush start");
      thiz->alpha_flushing = TRUE;
      GST_ALPHA_MASK_BROADCAST (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
      gst_event_unref (event);
      ret = TRUE;
      break;
    case GST_EVENT_SEGMENT_DONE:
      GST_ALPHA_MASK_LOCK (thiz);
      thiz->alpha_segment_done = TRUE;
      GST_INFO_OBJECT (thiz, "alpha segment-done");
      /* wake up the video chain, it might be waiting for a alpha buffer or
       * a alpha segment update */
      GST_ALPHA_MASK_BROADCAST (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
      gst_event_unref (event);
      ret = TRUE;
      break;
    case GST_EVENT_EOS:
      GST_ALPHA_MASK_LOCK (thiz);
      thiz->alpha_eos = TRUE;
      GST_INFO_OBJECT (thiz, "alpha EOS");
      /* wake up the video chain, it might be waiting for a alpha buffer or
       * a alpha segment update */
      GST_ALPHA_MASK_BROADCAST (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
      gst_event_unref (event);
      ret = TRUE;
      break;
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

static GstPadLinkReturn
gst_alpha_mask_alpha_pad_link (GstPad * pad, GstObject * parent, GstPad * peer)
{
  GstAlphaMask *thiz;

  thiz = GST_ALPHA_MASK (parent);
  if (G_UNLIKELY (!thiz))
    return GST_PAD_LINK_REFUSED;

  GST_DEBUG_OBJECT (thiz, "Alpha pad linked");

  thiz->alpha_linked = TRUE;

  return GST_PAD_LINK_OK;
}

static void
gst_alpha_mask_alpha_pad_unlink (GstPad * pad, GstObject * parent)
{
  GstAlphaMask *thiz;

  thiz = GST_ALPHA_MASK (parent);

  GST_DEBUG_OBJECT (thiz, "Alpha pad unlinked");

  thiz->alpha_linked = FALSE;

  gst_segment_init (&thiz->alpha_segment, GST_FORMAT_UNDEFINED);
}

static gboolean
gst_alpha_mask_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstAlphaMask *thiz;
  gboolean ret;

  thiz = GST_ALPHA_MASK (parent);

  /* Drop QOS events to ensure we get both streams completely merged */
  if (GST_EVENT_TYPE (event) == GST_EVENT_QOS) {
    gst_event_unref (event);
    return TRUE;
  }

  if (thiz->alpha_linked) {
    gst_event_ref (event);
    ret = gst_pad_push_event (thiz->video_sinkpad, event);
    gst_pad_push_event (thiz->alpha_sinkpad, event);
  } else {
    ret = gst_pad_push_event (thiz->video_sinkpad, event);
  }

  return ret;
}

static GstStateChangeReturn
gst_alpha_mask_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstAlphaMask *thiz = GST_ALPHA_MASK (element);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_ALPHA_MASK_LOCK (thiz);
      thiz->alpha_flushing = TRUE;
      thiz->video_flushing = TRUE;
      /* pop_alpha will broadcast on the GCond and thus also make the video
       * chain exit if it's waiting for a alpha buffer */
      gst_alpha_mask_pop_alpha (thiz);
      GST_ALPHA_MASK_UNLOCK (thiz);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      GST_ALPHA_MASK_LOCK (thiz);
      thiz->alpha_flushing = FALSE;
      thiz->video_flushing = FALSE;
      thiz->video_eos = FALSE;
      thiz->alpha_eos = FALSE;
      gst_segment_init (&thiz->segment, GST_FORMAT_TIME);
      gst_segment_init (&thiz->alpha_segment, GST_FORMAT_TIME);
      GST_ALPHA_MASK_UNLOCK (thiz);
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_alpha_mask_finalize (GObject * object)
{
  GstAlphaMask *thiz = GST_ALPHA_MASK (object);

  if (thiz->alpha_buffer) {
    gst_buffer_unref (thiz->alpha_buffer);
    thiz->alpha_buffer = NULL;
  }

  if (thiz->convert)
    gst_video_converter_free (thiz->convert);
  thiz->convert = NULL;

  g_mutex_clear (&thiz->lock);
  g_cond_clear (&thiz->cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_alpha_mask_class_init (GstAlphaMaskClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->finalize = gst_alpha_mask_finalize;

  gst_element_class_set_static_metadata (gstelement_class,
      "Alpha mask combinator",
      "Filter/Effect/Video",
      "Combines video and alpha streams", "Josep Torra <jtorra@oblong.com>");

  gst_element_class_add_static_pad_template (gstelement_class, &vsink_factory);
  gst_element_class_add_static_pad_template (gstelement_class, &asink_factory);
  gst_element_class_add_static_pad_template (gstelement_class, &src_factory);

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_alpha_mask_change_state);
}

static void
gst_alpha_mask_init (GstAlphaMask * thiz)
{
  GstPadTemplate *template;

  /* video sink */
  template = gst_static_pad_template_get (&vsink_factory);
  thiz->video_sinkpad = gst_pad_new_from_template (template, "video_sink");
  gst_object_unref (template);
  gst_pad_set_chain_function (thiz->video_sinkpad,
      GST_DEBUG_FUNCPTR (gst_alpha_mask_video_chain));
  gst_pad_set_event_function (thiz->video_sinkpad,
      GST_DEBUG_FUNCPTR (gst_alpha_mask_video_event));
  gst_element_add_pad (GST_ELEMENT (thiz), thiz->video_sinkpad);

  /* alpha sink */
  template = gst_static_pad_template_get (&asink_factory);
  thiz->alpha_sinkpad = gst_pad_new_from_template (template, "alpha_sink");
  gst_object_unref (template);
  gst_pad_set_chain_function (thiz->alpha_sinkpad,
      GST_DEBUG_FUNCPTR (gst_alpha_mask_alpha_chain));
  gst_pad_set_event_function (thiz->alpha_sinkpad,
      GST_DEBUG_FUNCPTR (gst_alpha_mask_alpha_event));
  gst_pad_set_link_function (thiz->alpha_sinkpad,
      GST_DEBUG_FUNCPTR (gst_alpha_mask_alpha_pad_link));
  gst_pad_set_unlink_function (thiz->alpha_sinkpad,
      GST_DEBUG_FUNCPTR (gst_alpha_mask_alpha_pad_unlink));
  gst_element_add_pad (GST_ELEMENT (thiz), thiz->alpha_sinkpad);

  /* video source */
  template = gst_static_pad_template_get (&src_factory);
  thiz->srcpad = gst_pad_new_from_template (template, "src");
  gst_object_unref (template);
  gst_pad_set_event_function (thiz->srcpad,
      GST_DEBUG_FUNCPTR (gst_alpha_mask_src_event));
  gst_element_add_pad (GST_ELEMENT (thiz), thiz->srcpad);

  thiz->convert = NULL;
  thiz->alpha_buffer = NULL;
  thiz->alpha_linked = FALSE;

  g_mutex_init (&thiz->lock);
  g_cond_init (&thiz->cond);
  gst_segment_init (&thiz->segment, GST_FORMAT_TIME);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "alphamask", GST_RANK_NONE,
          GST_TYPE_ALPHA_MASK)) {
    return FALSE;
  }

  GST_DEBUG_CATEGORY_INIT (alphamask_debug, "alphamask", 0,
      "Alpha mask element");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    alphamask, "Alpha mask combinator", plugin_init,
    VERSION, "LGPL", "gst-alphamask", "http://oblong.com/")
