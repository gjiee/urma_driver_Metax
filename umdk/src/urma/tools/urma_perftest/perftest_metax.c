/* SPDX-License-Identifier: MIT */
/*
 * MetaX (沐曦) GPU buffer alloc for urma_perftest.
 * Uses MACA runtime mcSetDevice / mcMalloc / mcFree.
 */

#include "perftest_metax.h"
#include "perftest_log.h"

#ifdef HAVE_METAX_MACA
#include <mc_runtime.h>
#endif

int perftest_metax_init(int device_id)
{
#ifdef HAVE_METAX_MACA
	mcError_t err;

	if (device_id < 0)
		device_id = 0;

	err = mcSetDevice(device_id);
	if (err != mcSuccess) {
		LOG_ERROR("mcSetDevice(%d) failed, err=%d\n", device_id, (int)err);
		return -1;
	}
	LOG_QUIET("MetaX MACA device %d selected for GPU buffer\n", device_id);
	return 0;
#else
	(void)device_id;
	LOG_ERROR("urma_perftest was built without MetaX MACA (mc_runtime.h).\n"
		  "Rebuild with MACA_PATH=/opt/maca or /usr/local/maca.\n");
	return -1;
#endif
}

void *perftest_metax_alloc(size_t len)
{
#ifdef HAVE_METAX_MACA
	void *ptr = NULL;
	mcError_t err;

	if (len == 0)
		return NULL;

	err = mcMalloc(&ptr, len);
	if (err != mcSuccess || ptr == NULL) {
		LOG_ERROR("mcMalloc(len=%zu) failed, err=%d\n", len, (int)err);
		return NULL;
	}
	return ptr;
#else
	(void)len;
	return NULL;
#endif
}

void perftest_metax_free(void *ptr)
{
#ifdef HAVE_METAX_MACA
	if (ptr)
		(void)mcFree(ptr);
#else
	(void)ptr;
#endif
}
