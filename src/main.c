#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
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
}

typedef struct {
    const ViplyOptions *opts;
    char **files;
    int num_files;
    int *current_idx;
    pthread_mutex_t *mutex;
    const char *out_dir;
} ThreadData;

void *worker(void *arg) {
    ThreadData *td = (ThreadData *)arg;
    while (1) {
        int idx;
        pthread_mutex_lock(td->mutex);
        idx = (*td->current_idx)++;
        pthread_mutex_unlock(td->mutex);

        if (idx >= td->num_files) break;

        char *in_path = td->files[idx];
        char *out_name = get_filename_without_ext(in_path);
        char *ext = td->opts->use_jpeg ? ".jpg" : ".webp";
        char *out_file = malloc(strlen(out_name) + strlen(ext) + 1);
        sprintf(out_file, "%s%s", out_name, ext);
        
        char *out_path = path_join(td->out_dir, out_file);
        
        printf("Processing: %s -> %s\n", in_path, out_path);
        if (viply_process_file(td->opts, in_path, out_path) != 0) {
            fprintf(stderr, "Error processing %s\n", in_path);
        }

        free(out_name);
        free(out_file);
        free(out_path);
    }
    return NULL;
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
        .resolution = 100
    };

    static struct option long_options[] = {
        {"jpeg", no_argument, 0, 'f'},
        {"jobs", required_argument, 0, 'j'},
        {"quality", required_argument, 0, 'q'},
        {"compression", required_argument, 0, 'c'},
        {"strip", no_argument, 0, 's'},
        {"date", no_argument, 0, 'd'},
        {"resolution", required_argument, 0, 'r'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "j:q:c:sdr:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f': opts.use_jpeg = true; break;
            case 'j': opts.jobs = atoi(optarg); break;
            case 'q': opts.quality = atoi(optarg); break;
            case 'c': opts.compression = atoi(optarg); break;
            case 's': opts.strip = true; break;
            case 'd': opts.keep_date = true; break;
            case 'r': opts.resolution = atoi(optarg); break;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    char *input = argv[optind];
    char *output = (optind + 1 < argc) ? argv[optind + 1] : NULL;

    if (is_directory(input)) {
        if (!output) {
            fprintf(stderr, "Error: Output directory required for directory input\n");
            return 1;
        }
        mkdir(output, 0755);

        DIR *d = opendir(input);
        if (!d) return 1;

        int count = 0;
        int capacity = 10;
        char **files = malloc(capacity * sizeof(char *));

        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_REG) {
                // Basic check for image extensions
                if (strstr(dir->d_name, ".jpg") || strstr(dir->d_name, ".jpeg") || 
                    strstr(dir->d_name, ".png") || strstr(dir->d_name, ".webp")) {
                    if (count >= capacity) {
                        capacity *= 2;
                        files = realloc(files, capacity * sizeof(char *));
                    }
                    files[count++] = path_join(input, dir->d_name);
                }
            }
        }
        closedir(d);

        if (count > 0) {
            vips_concurrency_set(1); // Parallelize externally
            pthread_t *threads = malloc(opts.jobs * sizeof(pthread_t));
            pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
            int current_idx = 0;

            ThreadData td = {&opts, files, count, &current_idx, &mutex, output};

            for (int i = 0; i < opts.jobs; i++) {
                pthread_create(&threads[i], NULL, worker, &td);
            }
            for (int i = 0; i < opts.jobs; i++) {
                pthread_join(threads[i], NULL);
            }

            free(threads);
            for (int i = 0; i < count; i++) free(files[i]);
            free(files);
        }
    } else {
        // Single file
        char *final_out = output;
        autofree char *alloc_out = NULL;
        if (!output) {
            char *base = get_filename_without_ext(input);
            const char *ext = opts.use_jpeg ? ".jpg" : ".webp";
            alloc_out = malloc(strlen("compressed-") + strlen(base) + strlen(ext) + 1);
            sprintf(alloc_out, "compressed-%s%s", base, ext);
            final_out = alloc_out;
            free(base);
        }
        printf("Processing: %s -> %s\n", input, final_out);
        if (viply_process_file(&opts, input, final_out) != 0) {
            fprintf(stderr, "Error processing file\n");
            return 1;
        }
    }

    vips_shutdown();
    return 0;
}
