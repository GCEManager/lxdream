/**
 * $Id: pvr2.h,v 1.9 2006-03-13 12:39:07 nkeynes Exp $
 *
 * PVR2 (video chip) functions and macros.
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

#include "dream.h"
#include "mem.h"
#include "video.h"
#include "pvr2/pvr2mmio.h"
#include <GL/gl.h>


#define DISPMODE_DE  0x00000001 /* Display enable */
#define DISPMODE_SD  0x00000002 /* Scan double */
#define DISPMODE_COL 0x0000000C /* Colour mode */
#define DISPMODE_CD  0x08000000 /* Clock double */

#define COLFMT_RGB15 0x00000000
#define COLFMT_RGB16 0x00000004
#define COLFMT_RGB24 0x00000008
#define COLFMT_RGB32 0x0000000C

#define DISPSIZE_MODULO 0x3FF00000 /* line skip +1 (32-bit words)*/
#define DISPSIZE_LPF    0x000FFC00 /* lines per field */
#define DISPSIZE_PPL    0x000003FF /* pixel words (32 bit) per line */

#define DISPCFG_VP 0x00000001 /* V-sync polarity */
#define DISPCFG_HP 0x00000002 /* H-sync polarity */
#define DISPCFG_I  0x00000010 /* Interlace enable */
#define DISPCFG_BS 0x000000C0 /* Broadcast standard */
#define DISPCFG_VO 0x00000100 /* Video output enable */

#define BS_NTSC 0x00000000
#define BS_PAL  0x00000040
#define BS_PALM 0x00000080 /* ? */
#define BS_PALN 0x000000C0 /* ? */

#define PVR2_RAM_BASE 0x05000000
#define PVR2_RAM_BASE_INT 0x04000000
#define PVR2_RAM_SIZE (8 * 1024 * 1024)
#define PVR2_RAM_PAGES (PVR2_RAM_SIZE>>12)

void pvr2_next_frame( void );
void pvr2_set_base_address( uint32_t );

#define PVR2_CMD_END_OF_LIST 0x00
#define PVR2_CMD_USER_CLIP   0x20
#define PVR2_CMD_POLY_OPAQUE 0x80
#define PVR2_CMD_MOD_OPAQUE  0x81
#define PVR2_CMD_POLY_TRANS  0x82
#define PVR2_CMD_MOD_TRANS   0x83
#define PVR2_CMD_POLY_PUNCHOUT 0x84
#define PVR2_CMD_VERTEX      0xE0
#define PVR2_CMD_VERTEX_LAST 0xF0

#define PVR2_POLY_TEXTURED 0x00000008
#define PVR2_POLY_SPECULAR 0x00000004
#define PVR2_POLY_SHADED   0x00000002
#define PVR2_POLY_UV_16BIT 0x00000001

#define PVR2_TEX_FORMAT_ARGB1555 0x00000000
#define PVR2_TEX_FORMAT_RGB565   0x08000000
#define PVR2_TEX_FORMAT_ARGB4444 0x10000000
#define PVR2_TEX_FORMAT_YUV422   0x18000000
#define PVR2_TEX_FORMAT_BUMPMAP  0x20000000
#define PVR2_TEX_FORMAT_IDX4     0x28000000
#define PVR2_TEX_FORMAT_IDX8     0x30000000

#define PVR2_TEX_MIPMAP      0x80000000
#define PVR2_TEX_COMPRESSED  0x40000000
#define PVR2_TEX_FORMAT_MASK 0x38000000
#define PVR2_TEX_UNTWIDDLED  0x04000000

#define PVR2_TEX_ADDR(x) ( ((x)&0x1FFFFF)<<3 );
#define PVR2_TEX_IS_MIPMAPPED(x) ( (x) & PVR2_TEX_MIPMAP )
#define PVR2_TEX_IS_COMPRESSED(x) ( (x) & PVR2_TEX_COMPRESSED )
#define PVR2_TEX_IS_TWIDDLED(x) (((x) & PVR2_TEX_UNTWIDDLED) == 0)

extern video_driver_t video_driver;

/****************************** Frame Buffer *****************************/

/**
 * Write to the interleaved memory address space (aka 64-bit address space).
 */
void pvr2_vram64_write( sh4addr_t dest, char *src, uint32_t length );

/**
 * Read from the interleaved memory address space (aka 64-bit address space)
 */
void pvr2_vram64_read( char *dest, sh4addr_t src, uint32_t length );

/**************************** Tile Accelerator ***************************/
/**
 * Process the data in the supplied buffer as an array of TA command lists.
 * Any excess bytes are held pending until a complete list is sent
 */
void pvr2_ta_write( char *buf, uint32_t length );


/**
 * (Re)initialize the tile accelerator in preparation for the next scene.
 * Normally called immediately before commencing polygon transmission.
 */
void pvr2_ta_init( void );

/********************************* Renderer ******************************/

/**
 * Initialize the rendering pipeline.
 * @return TRUE on success, FALSE on failure.
 */
gboolean pvr2_render_init( void );

/**
 * Render the current scene stored in PVR ram to the GL back buffer.
 */
void pvr2_render_scene( void );

/**
 * Display the scene rendered to the supplied address.
 * @return TRUE if there was an available render that was displayed,
 * otherwise FALSE (and no action was taken)
 */
gboolean pvr2_render_display_frame( uint32_t address );

/****************************** Texture Cache ****************************/

/**
 * Initialize the texture cache. Note that the GL context must have been
 * initialized before calling this function.
 */
void texcache_init( void );


/**
 * Flush all textures and delete. The cache will be non-functional until
 * the next call to texcache_init(). This would typically be done if
 * switching GL targets.
 */    
void texcache_shutdown( void );

/**
 * Evict all textures contained in the page identified by a texture address.
 */
void texcache_invalidate_page( uint32_t texture_addr );

/**
 * Return a texture ID for the texture specified at the supplied address
 * and given parameters (the same sequence of bytes could in theory have
 * multiple interpretations). We use the texture address as the primary
 * index, but allow for multiple instances at each address. The texture
 * will be bound to the GL_TEXTURE_2D target before being returned.
 * 
 * If the texture has already been bound, return the ID to which it was
 * bound. Otherwise obtain an unused texture ID and set it up appropriately.
 */
GLuint texcache_get_texture( uint32_t texture_addr, int width, int height,
			     int mode );
