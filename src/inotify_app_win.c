/* vim: set fdm=marker : */

#include <sys/eventfd.h>
#include <dirent.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <linux/limits.h>
#include <magic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "inotify_app.h"
#include "inotify_app_win.h"

/* Definitions {{{ */

struct _InotifyAppWindow
{
	GtkApplicationWindow parent;
	GtkWidget *directory_choose;
	GtkWidget *directory_choose_entry;
	GtkWidget *list;
	GtkWidget *view;
	GtkWidget *listening;
	GtkWidget *status_bar;
	GtkWidget *status_bar_entries;
	GtkWidget *status_bar_listening_image;
	GtkWidget *status_bar_listening_status;
	GtkWidget *status_bar_clear;
	GtkWidget *status_bar_err;
	GtkWidget *view_status_bar_contents;
	GtkWidget *view_status_bar_modified;
	GtkWidget *stack1;
	GtkWidget *page1;
	GtkWidget *page2;
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
		g_free(filepath);
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

#define event_case(str, mask, ev) if (mask & ev) str = #ev;

struct ListenerData
{
	GtkWidget *win;
	const char *dir;
};

struct ListenerThread
{
	GThread *thread;
	int efd;
	int running;
	int close;
};

struct ListenerLabelData
{
	GtkWidget *label;
	GtkWidget *clear;
	int count;
};

struct ListenerErrorData
{
	GtkWidget *label;
	char *error;
};

static struct ListenerThread *lt;

static gboolean worker_finish_in_idle(gpointer data)
{
	struct ListenerData *ld = data;
	g_free(ld);
	return FALSE;
}

static gboolean worker_set_err(gpointer data)
{
	struct ListenerErrorData *led = data;
	
	gtk_label_set_text(GTK_LABEL(led->label), led->error);
	if ((gtk_widget_get_visible(led->label)) == FALSE)
		gtk_widget_set_visible(led->label, TRUE);

	g_free(led->error);
	g_free(data);

	return FALSE;
}

static gboolean worker_update_label(gpointer data)
{
	struct ListenerLabelData *lld = data;

	int entries = atoi(gtk_label_get_text(GTK_LABEL(lld->label))) + lld->count;
	char *entries_str = g_strdup_printf("%d", entries);
	gtk_label_set_text(GTK_LABEL(lld->label), entries_str);
	g_free(entries_str);

	if ((gtk_widget_get_sensitive(lld->clear)) == FALSE)
		gtk_widget_set_sensitive(lld->clear, TRUE);

	g_free(data);

	return FALSE;
}

static gboolean worker_gui_set_stop(gpointer data)
{
	struct ListenerData *ld = data;
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(ld->win);

	gtk_button_set_label(GTK_BUTTON(win->listening), "Stop listening");
	gtk_widget_set_sensitive(win->directory_choose, FALSE);
	gtk_widget_set_sensitive(win->directory_choose_entry, FALSE);
	gtk_label_set_text(GTK_LABEL(win->status_bar_listening_status), "Listening...");
	gtk_image_set_from_icon_name(GTK_IMAGE(win->status_bar_listening_image), "gtk-media-record");

	return FALSE;
}

static gboolean worker_gui_set_start(gpointer data)
{
	struct ListenerData *ld = data;
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(ld->win);

	gtk_button_set_label(GTK_BUTTON(win->listening), "Start listening");
	gtk_widget_set_sensitive(win->directory_choose, TRUE);
	gtk_widget_set_sensitive(win->directory_choose_entry, TRUE);
	gtk_label_set_text(GTK_LABEL(win->status_bar_listening_status), "Not listening...");
	gtk_image_set_from_icon_name(GTK_IMAGE(win->status_bar_listening_image), "gtk-media-stop");

	return FALSE;
}

static gboolean worker_switch_page(gpointer data)
{
	struct ListenerData *ld = data;
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(ld->win);

	gtk_stack_set_visible_child(GTK_STACK(win->stack1), win->page2);

	return FALSE;
}

static int handle_events(int fd, int wd, gpointer data)
{
	char buf[4096] __attribute ((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *event;
	ssize_t len;
	GString *str;
	GtkTreeView *list;
	GtkTreeModel *model;
	GtkListStore *store;

	if (lt->close == 1)
		return 0;

	struct ListenerData *ld = data;
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(ld->win);

	len = read(fd, buf, sizeof(buf));
	if (len == -1 && errno != EAGAIN)
	{
		char *error = g_strdup_printf("perror: %s", strerror(errno));
		struct ListenerErrorData *err = g_new(struct ListenerErrorData, 1);
		err->error = error;
		err->label = win->status_bar_err;

		g_idle_add(worker_set_err, err);

		return -1;
	}

	if (len == -1)
		return 0;

	list = GTK_TREE_VIEW(win->list);
	model = gtk_tree_view_get_model(list);
	store = GTK_LIST_STORE(model);

	str = g_string_new(NULL);

	int count = 0;

	for (char *ptr = buf; ptr < buf + len;
			ptr += sizeof(struct inotify_event) + event->len, ++count)
	{
		if (lt->close == 1)
			break;

		GtkTreeIter iter;
		char *ev_str = "";

		event = (const struct inotify_event*) ptr;

		event_case(ev_str, event->mask, IN_OPEN);
		event_case(ev_str, event->mask, IN_CLOSE_NOWRITE);
		event_case(ev_str, event->mask, IN_CLOSE_WRITE);
		event_case(ev_str, event->mask, IN_MOVED_FROM);
		event_case(ev_str, event->mask, IN_MOVED_TO);
		event_case(ev_str, event->mask, IN_DELETE);
		event_case(ev_str, event->mask, IN_DELETE_SELF);
		event_case(ev_str, event->mask, IN_MODIFY);
		event_case(ev_str, event->mask, IN_MOVE_SELF);
		event_case(ev_str, event->mask, IN_CREATE);

		if (ld->dir[strlen(ld->dir) - 1] == '/')
			g_string_append(str, ld->dir);
		else
			g_string_append_printf(str, "%s/", ld->dir);

		if (event->len)
			g_string_append_printf(str, "%s", event->name);

		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter,
				0, ev_str,
				1, str->str,
				-1);

		g_string_erase(str, 0, -1);

		if (event->mask & IN_DELETE_SELF)
		{
			struct ListenerErrorData *err = g_new(struct ListenerErrorData, 1);
			err->error =  g_strdup("Listening directory was deleted!");
			err->label = win->status_bar_err;

			g_idle_add(worker_set_err, err);

			g_string_free(str, TRUE);
			return -1;
		}

		if (event->mask & IN_MOVE_SELF)
		{
			struct ListenerErrorData *err = g_new(struct ListenerErrorData, 1);
			err->error =  g_strdup("Listening directory was moved!");
			err->label = win->status_bar_err;

			g_idle_add(worker_set_err, err);

			g_string_free(str, TRUE);
			return -1;
		}
	}

	g_string_free(str, TRUE);
	return count;
}

static gpointer worker(gpointer data)
{
	int fd, wd;
	struct ListenerData *ld;
	InotifyAppWindow *win;

	fd = inotify_init1(IN_NONBLOCK);
	
	ld = data;
	win = INOTIFY_APP_WINDOW(ld->win);

	if (fd == -1)
	{
		char *error = g_strdup_printf("inotify_init1: %s", strerror(errno));
		struct ListenerErrorData *err = g_new(struct ListenerErrorData, 1);
		err->error = error;
		err->label = win->status_bar_err;

		g_idle_add(worker_set_err, err);

		lt->running = 0;
		g_idle_add(worker_finish_in_idle, ld);
		return NULL;
	}

	wd = inotify_add_watch(fd, ld->dir, 
			IN_OPEN | IN_CLOSE | IN_MOVE | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF);

	if (wd == -1)
	{
		char *error = g_strdup_printf("Can't watch '%s': %s", ld->dir, strerror(errno));
		struct ListenerErrorData *err = g_new(struct ListenerErrorData, 1);
		err->error = error;
		err->label = win->status_bar_err;

		g_idle_add(worker_set_err, err);
		
		close(fd);

		lt->running = 0;
		g_idle_add(worker_finish_in_idle, ld);
		return NULL;
	}

	struct stat dir_stat;
	stat(ld->dir, &dir_stat);

	if (!S_ISDIR(dir_stat.st_mode))
	{
		char *error = g_strdup_printf("Can't watch '%s': it is not directory!", ld->dir);
		struct ListenerErrorData *err = g_new(struct ListenerErrorData, 1);
		err->error = error;
		err->label = win->status_bar_err;

		g_idle_add(worker_set_err, err);

		close(fd);

		lt->running = 0;
		g_idle_add(worker_finish_in_idle, ld);
		return NULL;
	}

	g_idle_add(worker_gui_set_stop, ld);
	g_idle_add(worker_switch_page, ld);

	int poll_num;
	nfds_t nfds;
	struct pollfd fds[2];

	nfds = 2;

	fds[0].fd = lt->efd;
	fds[0].events = POLLIN;

	fds[1].fd = fd;
	fds[1].events = POLLIN;

	while (1) 
	{
		if (lt->close == 1)
			break;

		poll_num = poll(fds, nfds, -1);
		if (poll_num == -1)
		{
			if (errno == EINTR)
				continue;

			char *error = g_strdup_printf("poll: %s", strerror(errno));
			struct ListenerErrorData *err = g_new(struct ListenerErrorData, 1);
			err->error = error;
			err->label = win->status_bar_err;

			g_idle_add(worker_set_err, err);

			break;
		}

		if (poll_num > 0) 
		{
			if (fds[0].revents & POLLIN) 
			{
				break;
			}

			if (fds[1].revents & POLLIN) 
			{
				int res = handle_events(fd, wd, ld);

				if (res == -1)
					break;

				struct ListenerLabelData *lld = g_new(struct ListenerLabelData, 1);
				lld->label = win->status_bar_entries;
				lld->count = res;
				lld->clear = win->status_bar_clear;

				g_idle_add(worker_update_label, lld);
			}
		}
	}

	inotify_rm_watch(fd, wd);
	close(fd);

	g_idle_add(worker_gui_set_start, ld);

	lt->running = 0;
	g_idle_add(worker_finish_in_idle, ld);
	return NULL;
}

static void listening_clicked(GtkButton *button,
		gpointer data)
{
	InotifyAppWindow *win;

	win = INOTIFY_APP_WINDOW(data);
	
	if (lt && lt->running == 1)
	{
		eventfd_write(lt->efd, 1);
		g_thread_join(lt->thread);
		close(lt->efd);
		g_free(lt);
		lt = NULL;
	}
	else
	{
		GtkEntry *entry;
		GtkEntryBuffer *buffer;
		struct ListenerData *ld;
		int efd;

		efd = eventfd(0, 0);

		if (efd == -1)
		{
			char *error = g_strdup_printf("eventfd: %s", strerror(errno));
			gtk_label_set_text(GTK_LABEL(win->status_bar_err), error);

			if ((gtk_widget_get_visible(win->status_bar_err)) == FALSE)
				gtk_widget_set_visible(win->status_bar_err, TRUE);

			g_free(error);
			return;
		}

		entry = GTK_ENTRY(win->directory_choose_entry);
		buffer = gtk_entry_get_buffer(entry);

		const char *dir = gtk_entry_buffer_get_text(buffer);

		ld = (struct ListenerData*)g_malloc(sizeof(struct ListenerData));
		ld->dir = dir;
		ld->win = GTK_WIDGET(win);

		lt = (struct ListenerThread*)g_malloc(sizeof(struct ListenerThread));
		lt->running = 1;
		lt->close = 0;
		lt->efd = efd;
		lt->thread = g_thread_new("worker", worker, (gpointer) ld);
	}
}

/* }}} */

/* View {{{ */

struct dir_item_info
{
	char 	*ct;
	char 	*name;
	char 	*size;
	char 	*modified;
	gboolean is_dir;
};

char* transormBytes(off_t bytes)
{
	long double b = (long double) bytes;
	char *symb;

	while (1)
	{
		if (b < 1024)
		{
			symb = "";
			break;
		}

		b /= 1024;

		if (b < 1024)
		{
			symb = "Ki";
			break;
		}

		b /= 1024;

		if (b < 1024)
		{
			symb = "Mi";
			break;
		}

		b /= 1024;

		if (b < 1024)
		{
			symb = "Gi";
			break;
		}

		b /= 1024;

		if (b < 1024)
		{
			symb = "Ti";
			break;
		}

		b /= 1024;

		if (b < 1024)
		{
			symb = "Pi";
			break;
		}

		b /= 1024;

		if (b < 1024)
		{
			symb = "Ei";
			break;
		}

		b /= 1024;

		if (b < 1024)
		{
			symb = "Zi";
			break;
		}

		b /= 1024;
		symb = "Yi";
		break;
	}

	if (ceill(b) == b)			
		return g_strdup_printf("%.0Lf %sB", b, symb);
	else
		return g_strdup_printf("%.1Lf %sB", b, symb);
}

int dir_item_cmp(gconstpointer a, gconstpointer b)
{
	const struct dir_item_info *da = (const struct dir_item_info*) a;
	const struct dir_item_info *db = (const struct dir_item_info*) b;

	if (da->is_dir && !db->is_dir)
		return -1;

	if (!da->is_dir && db->is_dir)
		return 1;

	if (strcmp(da->name, "..") == 0)
		return -1;

	if (strcmp(db->name, "..") == 0)
		return 1;

	if (da->is_dir == db->is_dir)
	{
		if (da->name[0] == '.' && db->name[0] != '.')
			return 1;
		if (da->name[0] != '.' && db->name[0] == '.')
			return -1;
	}

	return strcoll(da->name, db->name);
}

void update_view(InotifyAppWindow *win, const char *dir, gboolean change_entry)
{
	GtkEntry *entry;
	GtkEntryBuffer *buffer;
	DIR *dp;
	magic_t magic;

	magic = magic_open(MAGIC_MIME_TYPE);

	g_assert(magic != NULL);
	g_assert(magic_load(magic, NULL) == 0);

	if (change_entry)
	{
		entry = GTK_ENTRY(win->directory_choose_entry);
		buffer = gtk_entry_get_buffer(entry);
	}

	dp = opendir(dir);

	if (dp != NULL)
	{
		GArray *arr;
		struct dirent *ep, *hep;
		DIR *hdp;
		struct stat dst;

		stat(dir, &dst);
		chdir(dir);

		arr = g_array_new(TRUE, TRUE, sizeof(struct dir_item_info));

		if (change_entry)
			gtk_entry_buffer_set_text(buffer, dir, -1);

		while ((ep = readdir(dp))) 
		{
			if (strcmp(ep->d_name, ".") == 0)
				continue;

			struct stat st;
			struct dir_item_info item;
			stat(ep->d_name, &st);

			if (S_ISDIR(st.st_mode))
			{
				hdp = opendir(ep->d_name);
				size_t sz = 0;

				if (hdp != NULL)
				{
					while ((hep = readdir(hdp)))
						sz++;

					if (sz <= 2)
						sz = 0;
					else
						sz -= 2;

					closedir(hdp);
				}

				item.size = g_strdup_printf("%lu items", sz);
				item.is_dir = TRUE;
			}
			else 
			{
				item.size = transormBytes(st.st_size);
				item.is_dir = FALSE;
			}

			char bf[64];
			struct tm ts = *localtime(&st.st_mtim.tv_sec);
			strftime(bf, sizeof(bf), "%d %b %Y %H:%M", &ts);

			item.modified = g_strdup(bf);
			item.name = g_strdup(ep->d_name);

			const char *mime = magic_file(magic, item.name);
			item.ct = g_content_type_from_mime_type(mime);

			g_array_append_val(arr, item);
		}

		g_array_sort(arr, dir_item_cmp);
		magic_close(magic);
		closedir(dp);

		GtkTreeView *view = GTK_TREE_VIEW(win->view);
		GtkTreeModel *model = gtk_tree_view_get_model(view);

		g_object_ref(model);
		gtk_tree_view_set_model(view, NULL);
		GtkListStore *store = GTK_LIST_STORE(model);
		gtk_list_store_clear(store);

		for (int i = 0; i < arr->len; ++i) 
		{
			GIcon *ct_icon;
			GtkTreeIter iter;

			struct dir_item_info item = g_array_index(arr, struct dir_item_info, i);

			if (strcmp(item.name, "..") == 0)
			{
				ct_icon = g_themed_icon_new("go-up");
			}
			else
				ct_icon = g_content_type_get_icon(item.ct);

			gtk_list_store_append(store, &iter);
			gtk_list_store_set(store, &iter,
					0, ct_icon,
					1, item.name,
					2, item.size,
					3, item.modified,
					-1);

			g_free(item.size);
			g_free(item.name);
			g_free(item.modified);
			g_free(item.ct);
		}

		gtk_tree_view_set_model(view, model);
		g_object_unref(model);

		char dbf[64];
		struct tm ts = *localtime(&dst.st_mtim.tv_sec);
		strftime(dbf, sizeof(dbf), "%d %b %Y %H:%M", &ts);
		gtk_label_set_text(GTK_LABEL(win->view_status_bar_modified), dbf);

		int dlen = arr->len;
		char *contents = g_strdup_printf("%d items", dlen);
		gtk_label_set_text(GTK_LABEL(win->view_status_bar_contents), contents);
		g_free(contents);

		g_array_free(arr, FALSE);
	}
	else
	{
		char *error = g_strdup_printf("Can't open '%s': %s", dir, strerror(errno));
		gtk_label_set_text(GTK_LABEL(win->status_bar_err), error);

		if ((gtk_widget_get_visible(win->status_bar_err)) == FALSE)
			gtk_widget_set_visible(win->status_bar_err, TRUE);

		g_free(error);
	}
}

void view_row_activated(GtkTreeView *view, 
		GtkTreePath *path,
		GtkTreeViewColumn *column,
		gpointer data)
{
	if (!lt)
	{
		InotifyAppWindow *win = INOTIFY_APP_WINDOW(data);
		GtkTreeIter iter;
		GtkTreeModel *model = gtk_tree_view_get_model(view);

		char *name, *full;

		if (gtk_tree_model_get_iter(model, &iter, path))
		{
			gtk_tree_model_get(model, &iter, 1, &name, -1);
			full = realpath(name, NULL);
			update_view(win, full, TRUE);
			g_free(full);
		}
	}
}

/* }}} */

/* Clear list {{{ */

static void clear_clicked(GtkButton *button,
		gpointer data)
{
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(data);
	
	GtkTreeView *list = GTK_TREE_VIEW(win->list);
	GtkTreeModel *model = gtk_tree_view_get_model(list);
	GtkListStore *store = GTK_LIST_STORE(model);

	gtk_list_store_clear(store);

	gtk_label_set_text(GTK_LABEL(win->status_bar_entries), "0");
	gtk_widget_set_sensitive(win->status_bar_clear, FALSE);

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

static void choose_entry_activated(GtkEntry *entry,
		gpointer data)
{
	GtkEntryBuffer *buffer = gtk_entry_get_buffer(entry);
	InotifyAppWindow *win = INOTIFY_APP_WINDOW(data);

	char *dir = g_strdup(gtk_entry_buffer_get_text(buffer));
	update_view(win, dir, FALSE);
	g_free(dir);
}

/* }}} */

/* Initialization {{{ */

static void inotify_app_window_init(InotifyAppWindow *win)
{
	gtk_widget_init_template(GTK_WIDGET(win));

	char cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) != NULL)
		update_view(win, cwd, TRUE);
	else
		update_view(win, "~", TRUE);

	gtk_widget_grab_focus(win->directory_choose);

	/* View {{{ */

	GtkTreeView *view;
	GtkTreeViewColumn *vcol;
	GtkCellRenderer *vrenderer;

	view = GTK_TREE_VIEW(win->view);

	vcol = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(vcol, "Name");

	vrenderer = gtk_cell_renderer_pixbuf_new();
	gtk_tree_view_column_pack_start(vcol, vrenderer, FALSE);
	gtk_tree_view_column_set_attributes(vcol, vrenderer, 
			"gicon", 0,
			NULL);

	vrenderer = gtk_cell_renderer_text_new();
	g_object_set(vrenderer, 
			"ellipsize", PANGO_ELLIPSIZE_END,
			NULL);
	gtk_tree_view_column_pack_end(vcol, vrenderer, TRUE);
	gtk_tree_view_column_set_attributes(vcol, vrenderer, 
			"text", 1,
			NULL);

	gtk_tree_view_column_set_expand(vcol, TRUE);
	gtk_tree_view_append_column(view, vcol);
	gtk_tree_view_set_tooltip_column(view, 1);

	vcol = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(vcol, "Size");

	vrenderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(vcol, vrenderer, FALSE);
	gtk_tree_view_column_set_attributes(vcol, vrenderer, 
			"text", 2,
			NULL);

	gtk_tree_view_append_column(view, vcol);

	vcol = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(vcol, "Modified");

	vrenderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(vcol, vrenderer, FALSE);
	gtk_tree_view_column_set_attributes(vcol, vrenderer, 
			"text", 3,
			NULL);

	gtk_tree_view_append_column(view, vcol);

	/* }}} */

	/* List {{{ */

	GtkTreeView *list;
	GtkTreeViewColumn *lcol;
	GtkCellRenderer *lrenderer;

	list = GTK_TREE_VIEW(win->list);

	lcol = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(lcol, "Event");

	lrenderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_end(lcol, lrenderer, TRUE);
	gtk_tree_view_column_set_attributes(lcol, lrenderer, 
			"text", 0,
			NULL);

	gtk_tree_view_append_column(list, lcol);

	lcol = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(lcol, "Item");

	lrenderer = gtk_cell_renderer_text_new();
	g_object_set(lrenderer, 
			"ellipsize", PANGO_ELLIPSIZE_END,
			NULL);
	gtk_tree_view_column_pack_start(lcol, lrenderer, TRUE);
	gtk_tree_view_column_set_attributes(lcol, lrenderer, 
			"text", 1,
			NULL);

	gtk_tree_view_column_set_expand(lcol, 1);
	gtk_tree_view_append_column(list, lcol);
	gtk_tree_view_set_tooltip_column(list, 1);

	/* }}} */

	g_signal_connect(win->directory_choose, "clicked", G_CALLBACK(directory_choose_clicked), win);
	g_signal_connect(win->listening, "clicked", G_CALLBACK(listening_clicked), win);
	g_signal_connect(win->status_bar_clear, "clicked", G_CALLBACK(clear_clicked), win);
	g_signal_connect(win->directory_choose_entry, "changed", G_CALLBACK(choose_entry_changed), win);
	g_signal_connect(win->directory_choose_entry, "activate", G_CALLBACK(choose_entry_activated), win);
	g_signal_connect(win->view, "row_activated", G_CALLBACK(view_row_activated), win);
}

static void inotify_app_window_class_init(InotifyAppWindowClass *class)
{
	gtk_widget_class_set_template_from_resource(GTK_WIDGET_CLASS(class), "/org/gtk/inotifyapp/window.ui");

	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, directory_choose);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, directory_choose_entry);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, list);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, view);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, listening);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_entries);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_listening_image);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_listening_status);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_clear);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, status_bar_err);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, view_status_bar_contents);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, view_status_bar_modified);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, stack1);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, page1);
	gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(class), InotifyAppWindow, page2);
}

InotifyAppWindow* inotify_app_window_new(InotifyApp *app)
{
	return g_object_new(INOTIFY_APP_WINDOW_TYPE, "application", app, NULL);
}

void inotify_app_window_open(InotifyAppWindow *win, GFile *file)
{
	char *dir = g_file_get_path(file);
	update_view(win, dir, TRUE);
	g_free(dir);
}

/* }}} */
