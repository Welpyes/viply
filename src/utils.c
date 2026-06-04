#include "utils.h"
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

bool is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

char *get_filename_without_ext(const char *path) {
    char *base = strdup(path);
    char *b = basename(base);
    char *last_dot = strrchr(b, '.');
    if (last_dot) *last_dot = '\0';
    char *res = strdup(b);
    free(base);
    return res;
}

char *path_join(const char *dir, const char *file) {
    size_t len = strlen(dir) + strlen(file) + 2;
    char *res = malloc(len);
    if (!res) return NULL;
    strcpy(res, dir);
    if (dir[strlen(dir) - 1] != '/') strcat(res, "/");
    strcat(res, file);
    return res;
}
