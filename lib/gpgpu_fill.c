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
 * Authors:
 *  Zhenyu Wang <zhenyuw@linux.intel.com>
 *  Dominik Zeromski <dominik.zeromski@intel.com>
 */

#include <intel_bufmgr.h>
#include <i915_drm.h>

#include "intel_reg.h"
#include "drmtest.h"

#include "gpgpu_fill.h"
#include "gpu_fill.h"

/* shaders/gpgpu/gpgpu_fill.gxa */
static const uint32_t gen7_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20200231, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400c21, 0x00000004, 0x00000010 },
	{ 0x00000001, 0x20440021, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800021, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800021, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880061, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00021, 0x00000020, 0x00000000 },
	{ 0x05800031, 0x24001ca8, 0x00000080, 0x060a8000 },
	{ 0x00600001, 0x2e000021, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20001ca8, 0x00000e00, 0x82000010 },
};

static const uint32_t gen8_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x0e000080, 0x060a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 },
};

static const uint32_t gen9_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x06000080, 0x060a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x06000e00, 0x82000010 },
};

/*
 * This sets up the gpgpu pipeline,
 *
 * +---------------+ <---- 4096
 * |       ^       |
 * |       |       |
 * |    various    |
 * |      state    |
 * |       |       |
 * |_______|_______| <---- 2048 + ?
 * |       ^       |
 * |       |       |
 * |   batch       |
 * |    commands   |
 * |       |       |
 * |       |       |
 * +---------------+ <---- 0 + ?
 *
 */

#define BATCH_STATE_SPLIT 2048

void
gen7_gpgpu_fillfunc(struct intel_batchbuffer *batch,
		    struct igt_buf *dst,
		    unsigned x, unsigned y,
		    unsigned width, unsigned height,
		    uint8_t color)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush(batch);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	/*
	 * const buffer needs to fill for every thread, but as we have just 1 thread
	 * per every group, so need only one curbe data.
	 *
	 * For each thread, just use thread group ID for buffer offset.
	 */
	curbe_buffer = gen7_fill_curbe_buffer_data(batch, color);

	interface_descriptor = gen7_fill_interface_descriptor(batch, dst,
							      gen7_gpgpu_kernel,
							      sizeof(gen7_gpgpu_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	batch->ptr = batch->buffer;

	/* GPGPU pipeline */
	OUT_BATCH(GEN7_PIPELINE_SELECT | PIPELINE_SELECT_GPGPU);

	gen7_emit_state_base_address(batch);
	gen7_emit_vfe_state_gpgpu(batch);
	gen7_emit_curbe_load(batch, curbe_buffer);
	gen7_emit_interface_descriptor_load(batch, interface_descriptor);
	gen7_emit_gpgpu_walk(batch, x, y, width, height);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = batch_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}

void
gen8_gpgpu_fillfunc(struct intel_batchbuffer *batch,
		    struct igt_buf *dst,
		    unsigned x, unsigned y,
		    unsigned width, unsigned height,
		    uint8_t color)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush(batch);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	/*
	 * const buffer needs to fill for every thread, but as we have just 1 thread
	 * per every group, so need only one curbe data.
	 *
	 * For each thread, just use thread group ID for buffer offset.
	 */
	curbe_buffer = gen7_fill_curbe_buffer_data(batch, color);

	interface_descriptor = gen8_fill_interface_descriptor(batch, dst,
							      gen8_gpgpu_kernel,
							      sizeof(gen8_gpgpu_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	batch->ptr = batch->buffer;

	/* GPGPU pipeline */
	OUT_BATCH(GEN7_PIPELINE_SELECT | PIPELINE_SELECT_GPGPU);

	gen8_emit_state_base_address(batch);
	gen8_emit_vfe_state_gpgpu(batch);
	gen7_emit_curbe_load(batch, curbe_buffer);
	gen8_emit_interface_descriptor_load(batch, interface_descriptor);
	gen8_emit_gpgpu_walk(batch, x, y, width, height);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = batch_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}

void
gen9_gpgpu_fillfunc(struct intel_batchbuffer *batch,
		    struct igt_buf *dst,
		    unsigned x, unsigned y,
		    unsigned width, unsigned height,
		    uint8_t color)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush(batch);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	/*
	 * const buffer needs to fill for every thread, but as we have just 1 thread
	 * per every group, so need only one curbe data.
	 *
	 * For each thread, just use thread group ID for buffer offset.
	 */
	curbe_buffer = gen7_fill_curbe_buffer_data(batch, color);

	interface_descriptor = gen8_fill_interface_descriptor(batch, dst,
							      gen9_gpgpu_kernel,
							      sizeof(gen9_gpgpu_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	batch->ptr = batch->buffer;

	/* GPGPU pipeline */
	OUT_BATCH(GEN7_PIPELINE_SELECT | PIPELINE_SELECT_GPGPU);

	gen9_emit_state_base_address(batch);
	gen8_emit_vfe_state_gpgpu(batch);
	gen7_emit_curbe_load(batch, curbe_buffer);
	gen7_emit_interface_descriptor_load(batch, interface_descriptor);
	gen8_emit_gpgpu_walk(batch, x, y, width, height);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = batch_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}
