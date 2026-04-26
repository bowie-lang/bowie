#ifndef BOWIE_MUSTACHE_H
#define BOWIE_MUSTACHE_H

#include "object.h"

/* Returns malloc'd rendered string. On error returns NULL, *err_out = malloc'd message. */
char *mustache_render(const char *tmpl, Object *data, char **err_out);

#endif
