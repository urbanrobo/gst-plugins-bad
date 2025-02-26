/* GStreamer
 * Copyright (C) 2020 Igalia, S.L.
 *     Author: Víctor Jáquez <vjaquez@igalia.com>
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
 * License along with this library; if not, write to the0
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-vapostproc
 * @title: vapostproc
 * @short_description: A VA-API base video postprocessing filter
 *
 * vapostproc applies different video filters to VA surfaces. These
 * filters vary depending on the installed and chosen
 * [VA-API](https://01.org/linuxmedia/vaapi) driver, but usually
 * resizing and color conversion are available.
 *
 * The generated surfaces can be mapped onto main memory as video
 * frames.
 *
 * Use gst-inspect-1.0 to introspect the available capabilities of the
 * driver's post-processor entry point.
 *
 * ## Example launch line
 * ```
 * gst-launch-1.0 videotestsrc ! "video/x-raw,format=(string)NV12" ! vapostproc ! autovideosink
 * ```
 *
 * Cropping is supported via buffers' crop meta. It's only done if the
 * postproccessor is not in passthrough mode or if downstream doesn't
 * support the crop meta API.
 *
 * ### Cropping example
 * ```
 * gst-launch-1.0 videotestsrc ! "video/x-raw,format=(string)NV12" ! videocrop bottom=50 left=100 ! vapostproc ! autovideosink
 * ```
 *
 * If the VA driver support color balance filter, with controls such
 * as hue, brightness, contrast, etc., those controls are exposed both
 * as element properties and through the #GstColorBalance interface.
 *
 * Since: 1.20
 *
 */

/* ToDo:
 *
 * + deinterlacing
 * + HDR tone mapping
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvavpp.h"

#include <gst/video/video.h>

#include <va/va_drmcommon.h>

#include "gstvaallocator.h"
#include "gstvabasetransform.h"
#include "gstvacaps.h"
#include "gstvadisplay_priv.h"
#include "gstvafilter.h"
#include "gstvapool.h"
#include "gstvautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_va_vpp_debug);
#define GST_CAT_DEFAULT gst_va_vpp_debug

#define GST_VA_VPP(obj)           ((GstVaVpp *) obj)
#define GST_VA_VPP_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_FROM_INSTANCE (obj), GstVaVppClass))
#define GST_VA_VPP_CLASS(klass)    ((GstVaVppClass *) klass)

#define SWAP(a, b) do { const __typeof__ (a) t = a; a = b; b = t; } while (0)

typedef struct _GstVaVpp GstVaVpp;
typedef struct _GstVaVppClass GstVaVppClass;

struct _GstVaVppClass
{
  /* GstVideoFilter overlaps functionality */
  GstVaBaseTransformClass parent_class;
};

struct _GstVaVpp
{
  GstVaBaseTransform parent;

  gboolean rebuild_filters;
  guint op_flags;

  /* filters */
  float denoise;
  float sharpen;
  float skintone;
  float brightness;
  float contrast;
  float hue;
  float saturation;
  gboolean auto_contrast;
  gboolean auto_brightness;
  gboolean auto_saturation;
  GstVideoOrientationMethod direction;
  GstVideoOrientationMethod prev_direction;
  GstVideoOrientationMethod tag_direction;

  GList *channels;
};

static GstElementClass *parent_class = NULL;

struct CData
{
  gchar *render_device_path;
  gchar *description;
};

/* convertions that disable passthrough */
enum
{
  VPP_CONVERT_SIZE = 1 << 0,
  VPP_CONVERT_FORMAT = 1 << 1,
  VPP_CONVERT_FILTERS = 1 << 2,
  VPP_CONVERT_DIRECTION = 1 << 3,
  VPP_CONVERT_FEATURE = 1 << 4,
  VPP_CONVERT_CROP = 1 << 5,
  VPP_CONVERT_DUMMY = 1 << 6,
};

/* *INDENT-OFF* */
static const gchar *caps_str =
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_VA,
        "{ NV12, I420, YV12, YUY2, RGBA, BGRA, P010_10LE, ARGB, ABGR }") " ;"
    GST_VIDEO_CAPS_MAKE ("{ VUYA, GRAY8, NV12, NV21, YUY2, UYVY, YV12, "
        "I420, P010_10LE, RGBA, BGRA, ARGB, ABGR  }");
/* *INDENT-ON* */

#define META_TAG_COLORSPACE meta_tag_colorspace_quark
static GQuark meta_tag_colorspace_quark;
#define META_TAG_SIZE meta_tag_size_quark
static GQuark meta_tag_size_quark;
#define META_TAG_ORIENTATION meta_tag_orientation_quark
static GQuark meta_tag_orientation_quark;
#define META_TAG_VIDEO meta_tag_video_quark
static GQuark meta_tag_video_quark;

static void gst_va_vpp_colorbalance_init (gpointer iface, gpointer data);
static void gst_va_vpp_rebuild_filters (GstVaVpp * self);

static void
gst_va_vpp_dispose (GObject * object)
{
  GstVaVpp *self = GST_VA_VPP (object);

  if (self->channels)
    g_list_free_full (g_steal_pointer (&self->channels), g_object_unref);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_va_vpp_update_passthrough (GstVaVpp * self, gboolean reconf)
{
  GstBaseTransform *trans = GST_BASE_TRANSFORM (self);
  gboolean old, new;

  old = gst_base_transform_is_passthrough (trans);

  GST_OBJECT_LOCK (self);
  new = (self->op_flags == 0);
  GST_OBJECT_UNLOCK (self);

  if (old != new) {
    GST_INFO_OBJECT (self, "%s passthrough", new ? "enabling" : "disabling");
    if (reconf)
      gst_base_transform_reconfigure_src (trans);
    gst_base_transform_set_passthrough (trans, new);
  }
}

static void
_update_properties_unlocked (GstVaVpp * self)
{
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);

  if (!btrans->filter)
    return;

  if ((self->direction != GST_VIDEO_ORIENTATION_AUTO
          && self->direction != self->prev_direction)
      || (self->direction == GST_VIDEO_ORIENTATION_AUTO
          && self->tag_direction != self->prev_direction)) {

    GstVideoOrientationMethod direction =
        (self->direction == GST_VIDEO_ORIENTATION_AUTO) ?
        self->tag_direction : self->direction;

    if (!gst_va_filter_set_orientation (btrans->filter, direction)) {
      if (self->direction == GST_VIDEO_ORIENTATION_AUTO)
        self->tag_direction = self->prev_direction;
      else
        self->direction = self->prev_direction;

      self->op_flags &= ~VPP_CONVERT_DIRECTION;

      /* FIXME: unlocked bus warning message */
      GST_WARNING_OBJECT (self,
          "Driver cannot set resquested orientation. Setting it back.");
    } else {
      self->prev_direction = direction;

      self->op_flags |= VPP_CONVERT_DIRECTION;

      gst_base_transform_reconfigure_src (GST_BASE_TRANSFORM (self));
    }
  } else {
    self->op_flags &= ~VPP_CONVERT_DIRECTION;
  }
}

static void
gst_va_vpp_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVaVpp *self = GST_VA_VPP (object);

  GST_OBJECT_LOCK (object);
  switch (prop_id) {
    case GST_VA_FILTER_PROP_DENOISE:
      self->denoise = g_value_get_float (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_SHARPEN:
      self->sharpen = g_value_get_float (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_SKINTONE:
      if (G_VALUE_TYPE (value) == G_TYPE_BOOLEAN)
        self->skintone = (float) g_value_get_boolean (value);
      else
        self->skintone = g_value_get_float (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_VIDEO_DIR:{
      GstVideoOrientationMethod direction = g_value_get_enum (value);
      self->prev_direction = (direction == GST_VIDEO_ORIENTATION_AUTO) ?
          self->tag_direction : self->direction;
      self->direction = direction;
      break;
    }
    case GST_VA_FILTER_PROP_HUE:
      self->hue = g_value_get_float (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_SATURATION:
      self->saturation = g_value_get_float (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_BRIGHTNESS:
      self->brightness = g_value_get_float (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_CONTRAST:
      self->contrast = g_value_get_float (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_AUTO_SATURATION:
      self->auto_saturation = g_value_get_boolean (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_AUTO_BRIGHTNESS:
      self->auto_brightness = g_value_get_boolean (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_AUTO_CONTRAST:
      self->auto_contrast = g_value_get_boolean (value);
      g_atomic_int_set (&self->rebuild_filters, TRUE);
      break;
    case GST_VA_FILTER_PROP_DISABLE_PASSTHROUGH:{
      gboolean disable_passthrough = g_value_get_boolean (value);
      if (disable_passthrough)
        self->op_flags |= VPP_CONVERT_DUMMY;
      else
        self->op_flags &= ~VPP_CONVERT_DUMMY;
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  _update_properties_unlocked (self);
  GST_OBJECT_UNLOCK (object);

  gst_va_vpp_update_passthrough (self, FALSE);
}

static void
gst_va_vpp_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVaVpp *self = GST_VA_VPP (object);

  GST_OBJECT_LOCK (object);
  switch (prop_id) {
    case GST_VA_FILTER_PROP_DENOISE:
      g_value_set_float (value, self->denoise);
      break;
    case GST_VA_FILTER_PROP_SHARPEN:
      g_value_set_float (value, self->sharpen);
      break;
    case GST_VA_FILTER_PROP_SKINTONE:
      if (G_VALUE_TYPE (value) == G_TYPE_BOOLEAN)
        g_value_set_boolean (value, self->skintone > 0);
      else
        g_value_set_float (value, self->skintone);
      break;
    case GST_VA_FILTER_PROP_VIDEO_DIR:
      g_value_set_enum (value, self->direction);
      break;
    case GST_VA_FILTER_PROP_HUE:
      g_value_set_float (value, self->hue);
      break;
    case GST_VA_FILTER_PROP_SATURATION:
      g_value_set_float (value, self->saturation);
      break;
    case GST_VA_FILTER_PROP_BRIGHTNESS:
      g_value_set_float (value, self->brightness);
      break;
    case GST_VA_FILTER_PROP_CONTRAST:
      g_value_set_float (value, self->contrast);
      break;
    case GST_VA_FILTER_PROP_AUTO_SATURATION:
      g_value_set_boolean (value, self->auto_saturation);
      break;
    case GST_VA_FILTER_PROP_AUTO_BRIGHTNESS:
      g_value_set_boolean (value, self->auto_brightness);
      break;
    case GST_VA_FILTER_PROP_AUTO_CONTRAST:
      g_value_set_boolean (value, self->auto_contrast);
      break;
    case GST_VA_FILTER_PROP_DISABLE_PASSTHROUGH:
      g_value_set_boolean (value, (self->op_flags & VPP_CONVERT_DUMMY));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (object);
}

static gboolean
gst_va_vpp_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  /* if we are not passthrough, we can handle crop meta */
  if (decide_query)
    gst_query_add_allocation_meta (query, GST_VIDEO_CROP_META_API_TYPE, NULL);

  return GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation (trans,
      decide_query, query);
}

static void
gst_va_vpp_update_properties (GstVaBaseTransform * btrans)
{
  GstVaVpp *self = GST_VA_VPP (btrans);

  gst_va_vpp_rebuild_filters (self);
  _update_properties_unlocked (self);
}

static gboolean
gst_va_vpp_set_info (GstVaBaseTransform * btrans, GstCaps * incaps,
    GstVideoInfo * in_info, GstCaps * outcaps, GstVideoInfo * out_info)
{
  GstVaVpp *self = GST_VA_VPP (btrans);
  GstCapsFeatures *infeat, *outfeat;

  if (!gst_video_info_is_equal (in_info, out_info)) {
    if (GST_VIDEO_INFO_FORMAT (in_info) != GST_VIDEO_INFO_FORMAT (out_info))
      self->op_flags |= VPP_CONVERT_FORMAT;
    else
      self->op_flags &= ~VPP_CONVERT_FORMAT;

    if (GST_VIDEO_INFO_WIDTH (in_info) != GST_VIDEO_INFO_WIDTH (out_info)
        || GST_VIDEO_INFO_HEIGHT (in_info) != GST_VIDEO_INFO_HEIGHT (out_info))
      self->op_flags |= VPP_CONVERT_SIZE;
    else
      self->op_flags &= ~VPP_CONVERT_SIZE;
  } else {
    self->op_flags &= ~VPP_CONVERT_FORMAT & ~VPP_CONVERT_SIZE;
  }

  infeat = gst_caps_get_features (incaps, 0);
  outfeat = gst_caps_get_features (outcaps, 0);
  if (!gst_caps_features_is_equal (infeat, outfeat))
    self->op_flags |= VPP_CONVERT_FEATURE;
  else
    self->op_flags &= ~VPP_CONVERT_FEATURE;

  if (gst_va_filter_set_video_info (btrans->filter, in_info, out_info)) {
    gst_va_vpp_update_passthrough (self, FALSE);
    return TRUE;
  }

  return FALSE;
}

static inline gboolean
_get_filter_value (GstVaVpp * self, VAProcFilterType type, gfloat * value)
{
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (self);
  switch (type) {
    case VAProcFilterNoiseReduction:
      *value = self->denoise;
      break;
    case VAProcFilterSharpening:
      *value = self->sharpen;
      break;
    case VAProcFilterSkinToneEnhancement:
      *value = self->skintone;
      break;
    default:
      ret = FALSE;
      break;
  }
  GST_OBJECT_UNLOCK (self);

  return ret;
}

static inline gboolean
_add_filter_buffer (GstVaVpp * self, VAProcFilterType type,
    const VAProcFilterCap * cap)
{
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);
  VAProcFilterParameterBuffer param;
  gfloat value = 0;

  if (!_get_filter_value (self, type, &value))
    return FALSE;
  if (value == cap->range.default_value)
    return FALSE;

  /* *INDENT-OFF* */
  param = (VAProcFilterParameterBuffer) {
    .type = type,
    .value = value,
  };
  /* *INDENT-ON* */

  return gst_va_filter_add_filter_buffer (btrans->filter, &param,
      sizeof (param), 1);
}

static inline gboolean
_get_filter_cb_value (GstVaVpp * self, VAProcColorBalanceType type,
    gfloat * value)
{
  gboolean ret = TRUE;

  GST_OBJECT_LOCK (self);
  switch (type) {
    case VAProcColorBalanceHue:
      *value = self->hue;
      break;
    case VAProcColorBalanceSaturation:
      *value = self->saturation;
      break;
    case VAProcColorBalanceBrightness:
      *value = self->brightness;
      break;
    case VAProcColorBalanceContrast:
      *value = self->contrast;
      break;
    case VAProcColorBalanceAutoSaturation:
      *value = self->auto_saturation;
      break;
    case VAProcColorBalanceAutoBrightness:
      *value = self->auto_brightness;
      break;
    case VAProcColorBalanceAutoContrast:
      *value = self->auto_contrast;
      break;
    default:
      ret = FALSE;
      break;
  }
  GST_OBJECT_UNLOCK (self);

  return ret;
}

static inline gboolean
_add_filter_cb_buffer (GstVaVpp * self,
    const VAProcFilterCapColorBalance * caps, guint num_caps)
{
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);
  VAProcFilterParameterBufferColorBalance param[VAProcColorBalanceCount] =
      { 0, };
  gfloat value;
  guint i, c = 0;

  value = 0;
  for (i = 0; i < num_caps && i < VAProcColorBalanceCount; i++) {
    if (!_get_filter_cb_value (self, caps[i].type, &value))
      continue;
    if (value == caps[i].range.default_value)
      continue;

    /* *INDENT-OFF* */
    param[c++] = (VAProcFilterParameterBufferColorBalance) {
      .type = VAProcFilterColorBalance,
      .attrib = caps[i].type,
      .value = value,
    };
    /* *INDENT-ON* */
  }

  if (c > 0) {
    return gst_va_filter_add_filter_buffer (btrans->filter, param,
        sizeof (*param), c);
  }
  return FALSE;
}

static void
_build_filters (GstVaVpp * self)
{
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);
  static const VAProcFilterType filter_types[] = { VAProcFilterNoiseReduction,
    VAProcFilterSharpening, VAProcFilterSkinToneEnhancement,
    VAProcFilterColorBalance,
  };
  guint i, num_caps;
  gboolean apply = FALSE;

  for (i = 0; i < G_N_ELEMENTS (filter_types); i++) {
    const gpointer caps = gst_va_filter_get_filter_caps (btrans->filter,
        filter_types[i], &num_caps);
    if (!caps)
      continue;

    switch (filter_types[i]) {
      case VAProcFilterNoiseReduction:
        apply |= _add_filter_buffer (self, filter_types[i], caps);
        break;
      case VAProcFilterSharpening:
        apply |= _add_filter_buffer (self, filter_types[i], caps);
        break;
      case VAProcFilterSkinToneEnhancement:
        apply |= _add_filter_buffer (self, filter_types[i], caps);
        break;
      case VAProcFilterColorBalance:
        apply |= _add_filter_cb_buffer (self, caps, num_caps);
        break;
      default:
        break;
    }
  }

  GST_OBJECT_LOCK (self);
  if (apply)
    self->op_flags |= VPP_CONVERT_FILTERS;
  else
    self->op_flags &= ~VPP_CONVERT_FILTERS;
  GST_OBJECT_UNLOCK (self);
}

static void
gst_va_vpp_rebuild_filters (GstVaVpp * self)
{
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);

  if (!g_atomic_int_get (&self->rebuild_filters))
    return;

  gst_va_filter_drop_filter_buffers (btrans->filter);
  _build_filters (self);
  g_atomic_int_set (&self->rebuild_filters, FALSE);
}

static void
gst_va_vpp_before_transform (GstBaseTransform * trans, GstBuffer * inbuf)
{
  GstVaVpp *self = GST_VA_VPP (trans);
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);
  GstClockTime ts, stream_time;
  gboolean is_passthrough;

  ts = GST_BUFFER_TIMESTAMP (inbuf);
  stream_time =
      gst_segment_to_stream_time (&trans->segment, GST_FORMAT_TIME, ts);

  GST_TRACE_OBJECT (self, "sync to %" GST_TIME_FORMAT, GST_TIME_ARGS (ts));

  if (GST_CLOCK_TIME_IS_VALID (stream_time))
    gst_object_sync_values (GST_OBJECT (self), stream_time);

  gst_va_vpp_rebuild_filters (self);
  gst_va_vpp_update_passthrough (self, TRUE);

  /* cropping is only enabled if vapostproc is not in passthrough */
  is_passthrough = gst_base_transform_is_passthrough (trans);
  GST_OBJECT_LOCK (self);
  if (!is_passthrough && gst_buffer_get_video_crop_meta (inbuf)) {
    self->op_flags |= VPP_CONVERT_CROP;
  } else {
    self->op_flags &= ~VPP_CONVERT_CROP;
  }
  gst_va_filter_enable_cropping (btrans->filter,
      (self->op_flags & VPP_CONVERT_CROP));
  GST_OBJECT_UNLOCK (self);
}

static GstFlowReturn
gst_va_vpp_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVaVpp *self = GST_VA_VPP (trans);
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (trans);
  GstBuffer *buf = NULL;
  GstFlowReturn res = GST_FLOW_OK;
  GstVaSample src, dst;

  if (G_UNLIKELY (!btrans->negotiated))
    goto unknown_format;

  res = gst_va_base_transform_import_buffer (btrans, inbuf, &buf);
  if (res != GST_FLOW_OK)
    return res;

  /* *INDENT-OFF* */
  src = (GstVaSample) {
    .buffer = buf,
  };

  dst = (GstVaSample) {
    .buffer = outbuf,
  };
  /* *INDENT-ON* */

  if (!gst_va_filter_process (btrans->filter, &src, &dst)) {
    gst_buffer_set_flags (outbuf, GST_BUFFER_FLAG_CORRUPTED);
  }

  gst_buffer_unref (buf);

  return res;

  /* ERRORS */
unknown_format:
  {
    GST_ELEMENT_ERROR (self, CORE, NOT_IMPLEMENTED, (NULL), ("unknown format"));
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static gboolean
gst_va_vpp_transform_meta (GstBaseTransform * trans, GstBuffer * inbuf,
    GstMeta * meta, GstBuffer * outbuf)
{
  GstVaVpp *self = GST_VA_VPP (trans);
  const GstMetaInfo *info = meta->info;
  const gchar *const *tags;

  tags = gst_meta_api_type_get_tags (info->api);

  if (!tags)
    return TRUE;

  /* don't copy colorspace/size/orientation specific metadata */
  if ((self->op_flags & VPP_CONVERT_FORMAT)
      && gst_meta_api_type_has_tag (info->api, META_TAG_COLORSPACE))
    return FALSE;
  else if ((self->op_flags & (VPP_CONVERT_SIZE | VPP_CONVERT_CROP))
      && gst_meta_api_type_has_tag (info->api, META_TAG_SIZE))
    return FALSE;
  else if ((self->op_flags & VPP_CONVERT_DIRECTION)
      && gst_meta_api_type_has_tag (info->api, META_TAG_ORIENTATION))
    return FALSE;
  else if (gst_meta_api_type_has_tag (info->api, META_TAG_VIDEO))
    return TRUE;

  return GST_BASE_TRANSFORM_CLASS (parent_class)->transform_meta (trans, outbuf,
      meta, inbuf);
}

/* Remove all the info for the cases when we can actually convert:
 * Delete all the video "format", rangify the resolution size, also
 * remove "colorimetry", "chroma-site" and "pixel-aspect-ratio".  All
 * the missing caps features should be added based on the template,
 * and the caps features' order in @caps is kept */
static GstCaps *
gst_va_vpp_complete_caps_features (GstCaps * caps, GstCaps * tmpl_caps)
{
  GstCaps *ret, *full_caps;
  GstStructure *structure;
  GstCapsFeatures *features;
  gboolean has_sys_mem = FALSE, has_dma = FALSE, has_va = FALSE;
  gint i, n;

  full_caps = gst_caps_new_empty ();

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (caps, i);
    features = gst_caps_get_features (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0
        && gst_caps_is_subset_structure_full (full_caps, structure, features))
      continue;

    if (gst_caps_features_is_any (features))
      continue;

    if (gst_caps_features_is_equal (features,
            GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)) {
      has_sys_mem = TRUE;
    } else {
      gboolean valid = FALSE;

      if (gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
        has_dma = TRUE;
        valid = TRUE;
      }
      if (gst_caps_features_contains (features, "memory:VAMemory")) {
        has_va = TRUE;
        valid = TRUE;
      }
      /* Not contain our supported feature */
      if (!valid)
        continue;
    }

    structure = gst_structure_copy (structure);

    gst_structure_set (structure, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
    /* if pixel aspect ratio, make a range of it */
    if (gst_structure_has_field (structure, "pixel-aspect-ratio")) {
      gst_structure_set (structure, "pixel-aspect-ratio",
          GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1, NULL);
    }
    gst_structure_remove_fields (structure, "format", "colorimetry",
        "chroma-site", NULL);

    gst_caps_append_structure_full (full_caps, structure,
        gst_caps_features_copy (features));
  }

  /* Adding the missing features. */
  n = gst_caps_get_size (tmpl_caps);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (tmpl_caps, i);
    features = gst_caps_get_features (tmpl_caps, i);

    if (gst_caps_features_contains (features, "memory:VAMemory") && !has_va)
      gst_caps_append_structure_full (full_caps, gst_structure_copy (structure),
          gst_caps_features_copy (features));

    if (gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_DMABUF) && !has_dma)
      gst_caps_append_structure_full (full_caps, gst_structure_copy (structure),
          gst_caps_features_copy (features));

    if (gst_caps_features_is_equal (features,
            GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY) && !has_sys_mem)
      gst_caps_append_structure_full (full_caps, gst_structure_copy (structure),
          gst_caps_features_copy (features));
  }

  ret = gst_caps_intersect_full (full_caps, tmpl_caps,
      GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (full_caps);

  return ret;
}

static GstCaps *
gst_va_vpp_transform_caps (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, GstCaps * filter)
{
  GstVaVpp *self = GST_VA_VPP (trans);
  GstCaps *ret, *tmpl_caps;

  GST_DEBUG_OBJECT (self,
      "Transforming caps %" GST_PTR_FORMAT " in direction %s", caps,
      (direction == GST_PAD_SINK) ? "sink" : "src");

  if (direction == GST_PAD_SINK) {
    tmpl_caps =
        gst_pad_get_pad_template_caps (GST_BASE_TRANSFORM_SRC_PAD (trans));
  } else {
    tmpl_caps =
        gst_pad_get_pad_template_caps (GST_BASE_TRANSFORM_SINK_PAD (trans));
  }

  ret = gst_va_vpp_complete_caps_features (caps, tmpl_caps);

  gst_caps_unref (tmpl_caps);

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = intersection;
  }

  GST_DEBUG_OBJECT (trans, "returning caps: %" GST_PTR_FORMAT, ret);

  return ret;
}

/*
 * This is an incomplete matrix of in formats and a score for the preferred output
 * format.
 *
 *         out: RGB24   RGB16  ARGB  AYUV  YUV444  YUV422 YUV420 YUV411 YUV410  PAL  GRAY
 *  in
 * RGB24          0      2       1     2     2       3      4      5      6      7    8
 * RGB16          1      0       1     2     2       3      4      5      6      7    8
 * ARGB           2      3       0     1     4       5      6      7      8      9    10
 * AYUV           3      4       1     0     2       5      6      7      8      9    10
 * YUV444         2      4       3     1     0       5      6      7      8      9    10
 * YUV422         3      5       4     2     1       0      6      7      8      9    10
 * YUV420         4      6       5     3     2       1      0      7      8      9    10
 * YUV411         4      6       5     3     2       1      7      0      8      9    10
 * YUV410         6      8       7     5     4       3      2      1      0      9    10
 * PAL            1      3       2     6     4       6      7      8      9      0    10
 * GRAY           1      4       3     2     1       5      6      7      8      9    0
 *
 * PAL or GRAY are never preferred, if we can we would convert to PAL instead
 * of GRAY, though
 * less subsampling is preferred and if any, preferably horizontal
 * We would like to keep the alpha, even if we would need to to colorspace conversion
 * or lose depth.
 */
#define SCORE_FORMAT_CHANGE       1
#define SCORE_DEPTH_CHANGE        1
#define SCORE_ALPHA_CHANGE        1
#define SCORE_CHROMA_W_CHANGE     1
#define SCORE_CHROMA_H_CHANGE     1
#define SCORE_PALETTE_CHANGE      1

#define SCORE_COLORSPACE_LOSS     2     /* RGB <-> YUV */
#define SCORE_DEPTH_LOSS          4     /* change bit depth */
#define SCORE_ALPHA_LOSS          8     /* lose the alpha channel */
#define SCORE_CHROMA_W_LOSS      16     /* vertical subsample */
#define SCORE_CHROMA_H_LOSS      32     /* horizontal subsample */
#define SCORE_PALETTE_LOSS       64     /* convert to palette format */
#define SCORE_COLOR_LOSS        128     /* convert to GRAY */

#define COLORSPACE_MASK (GST_VIDEO_FORMAT_FLAG_YUV | \
                         GST_VIDEO_FORMAT_FLAG_RGB | GST_VIDEO_FORMAT_FLAG_GRAY)
#define ALPHA_MASK      (GST_VIDEO_FORMAT_FLAG_ALPHA)
#define PALETTE_MASK    (GST_VIDEO_FORMAT_FLAG_PALETTE)

/* calculate how much loss a conversion would be */
static void
score_value (GstVaVpp * self, const GstVideoFormatInfo * in_info,
    const GValue * val, gint * min_loss, const GstVideoFormatInfo ** out_info)
{
  const gchar *fname;
  const GstVideoFormatInfo *t_info;
  GstVideoFormatFlags in_flags, t_flags;
  gint loss;

  fname = g_value_get_string (val);
  t_info = gst_video_format_get_info (gst_video_format_from_string (fname));
  if (!t_info || t_info->format == GST_VIDEO_FORMAT_UNKNOWN)
    return;

  /* accept input format immediately without loss */
  if (in_info == t_info) {
    *min_loss = 0;
    *out_info = t_info;
    return;
  }

  loss = SCORE_FORMAT_CHANGE;

  in_flags = GST_VIDEO_FORMAT_INFO_FLAGS (in_info);
  in_flags &= ~GST_VIDEO_FORMAT_FLAG_LE;
  in_flags &= ~GST_VIDEO_FORMAT_FLAG_COMPLEX;
  in_flags &= ~GST_VIDEO_FORMAT_FLAG_UNPACK;

  t_flags = GST_VIDEO_FORMAT_INFO_FLAGS (t_info);
  t_flags &= ~GST_VIDEO_FORMAT_FLAG_LE;
  t_flags &= ~GST_VIDEO_FORMAT_FLAG_COMPLEX;
  t_flags &= ~GST_VIDEO_FORMAT_FLAG_UNPACK;

  if ((t_flags & PALETTE_MASK) != (in_flags & PALETTE_MASK)) {
    loss += SCORE_PALETTE_CHANGE;
    if (t_flags & PALETTE_MASK)
      loss += SCORE_PALETTE_LOSS;
  }

  if ((t_flags & COLORSPACE_MASK) != (in_flags & COLORSPACE_MASK)) {
    loss += SCORE_COLORSPACE_LOSS;
    if (t_flags & GST_VIDEO_FORMAT_FLAG_GRAY)
      loss += SCORE_COLOR_LOSS;
  }

  if ((t_flags & ALPHA_MASK) != (in_flags & ALPHA_MASK)) {
    loss += SCORE_ALPHA_CHANGE;
    if (in_flags & ALPHA_MASK)
      loss += SCORE_ALPHA_LOSS;
  }

  if ((in_info->h_sub[1]) != (t_info->h_sub[1])) {
    loss += SCORE_CHROMA_H_CHANGE;
    if ((in_info->h_sub[1]) < (t_info->h_sub[1]))
      loss += SCORE_CHROMA_H_LOSS;
  }
  if ((in_info->w_sub[1]) != (t_info->w_sub[1])) {
    loss += SCORE_CHROMA_W_CHANGE;
    if ((in_info->w_sub[1]) < (t_info->w_sub[1]))
      loss += SCORE_CHROMA_W_LOSS;
  }

  if ((in_info->bits) != (t_info->bits)) {
    loss += SCORE_DEPTH_CHANGE;
    if ((in_info->bits) > (t_info->bits))
      loss += SCORE_DEPTH_LOSS;
  }

  GST_DEBUG_OBJECT (self, "score %s -> %s = %d",
      GST_VIDEO_FORMAT_INFO_NAME (in_info),
      GST_VIDEO_FORMAT_INFO_NAME (t_info), loss);

  if (loss < *min_loss) {
    GST_DEBUG_OBJECT (self, "found new best %d", loss);
    *out_info = t_info;
    *min_loss = loss;
  }
}

static void
gst_va_vpp_fixate_format (GstVaVpp * self, GstCaps * caps, GstCaps * result)
{
  GstStructure *ins, *outs;
  const gchar *in_format;
  const GstVideoFormatInfo *in_info, *out_info = NULL;
  gint min_loss = G_MAXINT;
  guint i, capslen;

  ins = gst_caps_get_structure (caps, 0);
  in_format = gst_structure_get_string (ins, "format");
  if (!in_format)
    return;

  GST_DEBUG_OBJECT (self, "source format %s", in_format);

  in_info =
      gst_video_format_get_info (gst_video_format_from_string (in_format));
  if (!in_info)
    return;

  outs = gst_caps_get_structure (result, 0);

  capslen = gst_caps_get_size (result);
  GST_DEBUG_OBJECT (self, "iterate %d structures", capslen);
  for (i = 0; i < capslen; i++) {
    GstStructure *tests;
    const GValue *format;

    tests = gst_caps_get_structure (result, i);
    format = gst_structure_get_value (tests, "format");
    gst_structure_remove_fields (tests, "height", "width", "pixel-aspect-ratio",
        "display-aspect-ratio", NULL);
    /* should not happen */
    if (format == NULL)
      continue;

    if (GST_VALUE_HOLDS_LIST (format)) {
      gint j, len;

      len = gst_value_list_get_size (format);
      GST_DEBUG_OBJECT (self, "have %d formats", len);
      for (j = 0; j < len; j++) {
        const GValue *val;

        val = gst_value_list_get_value (format, j);
        if (G_VALUE_HOLDS_STRING (val)) {
          score_value (self, in_info, val, &min_loss, &out_info);
          if (min_loss == 0)
            break;
        }
      }
    } else if (G_VALUE_HOLDS_STRING (format)) {
      score_value (self, in_info, format, &min_loss, &out_info);
    }
  }
  if (out_info)
    gst_structure_set (outs, "format", G_TYPE_STRING,
        GST_VIDEO_FORMAT_INFO_NAME (out_info), NULL);
}

static GstCaps *
gst_va_vpp_get_fixed_format (GstVaVpp * self, GstPadDirection direction,
    GstCaps * caps, GstCaps * othercaps)
{
  GstCaps *result;

  result = gst_caps_intersect (othercaps, caps);
  if (gst_caps_is_empty (result)) {
    gst_caps_unref (result);
    result = gst_caps_copy (othercaps);
  }

  gst_va_vpp_fixate_format (self, caps, result);

  /* fixate remaining fields */
  result = gst_caps_fixate (result);

  if (direction == GST_PAD_SINK) {
    if (gst_caps_is_subset (caps, result)) {
      gst_caps_replace (&result, caps);
    }
  }

  return result;
}

static GstCaps *
gst_va_vpp_fixate_size (GstVaVpp * self, GstPadDirection direction,
    GstCaps * caps, GstCaps * othercaps)
{
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);
  GstStructure *ins, *outs;
  const GValue *from_par, *to_par;
  GValue fpar = { 0, };
  GValue tpar = { 0, };

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);
  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  from_par = gst_structure_get_value (ins, "pixel-aspect-ratio");
  to_par = gst_structure_get_value (outs, "pixel-aspect-ratio");

  /* If we're fixating from the sinkpad we always set the PAR and
   * assume that missing PAR on the sinkpad means 1/1 and
   * missing PAR on the srcpad means undefined
   */
  if (direction == GST_PAD_SINK) {
    if (!from_par) {
      g_value_init (&fpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&fpar, 1, 1);
      from_par = &fpar;
    }
    if (!to_par) {
      g_value_init (&tpar, GST_TYPE_FRACTION_RANGE);
      gst_value_set_fraction_range_full (&tpar, 1, G_MAXINT, G_MAXINT, 1);
      to_par = &tpar;
    }
  } else {
    if (!to_par) {
      g_value_init (&tpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&tpar, 1, 1);
      to_par = &tpar;

      gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);
    }
    if (!from_par) {
      g_value_init (&fpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&fpar, 1, 1);
      from_par = &fpar;
    }
  }

  /* we have both PAR but they might not be fixated */
  {
    gint from_w, from_h, from_par_n, from_par_d, to_par_n, to_par_d;
    gint w = 0, h = 0;
    gint from_dar_n, from_dar_d;
    gint num, den;

    /* from_par should be fixed */
    g_return_val_if_fail (gst_value_is_fixed (from_par), othercaps);

    from_par_n = gst_value_get_fraction_numerator (from_par);
    from_par_d = gst_value_get_fraction_denominator (from_par);

    gst_structure_get_int (ins, "width", &from_w);
    gst_structure_get_int (ins, "height", &from_h);

    gst_structure_get_int (outs, "width", &w);
    gst_structure_get_int (outs, "height", &h);

    /* if video-orientation changes */
    switch (gst_va_filter_get_orientation (btrans->filter)) {
      case GST_VIDEO_ORIENTATION_90R:
      case GST_VIDEO_ORIENTATION_90L:
      case GST_VIDEO_ORIENTATION_UL_LR:
      case GST_VIDEO_ORIENTATION_UR_LL:
        if (direction == GST_PAD_SINK) {
          SWAP (from_w, from_h);
          SWAP (from_par_n, from_par_d);
        } else if (direction == GST_PAD_SRC) {
          SWAP (w, h);
          /* there's no need to swap 1/1 par */
        }
        break;
      default:
        break;
    }

    /* if both width and height are already fixed, we can't do anything
     * about it anymore */
    if (w && h) {
      guint n, d;

      GST_DEBUG_OBJECT (self, "dimensions already set to %dx%d, not fixating",
          w, h);
      if (!gst_value_is_fixed (to_par)) {
        if (gst_video_calculate_display_ratio (&n, &d, from_w, from_h,
                from_par_n, from_par_d, w, h)) {
          GST_DEBUG_OBJECT (self, "fixating to_par to %dx%d", n, d);
          if (gst_structure_has_field (outs, "pixel-aspect-ratio"))
            gst_structure_fixate_field_nearest_fraction (outs,
                "pixel-aspect-ratio", n, d);
          else if (n != d)
            gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                n, d, NULL);
        }
      }
      goto done;
    }

    /* Calculate input DAR */
    if (!gst_util_fraction_multiply (from_w, from_h, from_par_n, from_par_d,
            &from_dar_n, &from_dar_d)) {
      GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
          ("Error calculating the output scaled size - integer overflow"));
      goto done;
    }

    GST_DEBUG_OBJECT (self, "Input DAR is %d/%d", from_dar_n, from_dar_d);

    /* If either width or height are fixed there's not much we
     * can do either except choosing a height or width and PAR
     * that matches the DAR as good as possible
     */
    if (h) {
      GstStructure *tmp;
      gint set_w, set_par_n, set_par_d;

      GST_DEBUG_OBJECT (self, "height is fixed (%d)", h);

      /* If the PAR is fixed too, there's not much to do
       * except choosing the width that is nearest to the
       * width with the same DAR */
      if (gst_value_is_fixed (to_par)) {
        to_par_n = gst_value_get_fraction_numerator (to_par);
        to_par_d = gst_value_get_fraction_denominator (to_par);

        GST_DEBUG_OBJECT (self, "PAR is fixed %d/%d", to_par_n, to_par_d);

        if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_d,
                to_par_n, &num, &den)) {
          GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
              ("Error calculating the output scaled size - integer overflow"));
          goto done;
        }

        w = (guint) gst_util_uint64_scale_int_round (h, num, den);
        gst_structure_fixate_field_nearest_int (outs, "width", w);

        goto done;
      }

      /* The PAR is not fixed and it's quite likely that we can set
       * an arbitrary PAR. */

      /* Check if we can keep the input width */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      /* Might have failed but try to keep the DAR nonetheless by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, h, set_w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }

      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      /* Check if the adjusted PAR is accepted */
      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "width", G_TYPE_INT, set_w,
              "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        goto done;
      }

      /* Otherwise scale the width to the new PAR and check if the
       * adjusted with is accepted. If all that fails we can't keep
       * the DAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      w = (guint) gst_util_uint64_scale_int_round (h, num, den);
      gst_structure_fixate_field_nearest_int (outs, "width", w);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);

      goto done;
    } else if (w) {
      GstStructure *tmp;
      gint set_h, set_par_n, set_par_d;

      GST_DEBUG_OBJECT (self, "width is fixed (%d)", w);

      /* If the PAR is fixed too, there's not much to do
       * except choosing the height that is nearest to the
       * height with the same DAR */
      if (gst_value_is_fixed (to_par)) {
        to_par_n = gst_value_get_fraction_numerator (to_par);
        to_par_d = gst_value_get_fraction_denominator (to_par);

        GST_DEBUG_OBJECT (self, "PAR is fixed %d/%d", to_par_n, to_par_d);

        if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_d,
                to_par_n, &num, &den)) {
          GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
              ("Error calculating the output scaled size - integer overflow"));
          goto done;
        }

        h = (guint) gst_util_uint64_scale_int_round (w, den, num);
        gst_structure_fixate_field_nearest_int (outs, "height", h);

        goto done;
      }

      /* The PAR is not fixed and it's quite likely that we can set
       * an arbitrary PAR. */

      /* Check if we can keep the input height */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);

      /* Might have failed but try to keep the DAR nonetheless by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_h, w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }
      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      /* Check if the adjusted PAR is accepted */
      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "height", G_TYPE_INT, set_h,
              "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        goto done;
      }

      /* Otherwise scale the height to the new PAR and check if the
       * adjusted with is accepted. If all that fails we can't keep
       * the DAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scale sized - integer overflow"));
        goto done;
      }

      h = (guint) gst_util_uint64_scale_int_round (w, den, num);
      gst_structure_fixate_field_nearest_int (outs, "height", h);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);

      goto done;
    } else if (gst_value_is_fixed (to_par)) {
      GstStructure *tmp;
      gint set_h, set_w, f_h, f_w;

      to_par_n = gst_value_get_fraction_numerator (to_par);
      to_par_d = gst_value_get_fraction_denominator (to_par);

      /* Calculate scale factor for the PAR change */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_n,
              to_par_d, &num, &den)) {
        GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      /* Try to keep the input height (because of interlacing) */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);

      /* This might have failed but try to scale the width
       * to keep the DAR nonetheless */
      w = (guint) gst_util_uint64_scale_int_round (set_h, num, den);
      gst_structure_fixate_field_nearest_int (tmp, "width", w);
      gst_structure_get_int (tmp, "width", &set_w);
      gst_structure_free (tmp);

      /* We kept the DAR and the height is nearest to the original height */
      if (set_w == w) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);
        goto done;
      }

      f_h = set_h;
      f_w = set_w;

      /* If the former failed, try to keep the input width at least */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      /* This might have failed but try to scale the width
       * to keep the DAR nonetheless */
      h = (guint) gst_util_uint64_scale_int_round (set_w, den, num);
      gst_structure_fixate_field_nearest_int (tmp, "height", h);
      gst_structure_get_int (tmp, "height", &set_h);
      gst_structure_free (tmp);

      /* We kept the DAR and the width is nearest to the original width */
      if (set_h == h) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);
        goto done;
      }

      /* If all this failed, keep the dimensions with the DAR that was closest
       * to the correct DAR. This changes the DAR but there's not much else to
       * do here.
       */
      if (set_w * ABS (set_h - h) < ABS (f_w - w) * f_h) {
        f_h = set_h;
        f_w = set_w;
      }
      gst_structure_set (outs, "width", G_TYPE_INT, f_w, "height", G_TYPE_INT,
          f_h, NULL);
      goto done;
    } else {
      GstStructure *tmp;
      gint set_h, set_w, set_par_n, set_par_d, tmp2;

      /* width, height and PAR are not fixed but passthrough is not possible */

      /* First try to keep the height and width as good as possible
       * and scale PAR */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_h, set_w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }

      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);

        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* Otherwise try to scale width to keep the DAR with the set
       * PAR and height */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      w = (guint) gst_util_uint64_scale_int_round (set_h, num, den);
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", w);
      gst_structure_get_int (tmp, "width", &tmp2);
      gst_structure_free (tmp);

      if (tmp2 == w) {
        gst_structure_set (outs, "width", G_TYPE_INT, tmp2, "height",
            G_TYPE_INT, set_h, NULL);
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* ... or try the same with the height */
      h = (guint) gst_util_uint64_scale_int_round (set_w, den, num);
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", h);
      gst_structure_get_int (tmp, "height", &tmp2);
      gst_structure_free (tmp);

      if (tmp2 == h) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, tmp2, NULL);
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* If all fails we can't keep the DAR and take the nearest values
       * for everything from the first try */
      gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
          G_TYPE_INT, set_h, NULL);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);
    }
  }

done:
  if (from_par == &fpar)
    g_value_unset (&fpar);
  if (to_par == &tpar)
    g_value_unset (&tpar);

  return othercaps;
}

static GstCaps *
gst_va_vpp_fixate_caps (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, GstCaps * othercaps)
{
  GstVaVpp *self = GST_VA_VPP (trans);
  GstCaps *format;

  GST_DEBUG_OBJECT (self,
      "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %"
      GST_PTR_FORMAT, othercaps, caps);

  format = gst_va_vpp_get_fixed_format (self, direction, caps, othercaps);

  if (gst_caps_is_empty (format)) {
    GST_ERROR_OBJECT (self, "Could not convert formats");
    return format;
  }

  othercaps = gst_va_vpp_fixate_size (self, direction, caps, othercaps);
  if (gst_caps_get_size (othercaps) == 1) {
    gint i;
    const gchar *format_fields[] = { "format", "colorimetry", "chroma-site" };
    GstStructure *format_struct = gst_caps_get_structure (format, 0);
    GstStructure *fixated_struct;

    othercaps = gst_caps_make_writable (othercaps);
    fixated_struct = gst_caps_get_structure (othercaps, 0);

    for (i = 0; i < G_N_ELEMENTS (format_fields); i++) {
      if (gst_structure_has_field (format_struct, format_fields[i])) {
        gst_structure_set (fixated_struct, format_fields[i], G_TYPE_STRING,
            gst_structure_get_string (format_struct, format_fields[i]), NULL);
      } else {
        gst_structure_remove_field (fixated_struct, format_fields[i]);
      }
    }

    /* copy the framerate */
    {
      const GValue *framerate =
          gst_structure_get_value (fixated_struct, "framerate");
      if (framerate && !gst_value_is_fixed (framerate)) {
        GstStructure *st = gst_caps_get_structure (caps, 0);
        const GValue *fixated_framerate =
            gst_structure_get_value (st, "framerate");
        gst_structure_set_value (fixated_struct, "framerate",
            fixated_framerate);
      }
    }
  }
  gst_caps_unref (format);

  GST_DEBUG_OBJECT (self, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);

  return othercaps;
}

static void
_get_scale_factor (GstVaVpp * self, gdouble * w_factor, gdouble * h_factor)
{
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (self);
  gdouble w = GST_VIDEO_INFO_WIDTH (&btrans->in_info);
  gdouble h = GST_VIDEO_INFO_HEIGHT (&btrans->in_info);

  switch (self->direction) {
    case GST_VIDEO_ORIENTATION_90R:
    case GST_VIDEO_ORIENTATION_90L:
    case GST_VIDEO_ORIENTATION_UR_LL:
    case GST_VIDEO_ORIENTATION_UL_LR:{
      gdouble tmp = h;
      h = w;
      w = tmp;
      break;
    }
    default:
      break;
  }

  *w_factor = GST_VIDEO_INFO_WIDTH (&btrans->out_info);
  *w_factor /= w;

  *h_factor = GST_VIDEO_INFO_HEIGHT (&btrans->out_info);
  *h_factor /= h;
}

static gboolean
gst_va_vpp_src_event (GstBaseTransform * trans, GstEvent * event)
{
  GstVaVpp *self = GST_VA_VPP (trans);
  GstVaBaseTransform *btrans = GST_VA_BASE_TRANSFORM (trans);
  GstStructure *structure;
  const GstVideoInfo *in_info = &btrans->in_info, *out_info = &btrans->out_info;
  gdouble new_x = 0, new_y = 0, x = 0, y = 0, w_factor = 1, h_factor = 1;
  gboolean ret;

  GST_TRACE_OBJECT (self, "handling %s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NAVIGATION:
      if (GST_VIDEO_INFO_WIDTH (in_info) != GST_VIDEO_INFO_WIDTH (out_info)
          || GST_VIDEO_INFO_HEIGHT (in_info) != GST_VIDEO_INFO_HEIGHT (out_info)
          || gst_va_filter_get_orientation (btrans->filter) !=
          GST_VIDEO_ORIENTATION_IDENTITY) {

        event =
            GST_EVENT (gst_mini_object_make_writable (GST_MINI_OBJECT (event)));

        structure = (GstStructure *) gst_event_get_structure (event);
        if (!gst_structure_get_double (structure, "pointer_x", &x)
            || !gst_structure_get_double (structure, "pointer_y", &y))
          break;

        /* video-direction compensation */
        switch (self->direction) {
          case GST_VIDEO_ORIENTATION_90R:
            new_x = y;
            new_y = GST_VIDEO_INFO_WIDTH (in_info) - 1 - x;
            break;
          case GST_VIDEO_ORIENTATION_90L:
            new_x = GST_VIDEO_INFO_HEIGHT (in_info) - 1 - y;
            new_y = x;
            break;
          case GST_VIDEO_ORIENTATION_UR_LL:
            new_x = GST_VIDEO_INFO_HEIGHT (in_info) - 1 - y;
            new_y = GST_VIDEO_INFO_WIDTH (in_info) - 1 - x;
            break;
          case GST_VIDEO_ORIENTATION_UL_LR:
            new_x = y;
            new_y = x;
            break;
          case GST_VIDEO_ORIENTATION_180:
            /* FIXME: is this correct? */
            new_x = GST_VIDEO_INFO_WIDTH (in_info) - 1 - x;
            new_y = GST_VIDEO_INFO_HEIGHT (in_info) - 1 - y;
            break;
          case GST_VIDEO_ORIENTATION_HORIZ:
            new_x = GST_VIDEO_INFO_WIDTH (in_info) - 1 - x;
            new_y = y;
            break;
          case GST_VIDEO_ORIENTATION_VERT:
            new_x = x;
            new_y = GST_VIDEO_INFO_HEIGHT (in_info) - 1 - y;
            break;
          default:
            new_x = x;
            new_y = y;
            break;
        }

        /* scale compensation */
        _get_scale_factor (self, &w_factor, &h_factor);
        new_x *= w_factor;
        new_y *= h_factor;

        /* crop compensation is done by videocrop */

        GST_TRACE_OBJECT (self, "from %fx%f to %fx%f", x, y, new_x, new_y);
        gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE, new_x,
            "pointer_y", G_TYPE_DOUBLE, new_y, NULL);
      }
      break;
    default:
      break;
  }

  ret = GST_BASE_TRANSFORM_CLASS (parent_class)->src_event (trans, event);

  return ret;
}

static gboolean
gst_va_vpp_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstVaVpp *self = GST_VA_VPP (trans);
  GstTagList *taglist;
  gchar *orientation;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_TAG:
      gst_event_parse_tag (event, &taglist);

      if (!gst_tag_list_get_string (taglist, "image-orientation", &orientation))
        break;

      if (self->direction != GST_VIDEO_ORIENTATION_AUTO)
        break;

      GST_DEBUG_OBJECT (self, "tag orientation %s", orientation);

      GST_OBJECT_LOCK (self);
      if (!g_strcmp0 ("rotate-0", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_IDENTITY;
      else if (!g_strcmp0 ("rotate-90", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_90R;
      else if (!g_strcmp0 ("rotate-180", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_180;
      else if (!g_strcmp0 ("rotate-270", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_90L;
      else if (!g_strcmp0 ("flip-rotate-0", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_HORIZ;
      else if (!g_strcmp0 ("flip-rotate-90", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_UL_LR;
      else if (!g_strcmp0 ("flip-rotate-180", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_VERT;
      else if (!g_strcmp0 ("flip-rotate-270", orientation))
        self->tag_direction = GST_VIDEO_ORIENTATION_UR_LL;

      _update_properties_unlocked (self);
      GST_OBJECT_UNLOCK (self);

      gst_va_vpp_update_passthrough (self, FALSE);

      break;
    default:
      break;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->sink_event (trans, event);
}

static void
gst_va_vpp_class_init (gpointer g_class, gpointer class_data)
{
  GstCaps *doc_caps, *caps = NULL;
  GstPadTemplate *sink_pad_templ, *src_pad_templ;
  GObjectClass *object_class = G_OBJECT_CLASS (g_class);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (g_class);
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstVaBaseTransformClass *btrans_class = GST_VA_BASE_TRANSFORM_CLASS (g_class);
  GstVaDisplay *display;
  GstVaFilter *filter;
  struct CData *cdata = class_data;
  gchar *long_name;

  parent_class = g_type_class_peek_parent (g_class);

  btrans_class->render_device_path = g_strdup (cdata->render_device_path);

  if (cdata->description) {
    long_name = g_strdup_printf ("VA-API Video Postprocessor in %s",
        cdata->description);
  } else {
    long_name = g_strdup ("VA-API Video Postprocessor");
  }

  gst_element_class_set_metadata (element_class, long_name,
      "Filter/Converter/Video/Scaler/Hardware",
      "VA-API based video postprocessor",
      "Víctor Jáquez <vjaquez@igalia.com>");

  display = gst_va_display_drm_new_from_path (btrans_class->render_device_path);
  filter = gst_va_filter_new (display);

  if (gst_va_filter_open (filter))
    caps = gst_va_filter_get_caps (filter);
  else
    caps = gst_caps_from_string (caps_str);

  doc_caps = gst_caps_from_string (caps_str);

  sink_pad_templ = gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
      caps);
  gst_element_class_add_pad_template (element_class, sink_pad_templ);
  gst_pad_template_set_documentation_caps (sink_pad_templ,
      gst_caps_ref (doc_caps));

  src_pad_templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
      caps);
  gst_element_class_add_pad_template (element_class, src_pad_templ);
  gst_pad_template_set_documentation_caps (src_pad_templ,
      gst_caps_ref (doc_caps));
  gst_caps_unref (doc_caps);

  gst_caps_unref (caps);

  object_class->dispose = gst_va_vpp_dispose;
  object_class->set_property = gst_va_vpp_set_property;
  object_class->get_property = gst_va_vpp_get_property;

  trans_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_va_vpp_propose_allocation);
  trans_class->transform_caps = GST_DEBUG_FUNCPTR (gst_va_vpp_transform_caps);
  trans_class->fixate_caps = GST_DEBUG_FUNCPTR (gst_va_vpp_fixate_caps);
  trans_class->before_transform =
      GST_DEBUG_FUNCPTR (gst_va_vpp_before_transform);
  trans_class->transform = GST_DEBUG_FUNCPTR (gst_va_vpp_transform);
  trans_class->transform_meta = GST_DEBUG_FUNCPTR (gst_va_vpp_transform_meta);
  trans_class->src_event = GST_DEBUG_FUNCPTR (gst_va_vpp_src_event);
  trans_class->sink_event = GST_DEBUG_FUNCPTR (gst_va_vpp_sink_event);

  trans_class->transform_ip_on_passthrough = FALSE;

  btrans_class->set_info = GST_DEBUG_FUNCPTR (gst_va_vpp_set_info);
  btrans_class->update_properties =
      GST_DEBUG_FUNCPTR (gst_va_vpp_update_properties);

  gst_va_filter_install_properties (filter, object_class);

  g_free (long_name);
  g_free (cdata->description);
  g_free (cdata->render_device_path);
  g_free (cdata);
  gst_object_unref (filter);
  gst_object_unref (display);
}

static inline void
_create_colorbalance_channel (GstVaVpp * self, const gchar * label)
{
  GstColorBalanceChannel *channel;

  channel = g_object_new (GST_TYPE_COLOR_BALANCE_CHANNEL, NULL);
  channel->label = g_strdup_printf ("VA-%s", label);
  channel->min_value = -1000;
  channel->max_value = 1000;

  self->channels = g_list_append (self->channels, channel);
}

static void
gst_va_vpp_init (GTypeInstance * instance, gpointer g_class)
{
  GstVaVpp *self = GST_VA_VPP (instance);
  GParamSpec *pspec;

  self->direction = GST_VIDEO_ORIENTATION_IDENTITY;
  self->prev_direction = self->direction;
  self->tag_direction = GST_VIDEO_ORIENTATION_AUTO;

  pspec = g_object_class_find_property (g_class, "denoise");
  if (pspec)
    self->denoise = g_value_get_float (g_param_spec_get_default_value (pspec));

  pspec = g_object_class_find_property (g_class, "sharpen");
  if (pspec)
    self->sharpen = g_value_get_float (g_param_spec_get_default_value (pspec));

  pspec = g_object_class_find_property (g_class, "skin-tone");
  if (pspec) {
    const GValue *value = g_param_spec_get_default_value (pspec);
    if (G_VALUE_TYPE (value) == G_TYPE_BOOLEAN)
      self->skintone = g_value_get_boolean (value);
    else
      self->skintone = g_value_get_float (value);
  }

  /* color balance */
  pspec = g_object_class_find_property (g_class, "brightness");
  if (pspec) {
    self->brightness =
        g_value_get_float (g_param_spec_get_default_value (pspec));
    _create_colorbalance_channel (self, "BRIGHTNESS");
  }
  pspec = g_object_class_find_property (g_class, "contrast");
  if (pspec) {
    self->contrast = g_value_get_float (g_param_spec_get_default_value (pspec));
    _create_colorbalance_channel (self, "CONTRAST");
  }
  pspec = g_object_class_find_property (g_class, "hue");
  if (pspec) {
    self->hue = g_value_get_float (g_param_spec_get_default_value (pspec));
    _create_colorbalance_channel (self, "HUE");
  }
  pspec = g_object_class_find_property (g_class, "saturation");
  if (pspec) {
    self->saturation =
        g_value_get_float (g_param_spec_get_default_value (pspec));
    _create_colorbalance_channel (self, "SATURATION");
  }

  /* enable QoS */
  gst_base_transform_set_qos_enabled (GST_BASE_TRANSFORM (instance), TRUE);
}

static gpointer
_register_debug_category (gpointer data)
{
  GST_DEBUG_CATEGORY_INIT (gst_va_vpp_debug, "vapostproc", 0,
      "VA Video Postprocessor");

#define D(type) \
  G_PASTE (META_TAG_, type) = \
    g_quark_from_static_string (G_PASTE (G_PASTE (GST_META_TAG_VIDEO_, type), _STR))
  D (COLORSPACE);
  D (SIZE);
  D (ORIENTATION);
#undef D
  META_TAG_VIDEO = g_quark_from_static_string (GST_META_TAG_VIDEO_STR);

  return NULL;
}

gboolean
gst_va_vpp_register (GstPlugin * plugin, GstVaDevice * device, guint rank)
{
  static GOnce debug_once = G_ONCE_INIT;
  GType type;
  GTypeInfo type_info = {
    .class_size = sizeof (GstVaVppClass),
    .class_init = gst_va_vpp_class_init,
    .instance_size = sizeof (GstVaVpp),
    .instance_init = gst_va_vpp_init,
  };
  struct CData *cdata;
  gboolean ret;
  gchar *type_name, *feature_name;

  g_return_val_if_fail (GST_IS_PLUGIN (plugin), FALSE);
  g_return_val_if_fail (GST_IS_VA_DEVICE (device), FALSE);

  cdata = g_new (struct CData, 1);
  cdata->description = NULL;
  cdata->render_device_path = g_strdup (device->render_device_path);

  type_info.class_data = cdata;

  type_name = g_strdup ("GstVaPostProc");
  feature_name = g_strdup ("vapostproc");

  /* The first postprocessor to be registered should use a constant
   * name, like vapostproc, for any additional postprocessors, we
   * create unique names, using inserting the render device name. */
  if (g_type_from_name (type_name)) {
    gchar *basename = g_path_get_basename (device->render_device_path);
    g_free (type_name);
    g_free (feature_name);
    type_name = g_strdup_printf ("GstVa%sPostProc", basename);
    feature_name = g_strdup_printf ("va%spostproc", basename);
    cdata->description = basename;

    /* lower rank for non-first device */
    if (rank > 0)
      rank--;
  }

  g_once (&debug_once, _register_debug_category, NULL);

  type = g_type_register_static (GST_TYPE_VA_BASE_TRANSFORM, type_name,
      &type_info, 0);

  {
    GstVaFilter *filter = gst_va_filter_new (device->display);
    if (gst_va_filter_open (filter)
        && gst_va_filter_has_filter (filter, VAProcFilterColorBalance)) {
      const GInterfaceInfo info = { gst_va_vpp_colorbalance_init, NULL, NULL };
      g_type_add_interface_static (type, GST_TYPE_COLOR_BALANCE, &info);
    }
    gst_object_unref (filter);
  }

  ret = gst_element_register (plugin, feature_name, rank, type);

  g_free (type_name);
  g_free (feature_name);

  return ret;
}

/* Color Balance interface */
static const GList *
gst_va_vpp_colorbalance_list_channels (GstColorBalance * balance)
{
  GstVaVpp *self = GST_VA_VPP (balance);

  return self->channels;
}

static gboolean
_set_cb_val (GstVaVpp * self, const gchar * name,
    GstColorBalanceChannel * channel, gint value, gfloat * cb)
{
  GObjectClass *klass = G_OBJECT_CLASS (GST_VA_VPP_GET_CLASS (self));
  GParamSpec *pspec;
  GParamSpecFloat *fpspec;
  gfloat new_value;
  gboolean changed;

  pspec = g_object_class_find_property (klass, name);
  if (!pspec)
    return FALSE;

  fpspec = G_PARAM_SPEC_FLOAT (pspec);
  new_value = (value - channel->min_value) * (fpspec->maximum - fpspec->minimum)
      / (channel->max_value - channel->min_value) + fpspec->minimum;

  GST_OBJECT_LOCK (self);
  changed = new_value != *cb;
  *cb = new_value;
  value = (*cb + fpspec->minimum) * (channel->max_value - channel->min_value)
      / (fpspec->maximum - fpspec->minimum) + channel->min_value;
  GST_OBJECT_UNLOCK (self);

  if (changed) {
    GST_INFO_OBJECT (self, "%s: %d / %f", channel->label, value, new_value);
    gst_color_balance_value_changed (GST_COLOR_BALANCE (self), channel, value);
    g_atomic_int_set (&self->rebuild_filters, TRUE);
  }

  return TRUE;
}

static void
gst_va_vpp_colorbalance_set_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel, gint value)
{
  GstVaVpp *self = GST_VA_VPP (balance);

  if (g_str_has_suffix (channel->label, "HUE"))
    _set_cb_val (self, "hue", channel, value, &self->hue);
  else if (g_str_has_suffix (channel->label, "BRIGHTNESS"))
    _set_cb_val (self, "brightness", channel, value, &self->brightness);
  else if (g_str_has_suffix (channel->label, "CONTRAST"))
    _set_cb_val (self, "contrast", channel, value, &self->contrast);
  else if (g_str_has_suffix (channel->label, "SATURATION"))
    _set_cb_val (self, "saturation", channel, value, &self->saturation);
}

static gboolean
_get_cb_val (GstVaVpp * self, const gchar * name,
    GstColorBalanceChannel * channel, gfloat * cb, gint * val)
{
  GObjectClass *klass = G_OBJECT_CLASS (GST_VA_VPP_GET_CLASS (self));
  GParamSpec *pspec;
  GParamSpecFloat *fpspec;

  pspec = g_object_class_find_property (klass, name);
  if (!pspec)
    return FALSE;

  fpspec = G_PARAM_SPEC_FLOAT (pspec);

  GST_OBJECT_LOCK (self);
  *val = (*cb + fpspec->minimum) * (channel->max_value - channel->min_value)
      / (fpspec->maximum - fpspec->minimum) + channel->min_value;
  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gint
gst_va_vpp_colorbalance_get_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel)
{
  GstVaVpp *self = GST_VA_VPP (balance);
  gint value = 0;

  if (g_str_has_suffix (channel->label, "HUE"))
    _get_cb_val (self, "hue", channel, &self->hue, &value);
  else if (g_str_has_suffix (channel->label, "BRIGHTNESS"))
    _get_cb_val (self, "brightness", channel, &self->brightness, &value);
  else if (g_str_has_suffix (channel->label, "CONTRAST"))
    _get_cb_val (self, "contrast", channel, &self->contrast, &value);
  else if (g_str_has_suffix (channel->label, "SATURATION"))
    _get_cb_val (self, "saturation", channel, &self->saturation, &value);

  return value;
}

static GstColorBalanceType
gst_va_vpp_colorbalance_get_balance_type (GstColorBalance * balance)
{
  return GST_COLOR_BALANCE_HARDWARE;
}

static void
gst_va_vpp_colorbalance_init (gpointer iface, gpointer data)
{
  GstColorBalanceInterface *cbiface = iface;

  cbiface->list_channels = gst_va_vpp_colorbalance_list_channels;
  cbiface->set_value = gst_va_vpp_colorbalance_set_value;
  cbiface->get_value = gst_va_vpp_colorbalance_get_value;
  cbiface->get_balance_type = gst_va_vpp_colorbalance_get_balance_type;
}
