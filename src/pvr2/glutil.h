/**
 * $Id$
 *
 * GL-based support functions 
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

#ifndef lxdream_glutil_H
#define lxdream_glutil_H

#include <stdio.h>
#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Test if a specific extension is supported. From opengl.org
 * @param extension extension name to check for
 * @return TRUE if supported, otherwise FALSE.
 */
gboolean isGLExtensionSupported( const char *extension );

/**
 * Dump GL information to the output stream, usually for debugging purposes
 */
void glPrintInfo( FILE *out );

/**
 * Check for a GL error and print a message if there is one
 * @param context If not null, a string to be printed along side an error message
 * @return FALSE if there was an error, otherwise TRUE
 */
gboolean gl_check_error( const char *context );
/**
 * Test if secondary color (GL_COLOR_SUM) is supported.
 */
gboolean isGLSecondaryColorSupported();

gboolean isGLVertexBufferSupported();
gboolean isGLVertexRangeSupported();
gboolean isGLPixelBufferSupported();
gboolean isGLMultitextureSupported();
gboolean isGLMirroredTextureSupported();
GLint glGetMaxColourAttachments();

/****** Extension variant wrangling *****/

#if defined(GL_MIRRORED_REPEAT_ARB) && !defined(GL_MIRRORED_REPEAT)
#define GL_MIRRORED_REPEAT GL_MIRRORED_REPEAT_ARB
#endif

#if defined(GL_MAX_TEXTURE_UNITS_ARB) && !defined(GL_MAX_TEXTURE_UNITS)
#define GL_MAX_TEXTURE_UNITS GL_MAX_TEXTURE_UNITS_ARB
#endif

#if defined(GL_FRAMEBUFFER_EXT) && !defined(GL_FRAMEBUFFER)
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_EXT
#endif

#if defined(GL_RENDERBUFFER_EXT) && !defined(GL_RENDERBUFFER)
#define GL_RENDERBUFFER GL_RENDERBUFFER_EXT
#endif

#if defined(GL_COLOR_ATTACHMENT0_EXT) && !defined(GL_COLOR_ATTACHMENT0)
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#endif

#if defined(GL_MAX_COLOR_ATTACHMENTS_EXT) && !defined(GL_MAX_COLOR_ATTACHMENTS)
#define GL_MAX_COLOR_ATTACHMENTS GL_MAX_COLOR_ATTACHMENTS_EXT
#endif

#if defined(GL_STENCIL_ATTACHMENT_EXT) && !defined(GL_STENCIL_ATTACHMENT)
#define GL_STENCIL_ATTACHMENT GL_STENCIL_ATTACHMENT_EXT
#endif

#if defined(GL_DEPTH_ATTACHMENT_EXT) && !defined(GL_DEPTH_ATTACHMENT)
#define GL_DEPTH_ATTACHMENT GL_DEPTH_ATTACHMENT_EXT
#endif

#if defined(GL_DEPTH_COMPONENT24_OES) && !defined(GL_DEPTH_COMPONENT24)
#define GL_DEPTH_COMPONENT24 GL_DEPTH_COMPONENT24_OES
#endif

#ifndef GL_DEPTH24_STENCIL8
#if defined(GL_DEPTH24_STENCIL8_EXT)
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_EXT
#elif defined(GL_DEPTH24_STENCIL8_OES)
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_OES
#endif
#endif

#if defined(GL_FRAMEBUFFER_COMPLETE_EXT) && !defined(GL_FRAMEBUFFER_COMPLETE)
#define GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_EXT
#endif

#if defined(HAVE_OPENGL_FBO_EXT) && !defined(HAVE_OPENGL_FBO)
#define glGenFramebuffers glGenFramebuffersEXT
#define glGenRenderbuffers glGenRenderbuffersEXT
#define glBindFramebuffer glBindFramebufferEXT
#define glDeleteFramebuffers glDeleteFramebuffersEXT
#define glDeleteRenderbuffers glDeleteRenderbuffersEXT
#define glBindFramebuffer glBindFramebufferEXT
#define glBindRenderbuffer glBindRenderbufferEXT
#define glRenderbufferStorage glRenderbufferStorageEXT
#define glFramebufferRenderbuffer glFramebufferRenderbufferEXT
#define glFramebufferTexture2D glFramebufferTexture2DEXT
#define glCheckFramebufferStatus glCheckFramebufferStatusEXT
#endif


/****** Shader handling (gl_sl.c) *****/
gboolean glsl_is_supported(void);
const char *glsl_get_version(void);
gboolean glsl_load_shaders( );
void glsl_unload_shaders(void);
void glsl_clear_shader();

/* Convenience formatting function for driver use */
void fprint_extensions( FILE *out, const char *extensions );

#ifdef __cplusplus
}
#endif

#endif /* !lxdream_glutil_H */
