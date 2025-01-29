#ifndef GUI_H
#define GUI_H

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include "../Decoding/decoding.h"

void activate(GtkApplication *app, gpointer user_data);
int command_line_cb(GtkApplication *app, GApplicationCommandLine *cmdline, gpointer user_data);
void onPausePlayToggle(GtkButton *button, gpointer user_data);
gboolean onKeyPress(GtkEventController *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);

#endif // GUI_H