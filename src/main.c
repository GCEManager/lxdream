/*
 * Initial main.c file generated by Glade. Edit as required.
 * Glade will not overwrite this file.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gnome.h>

#include "interface.h"
#include "gui.h"
#include "sh4core.h"


int
main (int argc, char *argv[])
{
  GtkWidget *debug_win;

#ifdef ENABLE_NLS
  bindtextdomain (PACKAGE, PACKAGE_LOCALE_DIR);
  textdomain (PACKAGE);
#endif
  gnome_init ("dreamon", VERSION, argc, argv);

  debug_win = create_debug_win ();
  init_debug_win(debug_win);
  video_open();
  machine_init();
  init_mmr_win(); /* Note: must be done after sh4_init */
  sh4_reset();
  update_registers();
  gtk_widget_show (debug_win);
  set_disassembly_region( 0xA0000000 );
  emit( EMIT_INFO, -1, "DreamOn! ready..." );

  gtk_main ();
  return 0;
}

