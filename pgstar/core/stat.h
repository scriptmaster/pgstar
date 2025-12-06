#ifndef PGSTAR_CORE_STAT_H
#define PGSTAR_CORE_STAT_H

#include <time.h>

bool pgstar_file_exists(const char *path);
time_t pgstar_file_mtime(const char *path);

#endif /* PGSTAR_CORE_STAT_H */

