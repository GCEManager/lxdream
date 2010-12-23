/**
 * $Id$
 *
 * Tool to take an input .glsl file and write out a corresponding .c and .h
 * file based on the content. The .glsl file contains a number of shaders
 * marked with either #fragment <name> or #vertex <name>
 * a C file with appropriate escaping, as well as program definitions
 * written as #program <name> = <shader1> <shader2> ... <shaderN>
 *
 * Copyright (c) 2007-2010 Nathan Keynes.
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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstrfuncs.h>
#include <glib/glist.h>

#define MAX_LINE 4096
#define DEF_ALLOC_SIZE 4096
#define MAX_SHADERS 128

typedef enum {
    VERTEX_SHADER = 0,
    FRAGMENT_SHADER = 1
} shader_type_t;

typedef struct shader {
    shader_type_t type;
    const char *name;
    char *body;
} *shader_t;

typedef struct program {
    const char *name;
    gchar **shader_names;
} *program_t;

typedef struct glsldata {
    const char *filename;
    unsigned max_shaders;
    GList *shaders;
    GList *programs;
} *glsldata_t;

static struct glsldata *readInput( const char *filename )
{
    char buf[MAX_LINE];
    size_t current_size = 0, current_posn = 0;
    unsigned i;

    FILE *f = fopen( filename, "ro" );
    if( f == NULL ) {
        fprintf( stderr, "Error: unable to open input file '%s': %s\n", filename, strerror(errno) );
        exit(1);
    }

    shader_t shader = NULL;
    glsldata_t result = malloc(sizeof(struct glsldata));
    assert( result != NULL );
    result->filename = strdup(filename);
    result->shaders = NULL;
    result->programs = NULL;
    result->max_shaders = 0;

    while( fgets(buf, sizeof(buf), f) != NULL ) {
        if( strlen(buf) == 0 )
            continue;

        if( strncmp(buf, "#vertex ", 8) == 0 ) {
            shader = malloc(sizeof(struct shader));
            assert( shader != NULL );
            shader->type = VERTEX_SHADER;
            shader->name = strdup(g_strstrip(buf+8));
            shader->body = malloc(DEF_ALLOC_SIZE);
            shader->body[0] = '\0';
            current_size = DEF_ALLOC_SIZE;
            current_posn = 0;
            result->shaders = g_list_append(result->shaders, shader);
        } else if( strncmp( buf, "#fragment ", 10 ) == 0 ) {
            shader = malloc(sizeof(struct shader));
            assert( shader != NULL );
            shader->type = FRAGMENT_SHADER;
            shader->name = strdup(g_strstrip(buf+10));
            shader->body = malloc(DEF_ALLOC_SIZE);
            shader->body[0] = '\0';
            current_size = DEF_ALLOC_SIZE;
            current_posn = 0;
            result->shaders = g_list_append(result->shaders, shader);
        } else if( strncmp( buf, "#program ", 9 ) == 0 ) {
            shader = NULL;
            program_t program = malloc(sizeof(struct program));
            char *rest = buf+9;
            char *equals = strchr(rest, '=');
            if( equals == NULL ) {
                fprintf( stderr, "Error: invalid program line %s\n", buf );
                exit(2);
            }
            *equals = '\0';
            program->name = g_strdup(g_strstrip(rest));
            program->shader_names = g_strsplit_set(g_strstrip(equals+1), " \t\r,", 0);
            result->programs = g_list_append(result->programs, program);
            for(i=0;program->shader_names[i] != NULL; i++ );
            if( i > result->max_shaders )
                result->max_shaders = i;
        } else if( shader != NULL ) {
            size_t len = strlen(buf);
            if( current_posn + len > current_size ) {
                shader->body = realloc(shader->body, current_size*2);
                assert( shader->body != NULL );
                current_size *= 2;
            }
            strcpy( shader->body + current_posn, buf );
            current_posn += len;
        }
    }

    fclose(f);
    return result;
}

/**
 * Copy input to output, quoting " characters as we go.
 */
static void writeCString( FILE *out, const char *str )
{
    const char *p = str;

    while( *p != 0 ) {
        if( *p == '\"' ) {
            fputc( '\\', out );
        } else if( *p == '\n' ) {
            fputs( "\\n\\", out );
        }
        fputc( *p, out );
        p++;
    }
}

static void writeHeader( FILE *out, glsldata_t data )
{
    fprintf( out, "/*\n * This file automatically generated by genglsl from %s\n */\n", data->filename );
}

static void writeInterface( const char *filename, glsldata_t data )
{
    FILE *f = fopen(filename, "wo");
    if( f == NULL ) {
        fprintf( stderr, "Error: Unable to write interface file '%s': %s\n", filename, strerror(errno) );
        exit(1);
    }

    writeHeader( f, data );
    fprintf( f, "#ifndef lxdream_glsl_H\n#define lxdream_glsl_H 1\n\n" );

    fprintf( f, "typedef enum {\n" );
    const char *last_name = NULL;
    int count = 0;
    GList *shader_ptr;
    for( shader_ptr = data->shaders; shader_ptr != NULL; shader_ptr = shader_ptr->next ) {
        count++;
        shader_t shader = (shader_t)shader_ptr->data;
        fprintf( f, "    %s,\n", shader->name );
        last_name = shader->name;
    }
    fprintf( f, "} shader_id;\n\n" );

    if( last_name == NULL )
        last_name = "NULL";
    fprintf( f, "#define GLSL_LAST_SHADER %s\n", last_name );
    fprintf( f, "#define GLSL_NUM_SHADERS %d\n", count );
    fprintf( f, "#define GLSL_NO_SHADER -1\n\n" );
    fprintf( f, "#define GLSL_VERTEX_SHADER 1\n" );
    fprintf( f, "#define GLSL_FRAGMENT_SHADER 2\n" );

    fprintf( f, "typedef enum {\n" );
    last_name = NULL;
    count = 0;
    GList *program_ptr;
    for( program_ptr = data->programs; program_ptr != NULL; program_ptr = program_ptr->next ) {
        count++;
        program_t program = (program_t)program_ptr->data;
        fprintf( f, "    %s,\n", program->name );
        last_name = program->name;
    }
    fprintf( f, "} program_id;\n\n" );

    if( last_name == NULL )
        last_name = "NULL";
    fprintf( f, "#define GLSL_LAST_PROGRAM %s\n", last_name );
    fprintf( f, "#define GLSL_NUM_PROGRAMS %d\n", count );
    fprintf( f, "#define GLSL_NO_PROGRAM -1\n\n" );

    fprintf( f, "int glsl_load_programs();\n" );
    fprintf( f, "void glsl_use_program_id( program_id );\n" );

    fprintf( f, "#endif /* !lxdream_glsl_H */\n" );

    fclose(f);
}

static void writeSource( const char *filename, glsldata_t data )
{
    FILE *f = fopen(filename, "wo");
    if( f == NULL ) {
        fprintf( stderr, "Error: Unable to write interface file '%s': %s\n", filename, strerror(errno) );
        exit(1);
    }

    writeHeader( f, data );
    fprintf( f, "struct shader_def {\n    int type;\n    const char *source;\n};\n" );

    fprintf( f, "const struct shader_def shader_source[] = {\n" );
    GList *shader_ptr;
    for( shader_ptr = data->shaders; shader_ptr != NULL; shader_ptr = shader_ptr->next ) {
        shader_t shader = (shader_t)shader_ptr->data;
        fprintf( f, "    {%s,\"", (shader->type == VERTEX_SHADER ? "GLSL_VERTEX_SHADER" : "GLSL_FRAGMENT_SHADER") );
        writeCString( f, shader->body );
        fprintf( f, "\"},\n" );
    }
    fprintf( f, "    {GLSL_NO_SHADER,NULL}};\n\n" );

    fprintf( f, "const int program_list[][%d] = {\n", data->max_shaders+1 );
    GList *program_ptr;
    unsigned i;
    for( program_ptr = data->programs; program_ptr != NULL; program_ptr = program_ptr->next ) {
        program_t program = (program_t)program_ptr->data;
        fprintf( f, "    {" );
        for( i=0; program->shader_names[i] != NULL; i++ ) {
            fprintf(f, "%s,", program->shader_names[i] );
        }
        fprintf( f, "GLSL_NO_SHADER},\n" );
    }
    fprintf( f, "    {GLSL_NO_SHADER}};\n" );

    fclose(f);
}

const char *makeExtension(const char *basename, const char *ext)
{
    const char *oldext = strrchr(basename, '.');
    if( oldext == NULL ) {
        return g_strdup_printf("%s%s", basename, ext);
    } else {
        return g_strdup_printf("%.*s%s", oldext-basename, basename, ext);
    }
}

int main( int argc, char *argv[] )
{
    if( argc < 2 ) {
        fprintf( stderr, "Usage: genglsl <glsl-source-file> [output.c [output.h]]\n");
        exit(1);
    }

    glsldata_t data = readInput(argv[1]);

    const char *sourcefile, *ifacefile;
    if( argc > 2 ) {
        sourcefile = argv[2];
    } else {
        sourcefile = makeExtension(argv[1], ".def");
    }

    if( argc > 3 ) {
        ifacefile = argv[3];
    } else {
        ifacefile = makeExtension(sourcefile, ".h");
    }

    writeSource( sourcefile, data );
    writeInterface( ifacefile, data );
    return 0;
}
