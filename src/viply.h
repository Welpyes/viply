#ifndef VIPLY_H
#define VIPLY_H

#include <vips/vips.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef struct {
    bool use_jpeg;
    int jobs;
    int quality;
    int compression; // effort
    bool strip;
    bool keep_date;
    int resolution; // percentage
    bool recursive;
    bool dry_run;
    char *input;
    char *output;
} ViplyOptions;

// Cleanup macros
#define autofree __attribute__((cleanup(viply_free_ptr)))
#define autovips __attribute__((cleanup(viply_unref_vips)))

void viply_free_ptr(void *p);
void viply_unref_vips(void *p);

// Returns size in bytes if dry_run, 0 on success, -1 on error
// progress is 0-100 atomic int
long viply_process_file(const ViplyOptions *opts, const char *input_path, const char *output_path, _Atomic int *progress);

#endif
