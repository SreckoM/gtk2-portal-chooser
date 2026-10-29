/*
 * gtk2_portal_chooser.so -- LD_PRELOAD shim that replaces GTK2 file chooser
 * dialogs with the xdg-desktop-portal FileChooser (whatever backend the
 * portal config prefers, e.g. Strata).
 *
 * Intercepts gtk_file_chooser_dialog_new + setters, gtk_dialog_run and
 * gtk_file_chooser_get_filename(s). Custom filters (gtk_file_filter_add_custom)
 * are probed with a list of known extensions and turned into glob filters.
 * If the portal call fails, the real GTK2 dialog is shown instead.
 *
 * Save dialogs without a file name get one from the app's window title
 * ("scene.3b* - 3DCoat ..." -> "scene.3b"), with the extension switched to one
 * the dialog's filter accepts.
 *
 * Env: GTK2_PORTAL_DEBUG=1 logs to stderr (or to the file GTK2_PORTAL_LOG),
 * GTK2_PORTAL_DISABLE=1 bypasses.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>

typedef void GtkWidget;
typedef void GtkWindow;
typedef void GtkDialog;
typedef void GtkFileChooser;
typedef void GtkFileFilter;

typedef struct {
    int contains;
    const gchar *filename;
    const gchar *uri;
    const gchar *display_name;
    const gchar *mime_type;
} FilterInfo;
typedef gboolean (*FilterFunc)(const FilterInfo *, gpointer);

enum { ACT_OPEN, ACT_SAVE, ACT_SELECT_FOLDER, ACT_CREATE_FOLDER };
#define RESP_NONE (-1)
#define RESP_CANCEL (-6)
#define RESP_DELETE_EVENT (-4)

typedef struct {
    int action;
    char *title;
    GtkWindow *parent;
    char *folder;
    char *filename;
    gboolean multiple;
    int accept_id;
    GtkFileFilter *filter;
    GSList *results; /* char* paths */
    gboolean has_results;
} Chooser;

typedef struct {
    FilterFunc func;
    gpointer data;
} CustomFilter;

static GHashTable *choosers; /* GtkWidget* -> Chooser* */
static GHashTable *filters;  /* GtkFileFilter* -> GSList* of CustomFilter* */
static gboolean debug_on;
static FILE *log_fp;
static char *app_title; /* last title set on a non-chooser window */

#define DBG(...) do { if (debug_on) { fprintf(log_fp, "[gtk2-portal] " __VA_ARGS__); fputc('\n', log_fp); fflush(log_fp); } } while (0)

#define REAL(ret, name, ...) \
    static ret (*real_fn)(__VA_ARGS__); \
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, name)

static void init_once(void)
{
    if (choosers)
        return;
    choosers = g_hash_table_new(g_direct_hash, g_direct_equal);
    filters = g_hash_table_new(g_direct_hash, g_direct_equal);
    const char *log = g_getenv("GTK2_PORTAL_LOG");
    log_fp = log ? fopen(log, "a") : NULL;
    if (!log_fp)
        log_fp = stderr;
    debug_on = g_getenv("GTK2_PORTAL_DEBUG") != NULL || log_fp != stderr;
}

static Chooser *lookup(gpointer w)
{
    init_once();
    return g_hash_table_lookup(choosers, w);
}

static void chooser_free(Chooser *c)
{
    g_free(c->title);
    g_free(c->folder);
    g_free(c->filename);
    g_slist_free_full(c->results, g_free);
    g_free(c);
}

/* ---------- dialog construction ---------- */

GtkWidget *gtk_file_chooser_dialog_new(const gchar *title, GtkWindow *parent, int action,
                                       const gchar *first_button_text, ...)
{
    REAL(GtkWidget *, "gtk_file_chooser_dialog_new", const gchar *, GtkWindow *, int, const gchar *, ...);
    static GtkWidget *(*add_button)(GtkDialog *, const gchar *, int);
    if (!add_button)
        add_button = dlsym(RTLD_DEFAULT, "gtk_dialog_add_button");
    init_once();

    GtkWidget *w = real_fn(title, parent, action, NULL);
    Chooser *c = g_new0(Chooser, 1);
    c->action = action;
    c->title = g_strdup(title ? title : "");
    c->parent = parent;
    c->accept_id = -3; /* GTK_RESPONSE_ACCEPT */

    va_list ap;
    va_start(ap, first_button_text);
    gboolean found_accept = FALSE;
    for (const gchar *text = first_button_text; text; text = va_arg(ap, const gchar *)) {
        int id = va_arg(ap, int);
        add_button(w, text, id);
        /* ACCEPT, OK, YES, APPLY */
        if (!found_accept && (id == -3 || id == -5 || id == -8 || id == -10)) {
            c->accept_id = id;
            found_accept = TRUE;
        }
    }
    va_end(ap);

    g_hash_table_insert(choosers, w, c);
    DBG("new dialog %p action=%d title='%s' accept=%d", w, action, c->title, c->accept_id);
    return w;
}

gboolean gtk_file_chooser_set_current_folder(GtkFileChooser *ch, const gchar *folder)
{
    REAL(gboolean, "gtk_file_chooser_set_current_folder", GtkFileChooser *, const gchar *);
    Chooser *c = lookup(ch);
    if (c) {
        g_free(c->folder);
        c->folder = g_strdup(folder);
        DBG("set_current_folder '%s'", folder ? folder : "(null)");
    }
    return real_fn(ch, folder);
}

gboolean gtk_file_chooser_set_filename(GtkFileChooser *ch, const char *filename)
{
    REAL(gboolean, "gtk_file_chooser_set_filename", GtkFileChooser *, const char *);
    Chooser *c = lookup(ch);
    if (c) {
        g_free(c->filename);
        c->filename = g_strdup(filename);
        DBG("set_filename '%s'", filename ? filename : "(null)");
    }
    return real_fn(ch, filename);
}

void gtk_file_chooser_set_select_multiple(GtkFileChooser *ch, gboolean multiple)
{
    REAL(void, "gtk_file_chooser_set_select_multiple", GtkFileChooser *, gboolean);
    Chooser *c = lookup(ch);
    if (c)
        c->multiple = multiple;
    real_fn(ch, multiple);
}

void gtk_file_chooser_set_filter(GtkFileChooser *ch, GtkFileFilter *filter)
{
    REAL(void, "gtk_file_chooser_set_filter", GtkFileChooser *, GtkFileFilter *);
    Chooser *c = lookup(ch);
    if (c)
        c->filter = filter;
    real_fn(ch, filter);
}

void gtk_file_filter_add_custom(GtkFileFilter *filter, int needed, FilterFunc func,
                                gpointer data, GDestroyNotify notify)
{
    REAL(void, "gtk_file_filter_add_custom", GtkFileFilter *, int, FilterFunc, gpointer, GDestroyNotify);
    init_once();
    CustomFilter *cf = g_new0(CustomFilter, 1);
    cf->func = func;
    cf->data = data;
    GSList *l = g_hash_table_lookup(filters, filter);
    g_hash_table_insert(filters, filter, g_slist_append(l, cf));
    real_fn(filter, needed, func, data, notify);
}

void gtk_window_set_title(GtkWindow *window, const gchar *title)
{
    REAL(void, "gtk_window_set_title", GtkWindow *, const gchar *);
    if (!lookup(window) && title && *title) {
        g_free(app_title);
        app_title = g_strdup(title);
    }
    real_fn(window, title);
}

/* "scene.3b* - 3DCoat 2026.25" -> "scene.3b"; NULL if the title has no file name */
static char *name_from_title(void)
{
    if (!app_title)
        return NULL;
    const char *sep = strstr(app_title, " - ");
    char *part = sep ? g_strndup(app_title, sep - app_title) : g_strdup(app_title);
    g_strstrip(part);
    while (*part && part[strlen(part) - 1] == '*')
        part[strlen(part) - 1] = '\0';
    char *name = g_path_get_basename(g_strstrip(part[0] == '*' ? part + 1 : part));
    g_free(part);
    if (!strchr(name, '.') || name[0] == '.') {
        g_free(name);
        return NULL;
    }
    return name;
}

/* ---------- results ---------- */

gchar *gtk_file_chooser_get_filename(GtkFileChooser *ch)
{
    REAL(gchar *, "gtk_file_chooser_get_filename", GtkFileChooser *);
    Chooser *c = lookup(ch);
    if (c && c->has_results)
        return c->results ? g_strdup(c->results->data) : NULL;
    return real_fn(ch);
}

GSList *gtk_file_chooser_get_filenames(GtkFileChooser *ch)
{
    REAL(GSList *, "gtk_file_chooser_get_filenames", GtkFileChooser *);
    Chooser *c = lookup(ch);
    if (c && c->has_results)
        return g_slist_copy_deep(c->results, (GCopyFunc)g_strdup, NULL);
    return real_fn(ch);
}

void gtk_widget_destroy(GtkWidget *w)
{
    REAL(void, "gtk_widget_destroy", GtkWidget *);
    Chooser *c = lookup(w);
    if (c) {
        g_hash_table_remove(choosers, w);
        chooser_free(c);
    }
    real_fn(w);
}

/* ---------- filter probing ---------- */

static const char *probe_exts[] = {
    /* 3DCoat native / scenes */
    "3b", "3dc", "3dcpack", "3dctool", "3dcsmart", "3dcmat", "3dcbrush", "3dcmask",
    "3dcstencil", "3dcpattern", "3dcshader", "3dcalpha", "3dcbrushes",
    /* meshes */
    "obj", "fbx", "lwo", "lws", "stl", "ply", "dae", "abc", "usd", "usda", "usdc", "usdz",
    "gltf", "glb", "3ds", "blend", "ma", "mb", "max", "c4d", "x", "dxf", "iges", "igs",
    "step", "stp", "off", "ase", "ms3d", "md2", "md3", "b3d", "ifc", "wrl", "vrml", "x3d",
    "xyz", "pts", "pcd", "e57", "las", "vox", "mesh", "nif", "smd", "bvh", "ztl", "zpr",
    "lxo", "hip", "bgeo", "vdb", "skp", "3mf", "amf", "dff", "m3d", "mdl", "irr",
    /* images */
    "png", "jpg", "jpeg", "tga", "tif", "tiff", "bmp", "exr", "hdr", "psd", "dds", "webp",
    "gif", "tx", "svg", "pfm", "ico", "pcx", "ppm", "pgm", "pbm", "jp2", "heic", "avif",
    "ktx", "ktx2", "cr2", "nef", "dng", "raw", "rgb", "sgi", "iff", "pic",
    /* misc */
    "txt", "xml", "json", "py", "nut", "lua", "cpp", "h", "zip", "7z", "rar", "csv",
    "mtl", "pdf", "html", "ini", "cfg", "lut", "cube", "ttf", "otf", "pal", "aco", "ase",
    "fx", "glsl", "hlsl", "spp", "sbsar", "mat", "pat", "abr", "gih", "gbr", "mp4", "avi",
    "mov", "wav", "mp3", "sh", "exe",
    NULL
};

static gboolean filter_accepts(GSList *cfs, const char *name)
{
    char *dir = g_get_current_dir();
    char *path = g_build_filename(dir, name, NULL);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    FilterInfo info = { 1 | 2 | 4 | 8, path, uri, name, "application/octet-stream" };
    gboolean ok = FALSE;
    for (GSList *l = cfs; l && !ok; l = l->next) {
        CustomFilter *cf = l->data;
        ok = cf->func(&info, cf->data);
    }
    g_free(uri);
    g_free(path);
    g_free(dir);
    return ok;
}

/* Returns a floating a(sa(us)) filter list, or NULL if no usable filter. */
static GVariant *build_filters(Chooser *c, GVariant **current, char **first_ext)
{
    *current = NULL;
    *first_ext = NULL;
    if (!c->filter)
        return NULL;
    GSList *cfs = g_hash_table_lookup(filters, c->filter);
    if (!cfs)
        return NULL;
    /* a filter that also takes nonsense is effectively "all files" */
    if (filter_accepts(cfs, "zz_probe.qqzx") || filter_accepts(cfs, "zz_probe"))
        return NULL;

    GVariantBuilder globs;
    g_variant_builder_init(&globs, G_VARIANT_TYPE("a(us)"));
    GString *label = g_string_new(NULL);
    int n = 0;
    for (int i = 0; probe_exts[i]; i++) {
        const char *e = probe_exts[i];
        char *lower = g_strdup_printf("probe.%s", e);
        char *upper_ext = g_ascii_strup(e, -1);
        char *upper = g_strdup_printf("probe.%s", upper_ext);
        gboolean lo = filter_accepts(cfs, lower), up = filter_accepts(cfs, upper);
        if (lo || up) {
            char *gl = g_strdup_printf("*.%s", e);
            char *gu = g_strdup_printf("*.%s", upper_ext);
            g_variant_builder_add(&globs, "(us)", 0, gl);
            g_variant_builder_add(&globs, "(us)", 0, gu);
            g_string_append_printf(label, "%s%s", n ? ", " : "", e);
            if (!n)
                *first_ext = g_strdup(e);
            n++;
            g_free(gl);
            g_free(gu);
        }
        g_free(lower);
        g_free(upper);
        g_free(upper_ext);
    }
    DBG("probed filter: %s", n ? label->str : "(nothing matched)");
    if (!n) {
        g_variant_builder_clear(&globs);
        g_string_free(label, TRUE);
        return NULL;
    }
    char *name = g_strdup_printf("Supported (%s)", label->str);
    GVariant *mine = g_variant_ref_sink(g_variant_new("(s@a(us))", name, g_variant_builder_end(&globs)));
    GVariant *all = g_variant_new("(s@a(us))", "All files",
                                  g_variant_new_parsed("[(uint32 0, '*')]"));
    GVariant *items[] = { mine, all };
    GVariant *list = g_variant_new_array(G_VARIANT_TYPE("(sa(us))"), items, 2);
    *current = mine; /* caller adds it as current_filter, then unrefs */
    g_free(name);
    g_string_free(label, TRUE);
    return list;
}

/* ---------- portal call ---------- */

typedef struct {
    GMainLoop *loop;
    guint32 response;
    GVariant *results;
    gboolean done;
} Wait;

static void on_response(GDBusConnection *conn, const gchar *sender, const gchar *path,
                        const gchar *iface, const gchar *signal, GVariant *params, gpointer ud)
{
    Wait *w = ud;
    g_variant_get(params, "(u@a{sv})", &w->response, &w->results);
    w->done = TRUE;
    g_main_loop_quit(w->loop);
}

static char *parent_handle(GtkWindow *parent)
{
    if (!parent)
        return g_strdup("");
    gpointer (*get_window)(GtkWidget *) = dlsym(RTLD_DEFAULT, "gtk_widget_get_window");
    gulong (*get_xid)(gpointer) = dlsym(RTLD_DEFAULT, "gdk_x11_drawable_get_xid");
    gpointer gdkwin = get_window ? get_window(parent) : NULL;
    if (!gdkwin || !get_xid)
        return g_strdup("");
    return g_strdup_printf("x11:%lx", get_xid(gdkwin));
}

static void add_bytestring(GVariantBuilder *b, const char *key, const char *val)
{
    g_variant_builder_add(b, "{sv}", key, g_variant_new_bytestring(val));
}

/* Returns TRUE if the portal handled the dialog; *resp gets the GTK response id. */
static gboolean run_portal(Chooser *c, int *resp)
{
    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        DBG("no session bus: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    static int counter;
    char *token = g_strdup_printf("gtk2portal%d_%d", getpid(), ++counter);
    char *sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
    for (char *p = sender; *p; p++)
        if (*p == '.')
            *p = '_';
    char *req_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

    GMainContext *ctx = g_main_context_default();
    Wait w = { g_main_loop_new(ctx, FALSE), 2, NULL, FALSE };
    guint sub = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.portal.Desktop",
                                                   "org.freedesktop.portal.Request", "Response",
                                                   req_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                   on_response, &w, NULL);
    /* NO_MATCH_RULE needs an explicit match */
    char *rule = g_strdup_printf("type='signal',sender='org.freedesktop.portal.Desktop',"
                                 "interface='org.freedesktop.portal.Request',member='Response',path='%s'",
                                 req_path);
    g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                "org.freedesktop.DBus", "AddMatch", g_variant_new("(s)", rule),
                                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(TRUE));

    gboolean save = c->action == ACT_SAVE;
    const char *method = save ? "SaveFile" : "OpenFile";
    if (c->action == ACT_SELECT_FOLDER || c->action == ACT_CREATE_FOLDER)
        g_variant_builder_add(&opts, "{sv}", "directory", g_variant_new_boolean(TRUE));
    if (c->multiple && !save)
        g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(TRUE));

    GVariant *current = NULL;
    char *first_ext = NULL;
    GVariant *flist = build_filters(c, &current, &first_ext);

    char *folder = c->folder ? g_strdup(c->folder) : NULL;
    char *name = NULL;
    if (c->filename && g_file_test(c->filename, G_FILE_TEST_IS_DIR)) {
        if (!folder)
            folder = g_strdup(c->filename);
    } else if (c->filename) {
        if (save)
            name = g_path_get_basename(c->filename);
        if (!folder && g_path_is_absolute(c->filename))
            folder = g_path_get_dirname(c->filename);
    }
    if (save && !name && (name = name_from_title())) {
        /* e.g. "scene.3b" in an OBJ export dialog -> "scene.obj" */
        GSList *cfs = c->filter ? g_hash_table_lookup(filters, c->filter) : NULL;
        if (cfs && first_ext && !filter_accepts(cfs, name)) {
            char *dot = strrchr(name, '.');
            *dot = '\0';
            char *renamed = g_strdup_printf("%s.%s", name, first_ext);
            g_free(name);
            name = renamed;
        }
        DBG("name from window title '%s' -> '%s'", app_title, name);
    }
    if (name)
        g_variant_builder_add(&opts, "{sv}", "current_name", g_variant_new_string(name));
    if (folder && g_file_test(folder, G_FILE_TEST_IS_DIR))
        add_bytestring(&opts, "current_folder", folder);

    if (flist) {
        g_variant_builder_add(&opts, "{sv}", "filters", flist);
        g_variant_builder_add(&opts, "{sv}", "current_filter", current);
        g_variant_unref(current);
    }

    char *parent = parent_handle(c->parent);
    DBG("%s title='%s' folder='%s' name='%s' set_filename='%s' parent='%s'", method, c->title,
        folder ? folder : "", name ? name : "", c->filename ? c->filename : "", parent);
    GVariant *ret = g_dbus_connection_call_sync(
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser", method,
        g_variant_new("(ss@a{sv})", parent, c->title, g_variant_builder_end(&opts)),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

    gboolean handled = FALSE;
    if (!ret) {
        DBG("portal call failed: %s", err->message);
        g_clear_error(&err);
    } else {
        g_variant_unref(ret);
        if (!w.done)
            g_main_loop_run(w.loop);
        handled = TRUE;

        g_slist_free_full(c->results, g_free);
        c->results = NULL;
        if (w.response == 0 && w.results) {
            const char **uris = NULL;
            if (g_variant_lookup(w.results, "uris", "^a&s", &uris)) {
                for (int i = 0; uris[i]; i++) {
                    char *p = g_filename_from_uri(uris[i], NULL, NULL);
                    DBG("result: %s", uris[i]);
                    if (p)
                        c->results = g_slist_append(c->results, p);
                }
                g_free(uris);
            }
        }
        c->has_results = TRUE;
        *resp = c->results ? c->accept_id : RESP_CANCEL;
        DBG("response=%u -> gtk %d", w.response, *resp);
    }

    if (w.results)
        g_variant_unref(w.results);
    g_dbus_connection_signal_unsubscribe(bus, sub);
    g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "RemoveMatch", g_variant_new("(s)", rule),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_main_loop_unref(w.loop);
    g_free(parent);
    g_free(folder);
    g_free(name);
    g_free(first_ext);
    g_free(rule);
    g_free(req_path);
    g_free(sender);
    g_free(token);
    g_object_unref(bus);
    return handled;
}

int gtk_dialog_run(GtkDialog *dialog)
{
    REAL(int, "gtk_dialog_run", GtkDialog *);
    Chooser *c = lookup(dialog);
    if (c && !g_getenv("GTK2_PORTAL_DISABLE")) {
        int resp;
        if (run_portal(c, &resp))
            return resp;
        DBG("falling back to GTK2 dialog");
    }
    return real_fn(dialog);
}
