#include "viply.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>

void viply_free_ptr(void *p) {
    void **ptr = (void **)p;
    if (*ptr) free(*ptr);
}

void viply_unref_vips(void *p) {
    GObject **ptr = (GObject **)p;
    if (*ptr) g_object_unref(*ptr);
}

int viply_process_file(const ViplyOptions *opts, const char *input_path, const char *output_path) {
    autovips VipsImage *in = vips_image_new_from_file(input_path, NULL);
    if (!in) return -1;

    autovips VipsImage *processed = NULL;
    
    // Resolution scaling
    if (opts->resolution < 100) {
        double scale = opts->resolution / 100.0;
        if (vips_resize(in, &processed, scale, NULL)) return -1;
    } else {
        processed = in;
        g_object_ref(processed);
    }

    // Determine format
    bool to_jpeg = opts->use_jpeg;
    // Auto-detect from output extension if not forced
    if (!to_jpeg && output_path) {
        if (strstr(output_path, ".jpg") || strstr(output_path, ".jpeg")) {
            to_jpeg = true;
        }
    }

    int result = 0;
    if (to_jpeg) {
        result = vips_jpegsave(processed, output_path,
            "Q", opts->quality,
            "strip", opts->strip,
            "optimize_coding", TRUE,
            NULL);
    } else {
        // Default WebP
        result = vips_webpsave(processed, output_path,
            "Q", opts->quality,
            "effort", opts->compression,
            "strip", opts->strip,
            NULL);
    }

    if (result == 0 && opts->keep_date) {
        struct stat st;
        if (stat(input_path, &st) == 0) {
            struct utimbuf new_times;
            new_times.actime = st.st_atime;
            new_times.modtime = st.st_mtime;
            utime(output_path, &new_times);
        }
    }

    return result;
}
