#include "viply.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <string.h>

void viply_free_ptr(void *p) {
    void **ptr = (void **)p;
    if (*ptr) free(*ptr);
}

void viply_unref_vips(void *p) {
    GObject **ptr = (GObject **)p;
    if (*ptr) g_object_unref(*ptr);
}

static void eval_cb(VipsImage *image, VipsProgress *progress, _Atomic int *p_val) {
    (void)image;
    if (p_val) {
        atomic_store(p_val, progress->percent);
    }
}

long viply_process_file(const ViplyOptions *opts, const char *input_path, const char *output_path, _Atomic int *progress) {
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

    // Enable progress tracking
    vips_image_set_progress(processed, TRUE);
    if (progress) {
        atomic_store(progress, 0);
        g_signal_connect(processed, "eval", G_CALLBACK(eval_cb), progress);
    }

    // Determine format
    bool to_jpeg = opts->use_jpeg;
    if (!to_jpeg && output_path) {
        if (strstr(output_path, ".jpg") || strstr(output_path, ".jpeg")) {
            to_jpeg = true;
        }
    }

    if (opts->dry_run) {
        void *buf;
        size_t len;
        int result;
        if (to_jpeg) {
            result = vips_jpegsave_buffer(processed, &buf, &len, "Q", opts->quality, "strip", opts->strip, "optimize_coding", TRUE, NULL);
        } else {
            result = vips_webpsave_buffer(processed, &buf, &len, "Q", opts->quality, "effort", opts->compression, "strip", opts->strip, NULL);
        }
        if (result == 0) {
            g_free(buf);
            return (long)len;
        }
        return -1;
    }

    int result = 0;
    if (to_jpeg) {
        result = vips_jpegsave(processed, output_path,
            "Q", opts->quality,
            "strip", opts->strip,
            "optimize_coding", TRUE,
            NULL);
    } else {
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

    return result == 0 ? 0 : -1;
}
