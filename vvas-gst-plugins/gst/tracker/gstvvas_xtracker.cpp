/*
 * Copyright 2022 Xilinx, Inc.
 * Copyright (C) 2022-2023 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/vvas/gstinferencemeta.h>
#include <gst/vvas/gstinferenceprediction.h>
#include <gst/vvas/gstvvasallocator.h>
#include <gst/vvas/gstvvascoreutils.h>
#include <gst/vvas/gstvvassrcidmeta.h>
#ifdef XLNX_PCIe_PLATFORM
#include <experimental/xrt-next.h>
#else
#endif

#include "gstvvas_xtracker.h"
#include <vvas_core/vvas_context.h>
#include <vvas_core/vvas_tracker.hpp>
#include <vvas_utils/vvas_node.h>
extern "C"
{
#include <gst/vvas/gstvvasutils.h>
}

/**
 *  @brief Defines a static GstDebugCategory global variable "gst_vvas_xtracker_debug"
 */
GST_DEBUG_CATEGORY_STATIC (gst_vvas_xtracker_debug);

/** @def GST_CAT_DEFAULT
 *  @brief Setting gst_vvas_xtracker_debug as default debug category for logging
 */
#define GST_CAT_DEFAULT gst_vvas_xtracker_debug
/**
 *  @brief Defines a static GstDebugCategory global variable with name
 *         GST_CAT_PERFORMANCE for performance logging purpose
 */
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

typedef struct _GstVvas_XTrackerPrivate GstVvas_XTrackerPrivate;

/** @enum
 *  @brief  Contains properties related to tracker configuration
 */
enum
{
  PROP_0,                          // Default, değiştirme
  PROP_USE_MATCHING_COLOR_SPACE,         
  PROP_MODEL_PATH,                 // std::string MODEL_PATH;
  PROP_TRACKER_TYPE,               // VvasTrackerAlgoType tracker_type;
  PROP_OUTPUT_SIZE,                // int OUTPUT_SIZE;
  PROP_EXEMPLAR_SIZE,              // int EXEMPLAR_SIZE;
  PROP_SEARCH_SIZE,                // int SEARCH_SIZE;
  PROP_CONTEXT_AMOUNT,             // float CONTEXT_AMOUNT;
  PROP_INSTANCE_SIZE,              // int INSTANCE_SIZE;
  PROP_PENALTY_K,                  // float PENALTY_K;
  PROP_WINDOW_INFLUENCE,           // float WINDOW_INFLUENCE;
  PROP_LR,                         // float LR;
  PROP_W2,                         // float w2;
  PROP_W3,                         // float w3;
};
/** @struct TrackerInstances
 *  @brief  Holds tracker instances
 */
struct TrackerInstances
{
  /** pointer to base tracker */
  VvasTracker *vvasbase_tracker;
};

/** @struct _GstVvas_XTrackerPrivate
 *  @brief  Holds private members related tracker
 */
struct _GstVvas_XTrackerPrivate
{
  /** Contains image properties from input caps */
  GstVideoInfo *in_vinfo;
  /** Contains tracker configure information */
  VvasTrackerPRLConfig tconfig;
  /** contains sourceId and tracker instances mapping */
  GHashTable *tracker_instances_hash;
  /** global context for vvas tracker */
  VvasContext *vvas_gctx;
};

/**
 *  @brief Defines sink pad template
 */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{BGR}")));

/**
 *  @brief Defines source pad template
 */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{BGR}")));

#define gst_vvas_xtracker_parent_class parent_class

/** @brief  Glib's convenience macro for GstVvas_XTracker type implementation.
 *  @details This macro does below tasks:\n
 *           - Declares a class initialization function with prefix gst_vvas_xtracker \n
 */
G_DEFINE_TYPE_WITH_PRIVATE (GstVvas_XTracker, gst_vvas_xtracker,
    GST_TYPE_BASE_TRANSFORM);

/** @def GST_VVAS_XTRACKER_PRIVATE(self)
 *  @brief Get instance of GstVvas_XTrackerPrivate structure
 */
#define GST_VVAS_XTRACKER_PRIVATE(self) (GstVvas_XTrackerPrivate *) (gst_vvas_xtracker_get_instance_private (self))

/* Funtion declrations */
static void gst_vvas_xtracker_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_vvas_xtracker_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstFlowReturn gst_vvas_xtracker_transform_ip (GstBaseTransform * base,
    GstBuffer * outbuf);
static gboolean gst_vvas_xtracker_sink_event(GstBaseTransform *trans,
    GstEvent *event);

/** @def GST_TYPE_VVAS_TRACKER_ALGO_TYPE
 *  @brief Registers a new static enumeration type with the name GstVvasTrackerAlgoType
 */
#define GST_TYPE_VVAS_TRACKER_ALGO_TYPE (gst_vvas_tracker_tracker_algo_type ())

/** @enum GstVvasTrackerAlgoType
 *  @brief Enum representing tracker algorithm type
 *  @note Algorithm to be used for tracking objects across frames
 */
typedef enum
{
  /** PRL algorithm */
  GST_TRACKER_ALGO_PRL,
  GST_TRACKER_ALGO_NONE,
} GstVvasTrackerAlgoType;

/**
 *  @fn GType gst_vvas_tracker_tracker_algo_type (void)
 *  @return enumeration identifier type
 *  @brief  Registers a new static enumeration type with the GstVvasTrackerAlgoType
 */
static GType
gst_vvas_tracker_tracker_algo_type (void)
{
  static GType qtype = 0;
  if (qtype == 0) {
    static const GEnumValue tracker_algo_type[] = {
      {GST_TRACKER_ALGO_PRL, "Tracker PRL Algorithm", "PRL"},
      {0, NULL, NULL}
    };
    qtype =
        g_enum_register_static ("GstVvasTrackerAlgoType", tracker_algo_type);
  }
  return qtype;
}

/** @def GST_TYPE_VVAS_TRACKER_FEATURE_LENGTH
 *  @brief Registers a new static enumeration type with the name GstVvasTrackerFeatureLength
 */
#define GST_TYPE_VVAS_TRACKER_FEATURE_LENGTH (gst_vvas_tracker_feature_length_type ())

/**
 *  @fn GType gst_vvas_tracker_feature_length_type (void)
 *  @return enumeration identifier type
 *  @brief  Registers a new static enumeration type with the GstVvasTrackerFeatureLength
 */
static GType
gst_vvas_tracker_feature_length_type (void)
{
  static GType qtype = 0;
  if (qtype == 0) {
    static const GEnumValue feature_length_type[] = {
      {22, "Feature length of 22", "22"},
      {31, "Feature length of 31", "31"},
      {0, NULL, NULL}
    };
    qtype =
        g_enum_register_static ("GstVvasTrackerFeatureLength",
        feature_length_type);
  }
  return qtype;
}

/** @def GST_TYPE_VVAS_TRACKER_SEARCH_SCALE
 *  @brief Registers a new static enumeration type with the name GstVvasTrackerSearchScale
 */

/** @enum GstVvasTrackerSearchScale
 *  @brief Enum representing search scales to be used for tracking
 *  @note To set search scale to be used during tracking. Default searches in all scales.
 */
typedef enum
{
  /** Search for object both in up, same and down scale */
  GST_SEARCH_SCALE_ALL,
   /** Search for object in up and same scale only */
  GST_SEARCH_SCALE_UP,
  /** Search for object in down and same scale only */
  GST_SEARCH_SCALE_DOWN,
  /** Search for in same scale */
  GST_SEARCH_SCALE_NONE,
} GstVvasTrackerSearchScale;




/** @def GST_TYPE_VVAS_TRACKER_MATCHING_COLOR_SPACE
 *  @brief Registers a new static enumeration type with the name GstVVasTrackerMatchColorSpace
 */
#define GST_TYPE_VVAS_TRACKER_MATCHING_COLOR_SPACE (gst_vvas_tracker_match_color_space ())

/** @enum GstVVasTrackerMatchColorSpace
 *  @brief Enum representing color space used for object matching
 *  @note Color space to be used during object matching.  RGB is less complex
 *        compare to HSV.
 */
typedef enum
{
  /** Use RGB color space for object matching */
  GST_TRACKER_USE_RGB,
  /** Use HSV (Hue-Saturation-Value) color space for object matching */
  GST_TRACKER_USE_HSV,
} GstVVasTrackerMatchColorSpace;

/**
 *  @fn GType gst_vvas_tracker_match_color_space (void)
 *  @return enumeration identifier type
 *  @brief  Registers a new static enumeration type with the GstVVasTrackerMatchColorSpace
 */
static GType
gst_vvas_tracker_match_color_space (void)
{
  static GType qtype = 0;
  if (qtype == 0) {
    static const GEnumValue match_color_space_type[] = {
      {GST_TRACKER_USE_RGB, "Uses rgb color space for matching", "rgb"},
      {GST_TRACKER_USE_HSV, "Uses hsv color space for matching", "hsv"},
      {0, NULL, NULL}
    };
    qtype =
        g_enum_register_static ("GstVVasTrackerMatchColorSpace",
        match_color_space_type);
  }
  return qtype;
}

/** @def GST_VVAS_TRACKER_MODEL_PATH_DEFAULT
 *  @brief Default model path for PRL tracker.
 */
#define GST_VVAS_TRACKER_MODEL_PATH_DEFAULT ""

/** @def GST_VVAS_TRACKER_OBJ_MATCH_COLOR_DEFAULT
 *  @brief Default color space for matching objects.
 */
#define GST_VVAS_TRACKER_OBJ_MATCH_COLOR_DEFAULT   (TRACKER_USE_RGB)

/** @def GST_VVAS_TRACKER_TRACKER_TYPE_DEFAULT
 *  @brief Default algorithm for PRL tracking.
 */
#define GST_VVAS_TRACKER_TRACKER_TYPE_DEFAULT      (TRACKER_ALGO_NONE)

/** @def GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT
 *  @brief Default output size for PRL tracker.
 */
#define GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT       (11)

/** @def GST_VVAS_TRACKER_EXEMPLAR_SIZE_DEFAULT
 *  @brief Default exemplar size for PRL tracker.
 */
#define GST_VVAS_TRACKER_EXEMPLAR_SIZE_DEFAULT     (127)

/** @def GST_VVAS_TRACKER_SEARCH_SIZE_DEFAULT
 *  @brief Default search size for PRL tracker.
 */
#define GST_VVAS_TRACKER_SEARCH_SIZE_DEFAULT       (287)

/** @def GST_VVAS_TRACKER_CONTEXT_AMOUNT_DEFAULT
 *  @brief Default context amount for PRL tracker.
 */
#define GST_VVAS_TRACKER_CONTEXT_AMOUNT_DEFAULT    (0.5f)

/** @def GST_VVAS_TRACKER_INSTANCE_SIZE_DEFAULT
 *  @brief Default instance size for PRL tracker.
 */
#define GST_VVAS_TRACKER_INSTANCE_SIZE_DEFAULT     (287)

/** @def GST_VVAS_TRACKER_PENALTY_K_DEFAULT
 *  @brief Default penalty_k for PRL tracker.
 */
#define GST_VVAS_TRACKER_PENALTY_K_DEFAULT         (0.05f)

/** @def GST_VVAS_TRACKER_WINDOW_INFLUENCE_DEFAULT
 *  @brief Default window influence for PRL tracker.
 */
#define GST_VVAS_TRACKER_WINDOW_INFLUENCE_DEFAULT  (0.45f)

/** @def GST_VVAS_TRACKER_LR_DEFAULT
 *  @brief Default learning rate for PRL tracker.
 */
#define GST_VVAS_TRACKER_LR_DEFAULT                (0.40f)

/** @def GST_VVAS_TRACKER_W2_DEFAULT
 *  @brief Default w2 value for PRL tracker.
 */
#define GST_VVAS_TRACKER_W2_DEFAULT                (0.2f)

/** @def GST_VVAS_TRACKER_W3_DEFAULT
 *  @brief Default w3 value for PRL tracker.
 */
#define GST_VVAS_TRACKER_W3_DEFAULT                (0.1f)

/**
 *  @fn gboolean vvas_xtracker_deinit (GstVvas_XTracker * self)
 *  @param [inout] self - Pointer to GstVvas_XTracker structure.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  Free all allocated memory of trackers and image buffers
 *  @note  Calls deinit_tracker function from tracker library to deallocate memory
 *         of tracker objects.
 *
 */
static gboolean
vvas_xtracker_deinit (GstVvas_XTracker * self)
{
  bool iret = TRUE;
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->priv->tracker_instances_hash);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    struct TrackerInstances *instance;
    instance = (struct TrackerInstances *) value;
    if (instance && instance->vvasbase_tracker) {
      /* calling tracker deinitialization function */
      iret = vvas_tracker_destroy(instance->vvasbase_tracker);
      if (iret)
        GST_DEBUG_OBJECT (self, "successfully completed tracker deinit");
      else
        GST_ERROR_OBJECT (self, "Failed to free tracker instances");
    }
  }

  g_hash_table_unref(self->priv->tracker_instances_hash);
  return iret;
}

/**
 *  @fn gboolean gst_vvas_xtracker_start (GstBaseTransform * trans)
 *  @param [in] trans - Pointer to GstBaseTransform object.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  This API Allocates memory and sets the transform type.
 *  @details This API is registered with GObjectClass by overriding GstBaseTransform::start function pointer and
 *          this will be called when element start processing. It invokes tracker initialization and allocates memory.
 */
static gboolean
gst_vvas_xtracker_start (GstBaseTransform * trans)
{
  GstVvas_XTracker *self = GST_VVAS_XTRACKER (trans);
  GstVvas_XTrackerPrivate *priv = self->priv;
  VvasReturnType vret;

  self->priv = priv;
  priv->in_vinfo = gst_video_info_new ();

  /* Create global context for vvas core */
  priv->vvas_gctx = vvas_context_create (0, NULL, LOG_LEVEL_ERROR, &vret);
  if (!priv->vvas_gctx || VVAS_IS_ERROR (vret)) {
    GST_ERROR_OBJECT (self,
        "ERROR: Failed to create vvas global context for tracker\n");
  }

  gst_base_transform_set_in_place (trans, true);

  return TRUE;
}

/**
 *  @fn gboolean gst_vvas_xtracker_stop (GstBaseTransform * trans)
 *  @param [in] trans - Pointer to GstBaseTransform object.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  Free up Allocates memory.
 *  @details This API is registered with GObjectClass by overriding GstBaseTransform::stop function pointer and
 *          this will be called when element stops processing.
 *          It invokes tracker de-initialization and free up allocated memory.
 *
 */
static gboolean
gst_vvas_xtracker_stop (GstBaseTransform * trans)
{
  GstVvas_XTracker *self = GST_VVAS_XTRACKER (trans);
  GST_DEBUG_OBJECT (self, "stopping");

  if (self->priv->vvas_gctx) {
    vvas_context_destroy (self->priv->vvas_gctx);
  }
  gst_video_info_free (self->priv->in_vinfo);
  vvas_xtracker_deinit (self);
  return TRUE;
}

/**
 *  @fn gboolean gst_vvas_xtracker_set_caps (GstBaseTransform * trans,
 *                                GstCaps * incaps, GstCaps * outcaps)
 *  @param [in] trans - Pointer to GstBaseTransform object.
 *  @param [in] incaps - Pointer to input caps of GstCaps object.
 *  @param [in] outcaps - Pointer to output caps  of GstCaps object.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  API to get input and output capabilities.
 *  @details This API is registered with GObjectClass by overriding GObjectClass::set_caps function pointer and
 *          this will be called to get the input and output capabilities.
 */
static gboolean
gst_vvas_xtracker_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstVvas_XTracker *self = GST_VVAS_XTRACKER (trans);
  gboolean bret = TRUE;
  GstVvas_XTrackerPrivate *priv = self->priv;

  GST_INFO_OBJECT (self,
      "incaps = %" GST_PTR_FORMAT "and outcaps = %" GST_PTR_FORMAT, incaps,
      outcaps);

  /* Reading input caps into in_vinfo */
  if (!gst_video_info_from_caps (priv->in_vinfo, incaps)) {
    GST_ERROR_OBJECT (self, "Failed to parse input caps");
    return FALSE;
  }

  return bret;
}

/**
 *  @fn static void gst_vvas_xtracker_class_init (GstVvas_XTrackerClass * klass)
 *  @param [in]klass  - Handle to GstVvas_XTrackerClass
 *  @return None
 *  @brief  Add properties and signals of GstVvas_XTracker to parent GObjectClass \n
 *          and overrides function pointers present in itself and/or its parent class structures
 *  @details This function publishes properties those can be set/get from application on GstVvas_XTracker object.
 *           And, while publishing a property it also declares type, range of acceptable values, default value,
 *           readability/writability and in which GStreamer state a property can be changed.
 */
static void
gst_vvas_xtracker_class_init (GstVvas_XTrackerClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *transform_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_vvas_xtracker_set_property;
  gobject_class->get_property = gst_vvas_xtracker_get_property;

  transform_class->start = gst_vvas_xtracker_start;
  transform_class->stop = gst_vvas_xtracker_stop;
  transform_class->set_caps = gst_vvas_xtracker_set_caps;
  transform_class->transform_ip = gst_vvas_xtracker_transform_ip;
  transform_class->sink_event = gst_vvas_xtracker_sink_event;

  
  g_object_class_install_property (gobject_class, PROP_USE_MATCHING_COLOR_SPACE,
    g_param_spec_enum ("obj-match-color", "Object Match Color",
        "Object match color space for PRL tracker",
        GST_TYPE_VVAS_TRACKER_MATCHING_COLOR_SPACE,
        GST_VVAS_TRACKER_OBJ_MATCH_COLOR_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));


g_object_class_install_property (gobject_class, PROP_MODEL_PATH,
    g_param_spec_string ("model-path", "Model Path",
        "Path to the PRL model file",
        GST_VVAS_TRACKER_MODEL_PATH_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_OUTPUT_SIZE,
    g_param_spec_int ("output-size", "Output Size",
        "Output size for PRL tracker",
        1, 2048, GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_EXEMPLAR_SIZE,
    g_param_spec_int ("exemplar-size", "Exemplar Size",
        "Exemplar size for PRL tracker",
        1, 2048, GST_VVAS_TRACKER_EXEMPLAR_SIZE_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_SEARCH_SIZE,
    g_param_spec_int ("search-size", "Search Size",
        "Search size for PRL tracker",
        1, 2048, GST_VVAS_TRACKER_SEARCH_SIZE_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_CONTEXT_AMOUNT,
    g_param_spec_float ("context-amount", "Context Amount",
        "Context amount for PRL tracker",
        0.0, 10.0, GST_VVAS_TRACKER_CONTEXT_AMOUNT_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_INSTANCE_SIZE,
    g_param_spec_int ("instance-size", "Instance Size",
        "Instance size for PRL tracker",
        1, 2048, GST_VVAS_TRACKER_INSTANCE_SIZE_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_PENALTY_K,
    g_param_spec_float ("penalty-k", "Penalty K",
        "Penalty K for PRL tracker",
        0.0, 10.0, GST_VVAS_TRACKER_PENALTY_K_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_WINDOW_INFLUENCE,
    g_param_spec_float ("window-influence", "Window Influence",
        "Window influence for PRL tracker",
        0.0, 10.0, GST_VVAS_TRACKER_WINDOW_INFLUENCE_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_LR,
    g_param_spec_float ("lr", "Learning Rate",
        "Learning rate for PRL tracker",
        0.0, 1.0, GST_VVAS_TRACKER_LR_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_W2,
    g_param_spec_float ("w2", "W2",
        "W2 for PRL tracker",
        0.0, 10.0, GST_VVAS_TRACKER_W2_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

g_object_class_install_property (gobject_class, PROP_W3,
    g_param_spec_float ("w3", "W3",
        "W3 for PRL tracker",
        0.0, 10.0, GST_VVAS_TRACKER_W3_DEFAULT,
        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));


  gst_element_class_set_details_simple (gstelement_class,
      "VVAS Tracker Plugin",
      "Object Tracking",
      "Performs Object tracking based on cnn",
      "Sararge");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));

  GST_DEBUG_CATEGORY_INIT (gst_vvas_xtracker_debug, "vvas_xtracker", 0,
      "VVAS Tracker Plugin");
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");
}

/**
 *  @fn static void gst_vvas_xtracker_init (GstVvas_XTracker * self)
 *  @param [in] self  - Handle to GstVvas_XTracker instance
 *  @return None
 *  @brief  Initializes GstVvas_XTracker member variables to default values
 *
 */
static void
gst_vvas_xtracker_init (GstVvas_XTracker * self)
{
  GstVvas_XTrackerPrivate *priv = GST_VVAS_XTRACKER_PRIVATE (self);
  self->priv = priv;

  self->tracker_algo = GST_VVAS_TRACKER_TRACKER_TYPE_DEFAULT;
  //self->search_scale = GST_VVAS_TRACKER_SEARCH_SCALE_DEFAULT;
  self->match_color = GST_TYPE_VVAS_TRACKER_MATCHING_COLOR_SPACE;

  if (self->tracker_algo == GST_TRACKER_ALGO_PRL)
    priv->tconfig.tracker_type = TRACKER_PRL;
  

  if (self->match_color == GST_TRACKER_USE_RGB)
    priv->tconfig.obj_match_color = TRACKER_USE_RGB;
  else if (self->match_color == GST_TRACKER_USE_HSV)
    priv->tconfig.obj_match_color = TRACKER_USE_HSV;


  priv->tconfig.OUTPUT_SIZE   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.EXEMPLAR_SIZE   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.SEARCH_SIZE   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.CONTEXT_AMOUNT   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.INSTANCE_SIZE   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.PENALTY_K   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.WINDOW_INFLUENCE   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.LR   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.w2   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.w3   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tconfig.MODEL_PATH   = GST_VVAS_TRACKER_OUTPUT_SIZE_DEFAULT;
  priv->tracker_instances_hash =
      g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
}

/**
 *  @fn static void gst_vvas_xtracker_set_property (GObject * object, guint prop_id,
 *                                                  const GValue * value, GParamSpec * pspec)
 *  @param [in] object - Handle to GstVvas_XTracker typecast to GObject
 *  @param [in] prop_id - Property ID as defined in properties enum
 *  @param [in] value - value GValue which holds property value set by user
 *  @param [in] pspec - Handle to metadata of a property with property ID \p prop_id
 *  @return None
 *  @brief This API stores values sent from the user in GstVvas_XTracker object members.
 *  @details This API is registered with GObjectClass by overriding GObjectClass::set_property function pointer and
 *           this will be invoked when developer sets properties on GstVvas_XTracker object.
 *           Based on property value type, corresponding g_value_get_xxx API will be called to get
 *           property value from GValue handle.
 */


static void
gst_vvas_xtracker_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVvas_XTracker *self = GST_VVAS_XTRACKER (object);
  GstVvas_XTrackerPrivate *priv = self->priv;
  

  switch (prop_id) {
    case PROP_USE_MATCHING_COLOR_SPACE:
      self->match_color = g_value_get_enum (value);
      if (self->match_color == GST_TRACKER_USE_RGB)
        priv->tconfig.obj_match_color = TRACKER_USE_RGB;
      else if (self->match_color == GST_TRACKER_USE_HSV)
        priv->tconfig.obj_match_color = TRACKER_USE_HSV;
      break;
    case PROP_MODEL_PATH: {
      const gchar *model_path = g_value_get_string(value);
      if (model_path)
        priv->tconfig.MODEL_PATH = model_path;
      break;
    }
    case PROP_TRACKER_TYPE:
      self->tracker_algo = g_value_get_enum (value);
      if (self->tracker_algo == GST_TRACKER_ALGO_PRL)
        priv->tconfig.tracker_type = TRACKER_PRL;
      else
        GST_ERROR_OBJECT (self, "Invalid Tracker type %d set\n",
            self->tracker_algo);
      break;
    case PROP_OUTPUT_SIZE:
      priv->tconfig.OUTPUT_SIZE = g_value_get_int(value);
      break;
    case PROP_EXEMPLAR_SIZE:
      priv->tconfig.EXEMPLAR_SIZE = g_value_get_int(value);
      break;
    case PROP_SEARCH_SIZE:
      priv->tconfig.SEARCH_SIZE = g_value_get_int(value);
      break;
    case PROP_CONTEXT_AMOUNT:
      priv->tconfig.CONTEXT_AMOUNT = g_value_get_float(value);
      break;
    case PROP_INSTANCE_SIZE:
      priv->tconfig.INSTANCE_SIZE = g_value_get_int(value);
      break;
    case PROP_PENALTY_K:
      priv->tconfig.PENALTY_K = g_value_get_float(value);
      break;
    case PROP_WINDOW_INFLUENCE:
      priv->tconfig.WINDOW_INFLUENCE = g_value_get_float(value);
      break;
    case PROP_LR:
      priv->tconfig.LR = g_value_get_float(value);
      break;
    case PROP_W2:
      priv->tconfig.w2 = g_value_get_float(value);
      break;
    case PROP_W3:
      priv->tconfig.w3 = g_value_get_float(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}
/**
 *  @fn static void gst_vvas_xtracker_get_property (GObject * object, guint prop_id,
 *                                                  const GValue * value, GParamSpec * pspec)
 *  @param [in] object - Handle to GstVvas_XTracker typecasted to GObject
 *  @param [in] prop_id - Property ID as defined in properties enum
 *  @param [in] value - value GValue which holds property value set by user
 *  @param [in] pspec - Handle to metadata of a property with property ID \p prop_id
 *  @return None
 *  @brief This API gets values from GstVvas_XTracker object members.
 *  @details This API is registered with GObjectClass by overriding GObjectClass::get_property function pointer and
 *           this will be invoked when developer want gets properties from GstVvas_XTracker object.
 *           Based on property value type,corresponding g_value_set_xxx API will be called to set value of GValue type.
 */
static void
gst_vvas_xtracker_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVvas_XTracker *self = GST_VVAS_XTRACKER (object);
  GstVvas_XTrackerPrivate *priv = self->priv;

  switch (prop_id) {
    case PROP_USE_MATCHING_COLOR_SPACE:
      g_value_set_enum(value, priv->tconfig.obj_match_color);
      break;
    
      case PROP_MODEL_PATH:
      g_value_set_string(value, priv->tconfig.MODEL_PATH.c_str());
      break;

    case PROP_TRACKER_TYPE:
      g_value_set_enum(value, priv->tconfig.tracker_type);
      break;
    case PROP_OUTPUT_SIZE:
      g_value_set_int(value, priv->tconfig.OUTPUT_SIZE);
      break;
    case PROP_EXEMPLAR_SIZE:
      g_value_set_int(value, priv->tconfig.EXEMPLAR_SIZE);
      break;
    case PROP_SEARCH_SIZE:
      g_value_set_int(value, priv->tconfig.SEARCH_SIZE);
      break;
    case PROP_CONTEXT_AMOUNT:
      g_value_set_float(value, priv->tconfig.CONTEXT_AMOUNT);
      break;
    case PROP_INSTANCE_SIZE:
      g_value_set_int(value, priv->tconfig.INSTANCE_SIZE);
      break;
    case PROP_PENALTY_K:
      g_value_set_float(value, priv->tconfig.PENALTY_K);
      break;
    case PROP_WINDOW_INFLUENCE:
      g_value_set_float(value, priv->tconfig.WINDOW_INFLUENCE);
      break;
    case PROP_LR:
      g_value_set_float(value, priv->tconfig.LR);
      break;
    case PROP_W2:
      g_value_set_float(value, priv->tconfig.w2);
      break;
    case PROP_W3:
      g_value_set_float(value, priv->tconfig.w3);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/**
 *  @fn static gboolean gst_vvas_xtracker_sink_event (GstBaseTransform * trans, GstEvent * event)
 *  @param [in] trans - xtracker's parents instance handle which will be type casted to xtracker instance
 *  @param [in] event - GstEvent received by xtracker instance on sink pad
 *  @return TRUE if event handled successfully
 *          FALSE if event is not handled
 *  @brief Handle the GstEvent and invokes parent's event vmethod if event is not handled by xtracker
 */

static gboolean gst_vvas_xtracker_sink_event(GstBaseTransform *trans,
                                            GstEvent *event)
{
  GstVvas_XTracker *self = GST_VVAS_XTRACKER(trans);
  GstVvas_XTrackerPrivate *priv = self->priv;
  gboolean bret = TRUE;
  const GstStructure *structure = NULL;
  guint pad_idx;

  switch (GST_EVENT_TYPE(event))
  {
    case GST_EVENT_STREAM_START:
    {
      struct TrackerInstances *instance;
      structure = gst_event_get_structure(event);
      gst_structure_get_uint(structure, "pad-index", &pad_idx);
      if (!gst_structure_get_uint(structure, "pad-index", &pad_idx))
      {
        /* may be without funnel i.e., single stream */
        pad_idx = 0;
      }
      instance = (struct TrackerInstances *)malloc(sizeof(struct TrackerInstances));

      /* calling tracker initialization function */
      instance->vvasbase_tracker = vvas_tracker_create(priv->vvas_gctx, &priv->tconfig);
      if (instance->vvasbase_tracker == NULL)
      {
        GST_ERROR_OBJECT(self, "Failed to create tracker instance");
        return FALSE;
      }
      g_hash_table_insert(priv->tracker_instances_hash, GUINT_TO_POINTER(pad_idx), instance);

      bret = gst_pad_event_default(trans->sinkpad,
	            gst_element_get_parent(GST_ELEMENT(trans)), event);
      break;
    }

    case GST_EVENT_CUSTOM_DOWNSTREAM:
    {
      const GstStructure *structure = NULL;
      guint pad_idx;
      int iret;
      structure = gst_event_get_structure(event);
      if (!gst_structure_get_uint(structure, "pad-index", &pad_idx))
      {
        /* may be without funnel i.e., single stream */
        pad_idx = 0;
      }
      if (!g_strcmp0(gst_structure_get_name(structure), "pad-eos"))
      {
        struct TrackerInstances *instance =
	        (struct TrackerInstances *)g_hash_table_lookup(priv->tracker_instances_hash,
		  GUINT_TO_POINTER(pad_idx));
        GST_LOG_OBJECT(self, "received pad-eos");
        if (instance)
        {
          /* calling tracker deinitialization function */
          iret = vvas_tracker_destroy(instance->vvasbase_tracker);

          if (iret)
            GST_DEBUG_OBJECT(self, "successfully completed tracker deinit");
          else
            GST_ERROR_OBJECT(self, "Failed to free tracker instances");

          g_hash_table_remove(priv->tracker_instances_hash,
                              GINT_TO_POINTER(pad_idx));
        }
      }
    }

    default:
    {
      bret = gst_pad_event_default(trans->sinkpad,
	            gst_element_get_parent(GST_ELEMENT(trans)), event);
      break;
    }
  }
  return bret;
}

/**
 *  @fn gboolean gst_vvas_xtracker_transform_ip (GstBaseTransform * base, GstBuffer * buf)
 *  @param [inout] base - Pointer to GstBaseTransform object.
 *  @param [in] buf - Pointer to input buffer of type GstBuffer.
 *  @return TRUE on success \n
 *          FALSE on failure
 *  @brief  This API called every frame for inplace processing to updates the tracking objects info
 *  @details This API is registered with GObjectClass by overriding GstBaseTransform::transform_ip function pointer and
 *          this will be called for every frame for inplace processing. It prepares the input buffer
 *          for processing then invokes tracker. Upon processing updates prediction metadata with tracked objects.
 */
static GstFlowReturn
gst_vvas_xtracker_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  GstVvas_XTracker *self = GST_VVAS_XTRACKER (base);
  VvasReturnType vvas_ret = VVAS_RET_ERROR;
  VvasVideoFrame *pFrame;
  GstInferenceMeta *infer_meta = NULL;
  VvasInferPrediction *vvas_infer_meta = NULL;
  GstVvasSrcIDMeta *srcId_meta = NULL;
  struct TrackerInstances *instance;

  /* Get inference metadata from the Gstbuffer */
  infer_meta = ((GstInferenceMeta *) gst_buffer_get_meta (buf,
          gst_inference_meta_api_get_type ()));

  /* Convert gstinference meta to inference meta if Gstinference meta
     avalable. Else create inference meta structure */
  if (infer_meta != NULL)
    vvas_infer_meta = vvas_infer_from_gstinfer (infer_meta->prediction);

  /* Get SrcId metadata from the Gstbuffer */
  srcId_meta = ((GstVvasSrcIDMeta *)gst_buffer_get_meta(buf,
                        gst_vvas_srcid_meta_api_get_type()));

  if (srcId_meta)
  {
    instance = (struct TrackerInstances *)g_hash_table_lookup(
		    self->priv->tracker_instances_hash,
		    GUINT_TO_POINTER(srcId_meta->src_id));
  }
  else
  {
    /* may be single source... use index 0 */
    instance = (struct TrackerInstances *)g_hash_table_lookup(
		    self->priv->tracker_instances_hash, 0);
  }

  /* Check if buffer is from pool.  If buffer is from pool use aligments from
     pool to create vvas frame buffer */
  pFrame = vvas_videoframe_from_gstbuffer (self->priv->vvas_gctx, -1, buf,
      self->priv->in_vinfo, GST_MAP_READ);
  if (pFrame == NULL) {
    GST_ERROR_OBJECT (self, "Failed to convert gstbuffer to vvas video frame");
    return GST_FLOW_ERROR;
  }

  /* Calling vvas-core tracker function for frame processing */
  if (instance && instance->vvasbase_tracker) {
    vvas_ret = vvas_tracker_process (instance->vvasbase_tracker,
	                                 pFrame, &vvas_infer_meta);
  }
  else {
    GST_ERROR_OBJECT (self, "Tracker instance is not created");
    vvas_video_frame_free (pFrame);
    return GST_FLOW_ERROR;
  }

  if (VVAS_IS_ERROR (vvas_ret)) {
    GST_ERROR_OBJECT (self, "Failed to process frame");
    vvas_video_frame_free (pFrame);
    return GST_FLOW_ERROR;
  }

  vvas_video_frame_free (pFrame);

  if (vvas_infer_meta != NULL) {
    GstInferencePrediction *new_gst_pred = NULL;
    VvasList *iter = NULL;
    VvasList *pred_nodes = NULL;

    if (infer_meta == NULL) {
      infer_meta = (GstInferenceMeta *) gst_buffer_add_meta (buf,
          gst_inference_meta_get_info (), NULL);
    }

    pred_nodes = vvas_inferprediction_get_nodes (vvas_infer_meta);
    /** Convert root node */
    new_gst_pred = gst_infer_node_from_vvas_infer (vvas_infer_meta);
    /** Convert all leaf nodes and append to root */
    for (iter = pred_nodes; iter != NULL; iter = iter->next) {
      VvasInferPrediction *leaf = (VvasInferPrediction *) iter->data;
      gst_inference_prediction_append (new_gst_pred,
          gst_infer_node_from_vvas_infer (leaf));
    }
    vvas_list_free (pred_nodes);
    if (infer_meta->prediction)
      gst_inference_prediction_unref (infer_meta->prediction);
    infer_meta->prediction = new_gst_pred;
  }

  if (vvas_infer_meta != NULL) {
    vvas_inferprediction_free (vvas_infer_meta);
    vvas_infer_meta = NULL;
  }

  GST_LOG_OBJECT (self, "processed buffer %p", buf);

  return GST_FLOW_OK;
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
plugin_init (GstPlugin * vvas_xtracker)
{
  return gst_element_register (vvas_xtracker, "vvas_xtracker", GST_RANK_PRIMARY,
      GST_TYPE_VVAS_XTRACKER);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vvas_xtracker,
    "GStreamer VVAS plug-in for tracker",
    plugin_init, "0.1", GST_LICENSE_UNKNOWN,
    "GStreamer Xilinx Tracker", "http://xilinx.com/")
