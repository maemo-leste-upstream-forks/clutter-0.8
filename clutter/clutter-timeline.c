/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2006 OpenedHand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "clutter-timeline.h"
#include "clutter-main.h"
#include "clutter-private.h"   /* for DBG */

G_DEFINE_TYPE (ClutterTimeline, clutter_timeline, G_TYPE_OBJECT);

#define FPS_TO_INTERVAL(f) (1000/f)

struct ClutterTimelinePrivate
{
  guint    timeout_id;
  guint    fps;
  guint    nframes;
  guint    current_frame_num;
  gulong   last_frame_msecs;
  gulong   start_frame_secs;
  gboolean loop;
};

enum
{
  PROP_0,
  PROP_FPS,
  PROP_NFRAMES,
  PROP_LOOP
};

enum
{
  SIGNAL_NEW_FRAME,
  SIGNAL_COMPLETED,
  LAST_SIGNAL
};

static int timeline_signals[LAST_SIGNAL] = { 0 };

static void 
clutter_timeline_set_property (GObject      *object, 
			       guint         prop_id,
			       const GValue *value, 
			       GParamSpec   *pspec)
{
  ClutterTimeline        *timeline;
  ClutterTimelinePrivate *priv;

  timeline = CLUTTER_TIMELINE(object);
  priv = timeline->priv;

  switch (prop_id) 
    {
    case PROP_FPS:
      clutter_timeline_set_speed (timeline, g_value_get_int (value));
      break;
    case PROP_NFRAMES:
      priv->nframes = g_value_get_int (value);
      break;
    case PROP_LOOP:
      priv->loop = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void 
clutter_timeline_get_property (GObject    *object, 
			       guint       prop_id,
			       GValue     *value, 
			       GParamSpec *pspec)
{
  ClutterTimeline        *timeline;
  ClutterTimelinePrivate *priv;

  timeline = CLUTTER_TIMELINE(object);
  priv = timeline->priv;

  switch (prop_id) 
    {
    case PROP_FPS:
      g_value_set_int (value, priv->fps);
      break;
    case PROP_NFRAMES:
      g_value_set_int (value, priv->nframes);
      break;
    case PROP_LOOP:
      g_value_set_boolean (value, priv->loop);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void 
clutter_timeline_finalize (GObject *object)
{
  G_OBJECT_CLASS (clutter_timeline_parent_class)->finalize (object);
}

static void 
clutter_timeline_dispose (GObject *object)
{
  ClutterTimeline *self = CLUTTER_TIMELINE(object);
  ClutterTimelinePrivate *priv;

  priv = self->priv;

  if (priv != NULL)
    {
      if (priv->timeout_id)
	{
	  g_source_remove (priv->timeout_id);
	  priv->timeout_id = 0;
	}
    }

  G_OBJECT_CLASS (clutter_timeline_parent_class)->dispose (object);
}


static void
clutter_timeline_class_init (ClutterTimelineClass *klass)
{
  GObjectClass *object_class;

  object_class = (GObjectClass*) klass;

  object_class->set_property = clutter_timeline_set_property;
  object_class->get_property = clutter_timeline_get_property;
  object_class->finalize     = clutter_timeline_finalize;
  object_class->dispose      = clutter_timeline_dispose;

  g_type_class_add_private (klass, sizeof (ClutterTimelinePrivate));

  g_object_class_install_property
    (object_class, PROP_FPS,
     g_param_spec_int ("fps",
		       "Frames Per Second",
		       "Timeline frames per second",
		       0,
		       1000,
		       50,
		       G_PARAM_CONSTRUCT | G_PARAM_READWRITE));

  g_object_class_install_property
    (object_class, PROP_NFRAMES,
     g_param_spec_int ("num-frames",
		       "Total number of frames",
		       "Timelines total number of frames",
		       0,
		       G_MAXINT,
		       0,
		       G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE));

  g_object_class_install_property
    (object_class, PROP_LOOP,
     g_param_spec_boolean ("loop",
			   "Loop",
			   "Should the timeline automatically restart",
			   FALSE,
			   G_PARAM_CONSTRUCT | G_PARAM_READWRITE));

  timeline_signals[SIGNAL_NEW_FRAME] =
    g_signal_new ("new-frame",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterTimelineClass, new_frame),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__INT,
		  G_TYPE_NONE, 
		  1, G_TYPE_INT);

  timeline_signals[SIGNAL_COMPLETED] =
    g_signal_new ("completed",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterTimelineClass, completed),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);

}

static void
clutter_timeline_init (ClutterTimeline *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self,
					   CLUTTER_TYPE_TIMELINE,
					   ClutterTimelinePrivate);
}

static gboolean
timeline_timeout_func (gpointer data)
{
  ClutterTimeline        *timeline = CLUTTER_TIMELINE(data);
  ClutterTimelinePrivate *priv;
  GTimeVal                timeval;
  gint                    nframes;
  gulong                  msecs;

  priv = timeline->priv;

  /* Figure out potential frame skips */
  g_get_current_time (&timeval);

  /* Fire off signal */
  g_signal_emit (timeline, timeline_signals[SIGNAL_NEW_FRAME], 
		 0, priv->current_frame_num);

  /* Signal frees timeline ? */
  if (timeline == NULL)
    return FALSE;

  /* Signal removes source ? */
  if (!priv->timeout_id)
    {
      clutter_timeline_stop (timeline);
      return FALSE;
    }
  
  if (priv->last_frame_msecs)
    {
      /* Check time diff from out last call and adjust number 
       * of frames to advance accordingly. 
      */
      msecs = ((timeval.tv_sec - priv->start_frame_secs) * 1000) 
                 + (timeval.tv_usec / 1000);
      nframes =  (msecs - priv->last_frame_msecs ) / (1000 / priv->fps);
      if (nframes < 0) nframes = 1;

      if (nframes > 1)
	CLUTTER_DBG("*** Skipping %i frames ***", nframes);
    }
  else 
    {
      /* First frame, set up timings.*/
      priv->start_frame_secs = timeval.tv_sec;
      msecs     = timeval.tv_usec / 1000;
      nframes   = 1;
    }

  priv->last_frame_msecs = msecs; 

  /* Advance frames */
  priv->current_frame_num += nframes;;

  /* Handle loop or stop */
  if (priv->current_frame_num > priv->nframes)
    {
      priv->current_frame_num = priv->nframes;

      if (nframes > 1)
	g_signal_emit (timeline, timeline_signals[SIGNAL_NEW_FRAME], 
		       0, priv->current_frame_num);

      if (priv->loop)
	clutter_timeline_rewind (timeline);
      else
	{
	  clutter_timeline_stop (timeline);
	  g_signal_emit (timeline, timeline_signals[SIGNAL_COMPLETED], 0); 
	  return FALSE;
	}
    }

  return TRUE;
}

/**
 * clutter_timeline_start:
 * @timeline: A #ClutterTimeline
 *
 * Starts the #ClutterTimeline playing.
 **/
void
clutter_timeline_start (ClutterTimeline *timeline)
{
  ClutterTimelinePrivate *priv;

  priv = timeline->priv;

  if (!priv->timeout_id)
    priv->timeout_id = g_timeout_add (FPS_TO_INTERVAL(priv->fps),
				      timeline_timeout_func,
				      (gpointer)timeline);
}

/**
 * clutter_timeline_pause:
 * @timeline: A #ClutterTimeline
 *
 * Pauses the #ClutterTimeline on current frame
 **/
void
clutter_timeline_pause (ClutterTimeline *timeline)
{
  g_return_if_fail (CLUTTER_IS_TIMELINE (timeline));

  if (timeline->priv->timeout_id)
    g_source_remove (timeline->priv->timeout_id);

  timeline->priv->timeout_id = 0;
  timeline->priv->last_frame_msecs = 0;
}

/**
 * clutter_timeline_stop:
 * @timeline: A #ClutterTimeline
 *
 * Stops the #ClutterTimeline and moves to frame 0
 **/
void
clutter_timeline_stop (ClutterTimeline *timeline)
{
  clutter_timeline_pause (timeline);
  clutter_timeline_rewind (timeline);
}

void
clutter_timeline_set_loop (ClutterTimeline *timeline, gboolean loop)
{
  timeline->priv->loop = loop;
}

/**
 * clutter_timeline_rewind:
 * @timeline: A #ClutterTimeline
 *
 * Rewinds #ClutterTimeline to frame 0.
 **/
void
clutter_timeline_rewind (ClutterTimeline *timeline)
{
  clutter_timeline_advance (timeline, 0);
}

/**
 * clutter_timeline_advance:
 * @timeline: A #ClutterTimeline
 * @nframes: Number of frames to skip
 *
 * Advance timeline by requested number of frames.
 **/
void
clutter_timeline_skip (ClutterTimeline *timeline, guint nframes)
{
  ClutterTimelinePrivate *priv;

  g_return_if_fail (CLUTTER_IS_TIMELINE (timeline));

  priv = timeline->priv;

  priv->current_frame_num += nframes;

  if (priv->current_frame_num > priv->nframes)
    priv->current_frame_num = 1;
}

/**
 * clutter_timeline_advance:
 * @timeline: A #ClutterTimeline
 * @frame_num: Frame number to advance to
 *
 * Advance timeline to requested frame number
 **/
void
clutter_timeline_advance (ClutterTimeline *timeline, guint frame_num)
{
  ClutterTimelinePrivate *priv;

  g_return_if_fail (CLUTTER_IS_TIMELINE (timeline));

  priv = timeline->priv;

  if (frame_num < priv->nframes)
    priv->current_frame_num = frame_num;
}

/**
 * clutter_timeline_get_current_frame:
 * @timeline: A #ClutterTimeline
 *
 * Request the current frame number of the timeline.
 *
 * Return Value: current frame number
 **/
gint
clutter_timeline_get_current_frame (ClutterTimeline *timeline)
{
  g_return_val_if_fail (CLUTTER_IS_TIMELINE (timeline), 0);

  return timeline->priv->current_frame_num;
}

/**
 * clutter_timeline_get_n_frames:
 * @timeline: A #ClutterTimeline
 *
 * Request the totle number of frames for the #ClutterTimeline.
 *
 * Return Value: Number of frames for this #ClutterTimeline.
 **/
guint
clutter_timeline_get_n_frames (ClutterTimeline *timeline)
{
  g_return_val_if_fail (CLUTTER_IS_TIMELINE (timeline), 0);

  return timeline->priv->nframes;
}

/**
 * clutter_timeline_get_current_frame:
 * @timeline: A #ClutterTimeline
 * @fps: New speed of timeline as frames per second
 *
 * Set the speed in frames per second of the timeline.
 **/
void
clutter_timeline_set_speed (ClutterTimeline *timeline, guint fps)
{
  ClutterTimelinePrivate *priv;

  g_return_if_fail (CLUTTER_IS_TIMELINE (timeline));

  priv = timeline->priv;

  priv->fps = fps;

  /* if the timeline is playing restart */
  if (priv->timeout_id)
    {
      g_source_remove (priv->timeout_id);
      priv->timeout_id = g_timeout_add (FPS_TO_INTERVAL(priv->fps),
					timeline_timeout_func,
					(gpointer)timeline);
    }
}

/**
 * clutter_timeline_new:
 * @nframes: #ClutterTimeline number of frames
 * @fps: #ClutterTimeline  frames per second
 *
 * Create a new  #ClutterTimeline instance.
 *
 * Return Value: a new #ClutterTimeline
 */
ClutterTimeline *
clutter_timeline_new (guint nframes,
		      guint fps)
{
  return g_object_new (CLUTTER_TYPE_TIMELINE, 
		       "fps", fps, 
		       "num-frames", nframes, 
		       NULL);
}
