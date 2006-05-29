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

#include "clutter-stage.h"
#include "clutter-main.h"
#include "clutter-color.h"
#include "clutter-private.h" 	/* for DBG */

#include <GL/glx.h>
#include <GL/gl.h>

G_DEFINE_TYPE (ClutterStage, clutter_stage, CLUTTER_TYPE_GROUP);

static ClutterElementClass *parent_class;

struct ClutterStagePrivate
{
  Window          xwin;  
  gint            xwin_width, xwin_height;
  gboolean        want_fullscreen;
  gboolean        hide_cursor;
  ClutterColor    color;
};

enum
{
  PROP_0,
  PROP_FULLSCREEN,
  PROP_HIDE_CURSOR
};

enum
{
  SIGNAL_INPUT_EVENT,
  LAST_SIGNAL
};

static int stage_signals[LAST_SIGNAL] = { 0 };

static void
sync_fullscreen (ClutterStage *stage)
{
  Atom                 atom_WINDOW_STATE, atom_WINDOW_STATE_FULLSCREEN;

  atom_WINDOW_STATE 
    = XInternAtom(clutter_xdisplay(), "_NET_WM_STATE", False);
  atom_WINDOW_STATE_FULLSCREEN 
    = XInternAtom(clutter_xdisplay(), "_NET_WM_STATE_FULLSCREEN",False);

  if (stage->priv->want_fullscreen)
    {
      clutter_element_set_size (CLUTTER_ELEMENT(stage),
				DisplayWidth(clutter_xdisplay(), 
					     clutter_xscreen()),
				DisplayHeight(clutter_xdisplay(), 
					      clutter_xscreen()));

      if (stage->priv->xwin != None)
	XChangeProperty(clutter_xdisplay(), stage->priv->xwin,
			atom_WINDOW_STATE, XA_ATOM, 32,
			PropModeReplace,
			(unsigned char *)&atom_WINDOW_STATE_FULLSCREEN, 1);
    }
  else
    {
      /* FIXME */
      if (stage->priv->xwin != None)
	XDeleteProperty(clutter_xdisplay(), 
			stage->priv->xwin, atom_WINDOW_STATE);
    }
}

static void
sync_cursor_visible (ClutterStage *stage)
{
  if (stage->priv->xwin == None)
    return;
  
  if (stage->priv->hide_cursor)
    {
      XColor col;
      Pixmap pix;
      Cursor curs;

      pix = XCreatePixmap (clutter_xdisplay(), 
			   stage->priv->xwin, 1, 1, 1);
      memset (&col, 0, sizeof (col));
      curs = XCreatePixmapCursor (clutter_xdisplay(), 
				  pix, pix, &col, &col, 1, 1);
      XFreePixmap (clutter_xdisplay(), pix);
      XDefineCursor(clutter_xdisplay(), stage->priv->xwin, curs);
    }
  else
    {
      XUndefineCursor(clutter_xdisplay(), stage->priv->xwin);
    }
}

static void
frustum (GLfloat left,
	 GLfloat right,
	 GLfloat bottom,
	 GLfloat top,
	 GLfloat nearval,
	 GLfloat farval)
{
  GLfloat x, y, a, b, c, d;
  GLfloat m[16];

  x = (2.0 * nearval) / (right - left);
  y = (2.0 * nearval) / (top - bottom);
  a = (right + left) / (right - left);
  b = (top + bottom) / (top - bottom);
  c = -(farval + nearval) / ( farval - nearval);
  d = -(2.0 * farval * nearval) / (farval - nearval);

#define M(row,col)  m[col*4+row]
  M(0,0) = x;     M(0,1) = 0.0F;  M(0,2) = a;      M(0,3) = 0.0F;
  M(1,0) = 0.0F;  M(1,1) = y;     M(1,2) = b;      M(1,3) = 0.0F;
  M(2,0) = 0.0F;  M(2,1) = 0.0F;  M(2,2) = c;      M(2,3) = d;
  M(3,0) = 0.0F;  M(3,1) = 0.0F;  M(3,2) = -1.0F;  M(3,3) = 0.0F;
#undef M

  glMultMatrixf (m);
}

static void
perspective (GLfloat fovy,
	     GLfloat aspect,
	     GLfloat zNear,
	     GLfloat zFar)
{
  GLfloat xmin, xmax, ymin, ymax;

  ymax = zNear * tan (fovy * M_PI / 360.0);
  ymin = -ymax;
  xmin = ymin * aspect;
  xmax = ymax * aspect;

  frustum (xmin, xmax, ymin, ymax, zNear, zFar);
}

static void
sync_gl_viewport (ClutterStage *stage)
{
  /* Set For 2D */
#if 0
  glViewport (0, 0, stage->priv->xwin_width, stage->priv->xwin_height);
  glMatrixMode (GL_PROJECTION);
  glLoadIdentity ();
  glOrtho (0, stage->priv->xwin_width, stage->priv->xwin_height, 0, -1, 1);
  glMatrixMode (GL_MODELVIEW);
  glLoadIdentity ();

#endif
  /* For 3D */

  glViewport (0, 0, stage->priv->xwin_width, stage->priv->xwin_height);
  glMatrixMode (GL_PROJECTION);
  glLoadIdentity ();
  perspective (60.0f, 1.0f, 0.1f, 100.0f);
  glMatrixMode (GL_MODELVIEW);
  glLoadIdentity ();

  /* Then for 2D like transform */

  /* camera distance from screen, 0.5 * tan (FOV) */
#define DEFAULT_Z_CAMERA 0.866025404f

  glTranslatef (-0.5f, -0.5f, -DEFAULT_Z_CAMERA);
  glScalef (1.0f / stage->priv->xwin_width, 
	    -1.0f / stage->priv->xwin_height, 1.0f / stage->priv->xwin_width);
  glTranslatef (0.0f, -stage->priv->xwin_height, 0.0f);
}

static void
clutter_stage_show (ClutterElement *self)
{
  XMapWindow (clutter_xdisplay(), 
	      clutter_stage_get_xwindow (CLUTTER_STAGE(self)));
}

static void
clutter_stage_hide (ClutterElement *self)
{
  XUnmapWindow (clutter_xdisplay(), 
		clutter_stage_get_xwindow (CLUTTER_STAGE(self)));
}

static void
clutter_stage_unrealize (ClutterElement *element)
{
  ClutterStage        *stage;
  ClutterStagePrivate *priv;

  stage = CLUTTER_STAGE(element);
  priv = stage->priv;

  XDestroyWindow (clutter_xdisplay(), priv->xwin);
  priv->xwin = None;
}

static void
clutter_stage_realize (ClutterElement *element)
{
  ClutterStage        *stage;
  ClutterStagePrivate *priv;

  stage = CLUTTER_STAGE(element);

  priv = stage->priv;

  priv->xwin = XCreateSimpleWindow(clutter_xdisplay(),
				   clutter_root_xwindow(),
				   0, 0,
				   priv->xwin_width, priv->xwin_height,
				   0, 0, 
				   WhitePixel(clutter_xdisplay(), 
					      clutter_xscreen()));

  XSelectInput(clutter_xdisplay(), 
	       priv->xwin, 
	       StructureNotifyMask
	       |ExposureMask
	       |KeyPressMask
	       |KeyReleaseMask
	       |ButtonPressMask	    /* FIXME: Make optional ? */
	       |ButtonReleaseMask
	       |PropertyChangeMask);

  sync_fullscreen (stage);  
  sync_cursor_visible (stage);  

  glXMakeCurrent(clutter_xdisplay(), priv->xwin, clutter_gl_context());
  sync_gl_viewport (stage);
}

static void
clutter_stage_paint (ClutterElement *self)
{
  parent_class->paint(self);
}

static void
clutter_stage_allocate_coords (ClutterElement    *self,
			       ClutterElementBox *box)
{
  /* Do nothing, just stop group_allocate getting called */

  /* TODO: sync up with any configure events from WM ??  */
  return;
}

static void
clutter_stage_request_coords (ClutterElement    *self,
			      ClutterElementBox *box)
{
  ClutterStage        *stage;
  ClutterStagePrivate *priv;
  gint                 new_width, new_height;

  stage = CLUTTER_STAGE(self);

  priv = stage->priv;

  new_width  = ABS(box->x2 - box->x1);
  new_height = ABS(box->y2 - box->y1); 

  if (new_width != priv->xwin_width || new_height != priv->xwin_height)
    {
      priv->xwin_width  = new_width;
      priv->xwin_height = new_height;

      if (priv->xwin)
	XResizeWindow (clutter_xdisplay(), 
		       priv->xwin, 
		       priv->xwin_width, 
		       priv->xwin_height);
      sync_gl_viewport (stage);
    }

  if (priv->xwin) /* Do we want to bother ? */
    XMoveWindow (clutter_xdisplay(), 
		 priv->xwin,
		 box->x1,
		 box->y1);
}

static void 
clutter_stage_dispose (GObject *object)
{
  ClutterStage *self = CLUTTER_STAGE(object);

  if (self->priv)
    {
      if (self->priv->xwin)
	clutter_stage_unrealize (CLUTTER_ELEMENT(self));
    }

  G_OBJECT_CLASS (clutter_stage_parent_class)->dispose (object);
}

static void 
clutter_stage_finalize (GObject *object)
{
  ClutterStage *self = CLUTTER_STAGE(object);

  if (self->priv)
    {
      g_free(self->priv);
      self->priv = NULL;
    }

  G_OBJECT_CLASS (clutter_stage_parent_class)->finalize (object);
}


static void
clutter_stage_set_property (GObject      *object, 
			    guint         prop_id,
			    const GValue *value, 
			    GParamSpec   *pspec)
{
  ClutterStage        *stage;
  ClutterStagePrivate *priv;

  stage = CLUTTER_STAGE(object);
  priv = stage->priv;

  switch (prop_id) 
    {
    case PROP_FULLSCREEN:
      if (priv->want_fullscreen != g_value_get_boolean (value))
	{
	  priv->want_fullscreen = g_value_get_boolean (value);
	  sync_fullscreen (stage);
	}
      break;
    case PROP_HIDE_CURSOR:
      if (priv->hide_cursor != g_value_get_boolean (value))
	{
	  priv->hide_cursor = g_value_get_boolean (value);
	  sync_cursor_visible (stage);
	}
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

  stage = CLUTTER_STAGE(object);
  priv = stage->priv;

  switch (prop_id) 
    {
    case PROP_FULLSCREEN:
      g_value_set_boolean (value, priv->want_fullscreen);
      break;
    case PROP_HIDE_CURSOR:
      g_value_set_boolean (value, priv->hide_cursor);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    } 
}

static void
clutter_stage_class_init (ClutterStageClass *klass)
{
  GObjectClass        *gobject_class;
  ClutterElementClass *element_class;
  ClutterGroupClass     *group_class;

  gobject_class = (GObjectClass*)klass;
  element_class = (ClutterElementClass*)klass;
  group_class     = (ClutterGroupClass*)klass;

  element_class->realize    = clutter_stage_realize;
  element_class->unrealize  = clutter_stage_unrealize;
  element_class->show       = clutter_stage_show;
  element_class->hide       = clutter_stage_hide;
  element_class->paint      = clutter_stage_paint;

  element_class->request_coords  = clutter_stage_request_coords;
  element_class->allocate_coords = clutter_stage_allocate_coords;

  gobject_class->dispose      = clutter_stage_dispose;
  gobject_class->finalize     = clutter_stage_finalize;
  gobject_class->set_property = clutter_stage_set_property;
  gobject_class->get_property = clutter_stage_get_property;

  parent_class = g_type_class_peek_parent(group_class);

  g_object_class_install_property
    (gobject_class, PROP_FULLSCREEN,
     g_param_spec_boolean ("fullscreen",
			   "Fullscreen",
			   "Make Clutter stage fullscreen",
			   FALSE,
			   G_PARAM_CONSTRUCT | G_PARAM_READWRITE));

  g_object_class_install_property
    (gobject_class, PROP_HIDE_CURSOR,
     g_param_spec_boolean ("hide-cursor",
			   "Hide Cursor",
			   "Make Clutter stage cursor-less",
			   FALSE,
			   G_PARAM_CONSTRUCT | G_PARAM_READWRITE));

  stage_signals[SIGNAL_INPUT_EVENT] =
    g_signal_new ("input-event",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterStageClass, input_event),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__POINTER,
		  G_TYPE_NONE, 
		  1, G_TYPE_POINTER);
}

static void
clutter_stage_init (ClutterStage *self)
{
  ClutterStagePrivate *priv;

  priv        = g_new0 (ClutterStagePrivate, 1);

  priv->xwin_width  = 100;
  priv->xwin_height = 100;
  priv->color       = 0xffffffff;
  self->priv  = priv;

  clutter_element_set_size (CLUTTER_ELEMENT(self), 640, 480);
}

/**
 * clutter_stage_get_xwindow
 * @stage: A #ClutterStage
 *
 * Get the stages underlying x window ID.
 *
 * Return Value: Stage X Window XID
 **/
Window
clutter_stage_get_xwindow (ClutterStage *stage)
{
  return stage->priv->xwin;
}

/**
 * clutter_stage_set_color
 * @stage: A #ClutterStage
 * @color: A #ClutterColor
 * 
 * Set the stage color.
 **/
void
clutter_stage_set_color (ClutterStage *stage,
			 ClutterColor  color)
{
  stage->priv->color = color;

  if (CLUTTER_ELEMENT_IS_VISIBLE (CLUTTER_ELEMENT(stage)))
    clutter_element_queue_redraw (CLUTTER_ELEMENT(stage));
}

/**
 * clutter_stage_set_color
 * @stage: A #ClutterStage
 * 
 * Request the stage color.
 * 
 * Return Value: The stages #ClutterColor
 **/
ClutterColor
clutter_stage_get_color (ClutterStage *stage)
{
  return stage->priv->color;
}

static void
snapshot_pixbuf_free(guchar  *pixels, gpointer data)
{
  g_free(pixels);
}

/**
 * clutter_stage_snapshot
 * @stage A #ClutterStage
 * @x     x  coordinate of the first pixel that is read from stage
 * @y     y  coordinate of the first pixel that is read from stage
 * @width Width dimention of pixels to be read.
 * @height Height dimention of pixels to be read.
 *
 * Gets a pixel based representation of the current rendered stage.
 *
 * Return value: pixel representation as a  #GdkPixbuf
 **/
GdkPixbuf*
clutter_stage_snapshot (ClutterStage *stage,
			gint          x,
			gint          y,
			guint         width,
			guint         height)
{
  guchar    *data;
  GdkPixbuf *pixb, *fpixb;

  data = g_malloc0(sizeof(guchar)*width*height*3);

  glReadPixels (x, 
		clutter_element_get_height(CLUTTER_ELEMENT(clutter_stage())) 
		- y - height,
		width, 
		height, GL_RGB, GL_UNSIGNED_BYTE, data);

  pixb = gdk_pixbuf_new_from_data (data,
				   GDK_COLORSPACE_RGB, 
				   FALSE, 
				   8, 
				   width, 
				   height,
				   width * 3,
				   snapshot_pixbuf_free,
				   NULL);

  if (pixb == NULL)
    return NULL;

  fpixb = gdk_pixbuf_flip (pixb, TRUE);

  g_object_unref (pixb);

  return fpixb;
}

/* FIXME: better name - pos_to_element */
ClutterElement*
clutter_stage_pick (ClutterStage *stage, gint x, gint y)
{
  ClutterElement *found = NULL;
  GLuint          buff[64] = {0};
  GLint           hits, view[4], i;
 
  glSelectBuffer(64, buff);
  glGetIntegerv(GL_VIEWPORT, view);
  glRenderMode(GL_SELECT);

  glInitNames();

  glPushName(0);
 
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();

  /* This is gluPickMatrix(x, y, 1.0, 1.0, view); */
  glTranslatef((view[2] - 2 * (x - view[0])),
	       (view[3] - 2 * (y - view[1])), 0);
  glScalef(view[2], -view[3], 1.0);

  perspective (60.0f, 1.0f, 0.1f, 100.0f); 

  glMatrixMode(GL_MODELVIEW);

  clutter_element_paint(CLUTTER_ELEMENT(stage));

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();

  hits = glRenderMode(GL_RENDER);

  if (hits != 0)
    {
      /*
      for (i=0; i<hits; i++)
	g_print ("Hit at %i\n", buff[i * 4 + 3]);
      */
  
      found = clutter_group_find_child_by_id (clutter_stage(), buff[3]);
    }
  
  sync_gl_viewport (stage);

  return found;
}

