/**
 * $Id$
 *
 * Cocoa (NSOpenGL) video driver
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

#include <AppKit/NSOpenGL.h>
#include <Foundation/NSAutoreleasePool.h>
#include "drivers/video_nsgl.h"
#include "drivers/video_gl.h"
#include "pvr2/glutil.h"

static NSOpenGLContext *nsgl_context = nil;

gboolean video_nsgl_init_driver( NSView *view, display_driver_t driver )
{
    NSAutoreleasePool *pool = [NSAutoreleasePool new];
    NSOpenGLPixelFormatAttribute attributes[] = {
            NSOpenGLPFAWindow,
            // NSOpenGLPFADoubleBuffer,
            NSOpenGLPFADepthSize, (NSOpenGLPixelFormatAttribute)24,
            (NSOpenGLPixelFormatAttribute)nil };

    NSOpenGLPixelFormat *pixelFormat = 
        [[[NSOpenGLPixelFormat alloc] initWithAttributes: attributes] autorelease];
    nsgl_context = 
        [[NSOpenGLContext alloc] initWithFormat: pixelFormat shareContext: nil];
    [nsgl_context setView: view];
    [nsgl_context makeCurrentContext];
    [pool release];
    driver->swap_buffers = video_nsgl_swap_buffers;
    driver->capabilities.has_gl = TRUE;
    driver->capabilities.depth_bits = 24;
    gl_init_driver(driver, TRUE);
    return TRUE;
}

void video_nsgl_update()
{
    if( nsgl_context != nil ) {
        [nsgl_context update];
    }
}

void video_nsgl_make_current()
{
    if( nsgl_context != nil ) {
        [nsgl_context makeCurrentContext];
    }
}

void video_nsgl_swap_buffers()
{
    if( nsgl_context != nil ) {
        [nsgl_context flushBuffer];
    }
}

void video_nsgl_shutdown()
{
    if( nsgl_context != nil ) {
        [NSOpenGLContext clearCurrentContext];
        [nsgl_context release];
        nsgl_context = nil;
    }
}
