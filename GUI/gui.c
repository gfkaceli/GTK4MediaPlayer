#include "gui.h"

// GTK Callbacks
/*
  Function update_display 
  continuously updates the display by popping images
  callback for our animation functionality
*/
gboolean updateDisplay(GtkWidget *image_widget) {
    if (!is_paused) { // Only update if playing
        GdkPixbuf *pixbuf;
        if (videoBufferPop(&videoBuffer, &pixbuf)) {
            GdkPaintable *paintable =  (GdkPaintable *)gdk_texture_new_for_pixbuf(pixbuf); // Convert to GdkPaintable
            gtk_image_set_from_paintable(GTK_IMAGE(image_widget), paintable); // Use updated function
            g_object_unref(paintable); // Decrease reference count of paintable
            g_object_unref(pixbuf); // Decrease reference count after setting it
        }
    }
    return G_SOURCE_CONTINUE;
}

int command_line_cb(GtkApplication *app,
                           GApplicationCommandLine *cmdline,
                           gpointer user_data) {
  gchar **argv;
  gint argc;
  GError *error = NULL;

  // Get the arguments
  argv = g_application_command_line_get_arguments(cmdline, &argc);

  // Here, process the arguments. For example:
  for (int i = 0; i < argc; ++i) {
    g_print("Argument %d: %s\n", i, argv[i]);
  }

  // If your application has a GUI, you might want to activate it here
  g_application_activate(G_APPLICATION(app));

  // Free the arguments array
  g_strfreev(argv);

  // Return 0 if successful, or an error code if not
  return 0;
}

void onWindowDestroy(GtkWidget *widget, gpointer app) {
    is_running = 0;
    g_application_quit(G_APPLICATION(app));
}


// GTK Button Callback
void onPausePlayToggle(GtkButton *button, gpointer user_data) {
    togglePause();

    // If a valid button reference is passed, update its label
    if (button != NULL) {
        gtk_button_set_label(button, is_paused ? "Play" : "Pause");
    }

    if (!is_paused) {
        // Notify all threads to resume from pause
        pthread_cond_broadcast(&videoBuffer.notEmpty);
        pthread_cond_broadcast(&audioBuffer.notEmpty);
    }
}

// Handle key press events
// Handle key press events using GtkEventControllerKey
gboolean onKeyPress(GtkEventController *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    if (keyval == GDK_KEY_space) {  // Spacebar is pressed
        onPausePlayToggle(NULL, NULL);  // Toggle Play/Pause
        return TRUE;  // Event handled, stop propagation
    }
    return FALSE;  // Let other key events propagate
}


void activate(GtkApplication *app, gpointer user_data) {
        DecodeData *data = (DecodeData *)user_data;
    GtkWidget *window, *main_box, *scrolled_window, *image_widget, *button_box, *pause_button;
    GtkEventController *key_controller;  // Declare the key event controller

    // Create the main application window
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Media Player");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    // Create a key event controller
    key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(onKeyPress), NULL);
    gtk_widget_add_controller(window, key_controller);  // Attach the controller to the window

    // Create a vertical box to hold the image and controls
    main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_window_set_child(GTK_WINDOW(window), main_box);

    // Create a scrolled window for video display
    scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled_window, 800, 450); // Set a fixed size for the display
    image_widget = gtk_image_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), image_widget);
    gtk_box_append(GTK_BOX(main_box), scrolled_window);

    // Create a horizontal box for controls
    button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), button_box);

    // Create a Play/Pause button
    pause_button = gtk_button_new_with_label("Pause");
    g_signal_connect(pause_button, "clicked", G_CALLBACK(onPausePlayToggle), NULL);
    gtk_box_append(GTK_BOX(button_box), pause_button);

    // Connect destroy signal to clean up on window close
    g_signal_connect(window, "destroy", G_CALLBACK(onWindowDestroy), app);

    // Start updating the display periodically
    g_timeout_add(1000 / data->frame_rate, (GSourceFunc)updateDisplay, image_widget);

    gtk_widget_set_visible(window, true);
}