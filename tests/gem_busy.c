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

#include "igt.h"
#include "igt_rand.h"

#define LOCAL_EXEC_NO_RELOC (1<<11)

/* Exercise the busy-ioctl, ensuring the ABI is never broken */
IGT_TEST_DESCRIPTION("Basic check of busy-ioctl ABI.");

enum { TEST = 0, BUSY, BATCH };

static bool gem_busy(int fd, uint32_t handle)
{
	struct drm_i915_gem_busy busy;

	memset(&busy, 0, sizeof(busy));
	busy.handle = handle;

	do_ioctl(fd, DRM_IOCTL_I915_GEM_BUSY, &busy);

	return busy.busy != 0;
}

static void __gem_busy(int fd,
		       uint32_t handle,
		       uint32_t *read,
		       uint32_t *write)
{
	struct drm_i915_gem_busy busy;

	memset(&busy, 0, sizeof(busy));
	busy.handle = handle;

	do_ioctl(fd, DRM_IOCTL_I915_GEM_BUSY, &busy);

	*write = busy.busy & 0xffff;
	*read = busy.busy >> 16;
}

static uint32_t busy_blt(int fd)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	const int has_64bit_reloc = gen >= 8;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[2];
	struct drm_i915_gem_relocation_entry reloc[200], *r;
	uint32_t *map;
	int factor = 100;
	int i = 0;

	memset(object, 0, sizeof(object));
	object[0].handle = gem_create(fd, 1024*1024);
	object[1].handle = gem_create(fd, 4096);

	r = memset(reloc, 0, sizeof(reloc));
	map = gem_mmap__cpu(fd, object[1].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, object[1].handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

#define COPY_BLT_CMD		(2<<29|0x53<<22|0x6)
#define BLT_WRITE_ALPHA		(1<<21)
#define BLT_WRITE_RGB		(1<<20)
	while (factor--) {
		/* XY_SRC_COPY */
		map[i++] = COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
		if (has_64bit_reloc)
			map[i-1] += 2;
		map[i++] = 0xcc << 16 | 1 << 25 | 1 << 24 | (4*1024);
		map[i++] = 0;
		map[i++] = 256 << 16 | 1024;

		r->offset = i * sizeof(uint32_t);
		r->target_handle = object[0].handle;
		r->read_domains = I915_GEM_DOMAIN_RENDER;
		r->write_domain = I915_GEM_DOMAIN_RENDER;
		r++;
		map[i++] = 0;
		if (has_64bit_reloc)
			map[i++] = 0;

		map[i++] = 0;
		map[i++] = 4096;

		r->offset = i * sizeof(uint32_t);
		r->target_handle = object[0].handle;
		r->read_domains = I915_GEM_DOMAIN_RENDER;
		r->write_domain = 0;
		r++;
		map[i++] = 0;
		if (has_64bit_reloc)
			map[i++] = 0;
	}
	map[i++] = MI_BATCH_BUFFER_END;
	igt_assert(i <= 4096/sizeof(uint32_t));
	igt_assert(r - reloc <= ARRAY_SIZE(reloc));
	munmap(map, 4096);

	object[1].relocs_ptr = to_user_pointer(reloc);
	object[1].relocation_count = r - reloc;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = 2;
	if (gen >= 6)
		execbuf.flags = I915_EXEC_BLT;
	gem_execbuf(fd, &execbuf);
	igt_assert(gem_bo_busy(fd, object[0].handle));

	igt_debug("Created busy handle %d\n", object[0].handle);
	gem_close(fd, object[1].handle);
	return object[0].handle;
}

static bool exec_noop(int fd,
		      uint32_t *handles,
		      unsigned ring,
		      bool write)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec[3];

	memset(exec, 0, sizeof(exec));
	exec[0].handle = handles[BUSY];
	exec[1].handle = handles[TEST];
	if (write)
		exec[1].flags |= EXEC_OBJECT_WRITE;
	exec[2].handle = handles[BATCH];

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(exec);
	execbuf.buffer_count = 3;
	execbuf.flags = ring;
	igt_debug("Queuing handle for %s on ring %d\n",
		  write ? "writing" : "reading", ring & 0x7);
	return __gem_execbuf(fd, &execbuf) == 0;
}

static bool still_busy(int fd, uint32_t handle)
{
	uint32_t read, write;
	__gem_busy(fd, handle, &read, &write);
	return write;
}

static void semaphore(int fd, unsigned ring, uint32_t flags)
{
	uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle[3];
	uint32_t read, write;
	uint32_t active;
	unsigned i;

	gem_require_ring(fd, ring | flags);

	handle[TEST] = gem_create(fd, 4096);
	handle[BATCH] = gem_create(fd, 4096);
	gem_write(fd, handle[BATCH], 0, &bbe, sizeof(bbe));

	/* Create a long running batch which we can use to hog the GPU */
	handle[BUSY] = busy_blt(fd);

	/* Queue a batch after the busy, it should block and remain "busy" */
	igt_assert(exec_noop(fd, handle, ring | flags, false));
	igt_assert(still_busy(fd, handle[BUSY]));
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, 1 << ring);
	igt_assert_eq(write, 0);

	/* Requeue with a write */
	igt_assert(exec_noop(fd, handle, ring | flags, true));
	igt_assert(still_busy(fd, handle[BUSY]));
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, 1 << ring);
	igt_assert_eq(write, ring);

	/* Now queue it for a read across all available rings */
	active = 0;
	for (i = I915_EXEC_RENDER; i <= I915_EXEC_VEBOX; i++) {
		if (exec_noop(fd, handle, i | flags, false))
			active |= 1 << i;
	}
	igt_assert(still_busy(fd, handle[BUSY]));
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, active);
	igt_assert_eq(write, ring); /* from the earlier write */

	/* Check that our long batch was long enough */
	igt_assert(still_busy(fd, handle[BUSY]));

	/* And make sure it becomes idle again */
	gem_sync(fd, handle[TEST]);
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, 0);
	igt_assert_eq(write, 0);

	for (i = TEST; i <= BATCH; i++)
		gem_close(fd, handle[i]);
}

#define PARALLEL 1
#define HANG 2
static void one(int fd, unsigned ring, uint32_t flags, unsigned test_flags)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
#define SCRATCH 0
#define BATCH 1
	struct drm_i915_gem_relocation_entry store[1024+1];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned size = ALIGN(ARRAY_SIZE(store)*16 + 4, 4096);
	uint32_t read[2], write[2];
	struct timespec tv;
	uint32_t *batch, *bbe;
	int i, count, timeout;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = ring | flags;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[SCRATCH].handle = gem_create(fd, 4096);

	obj[BATCH].handle = gem_create(fd, size);
	obj[BATCH].relocs_ptr = to_user_pointer(store);
	obj[BATCH].relocation_count = ARRAY_SIZE(store);
	memset(store, 0, sizeof(store));

	batch = gem_mmap__wc(fd, obj[BATCH].handle, 0, size, PROT_WRITE);
	gem_set_domain(fd, obj[BATCH].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	i = 0;
	for (count = 0; count < 1024; count++) {
		store[count].target_handle = obj[SCRATCH].handle;
		store[count].presumed_offset = -1;
		store[count].offset = sizeof(uint32_t) * (i + 1);
		store[count].delta = sizeof(uint32_t) * count;
		store[count].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		store[count].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = 0;
			batch[++i] = 0;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = 0;
			store[count].offset += sizeof(uint32_t);
		} else {
			batch[i]--;
			batch[++i] = 0;
		}
		batch[++i] = count;
		i++;
	}

	bbe = &batch[i];
	store[count].target_handle = obj[BATCH].handle; /* recurse */
	store[count].presumed_offset = 0;
	store[count].offset = sizeof(uint32_t) * (i + 1);
	store[count].delta = 0;
	store[count].read_domains = I915_GEM_DOMAIN_COMMAND;
	store[count].write_domain = 0;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			store[count].delta = 1;
		}
	}
	i++;

	igt_assert(i < size/sizeof(*batch));
	igt_require(__gem_execbuf(fd, &execbuf) == 0);

	__gem_busy(fd, obj[SCRATCH].handle, &read[SCRATCH], &write[SCRATCH]);
	__gem_busy(fd, obj[BATCH].handle, &read[BATCH], &write[BATCH]);

	if (test_flags & PARALLEL) {
		const struct intel_execution_engine *e;

		for (e = intel_execution_engines; e->name; e++) {
			if (e->exec_id == 0 || e->exec_id == ring)
				continue;

			if (e->exec_id == I915_EXEC_BSD && gen == 6)
				continue;

			if (!gem_has_ring(fd, e->exec_id | e->flags))
				continue;

			igt_debug("Testing %s in parallel\n", e->name);
			one(fd, e->exec_id, e->flags, 0);
		}
	}

	timeout = 120;
	if ((test_flags & HANG) == 0) {
		*bbe = MI_BATCH_BUFFER_END;
		__sync_synchronize();
		timeout = 1;
	}

	igt_assert_eq(write[SCRATCH], ring);
	igt_assert_eq_u32(read[SCRATCH], 1 << ring);

	igt_assert_eq(write[BATCH], 0);
	igt_assert_eq_u32(read[BATCH], 1 << ring);

	/* Calling busy in a loop should be enough to flush the rendering */
	memset(&tv, 0, sizeof(tv));
	while (gem_busy(fd, obj[BATCH].handle))
		igt_assert(igt_seconds_elapsed(&tv) < timeout);
	igt_assert(!gem_busy(fd, obj[SCRATCH].handle));

	munmap(batch, size);
	batch = gem_mmap__wc(fd, obj[SCRATCH].handle, 0, 4096, PROT_READ);
	for (i = 0; i < 1024; i++)
		igt_assert_eq_u32(batch[i], i);
	munmap(batch, 4096);

	gem_close(fd, obj[BATCH].handle);
	gem_close(fd, obj[SCRATCH].handle);
}

static void xchg_u32(void *array, unsigned i, unsigned j)
{
	uint32_t *u32 = array;
	uint32_t tmp = u32[i];
	u32[i] = u32[j];
	u32[j] = tmp;
}

static void close_race(int fd)
{
#define N_HANDLES 4096
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t *handles;
	unsigned long *control;
	unsigned long count = 0;
	int i;

	intel_require_memory(N_HANDLES, 4096, CHECK_RAM);

	/* One thread spawning work and randomly closing fd.
	 * One background thread per cpu checking busyness.
	 */

	control = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(control != MAP_FAILED);

	handles = mmap(NULL, N_HANDLES*sizeof(*handles),
		   PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(handles != MAP_FAILED);

	for (i = 0; i < N_HANDLES; i++) {
		handles[i] = gem_create(fd, 4096);
	}

	igt_fork(child, ncpus) {
		struct drm_i915_gem_busy busy;
		uint32_t indirection[N_HANDLES];

		for (i = 0; i < N_HANDLES; i++)
			indirection[i] = i;

		hars_petruska_f54_1_random_perturb(child);

		memset(&busy, 0, sizeof(busy));
		do {
			igt_permute_array(indirection, N_HANDLES, xchg_u32);
			__sync_synchronize();
			for (i = 0; i < N_HANDLES; i++) {
				busy.handle = indirection[handles[i]];
				/* Check that the busy computation doesn't
				 * explode in the face of random gem_close().
				 */
				drmIoctl(fd, DRM_IOCTL_I915_GEM_BUSY, &busy);
			}
			count++;
		} while(*(volatile long *)control == 0);

		igt_debug("child[%d]: count = %lu\n", child, count);
		control[child] = count;
	}

	igt_until_timeout(20) {
		int j = rand() % N_HANDLES;

		gem_close(fd, handles[j]);
		__sync_synchronize();
		handles[j] = busy_blt(fd);

		count++;
	}
	control[0] = 1;
	igt_waitchildren();

	for (i = 0; i < ncpus; i++)
		control[ncpus + 1] += control[i + 1];
	igt_info("Total execs %lu, busy-ioctls %lu\n",
		 count, control[ncpus + 1] * N_HANDLES);

	for (i = 0; i < N_HANDLES; i++)
		gem_close(fd, handles[i]);

	munmap(handles, N_HANDLES * sizeof(*handles));
	munmap(control, 4096);

	gem_quiescent_gpu(fd);
}

static bool has_semaphores(int fd)
{
	struct drm_i915_getparam gp;
	int val = -1;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_HAS_SEMAPHORES;
	gp.value = &val;

	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
	errno = 0;

	return val > 0;
}

static bool has_extended_busy_ioctl(int fd)
{
	igt_spin_t *spin = igt_spin_batch_new(fd, I915_EXEC_RENDER, 0);
	uint32_t read, write;

	__gem_busy(fd, spin->handle, &read, &write);
	igt_spin_batch_free(fd, spin);

	return read != 0;
}

static void basic(int fd, unsigned ring, unsigned flags)
{
	igt_spin_t *spin = igt_spin_batch_new(fd, ring, 0);
	struct timespec tv;
	int timeout;
	bool busy;

	busy = gem_bo_busy(fd, spin->handle);

	timeout = 120;
	if ((flags & HANG) == 0) {
		igt_spin_batch_end(spin);
		timeout = 1;
	}

	igt_assert(busy);
	memset(&tv, 0, sizeof(tv));
	while (gem_bo_busy(fd, spin->handle)) {
		if (igt_seconds_elapsed(&tv) > timeout) {
			igt_debugfs_dump(fd, "i915_engine_info");
			igt_debugfs_dump(fd, "i915_hangcheck_info");
			igt_assert_f(igt_seconds_elapsed(&tv) < timeout,
				     "%s batch did not complete within %ds\n",
				     flags & HANG ? "Hanging" : "Normal",
				     timeout);
		}
	}

	igt_spin_batch_free(fd, spin);
}

static bool can_store_dword_imm(int fd)
{
	return intel_gen(intel_gen(intel_get_drm_devid(fd))) > 2;
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require(can_store_dword_imm(fd));
	}

	igt_subtest_group {
		igt_fixture {
			igt_fork_hang_detector(fd);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_group {
				igt_subtest_f("%sbusy-%s",
					      e->exec_id == 0 ? "basic-" : "",
					      e->name) {
					igt_require(gem_has_ring(fd, e->exec_id | e->flags));
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, 0);
				}
			}
		}

		igt_subtest_group {
			int gen = 0;

			igt_fixture {
				igt_require(has_extended_busy_ioctl(fd));
				gem_require_mmap_wc(fd);
				gen = intel_gen(intel_get_drm_devid(fd));
			}

			for (e = intel_execution_engines; e->name; e++) {
				/* default exec-id is purely symbolic */
				if (e->exec_id == 0)
					continue;

				igt_subtest_f("extended-%s", e->name) {
					gem_require_ring(fd, e->exec_id | e->flags);
					igt_skip_on_f(gen == 6 &&
						      e->exec_id == I915_EXEC_BSD,
						      "MI_STORE_DATA broken on gen6 bsd\n");
					gem_quiescent_gpu(fd);
					one(fd, e->exec_id, e->flags, 0);
					gem_quiescent_gpu(fd);
				}
			}

			for (e = intel_execution_engines; e->name; e++) {
				/* default exec-id is purely symbolic */
				if (e->exec_id == 0)
					continue;

				igt_subtest_f("extended-parallel-%s", e->name) {
					gem_require_ring(fd, e->exec_id | e->flags);
					igt_skip_on_f(gen == 6 &&
						      e->exec_id == I915_EXEC_BSD,
						      "MI_STORE_DATA broken on gen6 bsd\n");
					gem_quiescent_gpu(fd);
					one(fd, e->exec_id, e->flags, PARALLEL);
					gem_quiescent_gpu(fd);
				}
			}
		}

		igt_subtest_group {
			igt_fixture {
				igt_require(has_extended_busy_ioctl(fd));
				igt_require(has_semaphores(fd));
			}

			for (e = intel_execution_engines; e->name; e++) {
				/* default exec-id is purely symbolic */
				if (e->exec_id == 0)
					continue;

				igt_subtest_f("extended-semaphore-%s", e->name)
					semaphore(fd, e->exec_id, e->flags);
			}
		}

		igt_subtest("close-race")
			close_race(fd);

		igt_fixture {
			igt_stop_hang_detector();
		}
	}

	igt_subtest_group {
		igt_hang_t hang;

		igt_fixture {
			hang = igt_allow_hang(fd, 0, 0);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_f("%shang-%s",
				      e->exec_id == 0 ? "basic-" : "",
				      e->name) {
				igt_require(gem_has_ring(fd, e->exec_id | e->flags));
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG);
			}
		}

		igt_subtest_group {
			int gen = 0;

			igt_fixture {
				igt_require(has_extended_busy_ioctl(fd));
				gem_require_mmap_wc(fd);
				gen = intel_gen(intel_get_drm_devid(fd));
			}

			for (e = intel_execution_engines; e->name; e++) {
				/* default exec-id is purely symbolic */
				if (e->exec_id == 0)
					continue;

				igt_subtest_f("extended-hang-%s", e->name) {
					gem_require_ring(fd, e->exec_id | e->flags);
					igt_skip_on_f(gen == 6 &&
						      e->exec_id == I915_EXEC_BSD,
						      "MI_STORE_DATA broken on gen6 bsd\n");
					gem_quiescent_gpu(fd);
					one(fd, e->exec_id, e->flags, HANG);
					gem_quiescent_gpu(fd);
				}
			}
		}

		igt_fixture {
			igt_disallow_hang(fd, hang);
		}
	}

	igt_fixture {
		close(fd);
	}
}
