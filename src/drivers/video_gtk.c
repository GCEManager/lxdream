/**
 * $Id$
 *
 * The PC side of the video support (responsible for actually displaying / 
 * rendering frames)
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

#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <stdint.h>
#include "dream.h"
#include "display.h"
#include "drivers/video_glx.h"
#include "drivers/gl_common.h"
#include "gtkui/gtkui.h"

static GtkWidget *video_win = NULL;
int video_width = 640;
int video_height = 480;

gboolean video_gtk_init();
void video_gtk_shutdown();
gboolean video_gtk_display_blank( uint32_t colour );
uint16_t video_gtk_resolve_keysym( const gchar *keysym );

struct display_driver display_gtk_driver = { "gtk", video_gtk_init, video_gtk_shutdown,
					     video_gtk_resolve_keysym,
					     NULL, NULL, NULL, NULL, NULL, 
					     video_gtk_display_blank, NULL };

/**
 * Extract the keyval of the key event if no modifier keys were pressed -
 * in other words get the keyval of the key by itself. The other way around
 * would be to use the hardware keysyms directly rather than the keyvals,
 * but the mapping looks to be messier.
 */
uint16_t video_gtk_unmodified_keyval( GdkEventKey *event )
{
    GdkKeymap *keymap = gdk_keymap_get_default();
    guint keyval;
    
    gdk_keymap_translate_keyboard_state( keymap, event->hardware_keycode, 0, 0, &keyval, 
					 NULL, NULL, NULL );
    return keyval;
}

gboolean video_gtk_keydown_callback(GtkWidget       *widget,
				     GdkEventKey     *event,
				     gpointer         user_data)
{
    input_event_keydown( video_gtk_unmodified_keyval(event) );
    return TRUE;
}

gboolean video_gtk_keyup_callback(GtkWidget       *widget,
				  GdkEventKey     *event,
				  gpointer         user_data)
{
    input_event_keyup( video_gtk_unmodified_keyval(event) );
    return TRUE;
}

uint16_t video_gtk_resolve_keysym( const gchar *keysym )
{
    int val = gdk_keyval_from_name( keysym );
    if( val == GDK_VoidSymbol )
	return 0;
    return (uint16_t)val;
}

gboolean video_gtk_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer data )
{
    render_buffer_t buffer = pvr2_get_front_buffer();
    if( buffer == NULL ) {
	display_gtk_driver.display_blank(pvr2_get_border_colour());
    } else {
	display_gtk_driver.display_render_buffer(buffer);
    }
    return TRUE;
}

gboolean video_gtk_resize_callback(GtkWidget *widget, GdkEventConfigure *event, gpointer data )
{
    video_width = event->width;
    video_height = event->height;
    video_gtk_expose_callback(widget, NULL, data);
    return TRUE;
}

gboolean video_gtk_init()
{
  
    video_win = gtk_gui_get_renderarea();
    if( video_win == NULL ) {
	return FALSE;
    }

    g_signal_connect( video_win, "key_press_event", 
		      G_CALLBACK(video_gtk_keydown_callback), NULL );
    g_signal_connect( video_win, "key_release_event", 
		      G_CALLBACK(video_gtk_keyup_callback), NULL );
    g_signal_connect( video_win, "expose_event",
		      G_CALLBACK(video_gtk_expose_callback), NULL );
    g_signal_connect( video_win, "configure_event",
		      G_CALLBACK(video_gtk_resize_callback), NULL );
    gtk_widget_add_events( video_win, 
			   GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
			   GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK );
    gtk_widget_set_double_buffered( video_win, FALSE );
    video_width = video_win->allocation.width;
    video_height = video_win->allocation.height;
    Display *display = gdk_x11_display_get_xdisplay( gtk_widget_get_display(GTK_WIDGET(video_win)));
    Window window = GDK_WINDOW_XWINDOW( GTK_WIDGET(video_win)->window );
    if( ! video_glx_init_context( display, window ) ||
        ! video_glx_init_driver( &display_gtk_driver ) ) {
        return FALSE;
    }
    return TRUE;
}

gboolean video_gtk_display_blank( uint32_t colour )
{
    GdkGC *gc = gdk_gc_new(video_win->window);
    GdkColor color = {0, ((colour>>16)&0xFF)*257, ((colour>>8)&0xFF)*257, ((colour)&0xFF)*257 };
    GdkColormap *cmap = gdk_colormap_get_system();
    gdk_colormap_alloc_color( cmap, &color, TRUE, TRUE );
    gdk_gc_set_foreground( gc, &color );
    gdk_gc_set_background( gc, &color );
    gdk_draw_rectangle( video_win->window, gc, TRUE, 0, 0, video_width, video_height );
    gdk_gc_destroy(gc);
    gdk_colormap_free_colors( cmap, &color, 1 );
}

void video_gtk_shutdown()
{
    if( video_win != NULL ) {
	video_glx_shutdown();
    }

}

