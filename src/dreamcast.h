
#ifndef dreamcast_H
#define dreamcast_H 1

#include <stdlib.h>
#include <stdio.h>
#include <glib/gtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

void dreamcast_init(void);
void dreamcast_reset(void);
void dreamcast_stop(void);

#define DREAMCAST_SAVE_MAGIC "%!-DreamOn!Save\0"
#define DREAMCAST_SAVE_VERSION 0x00010000

int dreamcast_save_state( const gchar *filename );
int dreamcast_load_state( const gchar *filename );

int open_file( gchar *filename );


#ifdef __cplusplus
}
#endif

#endif /* !dream_machine_H */
