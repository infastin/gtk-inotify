#include "inotify_app.h"
#include "inotify_app_win.h"
#include <gtk/gtk.h>

struct _InotifyApp
{
	GtkApplication parent;
};

G_DEFINE_TYPE(InotifyApp, inotify_app, GTK_TYPE_APPLICATION);

static void inotify_app_init(InotifyApp *app)
{

}

static void inotify_app_activate(GApplication *app)
{
	InotifyAppWindow *win;
	win = inotify_app_window_new(INOTIFY_APP(app));
	gtk_window_present(GTK_WINDOW(win));
}

static void inotify_app_open(GApplication *app, 
		GFile **files,
		int n_files,
		const char *hint)
{
	InotifyAppWindow *win;
	win = inotify_app_window_new(INOTIFY_APP(app));
	gtk_window_present(GTK_WINDOW(win));
}

static void inotify_app_class_init(InotifyAppClass *class)
{
	G_APPLICATION_CLASS(class)->activate = inotify_app_activate;
	G_APPLICATION_CLASS(class)->open = inotify_app_open;
}

InotifyApp* inotify_app_new(void)
{
	return g_object_new(INOTIFY_APP_TYPE, 
			"application-id", "org.gtk.inotifyapp",
			NULL);
}
