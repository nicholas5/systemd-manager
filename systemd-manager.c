/*
 * systemd-manager.c
 *
 * GTK3 GUI for managing systemd services.
 * Inherits the current desktop GTK theme – zero custom colours injected.
 *
 * Build:
 *   gcc $(pkg-config --cflags gtk+-3.0) -o systemd-manager systemd-manager.c \
 *       $(pkg-config --libs gtk+-3.0) -lpthread -std=gnu11
 *
 * Run:
 *   sudo ./systemd-manager    # full control
 *   ./systemd-manager         # read-only (journal still works)
 */

#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

/* ─── Column indices ─────────────────────────────────────────────────────── */
enum {
    COL_ENABLED = 0,   /* gboolean  – start-on-boot           */
    COL_NAME,          /* string                               */
    COL_LOAD,          /* string                               */
    COL_ACTIVE,        /* string                               */
    COL_SUB,           /* string                               */
    COL_DESCRIPTION,   /* string                               */
    COL_ACTIVE_WEIGHT, /* gint – Pango weight for active col  */
    N_COLUMNS
};

/* ─── Filter state ───────────────────────────────────────────────────────── */
typedef enum {
    FILTER_ALL = 0,
    FILTER_ACTIVE,
    FILTER_INACTIVE,
    FILTER_FAILED,
    FILTER_ENABLED,
    FILTER_DISABLED,
    N_FILTERS
} FilterState;

static const char *filter_labels[N_FILTERS] = {
    "All", "Active", "Inactive", "Failed", "Enabled", "Disabled"
};

/* ─── Application state ──────────────────────────────────────────────────── */
typedef struct {
    GtkWidget            *window;
    GtkWidget            *tree_view;
    GtkListStore         *store;
    GtkTreeModelFilter   *filter_model;
    GtkTreeModelSort     *sort_model;

    GtkWidget            *search_entry;
    GtkWidget            *filter_btns[N_FILTERS];

    GtkWidget            *btn_start;
    GtkWidget            *btn_stop;
    GtkWidget            *btn_restart;
    GtkWidget            *btn_enable;
    GtkWidget            *btn_disable;
    GtkWidget            *btn_journal;
    GtkWidget            *btn_reload;
    GtkWidget            *btn_clear;

    GtkTextBuffer        *log_buf;
    GtkWidget            *log_view;
    GtkWidget            *log_scroll;

    GtkWidget            *statusbar;
    guint                 statusbar_ctx;

    FilterState           current_filter;
    gboolean              loading;
    gboolean              is_root;
} AppState;

static AppState app;

/* ─── Forward declarations ───────────────────────────────────────────────── */
static void load_services(void);
static void set_actions_sensitive(gboolean s);
static void log_append(const char *text);

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility: spawn a command, capture stdout + stderr
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { int rc; char *out; char *err; } CmdResult;

static CmdResult run_cmd(char **argv)
{
    CmdResult res = {-1, NULL, NULL};
    GError   *error = NULL;
    gint      out_fd = -1, err_fd = -1;
    GPid      pid;

    if (!g_spawn_async_with_pipes(
            NULL, argv, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
            NULL, NULL, &pid, NULL, &out_fd, &err_fd, &error)) {
        res.err = g_strdup(error ? error->message : "spawn failed");
        if (error) g_error_free(error);
        return res;
    }

    GIOChannel *ch_out = g_io_channel_unix_new(out_fd);
    GIOChannel *ch_err = g_io_channel_unix_new(err_fd);
    g_io_channel_set_encoding(ch_out, NULL, NULL);
    g_io_channel_set_encoding(ch_err, NULL, NULL);

    gchar *out_str = NULL, *err_str = NULL;
    gsize  out_len = 0,    err_len = 0;
    g_io_channel_read_to_end(ch_out, &out_str, &out_len, NULL);
    g_io_channel_read_to_end(ch_err, &err_str, &err_len, NULL);
    g_io_channel_shutdown(ch_out, FALSE, NULL);
    g_io_channel_shutdown(ch_err, FALSE, NULL);
    g_io_channel_unref(ch_out);
    g_io_channel_unref(ch_err);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    g_spawn_close_pid(pid);

    res.rc  = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    res.out = out_str ? out_str : g_strdup("");
    res.err = err_str ? err_str : g_strdup("");
    return res;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Log pane
 * ═══════════════════════════════════════════════════════════════════════════ */
static void log_append(const char *text)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app.log_buf, &end);
    gtk_text_buffer_insert(app.log_buf, &end, text, -1);

    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(app.log_scroll));
    gtk_adjustment_set_value(adj,
        gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
}

typedef struct { char *text; } LogIdleData;

static gboolean log_idle_cb(gpointer user_data)
{
    LogIdleData *d = user_data;
    log_append(d->text);
    g_free(d->text);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void log_async(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    LogIdleData *d = g_new(LogIdleData, 1);
    d->text = msg;
    g_idle_add(log_idle_cb, d);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Status bar
 * ═══════════════════════════════════════════════════════════════════════════ */
static void status_set(const char *msg)
{
    gtk_statusbar_pop(GTK_STATUSBAR(app.statusbar), app.statusbar_ctx);
    gtk_statusbar_push(GTK_STATUSBAR(app.statusbar), app.statusbar_ctx, msg);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Service row
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    char    *name;
    char    *load;
    char    *active;
    char    *sub;
    char    *description;
    gboolean enabled;
} ServiceRow;

static void service_row_free(gpointer p)
{
    ServiceRow *r = p;
    g_free(r->name);
    g_free(r->load);
    g_free(r->active);
    g_free(r->sub);
    g_free(r->description);
    g_free(r);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Populate store on main thread
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { GPtrArray *rows; } PopulateData;

static gboolean populate_store_cb(gpointer user_data)
{
    PopulateData *pd = user_data;
    gtk_list_store_clear(app.store);

    for (guint i = 0; i < pd->rows->len; i++) {
        ServiceRow *r = g_ptr_array_index(pd->rows, i);
        gint weight = PANGO_WEIGHT_NORMAL;
        if (g_strcmp0(r->active, "active") == 0)
            weight = PANGO_WEIGHT_SEMIBOLD;
        else if (g_strcmp0(r->active, "failed") == 0)
            weight = PANGO_WEIGHT_BOLD;

        GtkTreeIter it;
        gtk_list_store_append(app.store, &it);
        gtk_list_store_set(app.store, &it,
            COL_ENABLED,       r->enabled,
            COL_NAME,          r->name,
            COL_LOAD,          r->load,
            COL_ACTIVE,        r->active,
            COL_SUB,           r->sub,
            COL_DESCRIPTION,   r->description,
            COL_ACTIVE_WEIGHT, weight,
            -1);
    }

    char buf[180];
    g_snprintf(buf, sizeof(buf), "%u services  •  %s",
        pd->rows->len,
        app.is_root ? "running as root"
                    : "read-only – run with sudo to make changes");
    status_set(buf);

    g_ptr_array_free(pd->rows, TRUE);
    g_free(pd);
    app.loading = FALSE;
    return G_SOURCE_REMOVE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Background thread: fetch services
 * ═══════════════════════════════════════════════════════════════════════════ */
/*
 * Split a string on runs of whitespace (like Python str.split() or awk).
 * g_strsplit_set does NOT collapse consecutive delimiters, which breaks
 * "active   running" into ["active","","","running"].  This wrapper does.
 * Returns a NULL-terminated array; caller must g_strfreev().
 * max_tokens: maximum number of tokens (0 = unlimited).
 */
static gchar **split_whitespace(const gchar *str, gint max_tokens)
{
    GPtrArray *arr = g_ptr_array_new();
    const gchar *p = str;
    gint count = 0;

    while (*p) {
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (max_tokens > 0 && count == max_tokens - 1) {
            /* last slot: take the rest of the string as-is */
            g_ptr_array_add(arr, g_strdup(p));
            count++;
            break;
        }

        const gchar *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        g_ptr_array_add(arr, g_strndup(start, (gsize)(p - start)));
        count++;
    }
    g_ptr_array_add(arr, NULL);
    return (gchar **)g_ptr_array_free(arr, FALSE);
}

static int row_name_cmp(gconstpointer a, gconstpointer b)
{
    const ServiceRow *ra = *(const ServiceRow *const *)a;
    const ServiceRow *rb = *(const ServiceRow *const *)b;
    return g_strcmp0(ra->name, rb->name);
}

static void *fetch_services_thread(void *arg)
{
    (void)arg;

    /* 1 – list-units */
    char *argv_units[] = {
        "systemctl", "list-units",
        "--type=service", "--all",
        "--no-pager", "--plain", "--no-legend", NULL
    };
    CmdResult ru = run_cmd(argv_units);

    GHashTable *ht = g_hash_table_new_full(
        g_str_hash, g_str_equal, NULL, service_row_free);

    if (ru.rc == 0) {
        gchar **lines = g_strsplit(ru.out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!*line) continue;
            /* Strip leading non-ASCII bullet/dot chars (●, ☉, etc.) */
            while (*line && (unsigned char)*line > 127) line++;
            while (*line == ' ') line++;

            /*
             * Use split_whitespace() so that runs of spaces (e.g. the gap
             * between "active" and "running" in the output) are collapsed
             * correctly.  g_strsplit_set() does NOT collapse, so it would
             * produce empty tokens and shift every field after the first gap.
             * Limit to 5 tokens: name load active sub description(rest).
             */
            gchar **f = split_whitespace(line, 5);
            guint fc = g_strv_length(f);
            if (fc >= 4 && f[0] && *f[0]) {
                ServiceRow *r = g_new0(ServiceRow, 1);
                r->name        = g_strdup(f[0]);
                r->load        = g_strdup(f[1]);
                r->active      = g_strdup(f[2]);
                r->sub         = g_strdup(f[3]);
                r->description = g_strdup(fc > 4 ? f[4] : "");
                r->enabled     = FALSE;
                g_hash_table_insert(ht, r->name, r);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(ru.out);
    g_free(ru.err);

    /* 2 – list-unit-files */
    char *argv_files[] = {
        "systemctl", "list-unit-files",
        "--type=service",
        "--no-pager", "--plain", "--no-legend", NULL
    };
    CmdResult rf = run_cmd(argv_files);

    if (rf.rc == 0) {
        gchar **lines = g_strsplit(rf.out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!*line) continue;

            gchar **f = split_whitespace(line, 3);
            if (g_strv_length(f) < 2) { g_strfreev(f); continue; }

            const char *svc   = f[0];
            const char *state = f[1];
            gboolean enabled  =
                g_strcmp0(state, "enabled")         == 0 ||
                g_strcmp0(state, "enabled-runtime") == 0 ||
                g_strcmp0(state, "static")          == 0 ||
                g_strcmp0(state, "generated")       == 0;

            ServiceRow *existing = g_hash_table_lookup(ht, svc);
            if (existing) {
                existing->enabled = enabled;
            } else if (svc && *svc) {
                ServiceRow *r = g_new0(ServiceRow, 1);
                r->name        = g_strdup(svc);
                r->load        = g_strdup("not-loaded");
                r->active      = g_strdup("inactive");
                r->sub         = g_strdup("dead");
                r->description = g_strdup("");
                r->enabled     = enabled;
                g_hash_table_insert(ht, r->name, r);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(rf.out);
    g_free(rf.err);

    /* 3 – collect + sort */
    GPtrArray *rows = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, ht);
    while (g_hash_table_iter_next(&iter, &key, &val)) {
        g_ptr_array_add(rows, val);
        g_hash_table_iter_steal(&iter);
    }
    g_hash_table_destroy(ht);
    g_ptr_array_sort(rows, row_name_cmp);

    PopulateData *pd = g_new(PopulateData, 1);
    pd->rows = rows;
    g_idle_add(populate_store_cb, pd);
    return NULL;
}

static void load_services(void)
{
    if (app.loading) return;
    app.loading = TRUE;
    status_set("Loading services…");
    pthread_t tid;
    pthread_create(&tid, NULL, fetch_services_thread, NULL);
    pthread_detach(tid);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Background thread: run a systemctl action
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { char *service; char *action; } ActionData;

static void *run_action_thread(void *arg)
{
    ActionData *d = arg;
    log_async("→  systemctl %s %s\n", d->action, d->service);
    char *argv[] = { "systemctl", d->action, d->service, NULL };
    CmdResult r = run_cmd(argv);
    const char *msg =
        (r.out && *r.out) ? r.out :
        (r.err && *r.err) ? r.err :
        (r.rc == 0 ? "OK" : "Failed");
    log_async("%s\n\n", msg);
    g_free(r.out);
    g_free(r.err);
    g_free(d->service);
    g_free(d->action);
    g_free(d);
    g_idle_add((GSourceFunc)load_services, NULL);
    return NULL;
}

static void dispatch_action(const char *service, const char *action)
{
    ActionData *d = g_new(ActionData, 1);
    d->service = g_strdup(service);
    d->action  = g_strdup(action);
    pthread_t tid;
    pthread_create(&tid, NULL, run_action_thread, d);
    pthread_detach(tid);
}

static void *journal_thread(void *arg)
{
    char *service = arg;
    log_async("─── Journal: %s ───\n", service);
    char *argv[] = {
        "journalctl", "-u", service,
        "--no-pager", "-n", "80", "--output=short", NULL
    };
    CmdResult r = run_cmd(argv);
    log_async("%s\n\n", r.rc == 0 ? r.out : r.err);
    g_free(r.out);
    g_free(r.err);
    g_free(service);
    return NULL;
}

static void show_journal(const char *service)
{
    pthread_t tid;
    pthread_create(&tid, NULL, journal_thread, g_strdup(service));
    pthread_detach(tid);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
static char *get_selected_service(void)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
    GtkTreeModel *model;
    GtkTreeIter   it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return NULL;
    char *name = NULL;
    gtk_tree_model_get(model, &it, COL_NAME, &name, -1);
    return name;
}

static void set_actions_sensitive(gboolean s)
{
    gboolean rw = s && app.is_root;
    gtk_widget_set_sensitive(app.btn_start,   rw);
    gtk_widget_set_sensitive(app.btn_stop,    rw);
    gtk_widget_set_sensitive(app.btn_restart, rw);
    gtk_widget_set_sensitive(app.btn_enable,  rw);
    gtk_widget_set_sensitive(app.btn_disable, rw);
    gtk_widget_set_sensitive(app.btn_journal, s);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal handlers
 * ═══════════════════════════════════════════════════════════════════════════ */
static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)data;
    GtkTreeModel *model;
    GtkTreeIter   it;
    set_actions_sensitive(gtk_tree_selection_get_selected(sel, &model, &it));
}

static void on_action_btn(GtkButton *btn, gpointer action)
{
    (void)btn;
    char *svc = get_selected_service();
    if (!svc) return;
    if (g_strcmp0(action, "journal") == 0) {
        show_journal(svc);
    } else {
        if (!app.is_root)
            log_append("⚠  Permission denied – run with sudo for write operations.\n");
        else
            dispatch_action(svc, (const char *)action);
    }
    g_free(svc);
}

static void on_reload_btn(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    load_services();
}

static void on_clear_btn(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    gtk_text_buffer_set_text(app.log_buf, "", 0);
}

/* ─── Filter ─────────────────────────────────────────────────────────────── */
static gboolean row_visible_func(GtkTreeModel *model, GtkTreeIter *it, gpointer data)
{
    (void)data;
    char    *name   = NULL, *active = NULL, *sub = NULL;
    gboolean enabled = FALSE;
    gtk_tree_model_get(model, it,
        COL_NAME,    &name,
        COL_ACTIVE,  &active,
        COL_SUB,     &sub,
        COL_ENABLED, &enabled,
        -1);

    gboolean text_ok = TRUE;
    const char *needle = gtk_entry_get_text(GTK_ENTRY(app.search_entry));
    if (needle && *needle) {
        char *name_lo = name ? g_ascii_strdown(name, -1) : g_strdup("");
        char *ndl_lo  = g_ascii_strdown(needle, -1);
        text_ok = strstr(name_lo, ndl_lo) != NULL;
        g_free(name_lo);
        g_free(ndl_lo);
    }

    gboolean state_ok = TRUE;
    switch (app.current_filter) {
        case FILTER_ACTIVE:   state_ok = g_strcmp0(active, "active")   == 0; break;
        case FILTER_INACTIVE: state_ok = g_strcmp0(active, "inactive") == 0; break;
        case FILTER_FAILED:   state_ok = g_strcmp0(sub,    "failed")   == 0; break;
        case FILTER_ENABLED:  state_ok = enabled;  break;
        case FILTER_DISABLED: state_ok = !enabled; break;
        default: break;
    }

    g_free(name); g_free(active); g_free(sub);
    return text_ok && state_ok;
}

static void on_search_changed(GtkEditable *e, gpointer data)
{
    (void)e; (void)data;
    gtk_tree_model_filter_refilter(app.filter_model);
}

static void on_filter_btn(GtkToggleButton *btn, gpointer user_data)
{
    if (!gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(btn, TRUE);
        return;
    }
    FilterState nf = (FilterState)GPOINTER_TO_INT(user_data);
    for (int i = 0; i < N_FILTERS; i++) {
        if (i != (int)nf) {
            g_signal_handlers_block_matched(app.filter_btns[i],
                G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_filter_btn, NULL);
            gtk_toggle_button_set_active(
                GTK_TOGGLE_BUTTON(app.filter_btns[i]), FALSE);
            g_signal_handlers_unblock_matched(app.filter_btns[i],
                G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_filter_btn, NULL);
        }
    }
    app.current_filter = nf;
    gtk_tree_model_filter_refilter(app.filter_model);
}

static void on_enable_toggled(GtkCellRendererToggle *r, gchar *path_str, gpointer data)
{
    (void)r; (void)data;
    if (!app.is_root) {
        log_append("⚠  Permission denied – run with sudo to enable/disable services.\n");
        return;
    }
    GtkTreePath *sort_path   = gtk_tree_path_new_from_string(path_str);
    GtkTreePath *filter_path = gtk_tree_model_sort_convert_path_to_child_path(
                                    app.sort_model, sort_path);
    GtkTreePath *store_path  = gtk_tree_model_filter_convert_path_to_child_path(
                                    app.filter_model, filter_path);
    gtk_tree_path_free(sort_path);
    gtk_tree_path_free(filter_path);

    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app.store), &it, store_path)) {
        gtk_tree_path_free(store_path);
        return;
    }
    gtk_tree_path_free(store_path);

    gboolean current = FALSE;
    char *name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app.store), &it,
        COL_ENABLED, &current, COL_NAME, &name, -1);
    dispatch_action(name, current ? "disable" : "enable");
    g_free(name);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Build UI
 * ═══════════════════════════════════════════════════════════════════════════ */
static GtkWidget *make_action_btn(const char *label,
                                  const char *action,
                                  const char *tooltip)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(btn, tooltip);
    gtk_widget_set_sensitive(btn, FALSE);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_action_btn), (gpointer)action);
    return btn;
}

static void build_ui(void)
{
    /* Window */
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Systemd Service Manager");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1150, 720);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Header bar – rendered by the desktop theme */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Systemd Service Manager");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),
        app.is_root ? "Full control"
                    : "Read-only  –  run with sudo to make changes");
    gtk_window_set_titlebar(GTK_WINDOW(app.window), header);

    app.btn_reload = gtk_button_new_from_icon_name(
        "view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(app.btn_reload, "Refresh service list");
    g_signal_connect(app.btn_reload, "clicked", G_CALLBACK(on_reload_btn), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), app.btn_reload);

    /* Root vbox */
    GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), root_vbox);

    /* Toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 6);

    app.search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.search_entry), "Filter services…");
    gtk_widget_set_size_request(app.search_entry, 260, -1);
    g_signal_connect(app.search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.search_entry, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);

    /* Filter buttons */
    GtkWidget *fbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (int i = 0; i < N_FILTERS; i++) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(filter_labels[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), i == FILTER_ALL);
        g_signal_connect(btn, "toggled", G_CALLBACK(on_filter_btn), GINT_TO_POINTER(i));
        app.filter_btns[i] = btn;
        gtk_box_pack_start(GTK_BOX(fbox), btn, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(toolbar), fbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_vbox), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* Vertical paned */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_position(GTK_PANED(paned), 420);
    gtk_box_pack_start(GTK_BOX(root_vbox), paned, TRUE, TRUE, 0);

    /* ── Tree view ── */
    app.store = gtk_list_store_new(N_COLUMNS,
        G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING,  G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_INT);

    app.filter_model = GTK_TREE_MODEL_FILTER(
        gtk_tree_model_filter_new(GTK_TREE_MODEL(app.store), NULL));
    gtk_tree_model_filter_set_visible_func(
        app.filter_model, row_visible_func, NULL, NULL);

    app.sort_model = GTK_TREE_MODEL_SORT(
        gtk_tree_model_sort_new_with_model(GTK_TREE_MODEL(app.filter_model)));

    app.tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.sort_model));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app.tree_view), TRUE);
    gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(app.tree_view), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(app.tree_view), FALSE);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), NULL);

    /* Boot toggle column */
    {
        GtkCellRenderer *r = gtk_cell_renderer_toggle_new();
        gtk_cell_renderer_toggle_set_activatable(
            GTK_CELL_RENDERER_TOGGLE(r), app.is_root);
        g_signal_connect(r, "toggled", G_CALLBACK(on_enable_toggled), NULL);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            "Boot", r, "active", COL_ENABLED, NULL);
        gtk_tree_view_column_set_sort_column_id(col, COL_ENABLED);
        gtk_tree_view_column_set_min_width(col, 55);
        gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);
    }

    /* Text columns */
    typedef struct { const char *title; int col; int min_w; gboolean expand; gboolean weight; } ColDef;
    ColDef coldefs[] = {
        { "Service",     COL_NAME,        240, FALSE, FALSE },
        { "Load",        COL_LOAD,         80, FALSE, FALSE },
        { "Active",      COL_ACTIVE,       90, FALSE, TRUE  },
        { "Sub-state",   COL_SUB,          90, FALSE, FALSE },
        { "Description", COL_DESCRIPTION,   0, TRUE,  FALSE },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(coldefs); i++) {
        GtkCellRenderer   *r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *col;
        if (coldefs[i].weight) {
            col = gtk_tree_view_column_new_with_attributes(
                coldefs[i].title, r,
                "text",   coldefs[i].col,
                "weight", COL_ACTIVE_WEIGHT,
                NULL);
        } else {
            col = gtk_tree_view_column_new_with_attributes(
                coldefs[i].title, r,
                "text", coldefs[i].col,
                NULL);
        }
        gtk_tree_view_column_set_sort_column_id(col, coldefs[i].col);
        gtk_tree_view_column_set_resizable(col, TRUE);
        if (coldefs[i].min_w)
            gtk_tree_view_column_set_min_width(col, coldefs[i].min_w);
        gtk_tree_view_column_set_expand(col, coldefs[i].expand);
        gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);
    }

    GtkWidget *tree_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(tree_scroll), app.tree_view);
    gtk_paned_pack1(GTK_PANED(paned), tree_scroll, TRUE, TRUE);

    /* ── Detail pane (bottom) ── */
    GtkWidget *detail_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *abar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(abar), 6);

    app.btn_start   = make_action_btn("▶  Start",   "start",   "Start the service now");
    app.btn_stop    = make_action_btn("■  Stop",    "stop",    "Stop the service now");
    app.btn_restart = make_action_btn("↺  Restart", "restart", "Restart the service");
    app.btn_enable  = make_action_btn("✔  Enable",  "enable",  "Enable at boot");
    app.btn_disable = make_action_btn("✘  Disable", "disable", "Disable at boot");
    app.btn_journal = make_action_btn("📋  Journal", "journal", "Show recent journal entries");

    GtkWidget *act[] = {
        app.btn_start, app.btn_stop, app.btn_restart,
        app.btn_enable, app.btn_disable, app.btn_journal
    };
    for (gsize i = 0; i < G_N_ELEMENTS(act); i++)
        gtk_box_pack_start(GTK_BOX(abar), act[i], FALSE, FALSE, 0);

    app.btn_clear = gtk_button_new_with_label("Clear log");
    gtk_widget_set_tooltip_text(app.btn_clear, "Clear the output log");
    g_signal_connect(app.btn_clear, "clicked", G_CALLBACK(on_clear_btn), NULL);
    gtk_box_pack_end(GTK_BOX(abar), app.btn_clear, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(detail_vbox), abar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(detail_vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* Log text view – monospace, styled by the desktop theme */
    app.log_buf  = gtk_text_buffer_new(NULL);
    app.log_view = gtk_text_view_new_with_buffer(app.log_buf);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app.log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app.log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app.log_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app.log_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app.log_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app.log_view), 6);

    /* "monospace" style class lets the theme pick the right font/colour */
    gtk_style_context_add_class(
        gtk_widget_get_style_context(app.log_view), "monospace");

    app.log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app.log_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(app.log_scroll), app.log_view);
    gtk_box_pack_start(GTK_BOX(detail_vbox), app.log_scroll, TRUE, TRUE, 0);

    gtk_paned_pack2(GTK_PANED(paned), detail_vbox, TRUE, TRUE);

    /* Status bar */
    app.statusbar     = gtk_statusbar_new();
    app.statusbar_ctx = gtk_statusbar_get_context_id(
        GTK_STATUSBAR(app.statusbar), "main");
    gtk_box_pack_start(GTK_BOX(root_vbox), app.statusbar, FALSE, FALSE, 0);

    gtk_widget_show_all(app.window);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);
    memset(&app, 0, sizeof(app));
    app.is_root        = (geteuid() == 0);
    app.current_filter = FILTER_ALL;
    build_ui();
    g_idle_add((GSourceFunc)load_services, NULL);
    gtk_main();
    return 0;
}
