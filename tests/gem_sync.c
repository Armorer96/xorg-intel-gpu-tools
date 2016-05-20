/*
 * Copyright © 2016 Intel Corporation
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
 */

#include <time.h>

#include "igt.h"

IGT_TEST_DESCRIPTION("Basic check of ring<->ring write synchronisation.");

/*
 * Testcase: Basic check of sync
 *
 * Extremely efficient at catching missed irqs
 */

static double gettime(void)
{
	static clockid_t clock = -1;
	struct timespec ts;

	/* Stay on the same clock for consistency. */
	if (clock != (clockid_t)-1) {
		if (clock_gettime(clock, &ts))
			goto error;
		goto out;
	}

#ifdef CLOCK_MONOTONIC_RAW
	if (!clock_gettime(clock = CLOCK_MONOTONIC_RAW, &ts))
		goto out;
#endif
#ifdef CLOCK_MONOTONIC_COARSE
	if (!clock_gettime(clock = CLOCK_MONOTONIC_COARSE, &ts))
		goto out;
#endif
	if (!clock_gettime(clock = CLOCK_MONOTONIC, &ts))
		goto out;
error:
	igt_warn("Could not read monotonic time: %s\n",
			strerror(errno));
	igt_assert(0);
	return 0;

out:
	return ts.tv_sec + 1e-9*ts.tv_nsec;
}

static void
sync_ring(int fd, unsigned ring, int num_children)
{
	unsigned engines[16];
	const char *names[16];
	int num_engines = 0;

	if (ring == ~0u) {
		const struct intel_execution_engine *e;

		for (e = intel_execution_engines; e->name; e++) {
			if (e->exec_id == 0)
				continue;

			if (!gem_has_ring(fd, e->exec_id | e->flags))
				continue;

			if (e->exec_id == I915_EXEC_BSD) {
				int is_bsd2 = e->flags != 0;
				if (gem_has_bsd2(fd) != is_bsd2)
					continue;
			}

			names[num_engines] = e->name;
			engines[num_engines++] = e->exec_id | e->flags;
			if (num_engines == ARRAY_SIZE(engines))
				break;
		}

		num_children *= num_engines;
	} else {
		gem_require_ring(fd, ring);
		names[num_engines] = NULL;
		engines[num_engines++] = ring;
	}

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = (uintptr_t)&object;
		execbuf.buffer_count = 1;
		execbuf.flags = engines[child % num_engines];
		gem_execbuf(fd, &execbuf);

		start = gettime();
		cycles = 0;
		do {
			do {
				gem_execbuf(fd, &execbuf);
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < SLOW_QUICK(10, 1));
		igt_info("%s%sompleted %ld cycles: %.3f us\n",
			 names[child % num_engines] ?: "",
			 names[child % num_engines] ? " c" : "C",
			 cycles, elapsed*1e6/cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(20, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
sync_all(int fd, int num_children)
{
	const struct intel_execution_engine *e;
	unsigned engines[16];
	int num_engines = 0;

	for (e = intel_execution_engines; e->name; e++) {
		if (e->exec_id == 0)
			continue;

		if (!gem_has_ring(fd, e->exec_id | e->flags))
			continue;

		if (e->exec_id == I915_EXEC_BSD) {
			int is_bsd2 = e->flags != 0;
			if (gem_has_bsd2(fd) != is_bsd2)
				continue;
		}

		engines[num_engines++] = e->exec_id | e->flags;
		if (num_engines == ARRAY_SIZE(engines))
			break;
	}
	igt_require(num_engines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = (uintptr_t)&object;
		execbuf.buffer_count = 1;
		gem_execbuf(fd, &execbuf);

		start = gettime();
		cycles = 0;
		do {
			do {
				for (int n = 0; n < num_engines; n++) {
					execbuf.flags = engines[n];
					gem_execbuf(fd, &execbuf);
				}
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < SLOW_QUICK(10, 1));
		igt_info("Completed %ld cycles: %.3f us\n",
			 cycles, elapsed*1e6/cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(20, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

igt_main
{
	const struct intel_execution_engine *e;
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture
		fd = drm_open_driver(DRIVER_INTEL);

	igt_fork_hang_detector(fd);

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("%s", e->name)
			sync_ring(fd, e->exec_id | e->flags, 1);
		igt_subtest_f("forked-%s", e->name)
			sync_ring(fd, e->exec_id | e->flags, ncpus);
	}

	igt_subtest("basic-each")
		sync_ring(fd, ~0u, 1);
	igt_subtest("forked-each")
		sync_ring(fd, ~0u, ncpus);

	igt_subtest("basic-all")
		sync_all(fd, 1);
	igt_subtest("forked-all")
		sync_all(fd, ncpus);

	igt_stop_hang_detector();

	igt_fixture
		close(fd);
}
