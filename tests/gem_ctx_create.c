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

#include "igt.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>


int ret, fd;
struct drm_i915_gem_context_create create;

igt_main
{
	igt_fixture
		fd = drm_open_driver_render(DRIVER_INTEL);

	igt_subtest("basic") {
		create.ctx_id = rand();
		create.pad = 0;


		ret = drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create);
		igt_skip_on(ret != 0 && (errno == ENODEV || errno == EINVAL));
		igt_assert(ret == 0);
		igt_assert(create.ctx_id != 0);
	}

	igt_subtest("invalid-pad") {
		create.ctx_id = rand();
		create.pad = 0;

		ret = drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create);
		igt_skip_on(ret != 0 && (errno == ENODEV || errno == EINVAL));
		igt_assert(ret == 0);
		igt_assert(create.ctx_id != 0);

		create.pad = 1;

		igt_assert(drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create) < 0 &&
			   errno == EINVAL);
	}

	igt_fixture
		close(fd);
}
