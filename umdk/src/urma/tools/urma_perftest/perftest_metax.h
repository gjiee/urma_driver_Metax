/* SPDX-License-Identifier: MIT */
#ifndef PERFTEST_METAX_H
#define PERFTEST_METAX_H

#include <stddef.h>

int perftest_metax_init(int device_id);
void *perftest_metax_alloc(size_t len);
void perftest_metax_free(void *ptr);

#endif /* PERFTEST_METAX_H */
