/*
 * DO NOT EDIT THIS FILE - it is generated by Glade.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <bonobo.h>
#include <gnome.h>

#include "callbacks.h"
#include "interface.h"
#include "support.h"

#define GLADE_HOOKUP_OBJECT(component,widget,name) \
  g_object_set_data_full (G_OBJECT (component), name, \
    gtk_widget_ref (widget), (GDestroyNotify) gtk_widget_unref)

#define GLADE_HOOKUP_OBJECT_NO_REF(component,widget,name) \
  g_object_set_data (G_OBJECT (component), name, widget)

static GnomeUIInfo file1_menu_uiinfo[] =
{
  {
    GNOME_APP_UI_ITEM, N_("gtk-new"),
    NULL,
    (gpointer) on_new_file1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  {
    GNOME_APP_UI_ITEM, N_("gtk-open"),
    NULL,
    (gpointer) on_open1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  {
    GNOME_APP_UI_ITEM, N_("gtk-save"),
    NULL,
    (gpointer) on_save1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  {
    GNOME_APP_UI_ITEM, N_("gtk-save-as"),
    NULL,
    (gpointer) on_save_as1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  GNOMEUIINFO_SEPARATOR,
  {
    GNOME_APP_UI_ITEM, N_("gtk-quit"),
    NULL,
    (gpointer) on_exit1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  GNOMEUIINFO_END
};

static GnomeUIInfo view1_menu_uiinfo[] =
{
  {
    GNOME_APP_UI_ITEM, N_("Mem mapped Regs"),
    NULL,
    (gpointer) on_mem_mapped_regs1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  GNOMEUIINFO_END
};

static GnomeUIInfo settings1_menu_uiinfo[] =
{
  {
    GNOME_APP_UI_ITEM, N_("gtk-preferences"),
    NULL,
    (gpointer) on_preferences1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  GNOMEUIINFO_END
};

static GnomeUIInfo help1_menu_uiinfo[] =
{
  {
    GNOME_APP_UI_ITEM, N_("gnome-stock-about"),
    NULL,
    (gpointer) on_about1_activate, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  GNOMEUIINFO_END
};

static GnomeUIInfo menubar1_uiinfo[] =
{
  {
    GNOME_APP_UI_SUBTREE, N_("_File"),
    NULL,
    file1_menu_uiinfo, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  {
    GNOME_APP_UI_SUBTREE, N_("_View"),
    NULL,
    view1_menu_uiinfo, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  {
    GNOME_APP_UI_SUBTREE, N_("_Settings"),
    NULL,
    settings1_menu_uiinfo, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  {
    GNOME_APP_UI_SUBTREE, N_("_Help"),
    NULL,
    help1_menu_uiinfo, NULL, NULL,
    GNOME_APP_PIXMAP_NONE, NULL,
    0, (GdkModifierType) 0, NULL
  },
  GNOMEUIINFO_END
};

GtkWidget*
create_debug_win (void)
{
  GtkWidget *debug_win;
  GtkWidget *dock1;
  GtkWidget *toolbar1;
  GtkWidget *tmp_toolbar_icon;
  GtkWidget *load_btn;
  GtkWidget *reset_btn;
  GtkWidget *stop_btn;
  GtkWidget *step_btn;
  GtkWidget *run_btn;
  GtkWidget *runto_btn;
  GtkWidget *break_btn;
  GtkWidget *vpaned1;
  GtkWidget *hpaned1;
  GtkWidget *vbox2;
  GtkWidget *hbox1;
  GtkWidget *label21;
  GtkWidget *page_field;
  GtkWidget *page_locked_btn;
  GtkWidget *jump_pc_btn;
  GtkWidget *label22;
  GtkWidget *mode_box;
  GList *mode_box_items = NULL;
  GtkWidget *mode_field;
  GtkWidget *label23;
  GtkWidget *scrolledwindow7;
  GtkWidget *disasm_list;
  GtkWidget *label24;
  GtkWidget *label27;
  GtkWidget *label28;
  GtkWidget *label29;
  GtkWidget *scrolledwindow4;
  GtkWidget *reg_list;
  GtkWidget *label25;
  GtkWidget *label30;
  GtkWidget *scrolledwindow8;
  GtkWidget *output_list;
  GtkWidget *label26;
  GtkWidget *label31;
  GtkWidget *label32;
  GtkWidget *debug_appbar;

  debug_win = gnome_app_new ("Dream2", _("DreamOn! Debugger"));
  gtk_window_set_default_size (GTK_WINDOW (debug_win), 1000, 700);

  dock1 = GNOME_APP (debug_win)->dock;
  gtk_widget_show (dock1);

  gnome_app_create_menus (GNOME_APP (debug_win), menubar1_uiinfo);

  toolbar1 = gtk_toolbar_new ();
  gtk_widget_show (toolbar1);
  gnome_app_add_toolbar (GNOME_APP (debug_win), GTK_TOOLBAR (toolbar1), "toolbar1",
                                BONOBO_DOCK_ITEM_BEH_EXCLUSIVE,
                                BONOBO_DOCK_TOP, 1, 0, 0);
  gtk_container_set_border_width (GTK_CONTAINER (toolbar1), 1);
  gtk_toolbar_set_style (GTK_TOOLBAR (toolbar1), GTK_TOOLBAR_BOTH);

  tmp_toolbar_icon = gtk_image_new_from_stock ("gtk-open", gtk_toolbar_get_icon_size (GTK_TOOLBAR (toolbar1)));
  load_btn = gtk_toolbar_append_element (GTK_TOOLBAR (toolbar1),
                                GTK_TOOLBAR_CHILD_BUTTON,
                                NULL,
                                _("Load"),
                                _("New File"), NULL,
                                tmp_toolbar_icon, NULL, NULL);
  gtk_label_set_use_underline (GTK_LABEL (((GtkToolbarChild*) (g_list_last (GTK_TOOLBAR (toolbar1)->children)->data))->label), TRUE);
  gtk_widget_show (load_btn);

  tmp_toolbar_icon = gtk_image_new_from_stock ("gtk-refresh", gtk_toolbar_get_icon_size (GTK_TOOLBAR (toolbar1)));
  reset_btn = gtk_toolbar_append_element (GTK_TOOLBAR (toolbar1),
                                GTK_TOOLBAR_CHILD_BUTTON,
                                NULL,
                                _("Reset"),
                                _("Open File"), NULL,
                                tmp_toolbar_icon, NULL, NULL);
  gtk_label_set_use_underline (GTK_LABEL (((GtkToolbarChild*) (g_list_last (GTK_TOOLBAR (toolbar1)->children)->data))->label), TRUE);
  gtk_widget_show (reset_btn);

  tmp_toolbar_icon = gtk_image_new_from_stock ("gtk-stop", gtk_toolbar_get_icon_size (GTK_TOOLBAR (toolbar1)));
  stop_btn = gtk_toolbar_append_element (GTK_TOOLBAR (toolbar1),
                                GTK_TOOLBAR_CHILD_BUTTON,
                                NULL,
                                _("Stop"),
                                _("Save File"), NULL,
                                tmp_toolbar_icon, NULL, NULL);
  gtk_label_set_use_underline (GTK_LABEL (((GtkToolbarChild*) (g_list_last (GTK_TOOLBAR (toolbar1)->children)->data))->label), TRUE);
  gtk_widget_show (stop_btn);

  tmp_toolbar_icon = gtk_image_new_from_stock ("gtk-redo", gtk_toolbar_get_icon_size (GTK_TOOLBAR (toolbar1)));
  step_btn = gtk_toolbar_append_element (GTK_TOOLBAR (toolbar1),
                                GTK_TOOLBAR_CHILD_BUTTON,
                                NULL,
                                _("Step"),
                                NULL, NULL,
                                tmp_toolbar_icon, NULL, NULL);
  gtk_label_set_use_underline (GTK_LABEL (((GtkToolbarChild*) (g_list_last (GTK_TOOLBAR (toolbar1)->children)->data))->label), TRUE);
  gtk_widget_show (step_btn);

  tmp_toolbar_icon = gtk_image_new_from_stock ("gtk-go-forward", gtk_toolbar_get_icon_size (GTK_TOOLBAR (toolbar1)));
  run_btn = gtk_toolbar_append_element (GTK_TOOLBAR (toolbar1),
                                GTK_TOOLBAR_CHILD_BUTTON,
                                NULL,
                                _("Run"),
                                NULL, NULL,
                                tmp_toolbar_icon, NULL, NULL);
  gtk_label_set_use_underline (GTK_LABEL (((GtkToolbarChild*) (g_list_last (GTK_TOOLBAR (toolbar1)->children)->data))->label), TRUE);
  gtk_widget_show (run_btn);

  tmp_toolbar_icon = gtk_image_new_from_stock ("gtk-goto-last", gtk_toolbar_get_icon_size (GTK_TOOLBAR (toolbar1)));
  runto_btn = gtk_toolbar_append_element (GTK_TOOLBAR (toolbar1),
                                GTK_TOOLBAR_CHILD_BUTTON,
                                NULL,
                                _("Run to"),
                                NULL, NULL,
                                tmp_toolbar_icon, NULL, NULL);
  gtk_label_set_use_underline (GTK_LABEL (((GtkToolbarChild*) (g_list_last (GTK_TOOLBAR (toolbar1)->children)->data))->label), TRUE);
  gtk_widget_show (runto_btn);

  tmp_toolbar_icon = gtk_image_new_from_stock ("gtk-close", gtk_toolbar_get_icon_size (GTK_TOOLBAR (toolbar1)));
  break_btn = gtk_toolbar_append_element (GTK_TOOLBAR (toolbar1),
                                GTK_TOOLBAR_CHILD_BUTTON,
                                NULL,
                                _("Break"),
                                NULL, NULL,
                                tmp_toolbar_icon, NULL, NULL);
  gtk_label_set_use_underline (GTK_LABEL (((GtkToolbarChild*) (g_list_last (GTK_TOOLBAR (toolbar1)->children)->data))->label), TRUE);
  gtk_widget_show (break_btn);

  vpaned1 = gtk_vpaned_new ();
  gtk_widget_show (vpaned1);
  gnome_app_set_contents (GNOME_APP (debug_win), vpaned1);
  gtk_paned_set_position (GTK_PANED (vpaned1), 430);

  hpaned1 = gtk_hpaned_new ();
  gtk_widget_show (hpaned1);
  gtk_paned_pack1 (GTK_PANED (vpaned1), hpaned1, TRUE, TRUE);
  gtk_paned_set_position (GTK_PANED (hpaned1), 800);

  vbox2 = gtk_vbox_new (FALSE, 0);
  gtk_widget_show (vbox2);
  gtk_paned_pack1 (GTK_PANED (hpaned1), vbox2, FALSE, TRUE);

  hbox1 = gtk_hbox_new (FALSE, 0);
  gtk_widget_show (hbox1);
  gtk_box_pack_start (GTK_BOX (vbox2), hbox1, FALSE, FALSE, 3);

  label21 = gtk_label_new (_("Page"));
  gtk_widget_show (label21);
  gtk_box_pack_start (GTK_BOX (hbox1), label21, FALSE, FALSE, 4);
  gtk_label_set_justify (GTK_LABEL (label21), GTK_JUSTIFY_CENTER);

  page_field = gtk_entry_new ();
  gtk_widget_show (page_field);
  gtk_box_pack_start (GTK_BOX (hbox1), page_field, FALSE, TRUE, 0);

  page_locked_btn = gtk_check_button_new_with_mnemonic (_("Locked"));
  gtk_widget_show (page_locked_btn);
  gtk_box_pack_start (GTK_BOX (hbox1), page_locked_btn, FALSE, FALSE, 5);

  jump_pc_btn = gtk_button_new_with_mnemonic (_(" Jump to PC "));
  gtk_widget_show (jump_pc_btn);
  gtk_box_pack_start (GTK_BOX (hbox1), jump_pc_btn, FALSE, FALSE, 4);

  label22 = gtk_label_new (_("Mode"));
  gtk_widget_show (label22);
  gtk_box_pack_start (GTK_BOX (hbox1), label22, FALSE, FALSE, 5);
  gtk_label_set_justify (GTK_LABEL (label22), GTK_JUSTIFY_RIGHT);
  gtk_misc_set_alignment (GTK_MISC (label22), 1, 0.5);

  mode_box = gtk_combo_new ();
  g_object_set_data (G_OBJECT (GTK_COMBO (mode_box)->popwin),
                     "GladeParentKey", mode_box);
  gtk_widget_show (mode_box);
  gtk_box_pack_start (GTK_BOX (hbox1), mode_box, FALSE, FALSE, 0);
  mode_box_items = g_list_append (mode_box_items, (gpointer) "");
  gtk_combo_set_popdown_strings (GTK_COMBO (mode_box), mode_box_items);
  g_list_free (mode_box_items);

  mode_field = GTK_COMBO (mode_box)->entry;
  gtk_widget_show (mode_field);

  label23 = gtk_label_new ("");
  gtk_widget_show (label23);
  gtk_box_pack_start (GTK_BOX (hbox1), label23, TRUE, TRUE, 4);
  gtk_label_set_justify (GTK_LABEL (label23), GTK_JUSTIFY_CENTER);

  scrolledwindow7 = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_show (scrolledwindow7);
  gtk_box_pack_start (GTK_BOX (vbox2), scrolledwindow7, TRUE, TRUE, 0);
  GTK_WIDGET_UNSET_FLAGS (scrolledwindow7, GTK_CAN_FOCUS);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolledwindow7), GTK_SHADOW_IN);

  disasm_list = gtk_clist_new (4);
  gtk_widget_show (disasm_list);
  gtk_container_add (GTK_CONTAINER (scrolledwindow7), disasm_list);
  gtk_clist_set_column_width (GTK_CLIST (disasm_list), 0, 80);
  gtk_clist_set_column_width (GTK_CLIST (disasm_list), 1, 80);
  gtk_clist_set_column_width (GTK_CLIST (disasm_list), 2, 80);
  gtk_clist_set_column_width (GTK_CLIST (disasm_list), 3, 80);
  gtk_clist_column_titles_hide (GTK_CLIST (disasm_list));

  label24 = gtk_label_new ("");
  gtk_widget_show (label24);
  gtk_clist_set_column_widget (GTK_CLIST (disasm_list), 0, label24);

  label27 = gtk_label_new ("");
  gtk_widget_show (label27);
  gtk_clist_set_column_widget (GTK_CLIST (disasm_list), 1, label27);

  label28 = gtk_label_new ("");
  gtk_widget_show (label28);
  gtk_clist_set_column_widget (GTK_CLIST (disasm_list), 2, label28);

  label29 = gtk_label_new ("");
  gtk_widget_show (label29);
  gtk_clist_set_column_widget (GTK_CLIST (disasm_list), 3, label29);

  scrolledwindow4 = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_show (scrolledwindow4);
  gtk_paned_pack2 (GTK_PANED (hpaned1), scrolledwindow4, FALSE, TRUE);
  GTK_WIDGET_UNSET_FLAGS (scrolledwindow4, GTK_CAN_FOCUS);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow4), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolledwindow4), GTK_SHADOW_IN);

  reg_list = gtk_clist_new (2);
  gtk_widget_show (reg_list);
  gtk_container_add (GTK_CONTAINER (scrolledwindow4), reg_list);
  gtk_clist_set_column_width (GTK_CLIST (reg_list), 0, 80);
  gtk_clist_set_column_width (GTK_CLIST (reg_list), 1, 80);
  gtk_clist_column_titles_hide (GTK_CLIST (reg_list));

  label25 = gtk_label_new ("");
  gtk_widget_show (label25);
  gtk_clist_set_column_widget (GTK_CLIST (reg_list), 0, label25);

  label30 = gtk_label_new ("");
  gtk_widget_show (label30);
  gtk_clist_set_column_widget (GTK_CLIST (reg_list), 1, label30);

  scrolledwindow8 = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_show (scrolledwindow8);
  gtk_paned_pack2 (GTK_PANED (vpaned1), scrolledwindow8, FALSE, TRUE);
  GTK_WIDGET_UNSET_FLAGS (scrolledwindow8, GTK_CAN_FOCUS);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow8), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolledwindow8), GTK_SHADOW_IN);

  output_list = gtk_clist_new (3);
  gtk_widget_show (output_list);
  gtk_container_add (GTK_CONTAINER (scrolledwindow8), output_list);
  gtk_clist_set_column_width (GTK_CLIST (output_list), 0, 80);
  gtk_clist_set_column_width (GTK_CLIST (output_list), 1, 80);
  gtk_clist_set_column_width (GTK_CLIST (output_list), 2, 80);
  gtk_clist_column_titles_hide (GTK_CLIST (output_list));

  label26 = gtk_label_new ("");
  gtk_widget_show (label26);
  gtk_clist_set_column_widget (GTK_CLIST (output_list), 0, label26);

  label31 = gtk_label_new ("");
  gtk_widget_show (label31);
  gtk_clist_set_column_widget (GTK_CLIST (output_list), 1, label31);

  label32 = gtk_label_new ("");
  gtk_widget_show (label32);
  gtk_clist_set_column_widget (GTK_CLIST (output_list), 2, label32);

  debug_appbar = gnome_appbar_new (TRUE, TRUE, GNOME_PREFERENCES_NEVER);
  gtk_widget_show (debug_appbar);
  gnome_app_set_statusbar (GNOME_APP (debug_win), debug_appbar);

  g_signal_connect ((gpointer) debug_win, "delete_event",
                    G_CALLBACK (on_debug_win_delete_event),
                    NULL);
  gnome_app_install_menu_hints (GNOME_APP (debug_win), menubar1_uiinfo);
  g_signal_connect ((gpointer) load_btn, "clicked",
                    G_CALLBACK (on_load_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) reset_btn, "clicked",
                    G_CALLBACK (on_reset_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) stop_btn, "clicked",
                    G_CALLBACK (on_stop_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) step_btn, "clicked",
                    G_CALLBACK (on_step_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) run_btn, "clicked",
                    G_CALLBACK (on_run_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) runto_btn, "clicked",
                    G_CALLBACK (on_runto_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) break_btn, "clicked",
                    G_CALLBACK (on_break_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) page_field, "key_press_event",
                    G_CALLBACK (on_page_field_key_press_event),
                    NULL);
  g_signal_connect ((gpointer) page_locked_btn, "toggled",
                    G_CALLBACK (on_page_locked_btn_toggled),
                    NULL);
  g_signal_connect ((gpointer) jump_pc_btn, "clicked",
                    G_CALLBACK (on_jump_pc_btn_clicked),
                    NULL);
  g_signal_connect ((gpointer) mode_field, "changed",
                    G_CALLBACK (on_mode_field_changed),
                    NULL);
  g_signal_connect ((gpointer) disasm_list, "select_row",
                    G_CALLBACK (on_disasm_list_select_row),
                    NULL);
  g_signal_connect ((gpointer) disasm_list, "unselect_row",
                    G_CALLBACK (on_disasm_list_unselect_row),
                    NULL);
  g_signal_connect ((gpointer) output_list, "select_row",
                    G_CALLBACK (on_output_list_select_row),
                    NULL);

  /* Store pointers to all widgets, for use by lookup_widget(). */
  GLADE_HOOKUP_OBJECT_NO_REF (debug_win, debug_win, "debug_win");
  GLADE_HOOKUP_OBJECT (debug_win, dock1, "dock1");
  GLADE_HOOKUP_OBJECT (debug_win, menubar1_uiinfo[0].widget, "file1");
  GLADE_HOOKUP_OBJECT (debug_win, file1_menu_uiinfo[0].widget, "new_file1");
  GLADE_HOOKUP_OBJECT (debug_win, file1_menu_uiinfo[1].widget, "open1");
  GLADE_HOOKUP_OBJECT (debug_win, file1_menu_uiinfo[2].widget, "save1");
  GLADE_HOOKUP_OBJECT (debug_win, file1_menu_uiinfo[3].widget, "save_as1");
  GLADE_HOOKUP_OBJECT (debug_win, file1_menu_uiinfo[4].widget, "separator1");
  GLADE_HOOKUP_OBJECT (debug_win, file1_menu_uiinfo[5].widget, "exit1");
  GLADE_HOOKUP_OBJECT (debug_win, menubar1_uiinfo[1].widget, "view1");
  GLADE_HOOKUP_OBJECT (debug_win, view1_menu_uiinfo[0].widget, "mem_mapped_regs1");
  GLADE_HOOKUP_OBJECT (debug_win, menubar1_uiinfo[2].widget, "settings1");
  GLADE_HOOKUP_OBJECT (debug_win, settings1_menu_uiinfo[0].widget, "preferences1");
  GLADE_HOOKUP_OBJECT (debug_win, menubar1_uiinfo[3].widget, "help1");
  GLADE_HOOKUP_OBJECT (debug_win, help1_menu_uiinfo[0].widget, "about1");
  GLADE_HOOKUP_OBJECT (debug_win, toolbar1, "toolbar1");
  GLADE_HOOKUP_OBJECT (debug_win, load_btn, "load_btn");
  GLADE_HOOKUP_OBJECT (debug_win, reset_btn, "reset_btn");
  GLADE_HOOKUP_OBJECT (debug_win, stop_btn, "stop_btn");
  GLADE_HOOKUP_OBJECT (debug_win, step_btn, "step_btn");
  GLADE_HOOKUP_OBJECT (debug_win, run_btn, "run_btn");
  GLADE_HOOKUP_OBJECT (debug_win, runto_btn, "runto_btn");
  GLADE_HOOKUP_OBJECT (debug_win, break_btn, "break_btn");
  GLADE_HOOKUP_OBJECT (debug_win, vpaned1, "vpaned1");
  GLADE_HOOKUP_OBJECT (debug_win, hpaned1, "hpaned1");
  GLADE_HOOKUP_OBJECT (debug_win, vbox2, "vbox2");
  GLADE_HOOKUP_OBJECT (debug_win, hbox1, "hbox1");
  GLADE_HOOKUP_OBJECT (debug_win, label21, "label21");
  GLADE_HOOKUP_OBJECT (debug_win, page_field, "page_field");
  GLADE_HOOKUP_OBJECT (debug_win, page_locked_btn, "page_locked_btn");
  GLADE_HOOKUP_OBJECT (debug_win, jump_pc_btn, "jump_pc_btn");
  GLADE_HOOKUP_OBJECT (debug_win, label22, "label22");
  GLADE_HOOKUP_OBJECT (debug_win, mode_box, "mode_box");
  GLADE_HOOKUP_OBJECT (debug_win, mode_field, "mode_field");
  GLADE_HOOKUP_OBJECT (debug_win, label23, "label23");
  GLADE_HOOKUP_OBJECT (debug_win, scrolledwindow7, "scrolledwindow7");
  GLADE_HOOKUP_OBJECT (debug_win, disasm_list, "disasm_list");
  GLADE_HOOKUP_OBJECT (debug_win, label24, "label24");
  GLADE_HOOKUP_OBJECT (debug_win, label27, "label27");
  GLADE_HOOKUP_OBJECT (debug_win, label28, "label28");
  GLADE_HOOKUP_OBJECT (debug_win, label29, "label29");
  GLADE_HOOKUP_OBJECT (debug_win, scrolledwindow4, "scrolledwindow4");
  GLADE_HOOKUP_OBJECT (debug_win, reg_list, "reg_list");
  GLADE_HOOKUP_OBJECT (debug_win, label25, "label25");
  GLADE_HOOKUP_OBJECT (debug_win, label30, "label30");
  GLADE_HOOKUP_OBJECT (debug_win, scrolledwindow8, "scrolledwindow8");
  GLADE_HOOKUP_OBJECT (debug_win, output_list, "output_list");
  GLADE_HOOKUP_OBJECT (debug_win, label26, "label26");
  GLADE_HOOKUP_OBJECT (debug_win, label31, "label31");
  GLADE_HOOKUP_OBJECT (debug_win, label32, "label32");
  GLADE_HOOKUP_OBJECT (debug_win, debug_appbar, "debug_appbar");

  return debug_win;
}

GtkWidget*
create_mmr_win (void)
{
  GtkWidget *mmr_win;
  GtkWidget *vbox1;
  GtkWidget *mmr_notebook;
  GtkWidget *empty_notebook_page;
  GtkWidget *label16;
  GtkWidget *hbuttonbox1;
  GtkWidget *mmr_close;

  mmr_win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (mmr_win), _("DreamOn! - Memory-Mapped Registers"));
  gtk_window_set_default_size (GTK_WINDOW (mmr_win), 600, 600);

  vbox1 = gtk_vbox_new (FALSE, 0);
  gtk_widget_show (vbox1);
  gtk_container_add (GTK_CONTAINER (mmr_win), vbox1);

  mmr_notebook = gtk_notebook_new ();
  gtk_widget_show (mmr_notebook);
  gtk_box_pack_start (GTK_BOX (vbox1), mmr_notebook, TRUE, TRUE, 0);
  gtk_notebook_set_tab_pos (GTK_NOTEBOOK (mmr_notebook), GTK_POS_LEFT);

  empty_notebook_page = gtk_vbox_new (FALSE, 0);
  gtk_widget_show (empty_notebook_page);
  gtk_container_add (GTK_CONTAINER (mmr_notebook), empty_notebook_page);

  label16 = gtk_label_new (_("label16"));
  gtk_widget_show (label16);
  gtk_notebook_set_tab_label (GTK_NOTEBOOK (mmr_notebook), gtk_notebook_get_nth_page (GTK_NOTEBOOK (mmr_notebook), 0), label16);
  gtk_label_set_justify (GTK_LABEL (label16), GTK_JUSTIFY_CENTER);

  hbuttonbox1 = gtk_hbutton_box_new ();
  gtk_widget_show (hbuttonbox1);
  gtk_box_pack_start (GTK_BOX (vbox1), hbuttonbox1, FALSE, TRUE, 0);
  gtk_box_set_spacing (GTK_BOX (hbuttonbox1), 30);

  mmr_close = gtk_button_new_with_mnemonic (_("Close"));
  gtk_widget_show (mmr_close);
  gtk_container_add (GTK_CONTAINER (hbuttonbox1), mmr_close);
  GTK_WIDGET_SET_FLAGS (mmr_close, GTK_CAN_DEFAULT);

  g_signal_connect ((gpointer) mmr_win, "delete_event",
                    G_CALLBACK (on_mmr_win_delete_event),
                    NULL);
  g_signal_connect ((gpointer) mmr_close, "clicked",
                    G_CALLBACK (on_mmr_close_clicked),
                    NULL);

  /* Store pointers to all widgets, for use by lookup_widget(). */
  GLADE_HOOKUP_OBJECT_NO_REF (mmr_win, mmr_win, "mmr_win");
  GLADE_HOOKUP_OBJECT (mmr_win, vbox1, "vbox1");
  GLADE_HOOKUP_OBJECT (mmr_win, mmr_notebook, "mmr_notebook");
  GLADE_HOOKUP_OBJECT (mmr_win, label16, "label16");
  GLADE_HOOKUP_OBJECT (mmr_win, hbuttonbox1, "hbuttonbox1");
  GLADE_HOOKUP_OBJECT (mmr_win, mmr_close, "mmr_close");

  return mmr_win;
}

GtkWidget*
create_about_win (void)
{
  const gchar *authors[] = {
    "Nathan Keynes",
    NULL
  };
  const gchar *documenters[] = { NULL };
  /* TRANSLATORS: Replace this string with your names, one name per line. */
  gchar *translators = _("translator_credits");
  GdkPixbuf *about_win_logo_pixbuf;
  GtkWidget *about_win;

  if (!strcmp (translators, "translator_credits"))
    translators = NULL;
  about_win_logo_pixbuf = create_pixbuf ("dream/dcemu.gif");
  about_win = gnome_about_new ("Dream2", VERSION,
                        _("Copyright(c) 2002 Nathan Keynes"),
                        "",
                        authors,
                        documenters,
                        translators,
                        about_win_logo_pixbuf);
  gtk_container_set_border_width (GTK_CONTAINER (about_win), 1);

  /* Store pointers to all widgets, for use by lookup_widget(). */
  GLADE_HOOKUP_OBJECT_NO_REF (about_win, about_win, "about_win");

  return about_win;
}

