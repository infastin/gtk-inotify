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
	GtkCssProvider *css;
	GdkDisplay *display;
	GtkStyleContext *context;

	win = inotify_app_window_new(INOTIFY_APP(app));
	gtk_window_present(GTK_WINDOW(win));

	css = gtk_css_provider_new();
	context = gtk_widget_get_style_context(GTK_WIDGET(win));
	display = gtk_style_context_get_display(context);

	gtk_css_provider_load_from_resource(css, "/org/gtk/inotifyapp/custom.css");
	gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
}

static void inotify_app_open(GApplication *app, 
		GFile **files,
		int n_files,
		const char *hint)
{
	GList *windows;
	InotifyAppWindow *win;
	GtkCssProvider *css;
	GdkDisplay *display;
	GtkStyleContext *context;

	windows = gtk_application_get_windows (GTK_APPLICATION (app));
	if (windows)
		win = INOTIFY_APP_WINDOW(windows->data);
	else
		win = inotify_app_window_new(INOTIFY_APP(app));

	if (files[0])
		inotify_app_window_open(win, files[0]);

	gtk_window_present(GTK_WINDOW(win));

	css = gtk_css_provider_new();
	context = gtk_widget_get_style_context(GTK_WIDGET(win));
	display = gtk_style_context_get_display(context);

	gtk_css_provider_load_from_resource(css, "/org/gtk/inotifyapp/custom.css");
	gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
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
			"flags", G_APPLICATION_HANDLES_OPEN,
			NULL);
}
