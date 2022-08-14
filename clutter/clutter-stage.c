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

/**
 * SECTION:clutter-stage
 * @short_description: Top level visual element to which actors are placed.
 *
 * #ClutterStage is a top level 'window' on which child actors are placed
 * and manipulated.
 *
 * Clutter creates a default stage upon initialization, which can be retrieved
 * using clutter_stage_get_default(). Clutter always provides the default
 * stage, unless the backend is unable to create one. The stage returned
 * by clutter_stage_get_default() is guaranteed to always be the same.
 *
 * Backends might provide support for multiple stages. The support for this
 * feature can be checked at run-time using the clutter_feature_available()
 * function and the %CLUTTER_FEATURE_STAGE_MULTIPLE flag. If the backend used
 * supports multiple stages, new #ClutterStage instances can be created
 * using clutter_stage_new(). These stages must be managed by the developer
 * using clutter_actor_destroy(), which will take care of destroying all the
 * actors contained inside them.
 *
 * #ClutterStage is a proxy actor, wrapping the backend-specific
 * implementation of the windowing system. It is possible to subclass
 * #ClutterStage, as long as every overridden virtual function chains up to
 * the parent class corresponding function.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-backend.h"
#include "clutter-stage.h"
#include "clutter-main.h"
#include "clutter-color.h"
#include "clutter-util.h"
#include "clutter-marshal.h"
#include "clutter-enum-types.h"
#include "clutter-private.h"
#include "clutter-debug.h"
#include "clutter-stage-manager.h"
#include "clutter-stage-window.h"
#include "clutter-version.h" 	/* For flavour */
#include "clutter-id-pool.h"

#include "cogl/cogl.h"

/* ----------------------------------------------------------------------*/
/* ----------------------------------------------------------------------*/
/* ----------------------------------------------------------------------*/
/* This is whether we do damage using glViewport or glScissor.
 *
 *   * glScissor clips what we render while updating the whole screen
 *   * glViewport actually modifies the viewport to render to that area.
 *
 * On SGX with decent drivers there shouldn't be any real difference, although
 * it is much easier for floating point inaccuracy to creep in with glViewport
 *  */

/* For some reason glViewport doesn't work in scratchbox. The strange error is
 * that after the first viewport update, clutter never gets called to paint
 * again until a fullscreen update is made. */

/* If we're using double-buffering we want to update the area for this frame
 * AND the area for the last frame. */
#if CLUTTER_COGL_HAS_GLES
#define VIEWPORT_DAMAGE 0
/* We enable double buffering now, contrary to the comment below, since flipping
 * has been implemented and we use it. */
#define DOUBLE_BUFFER 1
/* We *should* be double-buffered, but because we're just blitting in
 * glSwapBuffers rather than flipping, we can do without the extra areas
 * drawn. THIS MUST BE SET TO 1 IF FLIPPING IS EVER IMPLEMENTED
 */
#else
#define VIEWPORT_DAMAGE 0
#define DOUBLE_BUFFER 1
#endif

#define MAX_BUFFER_AGE 2

/* ----------------------------------------------------------------------*/
/* ----------------------------------------------------------------------*/
/* ----------------------------------------------------------------------*/

struct _ClutterStagePrivate
{
  /* the stage implementation */
  ClutterActor       *impl;

  ClutterColor        color;
  ClutterPerspective  perspective;
  ClutterFog          fog;

  gchar              *title;
  ClutterActor       *key_focused_actor;

  guint               update_idle;	       /* repaint idler id */

  /* if this is invalid, everything will be repainted */
  ClutterGeometry     damaged_area;
  /* The damaged areas history */
  GSList              *damage_history;

  int                 shaped_mode;

  guint is_fullscreen     : 1;
  guint is_offscreen      : 1;
  guint is_cursor_visible : 1;
  guint is_user_resizable : 1;
  guint use_fog           : 1;
};

G_DEFINE_TYPE_WITH_CODE (ClutterStage,
                         clutter_stage,
                         CLUTTER_TYPE_GROUP,
                         G_ADD_PRIVATE (ClutterStage));

#define CLUTTER_STAGE_GET_PRIVATE(obj) \
(clutter_stage_get_instance_private (obj))

enum
{
  PROP_0,

  PROP_COLOR,
  PROP_FULLSCREEN,
  PROP_OFFSCREEN,
  PROP_CURSOR_VISIBLE,
  PROP_PERSPECTIVE,
  PROP_TITLE,
  PROP_USER_RESIZE,
  PROP_USE_FOG
};

enum
{
  FULLSCREEN,
  UNFULLSCREEN,
  ACTIVATE,
  DEACTIVATE,
  LAST_SIGNAL
};

static guint stage_signals[LAST_SIGNAL] = { 0, };

static void
clutter_stage_get_preferred_width (ClutterActor *self,
                                   ClutterUnit   for_height,
                                   ClutterUnit  *min_width_p,
                                   ClutterUnit  *natural_width_p)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  g_assert (priv->impl != NULL);

  CLUTTER_ACTOR_GET_CLASS (priv->impl)->get_preferred_width (priv->impl,
                                                             for_height,
                                                             min_width_p,
                                                             natural_width_p);
}

static void
clutter_stage_get_preferred_height (ClutterActor *self,
                                    ClutterUnit   for_width,
                                    ClutterUnit  *min_height_p,
                                    ClutterUnit  *natural_height_p)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  g_assert (priv->impl != NULL);

  CLUTTER_ACTOR_GET_CLASS (priv->impl)->get_preferred_height (priv->impl,
                                                              for_width,
                                                              min_height_p,
                                                              natural_height_p);
}
static void
clutter_stage_allocate (ClutterActor          *self,
                        const ClutterActorBox *box,
                        gboolean               origin_changed)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  g_assert (priv->impl != NULL);

  /* if the stage is fixed size (for instance, it's using a frame-buffer)
   * then we simply ignore any allocation request and override the
   * allocation chain.
   */
  if (G_LIKELY (!clutter_feature_available (CLUTTER_FEATURE_STAGE_STATIC)))
    {
      ClutterActorClass *klass;

      CLUTTER_NOTE (ACTOR, "Following allocation to %dx%d (origin %s)",
                    CLUTTER_UNITS_TO_DEVICE (box->x2 - box->x1),
                    CLUTTER_UNITS_TO_DEVICE (box->y2 - box->y1),
                    origin_changed ? "changed" : "not changed");

      klass = CLUTTER_ACTOR_CLASS (clutter_stage_parent_class);
      klass->allocate (self, box, origin_changed);

      klass = CLUTTER_ACTOR_GET_CLASS (priv->impl);
      klass->allocate (priv->impl, box, origin_changed);
    }
  else
    {
      ClutterActorBox override = { 0, };
      ClutterActorClass *klass;
      ClutterUnit natural_width, natural_height;

      /* propagate the allocation */
      klass = CLUTTER_ACTOR_GET_CLASS (priv->impl);
      klass->allocate (self, box, origin_changed);

      /* get the preferred size from the backend */
      clutter_actor_get_preferred_size (priv->impl,
                                        NULL, NULL,
                                        &natural_width, &natural_height);

      override.x1 = 0;
      override.y1 = 0;
      override.x2 = natural_width;
      override.y2 = natural_height;

      /* and store the overridden allocation */
      klass = CLUTTER_ACTOR_CLASS (clutter_stage_parent_class);
      klass->allocate (self, &override, origin_changed);
    }
}

/* 0 - no shape
 * 1 - shape (fullscreen, i.e. all Clutter drawing is clipped)
 */
void
clutter_stage_set_shaped_mode (ClutterActor *self, int mode)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  priv->shaped_mode = mode;
}

int
_clutter_stage_get_shaped_mode (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  return priv->shaped_mode;
}

static void
clutter_stage_paint (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  ClutterGeometry      *damage;
  gboolean             update_area;
  guint                width, height;

  CLUTTER_SET_PRIVATE_FLAGS (self, CLUTTER_ACTOR_IN_PAINT);

  CLUTTER_NOTE (PAINT, "Initializing stage paint");

  if (clutter_feature_available (CLUTTER_FEATURE_BUFFER_AGE))
    {
      ClutterBackend *backend = clutter_get_default_backend ();
      int age = _clutter_backend_buffer_age (backend, CLUTTER_STAGE (self));

      damage = g_new (ClutterGeometry, 1);
      *damage = priv->damaged_area;

      CLUTTER_NOTE (PAINT, "Back buffer age %d history depth %d", age,
                    g_slist_length (priv->damage_history));

      priv->damage_history = g_slist_prepend (priv->damage_history, damage);

      if (age != 0)
        {
          if (g_slist_length (priv->damage_history) > age)
            {
              GSList *l = priv->damage_history;
              int i = 0;

              for (l = l->next; l; l = l->next)
                {
                  if (i < age)
                    {
                      damage = l->data;
                      clutter_stage_set_damaged_area(self, *damage);
                    }

                  i++;

                  if (i > MAX_BUFFER_AGE)
                    {
                      g_slist_free_full (l->next, g_free);
                      l->next = NULL;
                    }
                }
            }
          else
            {
              CLUTTER_NOTE (PAINT, "Back buffer too old, redraw everything");
              priv->damaged_area.x = 0;
              priv->damaged_area.y = 0;
              priv->damaged_area.width = 0;
              priv->damaged_area.height = 0;
            }

        }
      else
        {
          CLUTTER_NOTE (PAINT, "Invalid back buffer: Resetting damage history");

          g_slist_free_full (priv->damage_history, g_free);
          priv->damage_history = NULL;
        }
    }
  else
    {
#if DOUBLE_BUFFER
      damage = priv->damage_history->data;

      /* Add the damaged area from last frame to this one, as we're
       * double-buffered so will have missed 2 frames worth of changes! */
      clutter_stage_set_damaged_area(self, *damage);
      *damage = priv->damaged_area;

#endif
    }

  /* If we only had a small area, redraw that */
  update_area = priv->damaged_area.width>0 && priv->damaged_area.height>0;
  /* Or if it was the whole screen, just skip the small redraw overhead */
  clutter_actor_get_size(self, &width, &height);
  if (priv->damaged_area.x<=0 && priv->damaged_area.y<=0 &&
      priv->damaged_area.width>=width &&
      priv->damaged_area.height>=height)
    update_area = FALSE;

  if (update_area)
    {
      CLUTTER_NOTE (PAINT,
          "Rendering ONLY the area x: %d, y: %d, width: %d, height: %d",
          priv->damaged_area.x, priv->damaged_area.y,
          priv->damaged_area.width, priv->damaged_area.height );
/*      g_debug("RENDERING ONLY x: %d, y: %d, width: %d, height: %d\n",
                priv->damaged_area.x, priv->damaged_area.y,
                priv->damaged_area.width, priv->damaged_area.height );*/

#if VIEWPORT_DAMAGE
      CLUTTER_SET_PRIVATE_FLAGS (self, CLUTTER_ACTOR_SYNC_MATRICES);
      _clutter_stage_maybe_setup_viewport(CLUTTER_STAGE(self));
      cogl_clip_set(
          CLUTTER_INT_TO_FIXED( priv->damaged_area.x ),
          CLUTTER_INT_TO_FIXED( priv->damaged_area.y ),
          CLUTTER_INT_TO_FIXED( priv->damaged_area.width ),
          CLUTTER_INT_TO_FIXED( priv->damaged_area.height ));
      cogl_modify_clip_viewport(priv->damaged_area.x,
                                height - (priv->damaged_area.y+priv->damaged_area.height),
                                priv->damaged_area.width,
                                priv->damaged_area.height );
#else
      cogl_push_matrix();
      CLUTTER_SET_PRIVATE_FLAGS (self, CLUTTER_ACTOR_SYNC_MATRICES);
      _clutter_stage_maybe_setup_viewport(CLUTTER_STAGE(self));

      cogl_clip_set(
          CLUTTER_INT_TO_FIXED( priv->damaged_area.x ),
          CLUTTER_INT_TO_FIXED( priv->damaged_area.y ),
          CLUTTER_INT_TO_FIXED( priv->damaged_area.width ),
          CLUTTER_INT_TO_FIXED( priv->damaged_area.height ));

      cogl_pop_matrix();
#endif
    }

  /* don't clear the background if just updating the area */
  cogl_paint_init (update_area ? 0 : &priv->color);

  if (priv->use_fog)
    {
      cogl_fog_set (&priv->color,
                    priv->fog.density,
                    priv->fog.z_near,
                    priv->fog.z_far);
    }

  CLUTTER_NOTE (PAINT, "Proxying the paint to the stage implementation");
  clutter_actor_paint (priv->impl);

  CLUTTER_UNSET_PRIVATE_FLAGS (self, CLUTTER_ACTOR_IN_PAINT);

  /* this will take care of painting every child */
  CLUTTER_ACTOR_CLASS (clutter_stage_parent_class)->paint (self);

  if (update_area)
    {
#if VIEWPORT_DAMAGE
      cogl_clip_unset();
      /* return our normal viewport in case it is needed in the future */
      CLUTTER_SET_PRIVATE_FLAGS (self, CLUTTER_ACTOR_SYNC_MATRICES);
      _clutter_stage_maybe_setup_viewport(CLUTTER_STAGE(self));
#else
      cogl_clip_unset();
#endif // VIEWPORT_DAMAGE
    }
  priv->damaged_area.x = 0;
  priv->damaged_area.y = 0;
  priv->damaged_area.width = 0;
  priv->damaged_area.height = 0;
}

static void
clutter_stage_pick (ClutterActor       *self,
		    const ClutterColor *color)
{
  /* Paint nothing, cogl_paint_init() effectively paints the stage
   * silhouette for us - see _clutter_do_pick().
   * Chain up to the groups paint howerer so our children get picked
   * - clutter_group_pick
   */
  CLUTTER_ACTOR_CLASS (clutter_stage_parent_class)->paint (self);
}

static void
clutter_stage_realize (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  CLUTTER_ACTOR_SET_FLAGS (self, CLUTTER_ACTOR_REALIZED);

  g_assert (priv->impl != NULL);
  CLUTTER_ACTOR_GET_CLASS (priv->impl)->realize (priv->impl);

  /* ensure that the stage is using the context if the
   * realization sequence was successful
   */
  if (CLUTTER_ACTOR_IS_REALIZED (priv->impl))
    clutter_stage_ensure_current (CLUTTER_STAGE (self));
  else
    CLUTTER_ACTOR_UNSET_FLAGS (self, CLUTTER_ACTOR_REALIZED);
}

static void
clutter_stage_unrealize (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  /* unset the flag */
  CLUTTER_ACTOR_UNSET_FLAGS (self, CLUTTER_ACTOR_REALIZED);

  /* and then unrealize the implementation */
  g_assert (priv->impl != NULL);
  CLUTTER_ACTOR_GET_CLASS (priv->impl)->unrealize (priv->impl);

  clutter_stage_ensure_current (CLUTTER_STAGE (self));
}

static void
clutter_stage_show (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  g_assert (priv->impl != NULL);

  if (!CLUTTER_ACTOR_IS_REALIZED (priv->impl))
    clutter_actor_realize (priv->impl);

  clutter_actor_show (priv->impl);

  CLUTTER_ACTOR_CLASS (clutter_stage_parent_class)->show (self);
}

static void
clutter_stage_hide (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  g_assert (priv->impl != NULL);
  clutter_actor_hide (priv->impl);

  CLUTTER_ACTOR_CLASS (clutter_stage_parent_class)->hide (self);
}

static void
clutter_stage_real_fullscreen (ClutterStage *stage)
{
  ClutterStagePrivate *priv = stage->priv;
  ClutterUnit natural_width, natural_height;
  ClutterActorBox box;

  /* we need to force an allocation here because the size
   * of the stage might have been changed by the backend
   *
   * this is a really bad solution to the issues caused by
   * the fact that fullscreening the stage on the X11 backends
   * is really an asynchronous operation
   */
  clutter_actor_get_preferred_size (CLUTTER_ACTOR (priv->impl),
                                    NULL, NULL,
                                    &natural_width, &natural_height);

  box.x1 = 0;
  box.y1 = 0;
  box.x2 = natural_width;
  box.y2 = natural_height;

  clutter_actor_allocate (CLUTTER_ACTOR (stage), &box, FALSE);
}

static void
clutter_stage_set_property (GObject      *object,
			    guint         prop_id,
			    const GValue *value,
			    GParamSpec   *pspec)
{
  ClutterStage        *stage;
  ClutterStagePrivate *priv;
  ClutterActor        *actor;

  stage = CLUTTER_STAGE (object);
  actor = CLUTTER_ACTOR (stage);
  priv = stage->priv;

  switch (prop_id)
    {
    case PROP_COLOR:
      clutter_stage_set_color (stage, g_value_get_boxed (value));
      break;
    case PROP_OFFSCREEN:
      if (priv->is_offscreen == g_value_get_boolean (value))
	return;

      if (CLUTTER_ACTOR_IS_REALIZED (actor))
        {
          /* Backend needs to check this prop and handle accordingly
           * in realise.
           * FIXME: More 'obvious' implementation needed?
          */
          clutter_actor_unrealize (actor);
          priv->is_offscreen = g_value_get_boolean (value);
          clutter_actor_realize (actor);

	  if (!CLUTTER_ACTOR_IS_REALIZED (actor))
	    priv->is_offscreen = ~g_value_get_boolean (value);
        }
      else
        priv->is_offscreen = g_value_get_boolean (value);
      break;
    case PROP_FULLSCREEN:
      if (g_value_get_boolean (value))
        clutter_stage_fullscreen (stage);
      else
        clutter_stage_unfullscreen (stage);
      break;
    case PROP_CURSOR_VISIBLE:
      if (g_value_get_boolean (value))
        clutter_stage_show_cursor (stage);
      else
        clutter_stage_hide_cursor (stage);
      break;
    case PROP_PERSPECTIVE:
      clutter_stage_set_perspectivex (stage, g_value_get_boxed (value));
      break;
    case PROP_TITLE:
      clutter_stage_set_title (stage, g_value_get_string (value));
      break;
    case PROP_USER_RESIZE:
      clutter_stage_set_user_resizable (stage, g_value_get_boolean (value));
      break;
    case PROP_USE_FOG:
      clutter_stage_set_use_fog (stage, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clutter_stage_get_property (GObject    *object,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  ClutterStage        *stage;
  ClutterStagePrivate *priv;
  ClutterColor         color;
  ClutterPerspective   perspective;

  stage = CLUTTER_STAGE(object);
  priv = stage->priv;

  switch (prop_id)
    {
    case PROP_COLOR:
      clutter_stage_get_color (stage, &color);
      g_value_set_boxed (value, &color);
      break;
    case PROP_OFFSCREEN:
      g_value_set_boolean (value, priv->is_offscreen);
      break;
    case PROP_FULLSCREEN:
      g_value_set_boolean (value, priv->is_fullscreen);
      break;
    case PROP_CURSOR_VISIBLE:
      g_value_set_boolean (value, priv->is_cursor_visible);
      break;
    case PROP_PERSPECTIVE:
      clutter_stage_get_perspectivex (stage, &perspective);
      g_value_set_boxed (value, &perspective);
      break;
    case PROP_TITLE:
      g_value_set_string (value, priv->title);
      break;
    case PROP_USER_RESIZE:
      g_value_set_boolean (value, priv->is_user_resizable);
      break;
    case PROP_USE_FOG:
      g_value_set_boolean (value, priv->use_fog);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
clutter_stage_dispose (GObject *object)
{
  ClutterStage        *stage = CLUTTER_STAGE (object);
  ClutterStagePrivate *priv = stage->priv;
  ClutterStageManager *stage_manager = clutter_stage_manager_get_default ();

  clutter_actor_unrealize (CLUTTER_ACTOR (object));

  if (priv->update_idle)
    {
      g_source_remove (priv->update_idle);
      priv->update_idle = 0;
    }

  _clutter_stage_manager_remove_stage (stage_manager, stage);

  if (priv->impl)
    {
      CLUTTER_NOTE (MISC, "Disposing of the stage implementation");
      g_object_unref (priv->impl);
      priv->impl = NULL;
    }

  g_slist_free_full (priv->damage_history, g_free);
  priv->damage_history = NULL;

  G_OBJECT_CLASS (clutter_stage_parent_class)->dispose (object);
}

static void
clutter_stage_finalize (GObject *object)
{
  ClutterStage *stage = CLUTTER_STAGE (object);

  g_free (stage->priv->title);

  G_OBJECT_CLASS (clutter_stage_parent_class)->finalize (object);
}


static void
clutter_stage_class_init (ClutterStageClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  gobject_class->set_property = clutter_stage_set_property;
  gobject_class->get_property = clutter_stage_get_property;
  gobject_class->dispose = clutter_stage_dispose;
  gobject_class->finalize = clutter_stage_finalize;

  actor_class->allocate = clutter_stage_allocate;
  actor_class->get_preferred_width = clutter_stage_get_preferred_width;
  actor_class->get_preferred_height = clutter_stage_get_preferred_height;
  actor_class->paint = clutter_stage_paint;
  actor_class->pick = clutter_stage_pick;
  actor_class->realize = clutter_stage_realize;
  actor_class->unrealize = clutter_stage_unrealize;
  actor_class->show = clutter_stage_show;
  actor_class->hide = clutter_stage_hide;

  /**
   * ClutterStage:fullscreen:
   *
   * Whether the stage should be fullscreen or not.
   */
  g_object_class_install_property
    (gobject_class, PROP_FULLSCREEN,
     g_param_spec_boolean ("fullscreen",
			   "Fullscreen",
			   "Whether the main stage is fullscreen",
			   FALSE,
			   G_PARAM_CONSTRUCT | CLUTTER_PARAM_READWRITE));
  /**
   * ClutterStage:offscreen:
   *
   * Whether the stage should be rendered in an offscreen buffer.
   */
  g_object_class_install_property
    (gobject_class, PROP_OFFSCREEN,
     g_param_spec_boolean ("offscreen",
			   "Offscreen",
			   "Whether the main stage is renderer offscreen",
			   FALSE,
			   G_PARAM_CONSTRUCT | CLUTTER_PARAM_READWRITE));
  /**
   * ClutterStage:cursor-visible:
   *
   * Whether the mouse pointer should be visible
   */
  g_object_class_install_property
    (gobject_class, PROP_CURSOR_VISIBLE,
     g_param_spec_boolean ("cursor-visible",
			   "Cursor Visible",
			   "Whether the mouse pointer is visible on the main stage ",
			   TRUE,
			   G_PARAM_CONSTRUCT | CLUTTER_PARAM_READWRITE));
  /**
   * ClutterStage:user-resizable:
   *
   * Whether the stage is resizable via user interaction.
   *
   * Since: 0.4
   */
  g_object_class_install_property
    (gobject_class, PROP_USER_RESIZE,
     g_param_spec_boolean ("user-resizable",
			   "User Resizable",
			   "Whether the stage is able to be resized via "
			   "user interaction",
			   FALSE,
			   G_PARAM_CONSTRUCT | CLUTTER_PARAM_READWRITE));
  /**
   * ClutterStage:color:
   *
   * The color of the main stage.
   */
  g_object_class_install_property
    (gobject_class, PROP_COLOR,
     g_param_spec_boxed ("color",
			 "Color",
			 "The color of the main stage",
			 CLUTTER_TYPE_COLOR,
			 CLUTTER_PARAM_READWRITE));

  /**
   * ClutterStage:perspective:
   *
   * The parameters used for the perspective projection from 3D
   * coordinates to 2D
   *
   * Since: 0.8.2
   */
  g_object_class_install_property
    (gobject_class, PROP_PERSPECTIVE,
     g_param_spec_boxed ("perspective",
			 "Perspective",
			 "Perspective projection parameters",
			 CLUTTER_TYPE_PERSPECTIVE,
			 CLUTTER_PARAM_READWRITE));

  /**
   * ClutterStage:title:
   *
   * The stage's title - usually displayed in stage windows title decorations.
   *
   * Since: 0.4
   */
  g_object_class_install_property
    (gobject_class, PROP_TITLE,
     g_param_spec_string ("title",
			  "Title",
			  "Stage Title",
			  NULL,
			  CLUTTER_PARAM_READWRITE));
  /**
   * ClutterStage:use-fog:
   *
   * Whether the stage should use a linear GL "fog" in creating the
   * depth-cueing effect, to enhance the perception of depth by fading
   * actors farther from the viewpoint.
   *
   * Since: 0.6
   */
  g_object_class_install_property (gobject_class,
                                   PROP_USE_FOG,
                                   g_param_spec_boolean ("use-fog",
                                                         "Use Fog",
                                                         "Whether to enable depth cueing",
                                                         FALSE,
                                                         CLUTTER_PARAM_READWRITE));

  /**
   * ClutterStage::fullscreen
   * @stage: the stage which was fullscreened
   *
   * The ::fullscreen signal is emitted when the stage is made fullscreen.
   *
   * Since: 0.6
   */
  stage_signals[FULLSCREEN] =
    g_signal_new ("fullscreen",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_FIRST,
		  G_STRUCT_OFFSET (ClutterStageClass, fullscreen),
		  NULL, NULL,
		  clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  /**
   * ClutterStage::unfullscreen
   * @stage: the stage which has left a fullscreen state.
   *
   * The ::unfullscreen signal is emitted when the stage leaves a fullscreen
   * state.
   *
   * Since: 0.6
   */
  stage_signals[UNFULLSCREEN] =
    g_signal_new ("unfullscreen",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterStageClass, unfullscreen),
		  NULL, NULL,
		  clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  /**
   * ClutterStage::activate
   * @stage: the stage which was activated
   *
   * The ::activate signal is emitted when the stage receives key focus
   * from the underlying window system.
   *
   * Since: 0.6
   */
  stage_signals[ACTIVATE] =
    g_signal_new ("activate",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterStageClass, activate),
		  NULL, NULL,
		  clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  /**
   * ClutterStage::deactivate
   * @stage: the stage which was deactivated
   *
   * The ::activate signal is emitted when the stage loses key focus
   * from the underlying window system.
   *
   * Since: 0.6
   */
  stage_signals[DEACTIVATE] =
    g_signal_new ("deactivate",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterStageClass, deactivate),
		  NULL, NULL,
		  clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);

  klass->fullscreen = clutter_stage_real_fullscreen;
}

static void
clutter_stage_init (ClutterStage *self)
{
  ClutterStagePrivate *priv;
  ClutterBackend *backend;
#if DOUBLE_BUFFER
  ClutterGeometry *damage;
#endif
  /* a stage is a top-level object */
  CLUTTER_SET_PRIVATE_FLAGS (self, CLUTTER_ACTOR_IS_TOPLEVEL);

  self->priv = priv = CLUTTER_STAGE_GET_PRIVATE (self);

  CLUTTER_NOTE (BACKEND, "Creating stage from the default backend");
  backend = clutter_get_default_backend ();
  priv->impl = _clutter_backend_create_stage (backend, self, NULL);
  if (!priv->impl)
    {
      g_warning ("Unable to create a new stage, falling back to the "
                 "default stage.");
      priv->impl = CLUTTER_ACTOR (_clutter_stage_get_default_window ());

      /* at this point we must have a default stage, or we're screwed */
      g_assert (priv->impl != NULL);
    }
  else
    g_object_ref_sink (priv->impl);

  priv->is_offscreen      = FALSE;
  priv->is_fullscreen     = FALSE;
  priv->is_user_resizable = FALSE;
  priv->is_cursor_visible = TRUE;
  priv->use_fog           = FALSE;

  priv->color.red   = 0x00;
  priv->color.green = 0x00;
  priv->color.blue  = 0x00;
  priv->color.alpha = 0xff;

  priv->perspective.fovy   = CFX_60; /* 60 Degrees */
  priv->perspective.aspect = CFX_ONE;
  priv->perspective.z_near = CLUTTER_FLOAT_TO_FIXED (0.1);
  priv->perspective.z_far  = CLUTTER_FLOAT_TO_FIXED (100.0);

  /* depth cueing */
  priv->fog.density = CLUTTER_FLOAT_TO_FIXED (0.1);
  priv->fog.z_near  = CLUTTER_FLOAT_TO_FIXED (1.0);
  priv->fog.z_far   = CLUTTER_FLOAT_TO_FIXED (2.0);

  priv->damaged_area.x = 0;
  priv->damaged_area.y = 0;
  priv->damaged_area.width = 0;
  priv->damaged_area.height = 0;

#if DOUBLE_BUFFER
  if (!clutter_feature_available (CLUTTER_FEATURE_BUFFER_AGE))
    {
      damage = g_new (ClutterGeometry, 1);
      *damage = priv->damaged_area;
      priv->damage_history = g_slist_prepend (priv->damage_history, damage);
    }
#endif

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);
  clutter_stage_set_key_focus (self, NULL);
}

/**
 * clutter_stage_get_default:
 *
 * Returns the main stage. The default #ClutterStage is a singleton,
 * so the stage will be created the first time this function is
 * called (typically, inside clutter_init()); all the subsequent
 * calls to clutter_stage_get_default() will return the same instance.
 *
 * Clutter guarantess the existence of the default stage.
 *
 * Return value: the main #ClutterStage.  You should never
 *   destroy or unref the returned actor.
 */
ClutterActor *
clutter_stage_get_default (void)
{
  ClutterStageManager *stage_manager = clutter_stage_manager_get_default ();
  ClutterStage *stage;

  stage = clutter_stage_manager_get_default_stage (stage_manager);
  if (G_UNLIKELY (stage == NULL))
    /* This will take care of automatically adding the stage to the
     * stage manager and setting it as the default. Its floating
     * reference will be claimed by the stage manager.
     */
    stage = g_object_new (CLUTTER_TYPE_STAGE, NULL);

  return CLUTTER_ACTOR (stage);
}

/**
 * clutter_stage_set_color
 * @stage: A #ClutterStage
 * @color: A #ClutterColor
 *
 * Set the stage color.
 **/
void
clutter_stage_set_color (ClutterStage       *stage,
			 const ClutterColor *color)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (color != NULL);

  priv = stage->priv;

  priv->color = *color;

  if (CLUTTER_ACTOR_IS_VISIBLE (stage))
    clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  g_object_notify (G_OBJECT (stage), "color");
}

/**
 * clutter_stage_get_color
 * @stage: A #ClutterStage
 * @color: return location for a #ClutterColor
 *
 * Retrieves the stage color.
 */
void
clutter_stage_get_color (ClutterStage *stage,
			 ClutterColor *color)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (color != NULL);

  priv = stage->priv;

  *color = priv->color;
}

/**
 * clutter_stage_set_perspectivex
 * @stage: A #ClutterStage
 * @perspective: A #ClutterPerspective
 *
 * Set the stage perspective. This is the fixed point version of
 * clutter_stage_set_perspective().
 **/
void
clutter_stage_set_perspectivex (ClutterStage       *stage,
				ClutterPerspective *perspective)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (perspective != NULL);

  priv = stage->priv;

  priv->perspective = *perspective;

  /* this will cause the viewport to be reset; see
   * clutter_maybe_setup_viewport() inside clutter-main.c
   */
  CLUTTER_SET_PRIVATE_FLAGS (stage, CLUTTER_ACTOR_SYNC_MATRICES);
}

/**
 * clutter_stage_get_perspectivex
 * @stage: A #ClutterStage
 * @perspective: return location for a #ClutterPerspective
 *
 * Retrieves the stage perspective. This is the fixed point version of
 * clutter_stage_get_perspective().
 */
void
clutter_stage_get_perspectivex (ClutterStage       *stage,
				ClutterPerspective *perspective)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (perspective != NULL);

  *perspective = stage->priv->perspective;
}

/**
 * clutter_stage_set_perspective
 * @stage: A #ClutterStage
 * @fovy: the field of view angle, in degrees, in the y direction
 * @aspect: the aspect ratio that determines the field of view in the x
 *   direction. The aspect ratio is the ratio of x (width) to y (height)
 * @z_near: the distance from the viewer to the near clipping
 *   plane (always positive)
 * @z_far: the  distance from the viewer to the far clipping
 *   plane (always positive)
 *
 * Sets the stage perspective.
 *
 * Since: 0.4
 */
void
clutter_stage_set_perspective (ClutterStage *stage,
                               gfloat        fovy,
                               gfloat        aspect,
                               gfloat        z_near,
                               gfloat        z_far)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  priv->perspective.fovy   = CLUTTER_FLOAT_TO_FIXED (fovy);
  priv->perspective.aspect = CLUTTER_FLOAT_TO_FIXED (aspect);
  priv->perspective.z_near = CLUTTER_FLOAT_TO_FIXED (z_near);
  priv->perspective.z_far  = CLUTTER_FLOAT_TO_FIXED (z_far);

  /* this will cause the viewport to be reset; see
   * clutter_maybe_setup_viewport() inside clutter-main.c
   */
  CLUTTER_SET_PRIVATE_FLAGS (stage, CLUTTER_ACTOR_SYNC_MATRICES);
}

/**
 * clutter_stage_get_perspective
 * @stage: A #ClutterStage
 * @fovy: return location for the field of view, in degrees, or %NULL
 * @aspect: return location for the aspect ratio, or %NULL
 * @z_near: return location for the distance of the viewer from the
 *   near clipping plane, or %NULL
 * @z_far: return location for the distance of the viewer from the
 *   far clipping plane, or %NULL
 *
 * Retrieves the stage perspective.
 *
 * Since: 0.4
 */
void
clutter_stage_get_perspective (ClutterStage       *stage,
			       gfloat             *fovy,
			       gfloat             *aspect,
			       gfloat             *z_near,
			       gfloat             *z_far)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (fovy)
    *fovy   = CLUTTER_FIXED_TO_FLOAT (priv->perspective.fovy);

  if (aspect)
    *aspect = CLUTTER_FIXED_TO_FLOAT (priv->perspective.aspect);

  if (z_near)
    *z_near = CLUTTER_FIXED_TO_FLOAT (priv->perspective.z_near);

  if (z_far)
    *z_far  = CLUTTER_FIXED_TO_FLOAT (priv->perspective.z_far);
}

/**
 * clutter_stage_fullscreen:
 * @stage: a #ClutterStage
 *
 * Asks to place the stage window in the fullscreen state. Note that you
 * shouldn't assume the window is definitely full screen afterward, because
 * other entities (e.g. the user or window manager) could unfullscreen it
 * again, and not all window managers honor requests to fullscreen windows.
 */
void
clutter_stage_fullscreen (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  if (!priv->is_fullscreen)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);

      /* Only set if backend implements.
       * Also see clutter_stage_event() for setting priv->is_fullscreen
       * on state change event.
      */
      if (iface->set_fullscreen)
	iface->set_fullscreen (impl, TRUE);
    }
}

/**
 * clutter_stage_unfullscreen:
 * @stage: a #ClutterStage
 *
 * Asks to toggle off the fullscreen state for the stage window. Note that
 * you shouldn't assume the window is definitely not full screen afterward,
 * because other entities (e.g. the user or window manager) could fullscreen
 * it again, and not all window managers honor requests to unfullscreen
 * windows.
 */
void
clutter_stage_unfullscreen (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  if (priv->is_fullscreen)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);

      /* Only set if backend implements.
       * Also see clutter_stage_event() for setting priv->is_fullscreen
       * on state change event.
      */
      if (iface->set_fullscreen)
	iface->set_fullscreen (impl, FALSE);
    }
}

/**
 * clutter_stage_set_user_resizable:
 * @stage: a #ClutterStage
 * @resizable: whether the stage should be user resizable.
 *
 * Sets if the stage is resizable by user interaction (e.g. via
 * window manager controls)
 *
 * Since: 0.4
 */
void
clutter_stage_set_user_resizable (ClutterStage *stage,
                                  gboolean      resizable)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (clutter_feature_available (CLUTTER_FEATURE_STAGE_USER_RESIZE)
      && priv->is_user_resizable != resizable)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);
      if (iface->set_user_resizable)
        {
          priv->is_user_resizable = resizable;

          iface->set_user_resizable (impl, resizable);

          g_object_notify (G_OBJECT (stage), "user-resizable");
        }
    }
}

/**
 * clutter_stage_get_user_resizable:
 * @stage: a #ClutterStage
 *
 * Retrieves the value set with clutter_stage_set_user_resizable().
 *
 * Return value: %TRUE if the stage is resizable by the user.
 *
 * Since: 0.4
 */
gboolean
clutter_stage_get_user_resizable (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return stage->priv->is_user_resizable;
}

/**
 * clutter_stage_show_cursor:
 * @stage: a #ClutterStage
 *
 * Shows the cursor on the stage window
 */
void
clutter_stage_show_cursor (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  if (!priv->is_cursor_visible)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);
      if (iface->set_cursor_visible)
        {
          priv->is_cursor_visible = TRUE;

          iface->set_cursor_visible (impl, TRUE);

          g_object_notify (G_OBJECT (stage), "cursor-visible");
        }
    }
}

/**
 * clutter_stage_hide_cursor:
 * @stage: a #ClutterStage
 *
 * Makes the cursor invisible on the stage window
 *
 * Since: 0.4
 */
void
clutter_stage_hide_cursor (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  if (priv->is_cursor_visible)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);
      if (iface->set_cursor_visible)
        {
          priv->is_cursor_visible = FALSE;

          iface->set_cursor_visible (impl, FALSE);

          g_object_notify (G_OBJECT (stage), "cursor-visible");
        }
    }
}

/**
 * clutter_stage_read_pixels:
 * @stage: A #ClutterStage
 * @x: x coordinate of the first pixel that is read from stage
 * @y: y coordinate of the first pixel that is read from stage
 * @width: Width dimention of pixels to be read, or -1 for the
 *   entire stage width
 * @height: Height dimention of pixels to be read, or -1 for the
 *   entire stage height
 *
 * Makes a screenshot of the stage in RGBA 8bit data, returns a
 * linear buffer with @width * 4 as rowstride.
 *
 * The alpha data contained in the returned buffer is driver-dependent,
 * and not guaranteed to hold any sensible value.
 *
 * Return value: a pointer to newly allocated memory with the buffer
 *   or %NULL if the read failed. Use g_free() on the returned data
 *   to release the resources it has allocated.
 */
guchar *
clutter_stage_read_pixels (ClutterStage *stage,
                           gint          x,
                           gint          y,
                           gint          width,
                           gint          height)
{
  guchar *pixels;
  guchar *temprow;
  GLint   viewport[4];
  gint    rowstride;
  gint    stage_width, stage_height;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  /* according to glReadPixels documentation pixels outside the viewport are
   * undefined, but no error should be provoked, thus this is probably unnneed.
   */
  g_return_val_if_fail (x >= 0 && y >= 0, NULL);

  /* Force a redraw of the stage before reading back pixels */
  clutter_redraw (stage);
  clutter_stage_ensure_current (stage);

  glGetIntegerv (GL_VIEWPORT, viewport);
  stage_width  = viewport[2];
  stage_height = viewport[3];

  if (width < 0 || width > stage_width)
    width = stage_width;

  if (height < 0 || height > stage_height)
    height = stage_height;

  rowstride = width * 4;

  pixels  = g_malloc (height * rowstride);
  temprow = g_malloc (rowstride);

  /* Setup the pixel store parameters that may have been changed by
     Cogl */
  glPixelStorei (GL_PACK_ALIGNMENT, 4);
#ifdef HAVE_COGL_GL
  glPixelStorei (GL_PACK_ROW_LENGTH, 0);
  glPixelStorei (GL_PACK_SKIP_PIXELS, 0);
  glPixelStorei (GL_PACK_SKIP_ROWS, 0);
#endif /* HAVE_COGL_GL */

  /* The y co-ordinate should be given in OpenGL's coordinate system
     so 0 is the bottom row */
  y = stage_height - 1 - y - height;

  glFinish ();

  /* check whether we need to read into a smaller temporary buffer */
  glReadPixels (x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

  /* vertically flip the buffer in-place */
  for (y = 0; y < height / 2; y++)
    {
      if (y != height - y - 1) /* skip center row */
        {
          memcpy (temprow,
                  pixels + y * rowstride, rowstride);
          memcpy (pixels + y * rowstride,
                  pixels + (height - y - 1) * rowstride, rowstride);
          memcpy (pixels + (height - y - 1) * rowstride,
                  temprow,
                  rowstride);
        }
    }

  g_free (temprow);

  return pixels;
}

/**
 * clutter_stage_get_actor_at_pos:
 * @stage: a #ClutterStage
 * @x: X coordinate to check
 * @y: Y coordinate to check
 *
 * Checks the scene at the coordinates @x and @y and returns a pointer
 * to the #ClutterActor at those coordinates.
 *
 * Return value: the actor at the specified coordinates, if any
 */
ClutterActor *
clutter_stage_get_actor_at_pos (ClutterStage *stage,
                                gint          x,
                                gint          y)
{
  return _clutter_do_pick (stage, x, y, CLUTTER_PICK_ALL);
}

/**
 * clutter_stage_event:
 * @stage: a #ClutterStage
 * @event: a #ClutterEvent
 *
 * This function is used to emit an event on the main stage.
 *
 * You should rarely need to use this function, except for
 * synthetised events.
 *
 * Return value: the return value from the signal emission
 *
 * Since: 0.4
 */
gboolean
clutter_stage_event (ClutterStage *stage,
                     ClutterEvent *event)
{
  ClutterStagePrivate *priv;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);
  g_return_val_if_fail (event != NULL, FALSE);

  priv = stage->priv;

  if (event->type == CLUTTER_DELETE)
    {
      gboolean retval = FALSE;

      g_signal_emit_by_name (stage, "event", event, &retval);

      return retval;
    }

  if (event->type != CLUTTER_STAGE_STATE)
    return FALSE;

  /* emit raw event */
  if (clutter_actor_event (CLUTTER_ACTOR (stage), event, FALSE))
    return TRUE;

  if (event->stage_state.changed_mask & CLUTTER_STAGE_STATE_FULLSCREEN)
    {
      if (event->stage_state.new_state & CLUTTER_STAGE_STATE_FULLSCREEN)
	{
	  priv->is_fullscreen = TRUE;
	  g_signal_emit (stage, stage_signals[FULLSCREEN], 0);

          g_object_notify (G_OBJECT (stage), "fullscreen");
	}
      else
	{
	  priv->is_fullscreen = FALSE;
	  g_signal_emit (stage, stage_signals[UNFULLSCREEN], 0);

          g_object_notify (G_OBJECT (stage), "fullscreen");
	}
    }

  if (event->stage_state.changed_mask & CLUTTER_STAGE_STATE_ACTIVATED)
    {
      if (event->stage_state.new_state & CLUTTER_STAGE_STATE_ACTIVATED)
	g_signal_emit (stage, stage_signals[ACTIVATE], 0);
      else
	g_signal_emit (stage, stage_signals[DEACTIVATE], 0);
    }

  return TRUE;
}

/**
 * clutter_stage_set_title
 * @stage: A #ClutterStage
 * @title: A utf8 string for the stage windows title.
 *
 * Sets the stage title.
 *
 * Since 0.4
 **/
void
clutter_stage_set_title (ClutterStage       *stage,
			 const gchar        *title)
{
  ClutterStagePrivate *priv;
  ClutterStageWindow *impl;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  g_free (priv->title);
  priv->title = g_strdup (title);

  impl = CLUTTER_STAGE_WINDOW (priv->impl);
  if (CLUTTER_STAGE_WINDOW_GET_IFACE(impl)->set_title != NULL)
    CLUTTER_STAGE_WINDOW_GET_IFACE (impl)->set_title (impl, priv->title);

  g_object_notify (G_OBJECT (stage), "title");
}

/**
 * clutter_stage_get_title
 * @stage: A #ClutterStage
 *
 * Gets the stage title.
 *
 * Return value: pointer to the title string for the stage. The
 * returned string is owned by the actor and should not
 * be modified or freed.
 *
 * Since: 0.4
 **/
const gchar *
clutter_stage_get_title (ClutterStage       *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  return stage->priv->title;
}

static void
on_key_focused_weak_notify (gpointer data,
			    GObject *where_the_object_was)
{
  ClutterStagePrivate *priv;
  ClutterStage        *stage = CLUTTER_STAGE (data);

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  priv->key_focused_actor = NULL;

  /* focused actor has dissapeared - fall back to stage
   * FIXME: need some kind of signal dance/block here.
  */
  clutter_stage_set_key_focus (stage, NULL);
}

/**
 * clutter_stage_set_key_focus:
 * @stage: the #ClutterStage
 * @actor: the actor to set key focus to, or %NULL
 *
 * Sets the key focus on @actor. An actor with key focus will receive
 * all the key events. If @actor is %NULL, the stage will receive
 * focus.
 *
 * Since: 0.6
 */
void
clutter_stage_set_key_focus (ClutterStage *stage,
			     ClutterActor *actor)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  priv = stage->priv;

  if (priv->key_focused_actor == actor)
    return;

  if (priv->key_focused_actor)
    {
      g_object_weak_unref (G_OBJECT (priv->key_focused_actor),
			   on_key_focused_weak_notify,
			   stage);
      g_signal_emit_by_name (priv->key_focused_actor, "focus-out");

      priv->key_focused_actor = NULL;
    }
  else
    g_signal_emit_by_name (stage, "focus-out");

  if (actor)
    {
      priv->key_focused_actor = actor;

      g_object_weak_ref (G_OBJECT (actor),
			 on_key_focused_weak_notify,
			 stage);
      g_signal_emit_by_name (priv->key_focused_actor, "focus-in");
    }
  else
    g_signal_emit_by_name (stage, "focus-in");
}

/**
 * clutter_stage_get_key_focus:
 * @stage: the #ClutterStage
 *
 * Retrieves the actor that is currently under key focus.
 *
 * Return value: the actor with key focus, or the stage
 *
 * Since: 0.6
 */
ClutterActor *
clutter_stage_get_key_focus (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  if (stage->priv->key_focused_actor)
    return stage->priv->key_focused_actor;

  return CLUTTER_ACTOR (stage);
}

/**
 * clutter_stage_get_use_fog:
 * @stage: the #ClutterStage
 *
 * Gets whether the depth cueing effect is enabled on @stage.
 *
 * Return value: %TRUE if the the depth cueing effect is enabled
 *
 * Since: 0.6
 */
gboolean
clutter_stage_get_use_fog (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return stage->priv->use_fog;
}

/**
 * clutter_stage_set_use_fog:
 * @stage: the #ClutterStage
 * @fog: %TRUE for enabling the depth cueing effect
 *
 * Sets whether the depth cueing effect on the stage should be enabled
 * or not.
 *
 * Depth cueing is a 3D effect that makes actors farther away from the
 * viewing point less opaque, by fading them with the stage color.

 * The parameters of the GL fog used can be changed using the
 * clutter_stage_set_fog() function.
 *
 * Since: 0.6
 */
void
clutter_stage_set_use_fog (ClutterStage *stage,
                           gboolean      fog)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (priv->use_fog != fog)
    {
      priv->use_fog = fog;

      CLUTTER_NOTE (MISC, "%s depth-cueing inside stage",
                    priv->use_fog ? "enabling" : "disabling");

      if (CLUTTER_ACTOR_IS_VISIBLE (stage))
        clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

      g_object_notify (G_OBJECT (stage), "use-fog");
    }
}

/**
 * clutter_stage_get_fog:
 * @stage: a #ClutterStage
 * @density: return location for the intensity dampening
 * @z_near: return location for the starting point of the depth cueing
 * @z_far: return location for the ending point of the depth cueing
 *
 * Retrieves the settings used by the GL fog to create the
 * depth cueing effect on the @stage.
 *
 * Since: 0.6
 */
void
clutter_stage_get_fog (ClutterStage *stage,
                       gdouble      *density,
                       gdouble      *z_near,
                       gdouble      *z_far)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (density)
    *density = CLUTTER_FIXED_TO_FLOAT (priv->fog.density);
  if (z_near)
    *z_near = CLUTTER_FIXED_TO_FLOAT (priv->fog.z_near);
  if (z_far)
    *z_far = CLUTTER_FIXED_TO_FLOAT (priv->fog.z_far);
}

/**
 * clutter_stage_set_fog:
 * @stage: the #ClutterStage
 * @density: density of the intensity dampening
 * @z_near: starting point of the depth cueing
 * @z_far: ending point of the depth cueing
 *
 * Sets the GL fog settings used to create the depth cueing effect
 * on the @stage.
 *
 * If the actors are all near the view point you will need a higher @density
 * and a smaller interval between @z_near and @z_far. On the other hand, if
 * actors are placed far away from the view point you will need a lower
 * @density but a bigger interval between @z_near and @z_far.
 *
 * Since: 0.6
 */
void
clutter_stage_set_fog (ClutterStage *stage,
                       gdouble       density,
                       gdouble       z_near,
                       gdouble       z_far)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  priv->fog.density = CLUTTER_FLOAT_TO_FIXED (density);
  priv->fog.z_near  = CLUTTER_FLOAT_TO_FIXED (z_near);
  priv->fog.z_far   = CLUTTER_FLOAT_TO_FIXED (z_far);

  if (priv->use_fog && CLUTTER_ACTOR_IS_VISIBLE (stage))
    clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

/**
 * clutter_stage_set_fogx:
 * @stage: the #ClutterStage
 * @fog: a #ClutterFog structure
 *
 * Sets the depth cueing settings for the @stage. This is the fixed point
 * version of clutter_stage_set_fog().
 *
 * Since: 0.6
 */
void
clutter_stage_set_fogx (ClutterStage *stage,
                        ClutterFog   *fog)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (fog != NULL);

  priv = stage->priv;

  priv->fog = *fog;

  if (priv->use_fog && CLUTTER_ACTOR_IS_VISIBLE (stage))
    clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

/**
 * clutter_stage_get_fogx:
 * @stage: the #ClutterStage
 * @fog: return location for a #ClutterFog structure
 *
 * Retrieves the current depth cueing settings from the stage. This is the
 * fixed point version of clutter_stage_get_fog().
 *
 * Since: 0.6
 */
void
clutter_stage_get_fogx (ClutterStage *stage,
                        ClutterFog   *fog)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (fog != NULL);

  *fog = stage->priv->fog;
}

/**
 * clutter_stage_get_resolution:
 * @stage: the #ClutterStage
 *
 * Retrieves the resolution (in DPI) of the stage from the default
 * backend.
 *
 * Return value: the resolution of the stage
 *
 * Since: 0.6
 */
gdouble
clutter_stage_get_resolution (ClutterStage *stage)
{
  ClutterMainContext *context;

  context = clutter_context_get_default ();
  g_assert (context != NULL);

  return clutter_backend_get_resolution (context->backend);
}

/**
 * clutter_stage_get_resolutionx:
 * @stage: the #ClutterStage
 *
 * Fixed point version of clutter_stage_get_resolution().
 *
 * Return value: the resolution of the stage
 *
 * Since: 0.6
 */
ClutterFixed
clutter_stage_get_resolutionx (ClutterStage *stage)
{
  ClutterFixed res;
  ClutterMainContext *context;

  context = clutter_context_get_default ();
  g_assert (context != NULL);

  res = clutter_backend_get_resolution (context->backend);

  return CLUTTER_FLOAT_TO_FIXED (res);
}

/*** Perspective boxed type ******/

static ClutterPerspective *
clutter_perspective_copy (const ClutterPerspective *perspective)
{
  ClutterPerspective *result;

  g_return_val_if_fail (perspective != NULL, NULL);

  result = g_slice_new (ClutterPerspective);
  *result = *perspective;

  return result;
}

static void
clutter_perspective_free (ClutterPerspective *perspective)
{
  if (G_LIKELY (perspective))
    g_slice_free (ClutterPerspective, perspective);
}

GType
clutter_perspective_get_type (void)
{
  static GType our_type = 0;

  if (!our_type)
    our_type =
      g_boxed_type_register_static (I_("ClutterPerspective"),
                                    (GBoxedCopyFunc) clutter_perspective_copy,
                                    (GBoxedFreeFunc) clutter_perspective_free);
  return our_type;
}

static ClutterFog *
clutter_fog_copy (const ClutterFog *fog)
{
  ClutterFog *copy;

  g_return_val_if_fail (fog != NULL, NULL);

  copy = g_slice_new0 (ClutterFog);
  *copy = *fog;

  return copy;
}

static void
clutter_fog_free (ClutterFog *fog)
{
  if (G_LIKELY (fog))
    g_slice_free (ClutterFog, fog);
}

GType
clutter_fog_get_type (void)
{
  static GType our_type = 0;

  if (G_UNLIKELY (our_type == 0))
    our_type =
      g_boxed_type_register_static (I_("ClutterFog"),
                                    (GBoxedCopyFunc) clutter_fog_copy,
                                    (GBoxedFreeFunc) clutter_fog_free);

  return our_type;
}

/**
 * clutter_stage_new:
 *
 * Creates a new, non-default stage. A non-default stage is a new
 * top-level actor which can be used as another container. It works
 * exactly like the default stage, but while clutter_stage_get_default()
 * will always return the same instance, you will have to keep a pointer
 * to any #ClutterStage returned by clutter_stage_create().
 *
 * The ability to support multiple stages depends on the current
 * backend. Use clutter_feature_available() and
 * %CLUTTER_FEATURE_STAGE_MULTIPLE to check at runtime whether a
 * backend supports multiple stages.
 *
 * Return value: a new stage, or %NULL if the default backend does
 *   not support multiple stages. Use clutter_actor_destroy() to
 *   programmatically close the returned stage.
 *
 * Since: 0.8
 */
ClutterActor *
clutter_stage_new (void)
{
  if (!clutter_feature_available (CLUTTER_FEATURE_STAGE_MULTIPLE))
    {
      g_warning ("Unable to create a new stage: the %s backend does not "
                 "support multiple stages.",
                 CLUTTER_FLAVOUR);
      return NULL;
    }

  /* The stage manager will grab the floating reference when the stage
     is added to it in the constructor */
  return g_object_new (CLUTTER_TYPE_STAGE, NULL);
}

/**
 * clutter_stage_ensure_current:
 * @stage: the #ClutterStage
 *
 * This function essentially makes sure the right GL context is
 * current for the passed stage. It is not intended to
 * be used by applications.
 *
 * Since: 0.8
 */
void
clutter_stage_ensure_current (ClutterStage *stage)
{
  ClutterMainContext *ctx = clutter_context_get_default ();

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  _clutter_backend_ensure_context (ctx->backend, stage);
}

static gboolean
redraw_update_idle (gpointer user_data)
{
  ClutterStage *stage = user_data;
  ClutterStagePrivate *priv = stage->priv;

  if (priv->update_idle)
    {
      g_source_remove (priv->update_idle);
      priv->update_idle = 0;
    }

  if (priv->shaped_mode)
    /* Clutter drawing should not be done in 'shaped mode' */
    return FALSE;

  CLUTTER_NOTE (MULTISTAGE, "redrawing via idle for stage:%p", stage);
  clutter_redraw (stage);

  return FALSE;
}

/**
 * clutter_stage_queue_redraw:
 * @stage: the #ClutterStage
 *
 * Queues a redraw for the passed stage.
 *
 * <note>Applications should call clutter_actor_queue_redraw() and not
 * this function.</note>
 *
 * Since: 0.8
 */
void
clutter_stage_queue_redraw (ClutterStage *stage)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  /* If we're being destroyed, don't queue a redraw */
  if (CLUTTER_PRIVATE_FLAGS (stage) & CLUTTER_ACTOR_IN_DESTRUCTION)
    return;

  /* Set our damaged area to cover everything */
  clutter_actor_get_geometry(CLUTTER_ACTOR(stage),
      &stage->priv->damaged_area);

  if (!stage->priv->update_idle)
    {
      CLUTTER_TIMESTAMP (SCHEDULER, "Adding idle source for stage: %p", stage);

      /* FIXME: weak_ref self in case we disappear before paint? */
      stage->priv->update_idle =
        clutter_threads_add_idle_full (CLUTTER_PRIORITY_REDRAW,
                                       redraw_update_idle,
                                       stage,
                                       NULL);
    }
}

/**
 * clutter_stage_queue_redraw_damage:
 * @stage: the #ClutterStage
 *
 * Queues a redraw for the passed stage.
 *
 * <note>Applications should call clutter_actor_queue_redraw_damage() and not
 * this function.</note>
 *
 * This differs from clutter_stage_queue_redraw in that if a damage area
 * has been set for the stage, it does not wipe it and instead try and
 * redraw the entire stage.
 *
 * Since: 0.8.2-maemo
 */
void
clutter_stage_queue_redraw_damage (ClutterStage *stage)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  if (!stage->priv->update_idle)
    {
      CLUTTER_TIMESTAMP (SCHEDULER, "Adding idle source for stage: %p", stage);

      /* FIXME: weak_ref self in case we disappear before paint? */
      if (CLUTTER_REDRAW_DAMAGE_INTERVAL > 0)
        {
          stage->priv->update_idle =
            clutter_threads_add_timeout_full ( CLUTTER_PRIORITY_REDRAW,
                                               CLUTTER_REDRAW_DAMAGE_INTERVAL,
                                               redraw_update_idle,
                                               stage,
                                               NULL);
          /* we're rendering with a timeout here so we can stop
           * double-redraws we're doing when we get multiple XDamage events
           * for what should be a single frame. */
        }
      else
        {
          stage->priv->update_idle =
                  clutter_threads_add_idle_full (CLUTTER_PRIORITY_REDRAW,
                                                 redraw_update_idle,
                                                 stage,
                                                 NULL);
        }
    }
}

/**
 * clutter_stage_is_default:
 * @stage: a #ClutterStage
 *
 * Checks if @stage is the default stage, or an instance created using
 * clutter_stage_new() but internally using the same implementation.
 *
 * Return value: %TRUE if the passed stage is the default one
 *
 * Since: 0.8
 */
gboolean
clutter_stage_is_default (ClutterStage *stage)
{
  ClutterStageWindow *impl;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  if (CLUTTER_ACTOR (stage) == clutter_stage_get_default ())
    return TRUE;

  impl = _clutter_stage_get_window (stage);
  if (impl == _clutter_stage_get_default_window ())
    return TRUE;

  return FALSE;
}

void
_clutter_stage_set_window (ClutterStage       *stage,
                           ClutterStageWindow *stage_window)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (CLUTTER_IS_STAGE_WINDOW (stage_window));

  if (stage->priv->impl)
    g_object_unref (stage->priv->impl);

  stage->priv->impl = CLUTTER_ACTOR (stage_window);
}

ClutterStageWindow *
_clutter_stage_get_window (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  return CLUTTER_STAGE_WINDOW (stage->priv->impl);
}

ClutterStageWindow *
_clutter_stage_get_default_window (void)
{
  ClutterActor *stage = clutter_stage_get_default ();

  return _clutter_stage_get_window (CLUTTER_STAGE (stage));
}

/**
 * clutter_stage_set_damaged_area:
 * @self: a #ClutterStage
 *
 * Sets the area of the stage that will be redrawn on the next redraw.
 * If this is invalid then the whole stage will be redrawn.
 *
 * Since: 0.8.2-maemo
 */
void
clutter_stage_set_damaged_area (ClutterActor *self, ClutterGeometry area)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  gint width, height;
  clutter_actor_get_size(self, (guint*)&width, (guint*)&height);

  /* check if this is invalid, invalidate the stage too */
  if (area.width <=0 || area.height <= 0)
    {
      priv->damaged_area.x = 0;
      priv->damaged_area.y = 0;
      priv->damaged_area.width = 0;
      priv->damaged_area.height = 0;
      return;
    }

  /* clip to the screen */
  if (area.x < 0)
    {
      area.width += area.x;
      area.x = 0;
    }
  if (area.y < 0)
    {
      area.height += area.y;
      area.y = 0;
    }
  if (area.x > width || area.y > height)
    { // offscreen
      area.x = 0;
      area.y = 0;
      area.width = 0;
      area.height = 0;
    }
  if (area.x + area.width > width)
    area.width = width - area.x;
  if (area.y + area.height > height)
    area.height = height - area.y;

  /* if this is invalid *now* (but not earlier),
   * it was offscreen - so ignore it */
  if (area.width <=0 || area.height <= 0)
    return;

  /* check if the stage is invalid */
  if (priv->damaged_area.width <=0 || priv->damaged_area.height <= 0)
    {
      /* If it wasn't defined, replace it */
      priv->damaged_area = area;
    }
  else
    {
      /* else expand it */
      if (area.x < priv->damaged_area.x)
        {
          priv->damaged_area.width += priv->damaged_area.x - area.x;
          priv->damaged_area.x = area.x;
        }
      if (area.y < priv->damaged_area.y)
        {
          priv->damaged_area.height += priv->damaged_area.y - area.y;
          priv->damaged_area.y = area.y;
        }
      if (area.x + area.width >
          priv->damaged_area.x + priv->damaged_area.width)
        {
          priv->damaged_area.width = (area.x + area.width) -
                                     priv->damaged_area.x;
        }
      if (area.y + area.height >
          priv->damaged_area.y + priv->damaged_area.height)
        {
          priv->damaged_area.height = (area.y + area.height) -
                                      priv->damaged_area.y;
        }
    }
}
