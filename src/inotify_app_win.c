/* vim: set fdm=marker : */

#include <errno.h>
#include <gtk/gtk.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <unistd.h>

#include "inotify_app.h"
#include "inotify_app_win.h"

/* Definitions {{{ */

struct _InotifyAppWindow
{
	GtkApplicationWindow parent;
	GtkWidget *directory_choose;
	GtkWidget *directory_choose_entry;
	GtkWidget *list;
	GtkWidget *listening;
	GtkWidget *status_bar;
	GtkWidget *status_bar_entries;
	GtkWidget *status_bar_listening_image;
	GtkWidget *status_bar_listening_status;
	GtkWidget *status_bar_clear;
	GtkWidget *status_bar_err;
};

G_DEFINE_TYPE(InotifyAppWindow, inotify_app_window, GTK_TYPE_APPLICATION_WINDOW);

/* }}} */

/* Choose directory {{{ */

static void on_response(GtkNativeDialog *native, int response, gpointer data)
{
	if (response == GTK_RESPONSE_ACCEPT)
	{
		GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);
		GFile *file = gtk_file_chooser_get_file(chooser);
		
		GtkEntry *entry = GTK_ENTRY(data);
		GtkEntryBuffer *buffer = gtk_entry_get_buffer(entry);
		char *filepath = g_file_get_path(file);

		gtk_entry_buffer_set_text(buffer, filepath, -1);

		g_object_unref(file);
	}

	g_object_unref(native);
}

static void directory_choose_clicked(GtkButton *button,
		gpointer data)
{
	InotifyAppWindow *win;
	GtkFileChooserNative *native;
	GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;

	win = INOTIFY_APP_WINDOW(data);
	native = gtk_file_chooser_native_new("Choose directory", GTK_WINDOW(win), action, "_Open", "_Cancel");

	g_signal_connect(native, "response", G_CALLBACK(on_response), win->directory_choose_entry);
	gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));

	if ((gtk_widget_get_visible(win->status_bar_err)) == TRUE)
		gtk_widget_set_visible(win->status_bar_err, FALSE);
}

/* }}} */

/* Listening {{{ */

#define event_case(str, mask, ev) if (mask & ev) g_string_append(str, #ev ": ");

struct ListenerData
{
	GtkWidget *win;
	const char *dir;
};

struct ListenerThread
{
	GThread *thread;
	int running;
	int close;
};

static struct ListenerThread *lt;

static int handle_events(int fd, int wd, gpointer data)
{
	char buf[4096] __attribute ((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *event;
	ssize_t len;
	GString *str;

	if (lt->close == 1)
		return 0;

	struct ListenerData *ld = data;
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(ld->win);

	len = read(fd, buf, sizeof(buf));
	if (len == -1 && errno != EAGAIN)
	{
		char *error = g_strdup_printf("perror: %s", strerror(errno));
		gtk_label_set_text(GTK_LABEL(win->status_bar_err), error);

		if ((gtk_widget_get_visible(win->status_bar_err)) == FALSE)
			gtk_widget_set_visible(win->status_bar_err, TRUE);

		g_free(error);

		return 0;
	}

	if (len == -1)
		return 1;

	str = g_string_new(NULL);

	for (char *ptr = buf; ptr < buf + len;
			ptr += sizeof(struct inotify_event) + event->len)
	{
		if (lt->close == 1)
			break;

		event = (const struct inotify_event*) ptr;

		event_case(str, event->mask, IN_OPEN);
		event_case(str, event->mask, IN_CLOSE_NOWRITE);
		event_case(str, event->mask, IN_CLOSE_WRITE);
		event_case(str, event->mask, IN_MOVED_FROM);
		event_case(str, event->mask, IN_MOVED_TO);
		event_case(str, event->mask, IN_DELETE);
		event_case(str, event->mask, IN_DELETE_SELF);
		event_case(str, event->mask, IN_MODIFY);
		event_case(str, event->mask, IN_MOVE_SELF);
		event_case(str, event->mask, IN_CREATE);

		g_string_append_printf(str, "%s/", ld->dir);

		if (event->len)
			g_string_append_printf(str, "%s", event->name);

		GtkWidget *img;

		if (event->mask & IN_ISDIR)
			img = gtk_image_new_from_icon_name("gtk-directory");
		else
			img = gtk_image_new_from_icon_name("gtk-file");

		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		
		GtkWidget *label = gtk_label_new(str->str);
		gtk_label_set_selectable(GTK_LABEL(label), TRUE);
		
		gtk_box_append(GTK_BOX(box), img);
		gtk_box_append(GTK_BOX(box), label);

		gtk_list_box_append(GTK_LIST_BOX(win->list), box);
		g_string_erase(str, 0, -1);

		int entries = atoi(gtk_label_get_text(GTK_LABEL(win->status_bar_entries))) + 1;
		char *entries_str = g_strdup_printf("%d", entries);
		gtk_label_set_text(GTK_LABEL(win->status_bar_entries), entries_str);
		g_free(entries_str);

		if ((gtk_widget_get_sensitive(win->status_bar_clear)) == FALSE)
			gtk_widget_set_sensitive(win->status_bar_clear, TRUE);

		if (event->mask & IN_DELETE_SELF)
		{
			gtk_label_set_text(GTK_LABEL(win->status_bar_err), "Listening directory was deleted!");

			if ((gtk_widget_get_visible(win->status_bar_err)) == FALSE)
				gtk_widget_set_visible(win->status_bar_err, TRUE);

			g_string_free(str, TRUE);
			return 0;
		}

		if (event->mask & IN_MOVE_SELF)
		{
			gtk_label_set_text(GTK_LABEL(win->status_bar_err), "Listening directory was moved!");

			if ((gtk_widget_get_visible(win->status_bar_err)) == FALSE)
				gtk_widget_set_visible(win->status_bar_err, TRUE);

			g_string_free(str, TRUE);
			return 0;
		}
	}

	g_string_free(str, TRUE);
	return 1;
}

static gboolean worker_finish_in_idle(gpointer data)
{
	struct ListenerData *ld = data;
	g_free(ld);
	return FALSE;
}

static gpointer worker(gpointer data)
{
	int fd, wd;

	fd = inotify_init1(IN_NONBLOCK);
	
	struct ListenerData *ld = data;
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(ld->win);

	if (fd == -1)
	{
		char *error = g_strdup_printf("inotify_init1: %s", strerror(errno));
		gtk_label_set_text(GTK_LABEL(win->status_bar_err), error);

		if ((gtk_widget_get_visible(win->status_bar_err)) == FALSE)
			gtk_widget_set_visible(win->status_bar_err, TRUE);

		g_free(error);

		lt->running = 0;
		g_idle_add(worker_finish_in_idle, ld);
		return NULL;
	}

	wd = inotify_add_watch(fd, ld->dir, 
			IN_OPEN | IN_CLOSE | IN_MOVE | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF);

	if (wd == -1)
	{
		char *error = g_strdup_printf("Can't watch '%s': %s", ld->dir, strerror(errno));
		gtk_label_set_text(GTK_LABEL(win->status_bar_err), error);

		if ((gtk_widget_get_visible(win->status_bar_err)) == FALSE)
			gtk_widget_set_visible(win->status_bar_err, TRUE);

		g_free(error);
		close(fd);

		lt->running = 0;
		g_idle_add(worker_finish_in_idle, ld);
		return NULL;
	}

	gtk_button_set_label(GTK_BUTTON(win->listening), "Stop listening");
	gtk_widget_set_sensitive(win->directory_choose, FALSE);
	gtk_widget_set_sensitive(win->directory_choose_entry, FALSE);
	gtk_label_set_text(GTK_LABEL(win->status_bar_listening_status), "Listening...");
	gtk_image_set_from_icon_name(GTK_IMAGE(win->status_bar_listening_image), "gtk-media-record");

	while (1) 
	{
		if (lt->close == 1)
			break;

		int res = handle_events(fd, wd, ld);

		if (res == 0)
			break;
	}

	inotify_rm_watch(fd, wd);
	close(fd);

	gtk_button_set_label(GTK_BUTTON(win->listening), "Start listening");
	gtk_widget_set_sensitive(win->directory_choose, TRUE);
	gtk_widget_set_sensitive(win->directory_choose_entry, TRUE);
	gtk_label_set_text(GTK_LABEL(win->status_bar_listening_status), "Not listening...");
	gtk_image_set_from_icon_name(GTK_IMAGE(win->status_bar_listening_image), "gtk-media-stop");

	lt->running = 0;
	g_idle_add(worker_finish_in_idle, ld);
	return NULL;
}

static void listening_clicked(GtkButton *button,
		gpointer data)
{
	InotifyAppWindow *win;
	GtkEntry *entry;
	GtkEntryBuffer *buffer;
	struct ListenerData *ld;

	win = INOTIFY_APP_WINDOW(data);

	if (lt && lt->running == 1)
	{
		lt->close = 1;
		g_thread_join(lt->thread);
		g_free(lt);
	}
	else
	{
		entry = GTK_ENTRY(win->directory_choose_entry);
		buffer = gtk_entry_get_buffer(entry);

		const char *dir = gtk_entry_buffer_get_text(buffer);

		ld = (struct ListenerData*)g_malloc(sizeof(struct ListenerData));
		ld->dir = dir;
		ld->win = GTK_WIDGET(win);

		lt = (struct ListenerThread*)g_malloc(sizeof(struct ListenerThread));
		lt->running = 1;
		lt->close = 0;
		lt->thread = g_thread_new("worker", worker, (gpointer) ld);
	}
}

/* }}} */

/* Clear list {{{1 */

static void clear_clicked(GtkButton *button,
		gpointer data)
{
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(data);
	
	GtkListBoxRow *row;
	GtkListBox *box = GTK_LIST_BOX(win->list);

	while ((row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(win->list), 0)))
	{
		gtk_list_box_remove(box, GTK_WIDGET(row));
	}

	gtk_label_set_text(GTK_LABEL(win->status_bar_entries), "0");
	gtk_widget_set_sensitive(win->status_bar_clear, TRUE);

	if ((gtk_widget_get_visible(win->status_bar_err)) == TRUE)
		gtk_widget_set_visible(win->status_bar_err, FALSE);
}


/* }}} */

/* Choose directory entry {{{ */

static void choose_entry_changed(GtkEditable *editable, 
		gpointer data)
{
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(data);

	if ((gtk_widget_get_visible(win->status_bar_err)) == TRUE)
		gtk_widget_set_visible(win->status_bar_err, FALSE);
}

/* }}} */

/* Initialization {{{ */

static void inotify_app_window_init(InotifyAppWindow *win)
{
	GtkEntry *entry;
	GtkEntryBuffer *buffer;

	gtk_widget_init_template(GTK_WIDGET(win));

	char cwd[PATH_MAX];
	
	entry = GTK_ENTRY(win->directory_choose_entry);
	buffer = gtk_entry_get_buffer(entry);

	if (getcwd(cwd, sizeof(cwd)) != NULL)
		gtk_entry_buffer_set_text(buffer, cwd, -1);
	else
		gtk_entry_buffer_set_text(buffer, "", -1);

	gtk_widget_grab_focus(win->list);

	g_signal_connect(win->directory_choose, "clicked", G_CALLBACK(directory_choose_clicked), win);
	g_signal_connect(win->listening, "clicked", G_CALLBACK(listening_clicked), win);
	g_signal_connect(win->status_bar_clear, "clicked", G_CALLBACK(clear_clicked), win);
	g_signal_connect(win->directory_choose_entry, "changed", G_CALLBACK(choose_entry_changed), win);
}

static void inotify_app_window_class_init(InotifyAppWindowClass *class)
{
	gtk_widget_class_set_template_from_resource(GTK_WIDGET_CLASS(class), "/org/gtk/inotifyapp/window.ui");

	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, directory_choose);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, directory_choose_entry);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, list);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, listening);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_entries);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_listening_image);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_listening_status);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_clear);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_err);
}

InotifyAppWindow* inotify_app_window_new(InotifyApp *app)
{
	return g_object_new(INOTIFY_APP_WINDOW_TYPE, "application", app, NULL);
}

/* }}} */
