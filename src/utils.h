#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>

bool is_directory(const char *path);
char *get_filename_without_ext(const char *path);
char *path_join(const char *dir, const char *file);

#endif
