/**
 * $Id:  $
 *
 * LIRC input device support
 *
 * Copyright (c) 2009 wahrhaft
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>

#include <glib/giochannel.h>
#include <glib.h>

#include <lirc/lirc_client.h>

#include "lxdream.h"
#include "display.h"
#include "maple/maple.h"
#include "drivers/input_lirc.h"

typedef struct input_lirc {
    struct input_driver driver;
    char name[5];
    int fd;
    GIOChannel *channel;
} *input_lirc_t;

#define MAX_KEYSYMS 256
static char *keysyms[MAX_KEYSYMS];

static uint16_t input_lirc_resolve_keysym( input_driver_t dev, const gchar *str );
static gchar *input_lirc_keysym_for_keycode( input_driver_t dev, uint16_t keycode );
static gboolean input_lirc_callback( GIOChannel *source, GIOCondition condition, gpointer data );

input_driver_t system_lirc_driver;

void input_lirc_create()
{
    input_lirc_t system_lirc_driver = g_malloc0(sizeof(struct input_lirc));
    strcpy(system_lirc_driver->name, "LIRC");
    system_lirc_driver->driver.id = system_lirc_driver->name;
    system_lirc_driver->driver.resolve_keysym = input_lirc_resolve_keysym;
    system_lirc_driver->driver.get_keysym_for_keycode = input_lirc_keysym_for_keycode;
    system_lirc_driver->driver.destroy = NULL;

    system_lirc_driver->fd = lirc_init("lxdream", 1);
    if (system_lirc_driver->fd == -1) {
        WARN("Could not initialize LIRC.  LIRC hotkeys will be disabled.");
        return;
    }
    
    system_lirc_driver->channel = g_io_channel_unix_new(system_lirc_driver->fd);
    g_io_channel_set_flags(system_lirc_driver->channel, G_IO_FLAG_IS_READABLE | G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(system_lirc_driver->channel, G_IO_IN|G_IO_ERR|G_IO_HUP, input_lirc_callback, system_lirc_driver);
    memset(keysyms, 0, MAX_KEYSYMS);
    input_register_device((input_driver_t)system_lirc_driver, MAX_KEYSYMS - 1);
    INFO("LIRC initialized");
}

void input_lirc_shutdown(void)
{
    input_lirc_t lirc = (input_lirc_t)system_lirc_driver;
    g_io_channel_shutdown(lirc->channel, FALSE, NULL );
    g_io_channel_unref(lirc->channel);
    lirc_deinit();
    g_free(system_lirc_driver);
}

static uint16_t input_lirc_resolve_keysym( input_driver_t dev, const gchar *str )
{
    //LIRC uses keysyms but no keycodes.  To generate a keycode, we'll just make them up as we go.
    //As long as we store keysyms instead of keycodes in any config files, this should be fine.
    uint16_t i;
    for (i = 1; i < MAX_KEYSYMS && keysyms[i] != NULL; i++) {
        if (strcasecmp(str, keysyms[i]) == 0) {
            //keycode already exists
            return i;
        }
    }
    
    if (i < MAX_KEYSYMS) {
        //this key is not in the list yet, so make a new keycode for it
        keysyms[i] = g_strdup(str);
        return i;
    } else {
        //if your remote has more than 256 buttons, you may need to increase MAX_KEYSYMS
        ERROR("LIRC has too many keysyms!");
        return 0;
    }
}

static gchar *input_lirc_keysym_for_keycode( input_driver_t dev, uint16_t keycode )
{
    //This won't work if you send in keycodes that haven't been fired by the callback
    //or looked up using resolve_keysym yet.  This shouldn't be a problem, since these
    //two functions are the only way to get a keycode in the first place.
    
    if (keycode < MAX_KEYSYMS)
        return g_strdup(keysyms[keycode]);
    else
        return NULL;
}

gboolean input_lirc_callback( GIOChannel *source, GIOCondition condition, gpointer data )
{
    int ret;
    char *code, *c;
    char keysym[256];
    
    input_lirc_t lirc = (input_lirc_t)data;
    
    if (condition & G_IO_IN)
    {
        //loop through all queued commands
        while ((ret = lirc_nextcode(&code)) == 0 && code != NULL)
        {
            INFO("LIRC code (%s)", code);
            //code contains id, repeat count, and keysym separated by spaces
            gchar **code_split = g_strsplit(code, " ", 4);
            
            //eliminate repeats by only accepting the first instance of a keysym
            if (atoi(code_split[1]) == 0)
            {
                input_event_keydown((input_driver_t)lirc, input_lirc_resolve_keysym((input_driver_t)lirc, code_split[2]), MAX_PRESSURE);
            }
            
            g_strfreev(code_split);
            free(code);
        }
    }
    return TRUE;
}

