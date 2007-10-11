/**
 * $Id: gtkui.h,v 1.2 2007-10-11 08:22:03 nkeynes Exp $
 *
 * Core GTK-based user interface
 *
 * Copyright (c) 2005 Nathan Keynes.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __lxdream_gtkui_H
#define __lxdream_gtkui_H

#include <gtk/gtk.h>
#include "gui.h"
#include "cpu.h"

/********************* Top-level windows *********************/

typedef struct main_window_info *main_window_t;
typedef struct debug_window_info *debug_window_t;
typedef struct mmio_window_info *mmio_window_t;

/**
 * Construct and show the main window, returning an 
 * opaque pointer to the window.
 */
main_window_t main_window_new( const gchar *title );

GtkWidget *main_window_get_renderarea( main_window_t win );
void main_window_set_running( main_window_t win, gboolean running );
void main_window_set_framerate( main_window_t win, float rate );
void main_window_set_speed( main_window_t win, double speed );

debug_window_t debug_window_new();

void debug_window_show( debug_window_t win, gboolean show );
void debug_window_set_running( debug_window_t win, gboolean running );
void debug_window_update(debug_window_t win);

mmio_window_t mmio_window_new();

void mmio_window_show( mmio_window_t win, gboolean show );

/******************** Video driver hooks *********************/

GtkWidget *gtk_gui_get_renderarea();

/******************* Callback declarations *******************/

void mount_action_callback( GtkAction *action, gpointer user_data);
void reset_action_callback( GtkAction *action, gpointer user_data);
void pause_action_callback( GtkAction *action, gpointer user_data);
void resume_action_callback( GtkAction *action, gpointer user_data);
void load_state_action_callback( GtkAction *action, gpointer user_data);
void save_state_action_callback( GtkAction *action, gpointer user_data);
void about_action_callback( GtkAction *action, gpointer user_data);
void debugger_action_callback( GtkAction *action, gpointer user_data);
void exit_action_callback( GtkAction *action, gpointer user_data);

void audio_settings_callback( GtkAction *action, gpointer user_data);
void controller_settings_callback( GtkAction *action, gpointer user_data);
void network_settings_callback( GtkAction *action, gpointer user_data);
void video_settings_callback( GtkAction *action, gpointer user_data);
void fullscreen_toggle_callback( GtkToggleAction *action, gpointer user_data);

/*************** Constant colour/font values *****************/
extern PangoFontDescription *gui_fixed_font;
extern GdkColor gui_colour_normal, gui_colour_changed, gui_colour_error;
extern GdkColor gui_colour_warn, gui_colour_pc, gui_colour_debug;
extern GdkColor gui_colour_trace, gui_colour_break, gui_colour_temp_break;
extern GdkColor gui_colour_white;

#endif /* __lxdream_gtkui_H */