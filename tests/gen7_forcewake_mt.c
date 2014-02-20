/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

/*
 * Testcase: Exercise a suspect workaround required for FORCEWAKE_MT
 *
 */

#include <sys/types.h>
#include <pthread.h>
#include <string.h>

#include "drm.h"
#include "i915_drm.h"
#include "i915_pciids.h"
#include "drmtest.h"
#include "intel_gpu_tools.h"

#define FORCEWAKE_MT 0xa188

struct thread {
	pthread_t thread;
	void *mmio;
	int fd;
	int bit;
};

static const struct pci_id_match match[] = {
	INTEL_IVB_D_IDS(NULL),
	INTEL_IVB_M_IDS(NULL),

	INTEL_HSW_D_IDS(NULL),
	INTEL_HSW_M_IDS(NULL),

	{ 0, 0, 0 },
};

static struct pci_device *__igfx_get(void)
{
	struct pci_device *dev;

	if (pci_system_init())
		return 0;

	dev = pci_device_find_by_slot(0, 0, 2, 0);
	if (dev == NULL || dev->vendor_id != 0x8086) {
		struct pci_device_iterator *iter;

		iter = pci_id_match_iterator_create(match);
		if (!iter)
			return 0;

		dev = pci_device_next(iter);
		pci_iterator_destroy(iter);
	}

	return dev;
}

static void *igfx_get_mmio(void)
{
	struct pci_device *pci = __igfx_get();
	int error;

	igt_skip_on(pci == NULL);
	igt_skip_on(intel_gen(pci->device_id) != 7);

	error = pci_device_probe(pci);
	igt_assert(error == 0);

	error = pci_device_map_range(pci,
				     pci->regions[0].base_addr,
				     2*1024*1024,
				     PCI_DEV_MAP_FLAG_WRITABLE,
				     &mmio);
	igt_assert(error == 0);
	igt_assert(mmio != NULL);

	return mmio;
}

static void *thread(void *arg)
{
	struct thread *t = arg;
	uint32_t *forcewake_mt = (uint32_t *)((char *)t->mmio + FORCEWAKE_MT);
	uint32_t bit = 1 << t->bit;

	while (1) {
		*forcewake_mt = bit << 16 | bit;
		igt_assert(*forcewake_mt & bit);
		*forcewake_mt = bit << 16;
		igt_assert((*forcewake_mt & bit) == 0);
	}

	return NULL;
}

#define MI_LOAD_REGISTER_IMM                    (0x22<<23)
#define MI_STORE_REGISTER_MEM                   (0x24<<23| 1<<22)

igt_simple_main
{
	struct thread t[16];
	int i;

	t[0].fd = drm_open_any();
	t[0].mmio = igfx_get_mmio();

	for (i = 2; i < 16; i++) {
		t[i] = t[0];
		t[i].bit = i;
		pthread_create(&t[i].thread, NULL, thread, &t[i]);
	}

	sleep(2);

	for (i = 0; i < 1000; i++) {
		struct drm_i915_gem_execbuffer2 execbuf;
		struct drm_i915_gem_exec_object2 exec;
		struct drm_i915_gem_relocation_entry reloc[2];
		uint32_t b[] = {
			MI_LOAD_REGISTER_IMM | 1,
			FORCEWAKE_MT,
			2 << 16 | 2,
			MI_STORE_REGISTER_MEM | 1,
			FORCEWAKE_MT,
			5*sizeof(uint32_t),
			MI_LOAD_REGISTER_IMM | 1,
			FORCEWAKE_MT,
			2 << 16,
			MI_STORE_REGISTER_MEM | 1,
			FORCEWAKE_MT,
			11*sizeof(uint32_t),
			MI_BATCH_BUFFER_END,
			0
		};

		memset(&exec, 0, sizeof(exec));
		exec.handle = gem_create(t[0].fd, 4096);
		exec.relocation_count = 2;
		exec.relocs_ptr = (uintptr_t)reloc;
		//exec.flags = EXEC_OBJECT_NEEDS_GTT;
		gem_write(t[0].fd, exec.handle, 0, b, sizeof(b));

		reloc[0].offset = 5 * sizeof(uint32_t);
		reloc[0].delta = 5 * sizeof(uint32_t);
		reloc[0].target_handle = exec.handle;
		reloc[0].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[0].write_domain = 0;
		reloc[0].presumed_offset = 0;

		reloc[1].offset = 11 * sizeof(uint32_t);
		reloc[1].delta = 11 * sizeof(uint32_t);
		reloc[1].target_handle = exec.handle;
		reloc[1].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[1].write_domain = 0;
		reloc[1].presumed_offset = 0;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = (uintptr_t)&exec;
		execbuf.buffer_count = 1;
		execbuf.batch_len = sizeof(b);
		execbuf.flags = I915_EXEC_SECURE;

		gem_execbuf(t[0].fd, &execbuf);
		gem_sync(t[0].fd, exec.handle);
		gem_read(t[0].fd, exec.handle, 0, b, sizeof(b));
		gem_close(t[0].fd, exec.handle);

		printf("[%d]={ %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x }\n",
		       i, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12]);

		igt_assert(b[5] & 2);
		igt_assert((b[11] & 2) == 0);

		usleep(1000);
	}
}
