/*
 * Copyright © 2015 Intel Corporation
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
 */

/*
 * Testcase: Test that only specific ioctl report a wedged GPU.
 *
 */

#include "igt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <drm.h>

#include "igt_vgem.h"

IGT_TEST_DESCRIPTION("Test that specific ioctls report a wedged GPU (EIO).");

static bool i915_reset_control(bool enable)
{
	const char *path = "/sys/module/i915/parameters/reset";
	int fd, ret;

	igt_debug("%s GPU reset\n", enable ? "Enabling" : "Disabling");

	fd = open(path, O_RDWR);
	igt_require(fd >= 0);

	ret = write(fd, &"NY"[enable], 1) == 1;
	close(fd);

	return ret;
}

static void trigger_reset(int fd)
{
	igt_force_gpu_reset();

	/* And just check the gpu is indeed running again */
	igt_debug("Checking that the GPU recovered\n");
	gem_quiescent_gpu(fd);
}

static void wedge_gpu(int fd)
{
	/* First idle the GPU then disable GPU resets before injecting a hang */
	gem_quiescent_gpu(fd);

	igt_require(i915_reset_control(false));

	igt_debug("Wedging GPU by injecting hang\n");
	igt_post_hang_ring(fd, igt_hang_ring(fd, I915_EXEC_DEFAULT));

	igt_assert(i915_reset_control(true));
}

static int __gem_throttle(int fd)
{
	int err = 0;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_THROTTLE, NULL))
		err = -errno;
	return err;
}

static void test_throttle(int fd)
{
	wedge_gpu(fd);

	igt_assert_eq(__gem_throttle(fd), -EIO);

	trigger_reset(fd);
}

static void test_execbuf(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec;
	uint32_t tmp[] = { MI_BATCH_BUFFER_END };

	memset(&exec, 0, sizeof(exec));
	memset(&execbuf, 0, sizeof(execbuf));

	exec.handle = gem_create(fd, 4096);
	gem_write(fd, exec.handle, 0, tmp, sizeof(tmp));

	execbuf.buffers_ptr = to_user_pointer(&exec);
	execbuf.buffer_count = 1;

	wedge_gpu(fd);

	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EIO);
	gem_close(fd, exec.handle);

	trigger_reset(fd);
}

static int __gem_wait(int fd, uint32_t handle, int64_t timeout)
{
	struct drm_i915_gem_wait wait;
	int err = 0;

	memset(&wait, 0, sizeof(wait));
	wait.bo_handle = handle;
	wait.timeout_ns = timeout;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_WAIT, &wait))
		err = -errno;

	return err;
}

static void test_wait(int fd)
{
	igt_hang_t hang;

	/* If the request we wait on completes due to a hang (even for
	 * that request), the user expects the return value to 0 (success).
	 */
	hang = igt_hang_ring(fd, I915_EXEC_DEFAULT);
	igt_assert_eq(__gem_wait(fd, hang.handle, -1), 0);
	igt_post_hang_ring(fd, hang);

	/* If the GPU is wedged during the wait, again we expect the return
	 * value to be 0 (success).
	 */
	igt_require(i915_reset_control(false));
	hang = igt_hang_ring(fd, I915_EXEC_DEFAULT);
	igt_assert_eq(__gem_wait(fd, hang.handle, -1), 0);
	igt_post_hang_ring(fd, hang);
	igt_require(i915_reset_control(true));

	trigger_reset(fd);
}

struct cork {
	int device;
	uint32_t handle;
	uint32_t fence;
};

static void plug(int fd, struct cork *c)
{
	struct vgem_bo bo;
	int dmabuf;

	c->device = __drm_open_driver(DRIVER_VGEM);
	igt_require(c->device != -1);

	bo.width = bo.height = 1;
	bo.bpp = 4;
	vgem_create(c->device, &bo);
	c->fence = vgem_fence_attach(c->device, &bo, VGEM_FENCE_WRITE);

	dmabuf = prime_handle_to_fd(c->device, bo.handle);
	c->handle = prime_fd_to_handle(fd, dmabuf);
	close(dmabuf);
}

static void unplug(struct cork *c)
{
	vgem_fence_signal(c->device, c->fence);
	close(c->device);
}

static void test_inflight(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	uint32_t bbe = MI_BATCH_BUFFER_END;
	igt_hang_t hang;
	struct cork cork;

	igt_require(i915_reset_control(false));
	hang = igt_hang_ring(fd, I915_EXEC_DEFAULT);

	plug(fd, &cork);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork.handle;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	gem_execbuf(fd, &execbuf);

	igt_post_hang_ring(fd, hang);
	unplug(&cork); /* only now submit our batches */
	igt_assert_eq(__gem_wait(fd, obj[1].handle, -1), 0);

	igt_require(i915_reset_control(true));
	trigger_reset(fd);
}

igt_main
{
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_hang_ring(fd, I915_EXEC_DEFAULT);
	}

	igt_subtest("throttle")
		test_throttle(fd);

	igt_subtest("execbuf")
		test_execbuf(fd);

	igt_subtest("wait")
		test_wait(fd);

	igt_subtest("in-flight")
		test_inflight(fd);

	igt_fixture
		close(fd);
}
