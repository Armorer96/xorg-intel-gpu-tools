/*
 * Copyright © 2012 Intel Corporation
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
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

/*
 * This test covers basic context switch functionality
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <drm.h>


IGT_TEST_DESCRIPTION("Test basic context switch functionality.");

/* Copied from gem_exec_nop.c */
static int exec(int fd, uint32_t handle, int ring, int ctx_id)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 gem_exec;
	int ret = 0;

	gem_exec.handle = handle;
	gem_exec.relocation_count = 0;
	gem_exec.relocs_ptr = 0;
	gem_exec.alignment = 0;
	gem_exec.offset = 0;
	gem_exec.flags = 0;
	gem_exec.rsvd1 = 0;
	gem_exec.rsvd2 = 0;

	execbuf.buffers_ptr = to_user_pointer(&gem_exec);
	execbuf.buffer_count = 1;
	execbuf.batch_start_offset = 0;
	execbuf.batch_len = 8;
	execbuf.cliprects_ptr = 0;
	execbuf.num_cliprects = 0;
	execbuf.DR1 = 0;
	execbuf.DR4 = 0;
	execbuf.flags = ring;
	i915_execbuffer2_set_context_id(execbuf, ctx_id);
	execbuf.rsvd2 = 0;

	ret = drmIoctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2,
			&execbuf);

	return ret;
}

static void big_exec(int fd, uint32_t handle, int ring)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 *gem_exec;
	uint32_t ctx_id1, ctx_id2;
	int num_buffers = gem_global_aperture_size(fd) / 4096;
	int i;

	/* Make sure we only fill half of RAM with gem objects. */
	igt_require(intel_get_total_ram_mb() * 1024 / 2 > num_buffers * 4);

	gem_exec = calloc(num_buffers + 1, sizeof(*gem_exec));
	igt_assert(gem_exec);
	memset(gem_exec, 0, (num_buffers + 1) * sizeof(*gem_exec));


	ctx_id1 = gem_context_create(fd);
	ctx_id2 = gem_context_create(fd);

	gem_exec[0].handle = handle;


	execbuf.buffers_ptr = to_user_pointer(gem_exec);
	execbuf.buffer_count = num_buffers + 1;
	execbuf.batch_start_offset = 0;
	execbuf.batch_len = 8;
	execbuf.cliprects_ptr = 0;
	execbuf.num_cliprects = 0;
	execbuf.DR1 = 0;
	execbuf.DR4 = 0;
	execbuf.flags = ring;
	execbuf.rsvd2 = 0;

	execbuf.buffer_count = 1;
	i915_execbuffer2_set_context_id(execbuf, ctx_id1);
	do_ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf);

	for (i = 0; i < num_buffers; i++) {
		uint32_t tmp_handle = gem_create(fd, 4096);

		gem_exec[i].handle = tmp_handle;
	}
	gem_exec[i].handle = handle;
	execbuf.buffer_count = i + 1;

	/* figure out how many buffers we can exactly fit */
	while (drmIoctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2,
			&execbuf) != 0) {
		i--;
		gem_close(fd, gem_exec[i].handle);
		gem_exec[i].handle = handle;
		execbuf.buffer_count--;
		igt_info("trying buffer count %i\n", i - 1);
	}

	igt_info("reduced buffer count to %i from %i\n",
	       i - 1, num_buffers);

	/* double check that it works */
	do_ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf);

	i915_execbuffer2_set_context_id(execbuf, ctx_id2);
	do_ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf);
	gem_sync(fd, handle);
}

uint32_t handle;
uint32_t batch[2] = {0, MI_BATCH_BUFFER_END};
uint32_t ctx_id, ctx_id2;
int fd;

igt_main
{
	igt_fixture {
		fd = drm_open_driver_render(DRIVER_INTEL);
		igt_require_gem(fd);

		handle = gem_create(fd, 4096);

		/* check that we can create contexts. */
		ctx_id = gem_context_create(fd);
		gem_context_destroy(fd, ctx_id);
		gem_write(fd, handle, 0, batch, sizeof(batch));
	}

	igt_subtest("basic") {
		ctx_id = gem_context_create(fd);
		igt_assert(exec(fd, handle, I915_EXEC_RENDER, ctx_id) == 0);
		gem_sync(fd, handle);
		gem_context_destroy(fd, ctx_id);

		ctx_id = gem_context_create(fd);
		igt_assert(exec(fd, handle, I915_EXEC_RENDER, ctx_id) == 0);
		gem_sync(fd, handle);
		gem_context_destroy(fd, ctx_id);

		igt_assert(exec(fd, handle, I915_EXEC_RENDER, ctx_id) < 0);
		gem_sync(fd, handle);
	}

	igt_subtest("eviction")
		big_exec(fd, handle, I915_EXEC_RENDER);

	igt_subtest("reset-pin-leak") {
		int i;

		igt_skip_on_simulation();

		/*
		 * Use an explicit context to isolate the test from
		 * any major code changes related to the per-file
		 * default context (eg. if they would be eliminated).
		 */
		ctx_id = gem_context_create(fd);

		/*
		 * Iterate enough times that the kernel will
		 * become unhappy if the ggtt pin count for
		 * the last context is leaked at every reset.
		 */
		for (i = 0; i < 20; i++) {
			igt_hang_t hang = igt_hang_ring(fd, I915_EXEC_RENDER);
			igt_assert(exec(fd, handle, I915_EXEC_RENDER, 0) == 0);
			igt_assert(exec(fd, handle, I915_EXEC_RENDER, ctx_id) == 0);
			igt_post_hang_ring(fd, hang);
		}

		gem_context_destroy(fd, ctx_id);
	}

	igt_subtest("lrc-lite-restore") {
		int i, j;

		/*
		 * Need 2 contexts to be able to replicate a lite restore,
		 * i.e. a running context is resubmitted.
		 */
		ctx_id = gem_context_create(fd);
		ctx_id2 = gem_context_create(fd);

		/*
		 * Queue several small batchbuffers to be sure we'll send execlists
		 * with 2 valid context, and likely cause a lite restore when ctxB
		 * is resubmitted at the top of the new execlist.
		 */
		for (i = 0; i < 20; i++) {
			for (j = 0; j < 200; j++) {
				igt_assert(exec(fd, handle, I915_EXEC_RENDER, ctx_id) == 0);
				igt_assert(exec(fd, handle, I915_EXEC_RENDER, ctx_id2) == 0);
			}

			gem_sync(fd, handle);
		}

		gem_context_destroy(fd, ctx_id);
		gem_context_destroy(fd, ctx_id2);
	}
}
