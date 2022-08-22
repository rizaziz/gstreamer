/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 * Copyright (C) 2020 Seungha Yang <seungha@centricular.com>
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
 * SECTION:element-d3d11videosink
 * @title: d3d11videosink
 *
 * Direct3D11 based video render element
 *
 * ## Example launch line
 * ```
 * gst-launch-1.0 videotestsrc ! d3d11upload ! d3d11videosink
 * ```
 * This pipeline will display test video stream on screen via d3d11videosink
 *
 * Since: 1.18
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstd3d11videosink.h"
#include "gstd3d11pluginutils.h"
#include <string>

#if GST_D3D11_WINAPI_APP
#include "gstd3d11window_corewindow.h"
#include "gstd3d11window_swapchainpanel.h"
#endif
#if (!GST_D3D11_WINAPI_ONLY_APP)
#include "gstd3d11window_win32.h"
#endif
#include "gstd3d11window_dummy.h"

enum
{
  PROP_0,
  PROP_ADAPTER,
  PROP_FORCE_ASPECT_RATIO,
  PROP_ENABLE_NAVIGATION_EVENTS,
  PROP_FULLSCREEN_TOGGLE_MODE,
  PROP_FULLSCREEN,
  PROP_DRAW_ON_SHARED_TEXTURE,
  PROP_ROTATE_METHOD,
  PROP_GAMMA_MODE,
  PROP_PRIMARIES_MODE,
};

#define DEFAULT_ADAPTER                   -1
#define DEFAULT_FORCE_ASPECT_RATIO        TRUE
#define DEFAULT_ENABLE_NAVIGATION_EVENTS  TRUE
#define DEFAULT_FULLSCREEN_TOGGLE_MODE    GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_NONE
#define DEFAULT_FULLSCREEN                FALSE
#define DEFAULT_DRAW_ON_SHARED_TEXTURE    FALSE
#define DEFAULT_GAMMA_MODE                GST_VIDEO_GAMMA_MODE_NONE
#define DEFAULT_PRIMARIES_MODE            GST_VIDEO_PRIMARIES_MODE_NONE

enum
{
  /* signals */
  SIGNAL_BEGIN_DRAW,

  /* actions */
  SIGNAL_DRAW,

  LAST_SIGNAL
};

static guint gst_d3d11_video_sink_signals[LAST_SIGNAL] = { 0, };

static GstStaticCaps pad_template_caps =
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, GST_D3D11_SINK_FORMATS) "; "
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY ","
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION,
        GST_D3D11_SINK_FORMATS) ";"
    GST_VIDEO_CAPS_MAKE (GST_D3D11_SINK_FORMATS) "; "
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY ","
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION,
        GST_D3D11_SINK_FORMATS));

GST_DEBUG_CATEGORY (d3d11_video_sink_debug);
#define GST_CAT_DEFAULT d3d11_video_sink_debug

struct _GstD3D11VideoSink
{
  GstVideoSink parent;
  GstD3D11Device *device;
  GstD3D11Window *window;
  gint video_width;
  gint video_height;

  GstVideoInfo info;

  guintptr window_id;

  gboolean caps_updated;

  /* properties */
  gint adapter;
  gboolean force_aspect_ratio;
  gboolean enable_navigation_events;
  GstD3D11WindowFullscreenToggleMode fullscreen_toggle_mode;
  gboolean fullscreen;
  gboolean draw_on_shared_texture;
  GstVideoGammaMode gamma_mode;
  GstVideoPrimariesMode primaries_mode;

  /* saved render rectangle until we have a window */
  GstVideoRectangle render_rect;
  gboolean pending_render_rect;

  /* For drawing on user texture */
  gboolean drawing;
  GstBuffer *current_buffer;
  CRITICAL_SECTION lock;

  gchar *title;

  /* method configured via property */
  GstVideoOrientationMethod method;
  /* method parsed from tag */
  GstVideoOrientationMethod tag_method;
  /* method currently selected based on "method" and "tag_method" */
  GstVideoOrientationMethod selected_method;
};

static void gst_d3d11_videosink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_d3d11_videosink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_d3d11_video_sink_finalize (GObject * object);
static gboolean
gst_d3d11_video_sink_draw_action (GstD3D11VideoSink * self,
    gpointer shared_handle, guint texture_misc_flags, guint64 acquire_key,
    guint64 release_key);

static void
gst_d3d11_video_sink_video_overlay_init (GstVideoOverlayInterface * iface);
static void
gst_d3d11_video_sink_navigation_init (GstNavigationInterface * iface);

static void gst_d3d11_video_sink_set_context (GstElement * element,
    GstContext * context);
static GstCaps *gst_d3d11_video_sink_get_caps (GstBaseSink * sink,
    GstCaps * filter);
static gboolean gst_d3d11_video_sink_set_caps (GstBaseSink * sink,
    GstCaps * caps);

static gboolean gst_d3d11_video_sink_start (GstBaseSink * sink);
static gboolean gst_d3d11_video_sink_stop (GstBaseSink * sink);
static gboolean gst_d3d11_video_sink_propose_allocation (GstBaseSink * sink,
    GstQuery * query);
static gboolean gst_d3d11_video_sink_query (GstBaseSink * sink,
    GstQuery * query);
static gboolean gst_d3d11_video_sink_unlock (GstBaseSink * sink);
static gboolean gst_d3d11_video_sink_unlock_stop (GstBaseSink * sink);
static gboolean gst_d3d11_video_sink_event (GstBaseSink * sink,
    GstEvent * event);
static GstFlowReturn
gst_d3d11_video_sink_show_frame (GstVideoSink * sink, GstBuffer * buf);
static gboolean gst_d3d11_video_sink_prepare_window (GstD3D11VideoSink * self);
static void gst_d3d11_video_sink_set_orientation (GstD3D11VideoSink * self,
    GstVideoOrientationMethod method, gboolean from_tag);

#define gst_d3d11_video_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstD3D11VideoSink, gst_d3d11_video_sink,
    GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_d3d11_video_sink_video_overlay_init);
    G_IMPLEMENT_INTERFACE (GST_TYPE_NAVIGATION,
        gst_d3d11_video_sink_navigation_init);
    GST_DEBUG_CATEGORY_INIT (d3d11_video_sink_debug,
        "d3d11videosink", 0, "Direct3D11 Video Sink"));

static void
gst_d3d11_video_sink_class_init (GstD3D11VideoSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS (klass);
  GstVideoSinkClass *videosink_class = GST_VIDEO_SINK_CLASS (klass);
  GstCaps *caps;

  gobject_class->set_property = gst_d3d11_videosink_set_property;
  gobject_class->get_property = gst_d3d11_videosink_get_property;
  gobject_class->finalize = gst_d3d11_video_sink_finalize;

  g_object_class_install_property (gobject_class, PROP_ADAPTER,
      g_param_spec_int ("adapter", "Adapter",
          "Adapter index for creating device (-1 for default)",
          -1, G_MAXINT32, DEFAULT_ADAPTER,
          (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
              G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_FORCE_ASPECT_RATIO,
      g_param_spec_boolean ("force-aspect-ratio",
          "Force aspect ratio",
          "When enabled, scaling will respect original aspect ratio",
          DEFAULT_FORCE_ASPECT_RATIO,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_ENABLE_NAVIGATION_EVENTS,
      g_param_spec_boolean ("enable-navigation-events",
          "Enable navigation events",
          "When enabled, navigation events are sent upstream",
          DEFAULT_ENABLE_NAVIGATION_EVENTS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_FULLSCREEN_TOGGLE_MODE,
      g_param_spec_flags ("fullscreen-toggle-mode",
          "Full screen toggle mode",
          "Full screen toggle mode used to trigger fullscreen mode change",
          GST_D3D11_WINDOW_TOGGLE_MODE_GET_TYPE, DEFAULT_FULLSCREEN_TOGGLE_MODE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_FULLSCREEN,
      g_param_spec_boolean ("fullscreen",
          "fullscreen",
          "Ignored when \"fullscreen-toggle-mode\" does not include \"property\"",
          DEFAULT_FULLSCREEN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /**
   * GstD3D11VideoSink:draw-on-shared-texture:
   *
   * Instruct the sink to draw on a shared texture provided by user.
   * User must watch #d3d11videosink::begin-draw signal and should call
   * #d3d11videosink::draw method on the #d3d11videosink::begin-draw
   * signal handler.
   *
   * Currently supported formats for user texture are:
   * - DXGI_FORMAT_R8G8B8A8_UNORM
   * - DXGI_FORMAT_B8G8R8A8_UNORM
   * - DXGI_FORMAT_R10G10B10A2_UNORM
   *
   * Since: 1.20
   */
  g_object_class_install_property (gobject_class, PROP_DRAW_ON_SHARED_TEXTURE,
      g_param_spec_boolean ("draw-on-shared-texture",
          "Draw on shared texture",
          "Draw on user provided shared texture instead of window. "
          "When enabled, user can pass application's own texture to sink "
          "by using \"draw\" action signal on \"begin-draw\" signal handler, "
          "so that sink can draw video data on application's texture. "
          "Supported texture formats for user texture are "
          "DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, and "
          "DXGI_FORMAT_R10G10B10A2_UNORM.",
          DEFAULT_DRAW_ON_SHARED_TEXTURE,
          (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
              G_PARAM_STATIC_STRINGS)));

  /**
   * GstD3D11VideoSink:rotate-method:
   *
   * Video rotation/flip method to use
   *
   * Since: 1.22
   */
  g_object_class_install_property (gobject_class, PROP_ROTATE_METHOD,
      g_param_spec_enum ("rotate-method", "Rotate Method",
          "Rotate method to use",
          GST_TYPE_VIDEO_ORIENTATION_METHOD, GST_VIDEO_ORIENTATION_IDENTITY,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /**
   * GstD3D11VideoSink:gamma-mode:
   *
   * Gamma conversion mode
   *
   * Since: 1.22
   */
  g_object_class_install_property (gobject_class, PROP_GAMMA_MODE,
      g_param_spec_enum ("gamma-mode", "Gamma mode",
          "Gamma conversion mode", GST_TYPE_VIDEO_GAMMA_MODE,
          DEFAULT_GAMMA_MODE, (GParamFlags) (GST_PARAM_MUTABLE_READY |
              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /**
   * GstD3D11VideoSink:primaries-mode:
   *
   * Primaries conversion mode
   *
   * Since: 1.22
   */
  g_object_class_install_property (gobject_class, PROP_PRIMARIES_MODE,
      g_param_spec_enum ("primaries-mode", "Primaries Mode",
          "Primaries conversion mode", GST_TYPE_VIDEO_PRIMARIES_MODE,
          DEFAULT_PRIMARIES_MODE, (GParamFlags) (GST_PARAM_MUTABLE_READY |
              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /**
   * GstD3D11VideoSink::begin-draw:
   * @videosink: the #d3d11videosink
   *
   * Emitted when sink has a texture to draw. Application needs to invoke
   * #d3d11videosink::draw action signal before returning from
   * #d3d11videosink::begin-draw signal handler.
   *
   * Since: 1.20
   */
  gst_d3d11_video_sink_signals[SIGNAL_BEGIN_DRAW] =
      g_signal_new ("begin-draw", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstD3D11VideoSinkClass, begin_draw),
      NULL, NULL, NULL, G_TYPE_NONE, 0, G_TYPE_NONE);

  /**
   * GstD3D11VideoSink::draw:
   * @videosink: the #d3d11videosink
   * @shard_handle: a pointer to HANDLE
   * @texture_misc_flags: a D3D11_RESOURCE_MISC_FLAG value
   * @acquire_key: a key value used for IDXGIKeyedMutex::AcquireSync
   * @release_key: a key value used for IDXGIKeyedMutex::ReleaseSync
   *
   * Draws on a shared texture. @shard_handle must be a valid pointer to
   * a HANDLE which was obtained via IDXGIResource::GetSharedHandle or
   * IDXGIResource1::CreateSharedHandle.
   *
   * If the texture was created with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX flag,
   * caller must specify valid @acquire_key and @release_key.
   * Otherwise (i.e., created with D3D11_RESOURCE_MISC_SHARED flag),
   * @acquire_key and @release_key will be ignored.
   *
   * Since: 1.20
   */
  gst_d3d11_video_sink_signals[SIGNAL_DRAW] =
      g_signal_new ("draw", G_TYPE_FROM_CLASS (klass),
      (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
      G_STRUCT_OFFSET (GstD3D11VideoSinkClass, draw), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 4, G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_UINT64,
      G_TYPE_UINT64);

  element_class->set_context =
      GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_set_context);

  gst_element_class_set_static_metadata (element_class,
      "Direct3D11 video sink", "Sink/Video",
      "A Direct3D11 based videosink",
      "Seungha Yang <seungha.yang@navercorp.com>");

  caps = gst_d3d11_get_updated_template_caps (&pad_template_caps);
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  basesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_get_caps);
  basesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_set_caps);
  basesink_class->start = GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_stop);
  basesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_propose_allocation);
  basesink_class->query = GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_query);
  basesink_class->unlock = GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_unlock);
  basesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_unlock_stop);
  basesink_class->event = GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_event);

  videosink_class->show_frame =
      GST_DEBUG_FUNCPTR (gst_d3d11_video_sink_show_frame);

  klass->draw = gst_d3d11_video_sink_draw_action;

  gst_type_mark_as_plugin_api (GST_D3D11_WINDOW_TOGGLE_MODE_GET_TYPE,
      (GstPluginAPIFlags) 0);
}

static void
gst_d3d11_video_sink_init (GstD3D11VideoSink * self)
{
  self->adapter = DEFAULT_ADAPTER;
  self->force_aspect_ratio = DEFAULT_FORCE_ASPECT_RATIO;
  self->enable_navigation_events = DEFAULT_ENABLE_NAVIGATION_EVENTS;
  self->fullscreen_toggle_mode = DEFAULT_FULLSCREEN_TOGGLE_MODE;
  self->fullscreen = DEFAULT_FULLSCREEN;
  self->draw_on_shared_texture = DEFAULT_DRAW_ON_SHARED_TEXTURE;
  self->gamma_mode = DEFAULT_GAMMA_MODE;
  self->primaries_mode = DEFAULT_PRIMARIES_MODE;

  InitializeCriticalSection (&self->lock);
}

static void
gst_d3d11_videosink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (object);
  GstD3D11CSLockGuard lk (&self->lock);

  switch (prop_id) {
    case PROP_ADAPTER:
      self->adapter = g_value_get_int (value);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      self->force_aspect_ratio = g_value_get_boolean (value);
      if (self->window)
        g_object_set (self->window,
            "force-aspect-ratio", self->force_aspect_ratio, NULL);
      break;
    case PROP_ENABLE_NAVIGATION_EVENTS:
      self->enable_navigation_events = g_value_get_boolean (value);
      if (self->window) {
        g_object_set (self->window,
            "enable-navigation-events", self->enable_navigation_events, NULL);
      }
      break;
    case PROP_FULLSCREEN_TOGGLE_MODE:
      self->fullscreen_toggle_mode =
          (GstD3D11WindowFullscreenToggleMode) g_value_get_flags (value);
      if (self->window) {
        g_object_set (self->window,
            "fullscreen-toggle-mode", self->fullscreen_toggle_mode, NULL);
      }
      break;
    case PROP_FULLSCREEN:
      self->fullscreen = g_value_get_boolean (value);
      if (self->window) {
        g_object_set (self->window, "fullscreen", self->fullscreen, NULL);
      }
      break;
    case PROP_DRAW_ON_SHARED_TEXTURE:
      self->draw_on_shared_texture = g_value_get_boolean (value);
      break;
    case PROP_ROTATE_METHOD:
      gst_d3d11_video_sink_set_orientation (self,
          (GstVideoOrientationMethod) g_value_get_enum (value), FALSE);
      break;
    case PROP_GAMMA_MODE:
      self->gamma_mode = (GstVideoGammaMode) g_value_get_enum (value);
      break;
    case PROP_PRIMARIES_MODE:
      self->primaries_mode = (GstVideoPrimariesMode) g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_d3d11_videosink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (object);
  GstD3D11CSLockGuard lk (&self->lock);

  switch (prop_id) {
    case PROP_ADAPTER:
      g_value_set_int (value, self->adapter);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      g_value_set_boolean (value, self->force_aspect_ratio);
      break;
    case PROP_ENABLE_NAVIGATION_EVENTS:
      g_value_set_boolean (value, self->enable_navigation_events);
      break;
    case PROP_FULLSCREEN_TOGGLE_MODE:
      g_value_set_flags (value, self->fullscreen_toggle_mode);
      break;
    case PROP_FULLSCREEN:
      if (self->window) {
        g_object_get_property (G_OBJECT (self->window), pspec->name, value);
      } else {
        g_value_set_boolean (value, self->fullscreen);
      }
      break;
    case PROP_DRAW_ON_SHARED_TEXTURE:
      g_value_set_boolean (value, self->draw_on_shared_texture);
      break;
    case PROP_ROTATE_METHOD:
      g_value_set_enum (value, self->method);
      break;
    case PROP_GAMMA_MODE:
      g_value_set_enum (value, self->gamma_mode);
      break;
    case PROP_PRIMARIES_MODE:
      g_value_set_enum (value, self->primaries_mode);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_d3d11_video_sink_finalize (GObject * object)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (object);

  DeleteCriticalSection (&self->lock);
  g_free (self->title);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_d3d11_video_sink_set_context (GstElement * element, GstContext * context)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (element);

  gst_d3d11_handle_set_context (element, context, self->adapter, &self->device);

  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static GstCaps *
gst_d3d11_video_sink_get_caps (GstBaseSink * sink, GstCaps * filter)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);
  GstCaps *caps = NULL;

  caps = gst_pad_get_pad_template_caps (GST_VIDEO_SINK_PAD (sink));

  if (self->device) {
    gboolean is_hardware = FALSE;

    g_object_get (self->device, "hardware", &is_hardware, NULL);

    /* In case of WARP device, conversion via shader would be inefficient than
     * upstream videoconvert. Allow native formats in this case */
    if (!is_hardware) {
      GValue format_list = G_VALUE_INIT;
      GValue format = G_VALUE_INIT;

      g_value_init (&format_list, GST_TYPE_LIST);
      g_value_init (&format, G_TYPE_STRING);

      g_value_set_string (&format, "RGBA");
      gst_value_list_append_and_take_value (&format_list, &format);

      format = G_VALUE_INIT;
      g_value_init (&format, G_TYPE_STRING);
      g_value_set_string (&format, "BGRA");
      gst_value_list_append_and_take_value (&format_list, &format);

      caps = gst_caps_make_writable (caps);
      gst_caps_set_value (caps, "format", &format_list);
      g_value_unset (&format_list);
    }
  }

  if (filter) {
    GstCaps *isect;
    isect = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = isect;
  }

  return caps;
}

static gboolean
gst_d3d11_video_sink_set_caps (GstBaseSink * sink, GstCaps * caps)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);

  GST_DEBUG_OBJECT (self, "set caps %" GST_PTR_FORMAT, caps);

  /* We will update window on show_frame() */
  self->caps_updated = TRUE;

  return TRUE;
}

static gboolean
gst_d3d11_video_sink_update_window (GstD3D11VideoSink * self, GstCaps * caps)
{
  gint video_width, video_height;
  gint video_par_n, video_par_d;        /* video's PAR */
  gint display_par_n = 1, display_par_d = 1;    /* display's PAR */
  guint num, den;
  GError *error = NULL;
  GstStructure *config;

  GST_DEBUG_OBJECT (self, "Updating window with caps %" GST_PTR_FORMAT, caps);

  self->caps_updated = FALSE;

  EnterCriticalSection (&self->lock);
  if (!gst_d3d11_video_sink_prepare_window (self)) {
    LeaveCriticalSection (&self->lock);

    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, (nullptr),
        ("Failed to open window."));

    return FALSE;
  }

  if (!gst_video_info_from_caps (&self->info, caps)) {
    GST_DEBUG_OBJECT (self,
        "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    LeaveCriticalSection (&self->lock);
    return FALSE;
  }

  video_width = GST_VIDEO_INFO_WIDTH (&self->info);
  video_height = GST_VIDEO_INFO_HEIGHT (&self->info);
  video_par_n = GST_VIDEO_INFO_PAR_N (&self->info);
  video_par_d = GST_VIDEO_INFO_PAR_D (&self->info);

  /* get aspect ratio from caps if it's present, and
   * convert video width and height to a display width and height
   * using wd / hd = wv / hv * PARv / PARd */

  if (!gst_video_calculate_display_ratio (&num, &den, video_width,
          video_height, video_par_n, video_par_d, display_par_n,
          display_par_d)) {
    LeaveCriticalSection (&self->lock);

    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (nullptr),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }

  GST_DEBUG_OBJECT (self,
      "video width/height: %dx%d, calculated display ratio: %d/%d format: %s",
      video_width, video_height, num, den,
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&self->info)));

  /* now find a width x height that respects this display ratio.
   * prefer those that have one of w/h the same as the incoming video
   * using wd / hd = num / den
   */

  /* start with same height, because of interlaced video
   * check hd / den is an integer scale factor, and scale wd with the PAR
   */
  if (video_height % den == 0) {
    GST_DEBUG_OBJECT (self, "keeping video height");
    GST_VIDEO_SINK_WIDTH (self) = (guint)
        gst_util_uint64_scale_int (video_height, num, den);
    GST_VIDEO_SINK_HEIGHT (self) = video_height;
  } else if (video_width % num == 0) {
    GST_DEBUG_OBJECT (self, "keeping video width");
    GST_VIDEO_SINK_WIDTH (self) = video_width;
    GST_VIDEO_SINK_HEIGHT (self) = (guint)
        gst_util_uint64_scale_int (video_width, den, num);
  } else {
    GST_DEBUG_OBJECT (self, "approximating while keeping video height");
    GST_VIDEO_SINK_WIDTH (self) = (guint)
        gst_util_uint64_scale_int (video_height, num, den);
    GST_VIDEO_SINK_HEIGHT (self) = video_height;
  }

  GST_DEBUG_OBJECT (self, "scaling to %dx%d",
      GST_VIDEO_SINK_WIDTH (self), GST_VIDEO_SINK_HEIGHT (self));
  self->video_width = video_width;
  self->video_height = video_height;

  if (GST_VIDEO_SINK_WIDTH (self) <= 0 || GST_VIDEO_SINK_HEIGHT (self) <= 0) {
    LeaveCriticalSection (&self->lock);

    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (nullptr),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }

  if (self->pending_render_rect) {
    GstVideoRectangle rect = self->render_rect;

    self->pending_render_rect = FALSE;
    gst_d3d11_window_set_render_rectangle (self->window, &rect);
  }

  config = gst_structure_new ("convert-config",
      GST_D3D11_CONVERTER_OPT_GAMMA_MODE,
      GST_TYPE_VIDEO_GAMMA_MODE, self->gamma_mode,
      GST_D3D11_CONVERTER_OPT_PRIMARIES_MODE,
      GST_TYPE_VIDEO_PRIMARIES_MODE, self->primaries_mode, nullptr);

  if (!gst_d3d11_window_prepare (self->window, GST_VIDEO_SINK_WIDTH (self),
          GST_VIDEO_SINK_HEIGHT (self), caps, config, &error)) {
    GstMessage *error_msg;

    LeaveCriticalSection (&self->lock);

    GST_ERROR_OBJECT (self, "cannot create swapchain");
    error_msg = gst_message_new_error (GST_OBJECT_CAST (self),
        error, "Failed to prepare d3d11window");
    g_clear_error (&error);
    gst_element_post_message (GST_ELEMENT (self), error_msg);

    return FALSE;
  }

  if (self->title) {
    gst_d3d11_window_set_title (self->window, self->title);
    g_clear_pointer (&self->title, g_free);
  }

  LeaveCriticalSection (&self->lock);

  return TRUE;
}

static void
gst_d3d11_video_sink_key_event (GstD3D11Window * window, const gchar * event,
    const gchar * key, GstD3D11VideoSink * self)
{
  GstEvent *key_event;

  if (!self->enable_navigation_events || !event || !key)
    return;

  GST_LOG_OBJECT (self, "send key event %s, key %s", event, key);
  if (g_strcmp0 ("key-press", event) == 0) {
    key_event = gst_navigation_event_new_key_press (key,
        GST_NAVIGATION_MODIFIER_NONE);
  } else if (g_strcmp0 ("key-release", event) == 0) {
    key_event = gst_navigation_event_new_key_release (key,
        GST_NAVIGATION_MODIFIER_NONE);
  } else {
    return;
  }

  gst_navigation_send_event_simple (GST_NAVIGATION (self), key_event);
}

static void
gst_d3d11_video_mouse_key_event (GstD3D11Window * window, const gchar * event,
    gint button, gdouble x, gdouble y, GstD3D11VideoSink * self)
{
  GstEvent *mouse_event;

  if (!self->enable_navigation_events || !event)
    return;

  GST_LOG_OBJECT (self,
      "send mouse event %s, button %d (%.1f, %.1f)", event, button, x, y);
  if (g_strcmp0 ("mouse-button-press", event) == 0) {
    mouse_event = gst_navigation_event_new_mouse_button_press (button, x, y,
        GST_NAVIGATION_MODIFIER_NONE);
  } else if (g_strcmp0 ("mouse-button-release", event) == 0) {
    mouse_event = gst_navigation_event_new_mouse_button_release (button, x, y,
        GST_NAVIGATION_MODIFIER_NONE);
  } else if (g_strcmp0 ("mouse-move", event) == 0) {
    mouse_event = gst_navigation_event_new_mouse_move (x, y,
        GST_NAVIGATION_MODIFIER_NONE);
  } else {
    return;
  }

  gst_navigation_send_event_simple (GST_NAVIGATION (self), mouse_event);
}

static gboolean
gst_d3d11_video_sink_start (GstBaseSink * sink)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);

  GST_DEBUG_OBJECT (self, "Start");

  if (!gst_d3d11_ensure_element_data (GST_ELEMENT_CAST (self), self->adapter,
          &self->device)) {
    GST_ERROR_OBJECT (sink, "Cannot create d3d11device");
    return FALSE;
  }

  return TRUE;
}

/* called with lock */
static gboolean
gst_d3d11_video_sink_prepare_window (GstD3D11VideoSink * self)
{
  GstD3D11WindowNativeType window_type = GST_D3D11_WINDOW_NATIVE_TYPE_HWND;

  if (self->window)
    return TRUE;

  if (self->draw_on_shared_texture) {
    GST_INFO_OBJECT (self,
        "Create dummy window for rendering on shared texture");
    self->window = gst_d3d11_window_dummy_new (self->device);
    goto done;
  }

  if (!self->window_id)
    gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (self));

  if (self->window_id) {
    window_type =
        gst_d3d11_window_get_native_type_from_handle (self->window_id);

    if (window_type != GST_D3D11_WINDOW_NATIVE_TYPE_NONE) {
      GST_DEBUG_OBJECT (self, "Have window handle %" G_GUINTPTR_FORMAT,
          self->window_id);
      gst_video_overlay_got_window_handle (GST_VIDEO_OVERLAY (self),
          self->window_id);
    }
  }

  GST_DEBUG_OBJECT (self, "Create window (type: %s)",
      gst_d3d11_window_get_native_type_to_string (window_type));

#if GST_D3D11_WINAPI_ONLY_APP
  if (window_type != GST_D3D11_WINDOW_NATIVE_TYPE_CORE_WINDOW &&
      window_type != GST_D3D11_WINDOW_NATIVE_TYPE_SWAP_CHAIN_PANEL) {
    GST_ERROR_OBJECT (self, "Overlay handle must be set before READY state");
    return FALSE;
  }
#endif

  switch (window_type) {
#if (!GST_D3D11_WINAPI_ONLY_APP)
    case GST_D3D11_WINDOW_NATIVE_TYPE_HWND:
      self->window = gst_d3d11_window_win32_new (self->device, self->window_id);
      break;
#endif
#if GST_D3D11_WINAPI_APP
    case GST_D3D11_WINDOW_NATIVE_TYPE_CORE_WINDOW:
      self->window = gst_d3d11_window_core_window_new (self->device,
          self->window_id);
      break;
    case GST_D3D11_WINDOW_NATIVE_TYPE_SWAP_CHAIN_PANEL:
      self->window = gst_d3d11_window_swap_chain_panel_new (self->device,
          self->window_id);
      break;
#endif
    default:
      break;
  }

done:
  if (!self->window) {
    GST_ERROR_OBJECT (self, "Cannot create d3d11window");
    return FALSE;
  }

  g_object_set (self->window,
      "force-aspect-ratio", self->force_aspect_ratio,
      "fullscreen-toggle-mode", self->fullscreen_toggle_mode,
      "fullscreen", self->fullscreen,
      "enable-navigation-events", self->enable_navigation_events, NULL);

  gst_d3d11_window_set_orientation (self->window, self->selected_method);

  g_signal_connect (self->window, "key-event",
      G_CALLBACK (gst_d3d11_video_sink_key_event), self);
  g_signal_connect (self->window, "mouse-event",
      G_CALLBACK (gst_d3d11_video_mouse_key_event), self);

  return TRUE;
}

static gboolean
gst_d3d11_video_sink_stop (GstBaseSink * sink)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);
  GstD3D11CSLockGuard lk (&self->lock);

  GST_DEBUG_OBJECT (self, "Stop");

  if (self->window)
    gst_d3d11_window_unprepare (self->window);

  gst_clear_object (&self->window);
  gst_clear_object (&self->device);
  g_clear_pointer (&self->title, g_free);

  return TRUE;
}

static gboolean
gst_d3d11_video_sink_propose_allocation (GstBaseSink * sink, GstQuery * query)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);
  GstCaps *caps;
  GstBufferPool *pool = NULL;
  GstVideoInfo info;
  guint size;
  gboolean need_pool;

  if (!self->device)
    return FALSE;

  gst_query_parse_allocation (query, &caps, &need_pool);

  if (caps == NULL)
    goto no_caps;

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_caps;

  /* the normal size of a frame */
  size = info.size;

  if (need_pool) {
    GstCapsFeatures *features;
    GstStructure *config;
    gboolean is_d3d11 = FALSE;

    features = gst_caps_get_features (caps, 0);
    if (features
        && gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY)) {
      GST_DEBUG_OBJECT (self, "upstream support d3d11 memory");
      pool = gst_d3d11_buffer_pool_new (self->device);
      is_d3d11 = TRUE;
    } else {
      pool = gst_video_buffer_pool_new ();
    }

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    if (!is_d3d11) {
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
    }

    size = GST_VIDEO_INFO_SIZE (&info);
    if (is_d3d11) {
      GstD3D11AllocationParams *d3d11_params;

      d3d11_params =
          gst_d3d11_allocation_params_new (self->device,
          &info, GST_D3D11_ALLOCATION_FLAG_DEFAULT, D3D11_BIND_SHADER_RESOURCE,
          0);

      gst_buffer_pool_config_set_d3d11_allocation_params (config, d3d11_params);
      gst_d3d11_allocation_params_free (d3d11_params);
    }

    gst_buffer_pool_config_set_params (config, caps, (guint) size, 2, 0);

    if (!gst_buffer_pool_set_config (pool, config)) {
      GST_ERROR_OBJECT (pool, "Couldn't set config");
      gst_object_unref (pool);

      return FALSE;
    }

    /* d3d11 buffer pool will update buffer size based on allocated texture,
     * get size from config again */
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, nullptr, &size, nullptr,
        nullptr);
    gst_structure_free (config);

    if (is_d3d11) {
      /* In case of system memory, we will upload video frame to GPU memory,
       * (which is copy in any case), so crop meta support for system memory
       * is almost pointless */
      gst_query_add_allocation_meta (query,
          GST_VIDEO_CROP_META_API_TYPE, nullptr);
    }
  }

  /* We need at least 2 buffers because we hold on to the last one for redrawing
   * on window-resize event */
  gst_query_add_allocation_pool (query, pool, size, 2, 0);
  if (pool)
    g_object_unref (pool);

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  gst_query_add_allocation_meta (query,
      GST_VIDEO_OVERLAY_COMPOSITION_META_API_TYPE, NULL);

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_WARNING_OBJECT (self, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_WARNING_OBJECT (self, "invalid caps specified");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_d3d11_video_sink_query (GstBaseSink * sink, GstQuery * query)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      if (gst_d3d11_handle_context_query (GST_ELEMENT (self), query,
              self->device)) {
        return TRUE;
      }
      break;
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->query (sink, query);
}

static gboolean
gst_d3d11_video_sink_unlock (GstBaseSink * sink)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);
  GstD3D11CSLockGuard lk (&self->lock);

  if (self->window)
    gst_d3d11_window_unlock (self->window);

  return TRUE;
}

static gboolean
gst_d3d11_video_sink_unlock_stop (GstBaseSink * sink)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);
  GstD3D11CSLockGuard lk (&self->lock);

  if (self->window)
    gst_d3d11_window_unlock_stop (self->window);

  return TRUE;
}

static gboolean
gst_d3d11_video_sink_event (GstBaseSink * sink, GstEvent * event)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_TAG:{
      GstTagList *taglist;
      gchar *title = nullptr;
      GstVideoOrientationMethod method = GST_VIDEO_ORIENTATION_IDENTITY;

      gst_event_parse_tag (event, &taglist);
      gst_tag_list_get_string (taglist, GST_TAG_TITLE, &title);

      if (title) {
        const gchar *app_name = g_get_application_name ();
        std::string title_string;
        GstD3D11CSLockGuard lk (&self->lock);

        if (app_name) {
          title_string = std::string (title) + " : " + std::string (app_name);
        } else {
          title_string = std::string (title);
        }

        if (self->window) {
          gst_d3d11_window_set_title (self->window, title_string.c_str ());
        } else {
          g_free (self->title);
          self->title = g_strdup (title_string.c_str ());
        }

        g_free (title);
      }

      if (gst_video_orientation_from_tag (taglist, &method)) {
        GstD3D11CSLockGuard lk (&self->lock);
        gst_d3d11_video_sink_set_orientation (self, method, TRUE);
      }
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
}

/* called with lock */
static void
gst_d3d11_video_sink_set_orientation (GstD3D11VideoSink * self,
    GstVideoOrientationMethod method, gboolean from_tag)
{
  if (method == GST_VIDEO_ORIENTATION_CUSTOM) {
    GST_WARNING_OBJECT (self, "Unsupported custom orientation");
    return;
  }

  if (from_tag)
    self->tag_method = method;
  else
    self->method = method;

  if (self->method == GST_VIDEO_ORIENTATION_AUTO) {
    self->selected_method = self->tag_method;
  } else {
    self->selected_method = self->method;
  }

  if (self->window)
    gst_d3d11_window_set_orientation (self->window, self->selected_method);
}

static void
gst_d3d11_video_sink_check_device_update (GstD3D11VideoSink * self,
    GstBuffer * buf)
{
  GstMemory *mem;
  GstD3D11Memory *dmem;
  gboolean update_device = FALSE;

  /* We have configured window already, cannot update device */
  if (self->window)
    return;

  mem = gst_buffer_peek_memory (buf, 0);
  if (!gst_is_d3d11_memory (mem))
    return;

  dmem = GST_D3D11_MEMORY_CAST (mem);
  /* Same device, nothing to do */
  if (dmem->device == self->device)
    return;

  if (self->adapter < 0) {
    update_device = TRUE;
  } else {
    guint adapter = 0;

    g_object_get (dmem->device, "adapter", &adapter, NULL);
    /* The same GPU as what user wanted, update */
    if (adapter == (guint) self->adapter)
      update_device = TRUE;
  }

  if (!update_device)
    return;

  GST_INFO_OBJECT (self, "Updating device %" GST_PTR_FORMAT " -> %"
      GST_PTR_FORMAT, self->device, dmem->device);

  gst_object_unref (self->device);
  self->device = (GstD3D11Device *) gst_object_ref (dmem->device);
}

static GstFlowReturn
gst_d3d11_video_sink_show_frame (GstVideoSink * sink, GstBuffer * buf)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (sink);
  GstFlowReturn ret = GST_FLOW_OK;

  gst_d3d11_video_sink_check_device_update (self, buf);

  if (self->caps_updated || !self->window) {
    GstCaps *caps = gst_pad_get_current_caps (GST_BASE_SINK_PAD (sink));
    gboolean update_ret;

    /* shouldn't happen */
    if (!caps)
      return GST_FLOW_NOT_NEGOTIATED;

    update_ret = gst_d3d11_video_sink_update_window (self, caps);
    gst_caps_unref (caps);

    if (!update_ret)
      return GST_FLOW_NOT_NEGOTIATED;
  }

  gst_d3d11_window_show (self->window);

  if (self->draw_on_shared_texture) {
    GstD3D11CSLockGuard lk (&self->lock);

    self->current_buffer = buf;
    self->drawing = TRUE;

    GST_LOG_OBJECT (self, "Begin drawing");

    /* Application should call draw method on this callback */
    g_signal_emit (self, gst_d3d11_video_sink_signals[SIGNAL_BEGIN_DRAW], 0,
        NULL);

    GST_LOG_OBJECT (self, "End drawing");
    self->drawing = FALSE;
    self->current_buffer = nullptr;
  } else {
    ret = gst_d3d11_window_render (self->window, buf);
  }

  if (ret == GST_D3D11_WINDOW_FLOW_CLOSED) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("Output window was closed"), (NULL));

    ret = GST_FLOW_ERROR;
  }

  return ret;
}

/* VideoOverlay interface */
static void
gst_d3d11_video_sink_set_window_handle (GstVideoOverlay * overlay,
    guintptr window_id)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (overlay);

  GST_DEBUG ("set window handle %" G_GUINTPTR_FORMAT, window_id);

  self->window_id = window_id;
}

static void
gst_d3d11_video_sink_set_render_rectangle (GstVideoOverlay * overlay, gint x,
    gint y, gint width, gint height)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (overlay);
  GstD3D11CSLockGuard lk (&self->lock);

  GST_DEBUG_OBJECT (self,
      "render rect x: %d, y: %d, width: %d, height %d", x, y, width, height);

  if (self->window) {
    GstVideoRectangle rect;

    rect.x = x;
    rect.y = y;
    rect.w = width;
    rect.h = height;

    self->render_rect = rect;

    gst_d3d11_window_set_render_rectangle (self->window, &rect);
  } else {
    self->render_rect.x = x;
    self->render_rect.y = y;
    self->render_rect.w = width;
    self->render_rect.h = height;
    self->pending_render_rect = TRUE;
  }
}

static void
gst_d3d11_video_sink_expose (GstVideoOverlay * overlay)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (overlay);
  GstD3D11CSLockGuard lk (&self->lock);

  if (self->window && self->window->swap_chain)
    gst_d3d11_window_render (self->window, nullptr);
}

static void
gst_d3d11_video_sink_video_overlay_init (GstVideoOverlayInterface * iface)
{
  iface->set_window_handle = gst_d3d11_video_sink_set_window_handle;
  iface->set_render_rectangle = gst_d3d11_video_sink_set_render_rectangle;
  iface->expose = gst_d3d11_video_sink_expose;
}

/* Navigation interface */
static void
gst_d3d11_video_sink_navigation_send_event (GstNavigation * navigation,
    GstEvent * event)
{
  GstD3D11VideoSink *self = GST_D3D11_VIDEO_SINK (navigation);

  /* TODO: add support for translating native coordinate and video coordinate
   * when force-aspect-ratio is set */
  if (event) {
    gboolean handled;

    gst_event_ref (event);
    handled = gst_pad_push_event (GST_VIDEO_SINK_PAD (self), event);

    if (!handled)
      gst_element_post_message (GST_ELEMENT_CAST (self),
          gst_navigation_message_new_event (GST_OBJECT_CAST (self), event));

    gst_event_unref (event);
  }
}

static void
gst_d3d11_video_sink_navigation_init (GstNavigationInterface * iface)
{
  iface->send_event_simple = gst_d3d11_video_sink_navigation_send_event;
}

static gboolean
gst_d3d11_video_sink_draw_action (GstD3D11VideoSink * self,
    gpointer shared_handle, guint texture_misc_flags,
    guint64 acquire_key, guint64 release_key)
{
  GstFlowReturn ret;
  g_return_val_if_fail (shared_handle != NULL, FALSE);

  if (!self->draw_on_shared_texture) {
    GST_ERROR_OBJECT (self, "Invalid draw call, we are drawing on window");
    return FALSE;
  }

  if (!shared_handle) {
    GST_ERROR_OBJECT (self, "Invalid handle");
    return FALSE;
  }

  GstD3D11CSLockGuard lk (&self->lock);
  if (!self->drawing || !self->current_buffer) {
    GST_WARNING_OBJECT (self, "Nothing to draw");
    return FALSE;
  }

  GST_LOG_OBJECT (self, "Drawing on shared handle %p, MiscFlags: 0x%x"
      ", acquire key: %" G_GUINT64_FORMAT ", release key: %"
      G_GUINT64_FORMAT, shared_handle, texture_misc_flags, acquire_key,
      release_key);

  ret = gst_d3d11_window_render_on_shared_handle (self->window,
      self->current_buffer, shared_handle, texture_misc_flags, acquire_key,
      release_key);

  return ret == GST_FLOW_OK;
}