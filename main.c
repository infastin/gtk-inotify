#include <gtk/gtk.h>
#include <stdio.h>
#include "inotify_app.h"

int main(int argc, char *argv[])
{
	return g_application_run(G_APPLICATION(inotify_app_new()), argc, argv);
}
