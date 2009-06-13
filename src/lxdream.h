/**
 * $Id$
 *
 * Common type definitions and forward declarations
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

#ifndef lxdream_lxdream_H
#define lxdream_lxdream_H 1

#include <stdint.h>
#include <glib/gtypes.h>

#include "../config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_NAME PACKAGE
extern const char lxdream_package_name[];
extern const char lxdream_short_version[];
extern const char lxdream_full_version[];
extern const char lxdream_copyright[];


#define MB *1024*1024
#define KB *1024

#ifndef max
#define max(a,b) ( (a) > (b) ? (a) : (b) )
#endif

/**
 * A 29-bit address in SH4 external address space
 */
typedef uint32_t sh4addr_t;

/**
 * A 32-bit address in SH4 virtual address space.
 */
typedef uint32_t sh4vma_t;

/**
 * A direct pointer into SH4 memory
 */
typedef unsigned char *sh4ptr_t;

/******************* Forward type declarations ******************/

typedef struct render_buffer *render_buffer_t;
typedef struct frame_buffer *frame_buffer_t;

/*************************** Logging ****************************/

#define EMIT_FATAL 0
#define EMIT_ERR 1
#define EMIT_WARN 2
#define EMIT_INFO 3
#define EMIT_DEBUG 4
#define EMIT_TRACE 5

#ifdef MODULE
#define MODULE_NAME MODULE.name
#else
#define MODULE_NAME "*****"
#endif

gboolean set_global_log_level( const gchar *level );
void log_message( void *, int level, const char *source, const char *msg, ... );

#define FATAL( ... ) log_message( NULL, EMIT_FATAL, MODULE_NAME, __VA_ARGS__ )
#define ERROR( ... ) log_message( NULL, EMIT_ERR, MODULE_NAME, __VA_ARGS__ )
#define WARN( ... ) log_message( NULL, EMIT_WARN, MODULE_NAME, __VA_ARGS__ )
#define INFO( ... ) log_message( NULL, EMIT_INFO, MODULE_NAME, __VA_ARGS__ )
#define DEBUG( ... ) log_message( NULL, EMIT_DEBUG, MODULE_NAME, __VA_ARGS__ )
#define TRACE( ... ) log_message( NULL, EMIT_TRACE, MODULE_NAME, __VA_ARGS__ )

/****************** System paths ****************/
const char *get_sysconf_path();
const char *get_locale_path();
const char *get_plugin_path();

#ifdef HAVE_FASTCALL
#define FASTCALL __attribute__((regparm(2)))
#else
#define FASTCALL
#endif

#ifdef __cplusplus
}
#endif

#endif /* !lxdream_lxdream_H */
