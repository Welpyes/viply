#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include "viply.h"
#include "utils.h"

void print_usage(const char *prog) {
    printf("Usage: %s [options] <input> [output]\n", prog);
    printf("Options:\n");
    printf("  --jpeg             Use JPEG instead of WebP\n");
    printf("  -j, --jobs N       Parallel jobs (default: nproc/2)\n");
    printf("  -q, --quality N    Quality 0-100 (default: 80)\n");
    printf("  -c, --compression N Effort 1-6 (default: 4)\n");
    printf("  -s, --strip        Strip metadata\n");
    printf("  -d, --date         Keep file dates\n");
    printf("  -r, --resolution N Scale percentage (default: 100)\n");
    printf("  -R, --recursive    Recursive directory scan\n");
    printf("  --dry-run          Show potential savings without writing\n");
}

typedef struct {
    char *in;
    char *out;
} FileJob;

typedef struct {
    const ViplyOptions *opts;
    FileJob *jobs;
    int total_jobs;
    atomic_int *current_idx;
    atomic_long *total_in_size;
    atomic_long *total_out_size;
} ThreadData;

void draw_progress(int current, int total) {
    int width = 40;
    float ratio = (float)current / total;
    int pos = width * ratio;
    printf("\r[");
    for (int i = 0; i < width; i++) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %d%% (%d/%d)", (int)(ratio * 100), current, total);
    fflush(stdout);
}

void *worker(void *arg) {
    ThreadData *td = (ThreadData *)arg;
    while (1) {
        int idx = atomic_fetch_add(td->current_idx, 1);
        if (idx >= td->total_jobs) break;

        FileJob *job = &td->jobs[idx];
        
        struct stat st;
        if (stat(job->in, &st) == 0) {
            atomic_fetch_add(td->total_in_size, st.st_size);
        }

        long res = viply_process_file(td->opts, job->in, job->out);
        if (res >= 0) {
            if (td->opts->dry_run) {
                atomic_fetch_add(td->total_out_size, res);
            } else {
                struct stat out_st;
                if (stat(job->out, &out_st) == 0) {
                    atomic_fetch_add(td->total_out_size, out_st.st_size);
                }
            }
        }
        
        // Progress update by one thread
        if (idx % 5 == 0 || idx == td->total_jobs - 1) {
            draw_progress(idx + 1, td->total_jobs);
        }
    }
    return NULL;
}

void scan_directory(const char *in_dir, const char *out_dir, const ViplyOptions *opts, FileJob **jobs, int *count, int *capacity) {
    DIR *d = opendir(in_dir);
    if (!d) return;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;

        char *full_in = path_join(in_dir, dir->d_name);
        
        if (dir->d_type == DT_DIR && opts->recursive) {
            char *full_out = path_join(out_dir, dir->d_name);
            if (!opts->dry_run) mkdir(full_out, 0755);
            scan_directory(full_in, full_out, opts, jobs, count, capacity);
            free(full_in);
            free(full_out);
        } else if (dir->d_type == DT_REG) {
            if (strstr(dir->d_name, ".jpg") || strstr(dir->d_name, ".jpeg") || 
                strstr(dir->d_name, ".png") || strstr(dir->d_name, ".webp")) {
                
                if (*count >= *capacity) {
                    *capacity *= 2;
                    *jobs = realloc(*jobs, *capacity * sizeof(FileJob));
                }

                char *name = get_filename_without_ext(dir->d_name);
                const char *ext = opts->use_jpeg ? ".jpg" : ".webp";
                char *out_name = malloc(strlen(name) + strlen(ext) + 1);
                sprintf(out_name, "%s%s", name, ext);
                
                (*jobs)[*count].in = full_in;
                (*jobs)[*count].out = path_join(out_dir, out_name);
                (*count)++;
                
                free(name);
                free(out_name);
            } else {
                free(full_in);
            }
        } else {
            free(full_in);
        }
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (VIPS_INIT(argv[0])) return 1;

    ViplyOptions opts = {
        .use_jpeg = false,
        .jobs = sysconf(_SC_NPROCESSORS_ONLN) / 2,
        .quality = 80,
        .compression = 4,
        .strip = false,
        .keep_date = false,
        .resolution = 100,
        .recursive = false,
        .dry_run = false
    };

    static struct option long_options[] = {
        {"jpeg", no_argument, 0, 'f'},
        {"jobs", required_argument, 0, 'j'},
        {"quality", required_argument, 0, 'q'},
        {"compression", required_argument, 0, 'c'},
        {"strip", no_argument, 0, 's'},
        {"date", no_argument, 0, 'd'},
        {"resolution", required_argument, 0, 'r'},
        {"recursive", no_argument, 0, 'R'},
        {"dry-run", no_argument, 0, 'D'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "j:q:c:sdr:RD", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f': opts.use_jpeg = true; break;
            case 'j': opts.jobs = atoi(optarg); break;
            case 'q': opts.quality = atoi(optarg); break;
            case 'c': opts.compression = atoi(optarg); break;
            case 's': opts.strip = true; break;
            case 'd': opts.keep_date = true; break;
            case 'r': opts.resolution = atoi(optarg); break;
            case 'R': opts.recursive = true; break;
            case 'D': opts.dry_run = true; break;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    char *input = argv[optind];
    char *output = (optind + 1 < argc) ? argv[optind + 1] : NULL;

    FileJob *jobs = NULL;
    int total_jobs = 0;

    if (is_directory(input)) {
        if (!output) {
            fprintf(stderr, "Error: Output directory required for directory input\n");
            return 1;
        }
        if (!opts.dry_run) mkdir(output, 0755);

        int capacity = 100;
        jobs = malloc(capacity * sizeof(FileJob));
        scan_directory(input, output, &opts, &jobs, &total_jobs, &capacity);
    } else {
        total_jobs = 1;
        jobs = malloc(sizeof(FileJob));
        jobs[0].in = strdup(input);
        if (output) {
            jobs[0].out = strdup(output);
        } else {
            char *base = get_filename_without_ext(input);
            const char *ext = opts.use_jpeg ? ".jpg" : ".webp";
            char *alloc_out = malloc(strlen("compressed-") + strlen(base) + strlen(ext) + 1);
            sprintf(alloc_out, "compressed-%s%s", base, ext);
            jobs[0].out = alloc_out;
            free(base);
        }
    }

    if (total_jobs > 0) {
        vips_concurrency_set(1);
        atomic_int current_idx = 0;
        atomic_long total_in_size = 0;
        atomic_long total_out_size = 0;

        pthread_t *threads = malloc(opts.jobs * sizeof(pthread_t));
        ThreadData td = {&opts, jobs, total_jobs, &current_idx, &total_in_size, &total_out_size};

        for (int i = 0; i < opts.jobs; i++) {
            pthread_create(&threads[i], NULL, worker, &td);
        }
        for (int i = 0; i < opts.jobs; i++) {
            pthread_join(threads[i], NULL);
        }
        printf("\nDone.\n");

        if (total_in_size > 0) {
            double saved = (double)(total_in_size - total_out_size) / total_in_size * 100.0;
            printf("Summary: %.2fMB -> %.2fMB (Saved %.1f%%)\n", 
                (double)total_in_size / 1024 / 1024, 
                (double)total_out_size / 1024 / 1024, 
                saved);
        }

        free(threads);
        for (int i = 0; i < total_jobs; i++) {
            free(jobs[i].in);
            free(jobs[i].out);
        }
        free(jobs);
    }

    vips_shutdown();
    return 0;
}
