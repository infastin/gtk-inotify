#ifndef INOTIFY_APP_WIN_H_GU1TARQN
#define INOTIFY_APP_WIN_H_GU1TARQN

#include <gtk/gtk.h>
#include "inotify_app.h"

#define INOTIFY_APP_WINDOW_TYPE (inotify_app_window_get_type())
G_DECLARE_FINAL_TYPE(InotifyAppWindow, inotify_app_window, INOTIFY, APP_WINDOW, GtkApplicationWindow)

InotifyAppWindow *inotify_app_window_new(InotifyApp *app);
void inotify_app_window_open(InotifyAppWindow *win, GFile *file);

#endif /* end of include guard: INOTIFY_APP_WIN_H_GU1TARQN */
