/*
 * Simple slideshow application, cycling images between
 * two ClutterTextures
 *
 * Run by passing one or more image paths or directory globs
 * which will pick up image files
 *
 * When running, press any key to go to the next image
 */
#include <stdlib.h>
#include <clutter/clutter.h>

static guint stage_side = 600;
static guint animation_duration_ms = 1500;

static const ClutterColor stage_color = { 0x33, 0x33, 0x55, 0xff };

typedef struct {
  ClutterActor *front;
  ClutterActor *back;
  ClutterState *transitions;
  GSList       *image_paths;
  guint         next_image_index;
} State;

static gboolean
load_next_image (State *app)
{
  /* don't do anything if already animating */
  ClutterTimeline *timeline = clutter_state_get_timeline (app->transitions);

  if (clutter_timeline_is_playing (timeline) == 1)
    {
      g_debug ("Animation is running already");
      return FALSE;
    }

  if (!app->next_image_index)
      app->next_image_index = 0;

  gpointer next = g_slist_nth_data (app->image_paths, app->next_image_index);

  if (next == NULL)
    return FALSE;

  gchar *image_path = (gchar *)next;

  g_debug ("Loading %s", image_path);

  CoglHandle *cogl_texture;
  cogl_texture = clutter_texture_get_cogl_texture (CLUTTER_TEXTURE (app->front));

  if (cogl_texture != NULL)
    {
      /* copy the current texture into the background */
      clutter_texture_set_cogl_texture (CLUTTER_TEXTURE (app->back), cogl_texture);

      /* make the back opaque and front transparent */
      clutter_state_warp_to_state (app->transitions, "show-back");
    }

  /* load the next image into the front */
  GError *error = NULL;
  clutter_texture_set_from_file (CLUTTER_TEXTURE (app->front),
                                 image_path,
                                 &error);

  if (error != NULL)
    {
      g_warning ("Error loading %s\n%s", image_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  /* fade in the front texture and fade out the back texture */
  clutter_state_set_state (app->transitions, "show-front");

  app->next_image_index++;

  return TRUE;
}

static gboolean
_key_pressed_cb (ClutterActor *actor,
                 ClutterEvent *event,
                 gpointer      user_data)
{
  State *app = (State *)user_data;

  load_next_image (app);

  return TRUE;
}

int
main (int argc, char *argv[])
{
  if (argc < 2)
    {
      g_print ("Usage: %s <image paths to load>\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  State *app = g_new0 (State, 1);
  app->image_paths = NULL;

  /*
   * NB if your shell globs arguments to this program so argv
   * includes non-image files, they will fail to load and throw errors
   */
  guint i;
  for (i = 1; i < argc; i++)
    app->image_paths = g_slist_append (app->image_paths, argv[i]);

  GError *error = NULL;

  /* UI */
  ClutterActor *stage;
  ClutterLayoutManager *layout;
  ClutterActor *box;

  clutter_init (&argc, &argv);

  stage = clutter_stage_get_default ();
  g_signal_connect (stage, "destroy", G_CALLBACK (clutter_main_quit), NULL);
  clutter_stage_set_title (CLUTTER_STAGE (stage), "cross-fade");
  clutter_actor_set_size (stage, stage_side, stage_side);
  clutter_stage_set_color (CLUTTER_STAGE (stage), &stage_color);

  layout = clutter_bin_layout_new (CLUTTER_BIN_ALIGNMENT_CENTER,
                                   CLUTTER_BIN_ALIGNMENT_CENTER);

  box = clutter_box_new (layout);
  clutter_actor_set_size (box, stage_side, stage_side);

  app->back = clutter_texture_new ();
  clutter_texture_set_keep_aspect_ratio (CLUTTER_TEXTURE (app->back), TRUE);

  app->front = clutter_texture_new ();
  clutter_texture_set_keep_aspect_ratio (CLUTTER_TEXTURE (app->front), TRUE);

  clutter_container_add_actor (CLUTTER_CONTAINER (box), app->back);
  clutter_container_add_actor (CLUTTER_CONTAINER (box), app->front);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), box);

  /* animations */
  app->transitions = clutter_state_new ();
  clutter_state_set (app->transitions, NULL, "show-front",
                     app->front, "opacity", CLUTTER_EASE_IN_CUBIC, 255,
                     app->back, "opacity", CLUTTER_EASE_IN_CUBIC, 0,
                     NULL);
  clutter_state_set (app->transitions, NULL, "show-back",
                     app->front, "opacity", CLUTTER_LINEAR, 0,
                     app->back, "opacity", CLUTTER_LINEAR, 255,
                     NULL);
  clutter_state_set_duration (app->transitions,
                              NULL,
                              NULL,
                              animation_duration_ms);

  /* display the next (first) image */
  load_next_image (app);

  /* key press displays the next image */
  g_signal_connect (stage,
                    "key-press-event",
                    G_CALLBACK (_key_pressed_cb),
                    app);

  clutter_actor_show (stage);

  clutter_main ();

  g_slist_free (app->image_paths);
  g_object_unref (app->transitions);
  g_free (app);

  if (error != NULL)
    g_error_free (error);

  return EXIT_SUCCESS;
}
