#ifndef INOTIFY_APP_H
#define INOTIFY_APP_H

#include <gtk/gtk.h>

#define INOTIFY_APP_TYPE (inotify_app_get_type())
G_DECLARE_FINAL_TYPE(InotifyApp, inotify_app, INOTIFY, APP, GtkApplication)

InotifyApp *inotify_app_new(void);

#endif /* end of include guard: INOTIFY_APP_H */
