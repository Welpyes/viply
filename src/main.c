#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <libgen.h>
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
    atomic_int *finished_count;
    atomic_long *total_in_size;
    atomic_long *total_out_size;
    char **active_files; // Array of size 'jobs' to track active filenames
    pthread_mutex_t ui_mutex;
} ThreadData;

void draw_ui(ThreadData *td, int worker_id, const char *filename, bool finished) {
    pthread_mutex_lock(&td->ui_mutex);

    int total = td->total_jobs;
    int finished_count = atomic_load(td->finished_count);
    
    // 1. Clear active zone and progress bar
    // Move up (jobs + 3) lines: 'jobs' for active + 1 for header + 1 for separator + 1 for progress bar
    printf("\033[%dA", td->opts->jobs + 3);
    
    // 2. If finished, print to scrolling "Completed" section
    if (finished) {
        printf("\r\033[2K%-50s [COMPLETE]\n", filename);
    } else {
        // Just maintain the line if starting
    }

    // 3. Active Header
    printf("\033[2K--- COMPRESSING ---\n");

    // 4. Print "Active" section
    if (filename) {
        if (finished) td->active_files[worker_id] = NULL;
        else td->active_files[worker_id] = (char *)filename;
    }

    for (int i = 0; i < td->opts->jobs; i++) {
        printf("\033[2K"); // Clear line
        if (td->active_files[i]) {
            printf("  %-50s\n", td->active_files[i]);
        } else {
            printf("  --\n");
        }
    }

    // 5. Separator
    printf("\033[2K------------------------------------------------------------\n");

    // 6. Progress Bar
    int width = 40;
    float ratio = (float)finished_count / total;
    int pos = width * ratio;
    printf("\033[2K[");
    for (int i = 0; i < width; i++) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %d%% (%d/%d)", (int)(ratio * 100), finished_count, total);
    printf("\n");

    pthread_mutex_unlock(&td->ui_mutex);
}

void *worker(void *arg) {
    void **args = (void **)arg;
    ThreadData *td = (ThreadData *)args[0];
    int worker_id = (int)(size_t)args[1];
    free(arg);

    while (1) {
        int idx = atomic_fetch_add(td->current_idx, 1);
        if (idx >= td->total_jobs) break;

        FileJob *job = &td->jobs[idx];
        char *fname = basename(job->in);

        draw_ui(td, worker_id, fname, false);

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

        atomic_fetch_add(td->finished_count, 1);
        draw_ui(td, worker_id, fname, true);
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
        atomic_int finished_count = 0;
        atomic_long total_in_size = 0;
        atomic_long total_out_size = 0;

        char **active_files = calloc(opts.jobs, sizeof(char *));
        pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
        
        ThreadData td = {&opts, jobs, total_jobs, &current_idx, &finished_count, &total_in_size, &total_out_size, active_files, ui_mutex};

        // Prep UI space
        for (int i = 0; i < opts.jobs + 3; i++) printf("\n");

        pthread_t *threads = malloc(opts.jobs * sizeof(pthread_t));
        for (int i = 0; i < opts.jobs; i++) {
            void **args = malloc(2 * sizeof(void *));
            args[0] = &td;
            args[1] = (void *)(size_t)i;
            pthread_create(&threads[i], NULL, worker, args);
        }
        for (int i = 0; i < opts.jobs; i++) {
            pthread_join(threads[i], NULL);
        }

        if (total_in_size > 0) {
            double saved = (double)(total_in_size - total_out_size) / total_in_size * 100.0;
            printf("\nSummary: %.2fMB -> %.2fMB (Saved %.1f%%)\n", 
                (double)total_in_size / 1024 / 1024, 
                (double)total_out_size / 1024 / 1024, 
                saved);
        }

        free(threads);
        free(active_files);
        for (int i = 0; i < total_jobs; i++) {
            free(jobs[i].in);
            free(jobs[i].out);
        }
        free(jobs);
    }

    vips_shutdown();
    return 0;
}
