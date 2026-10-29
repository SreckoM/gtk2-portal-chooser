/*
 * Minimal GTK2 file chooser, set up the way 3DCoat does it (custom filter,
 * gtk_dialog_run). Used to check the shim without starting 3DCoat.
 *
 *   ./test_chooser          Open dialog
 *   ./test_chooser save     Save dialog with a default file name
 *
 * Declares the few GTK2 functions it needs, so no GTK2 headers are required.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int contains;
    const char *filename, *uri, *display_name, *mime_type;
} Info;

void gtk_init(int *, char ***);
void *gtk_file_chooser_dialog_new(const char *, void *, int, const char *, ...);
void *gtk_file_filter_new(void);
void gtk_file_filter_add_custom(void *, int, int (*)(const Info *, void *), void *, void *);
void gtk_file_chooser_set_filter(void *, void *);
int gtk_file_chooser_set_current_folder(void *, const char *);
int gtk_file_chooser_set_filename(void *, const char *);
int gtk_dialog_run(void *);
char *gtk_file_chooser_get_filename(void *);
void gtk_widget_destroy(void *);

static int only_3d(const Info *info, void *data)
{
    const char *e = strrchr(info->filename, '.');
    return e && (!strcasecmp(e, ".obj") || !strcasecmp(e, ".fbx") || !strcasecmp(e, ".3b"));
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    int save = argc > 1 && !strcmp(argv[1], "save");
    const char *home = getenv("HOME");

    void *d = gtk_file_chooser_dialog_new(save ? "Test save" : "Test open", NULL, save,
                                          "Cancel", -6, save ? "Save" : "Open", -3, NULL);
    void *filter = gtk_file_filter_new();
    gtk_file_filter_add_custom(filter, 1, only_3d, NULL, NULL);
    gtk_file_chooser_set_filter(d, filter);
    gtk_file_chooser_set_current_folder(d, home);
    if (save) {
        char path[4096];
        snprintf(path, sizeof path, "%s/untitled.3b", home);
        gtk_file_chooser_set_filename(d, path);
    }

    int r = gtk_dialog_run(d);
    char *fn = gtk_file_chooser_get_filename(d);
    printf("response=%d file=%s\n", r, fn ? fn : "(none)");
    gtk_widget_destroy(d);
    return 0;
}
