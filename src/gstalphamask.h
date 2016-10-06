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

#ifndef __GST_ALPHA_MASK_H__
#define __GST_ALPHA_MASK_H__

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_ALPHA_MASK            (gst_alpha_mask_get_type())
#define GST_ALPHA_MASK(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),\
                                         GST_TYPE_ALPHA_MASK, GstAlphaMask))
#define GST_ALPHA_MASK_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),\
                                         GST_TYPE_ALPHA_MASK, GstAlphaMaskClass))
#define GST_ALPHA_MASK_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),\
                                         GST_TYPE_ALPHA_MASK, GstAlphaMaskClass))
#define GST_IS_ALPHA_MASK(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),\
                                         GST_TYPE_ALPHA_MASK))
#define GST_IS_ALPHA_MASK_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),\
                                         GST_TYPE_ALPHA_MASK))

typedef struct _GstAlphaMask      GstAlphaMask;
typedef struct _GstAlphaMaskClass GstAlphaMaskClass;

/**
 * GstAlphaMask:
 *
 * Opaque alphamask object structure
 */
struct _GstAlphaMask {
    GstElement               element;

    GstPad                  *video_sinkpad;
    GstPad                  *alpha_sinkpad;
    GstPad                  *srcpad;

    GstSegment               segment;
    GstSegment               alpha_segment;
    GstBuffer               *alpha_buffer;
    gboolean                 alpha_linked;
    gboolean                 video_flushing;
    gboolean                 video_eos;
    gboolean                 alpha_flushing;
    gboolean                 alpha_eos;

    GMutex                   lock;
    GCond                    cond;  /* to signal removal of a queued alpha
                                     * buffer, arrival of a alpha buffer,
                                     * a alpha segment update, or a change
                                     * in status (e.g. shutdown, flushing) */

    /* stream details */
    GstVideoInfo             iinfo;
    GstVideoInfo             ainfo;
    GstVideoInfo             oinfo;
    gint                     width;
    gint                     height;
    GstVideoFormat           iformat;
    GstVideoFormat           oformat;

    GstVideoConverter       *convert;
};

struct _GstAlphaMaskClass {
    GstElementClass parent_class;
};

GType gst_alpha_mask_get_type(void) G_GNUC_CONST;

G_END_DECLS

#endif /* __GST_ALPHA_MASK_H */
