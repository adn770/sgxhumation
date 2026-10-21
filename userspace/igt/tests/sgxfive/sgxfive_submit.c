// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Josep Torra
 *
 * SUBMIT_JOBS with dummy transfers, which need no GPU programs: syncobj waits
 * and signals (binary and timeline), several contexts, and the validation
 * rules. A call is checked as a whole before any job runs, and on an error
 * jobs.count reports the job that failed.
 */
#include <errno.h>
#include <string.h>

#include "igt.h"
#include "igt_sgxfive.h"
#include "igt_syncobj.h"

#define WAIT_MS 5000

int igt_main()
{
	struct drm_sgxfive_dev_query_job_info info = { };
	struct drm_sgxfive_job jobs[2];
	uint32_t vm = 0, ctx = 0, failed;
	int fd = -1;

	igt_fixture() {
		fd = igt_sgxfive_open();
		igt_sgxfive_dev_query(fd, DRM_SGXFIVE_DEV_QUERY_JOB_INFO, &info, sizeof(info));
		vm = igt_sgxfive_create_vm(fd);
		ctx = igt_sgxfive_create_ctx(fd, vm);
	}

	igt_subtest("dummy-signals") {
		uint32_t s = syncobj_create(fd, 0);

		igt_assert_eq(igt_sgxfive_wait(fd, s, 0, 0), -ETIME);
		igt_sgxfive_submit_dummy(fd, ctx, 0, s);
		igt_assert_eq(igt_sgxfive_wait(fd, s, 0, WAIT_MS), 0);
		syncobj_destroy(fd, s);
	}

	igt_subtest("dummy-chain") {
		uint32_t s = syncobj_create(fd, 0);

		igt_sgxfive_submit_dummy(fd, ctx, 0, s);
		for (int i = 0; i < 64; i++)
			igt_sgxfive_submit_dummy(fd, ctx, s, s);
		igt_assert_eq(igt_sgxfive_wait(fd, s, 0, WAIT_MS), 0);
		syncobj_destroy(fd, s);
	}

	igt_subtest("many-contexts") {
		uint32_t v2 = igt_sgxfive_create_vm(fd);
		uint32_t c[4], s[4];

		for (int i = 0; i < 4; i++) {
			c[i] = igt_sgxfive_create_ctx(fd, i % 2 ? v2 : vm);
			s[i] = syncobj_create(fd, 0);
			igt_sgxfive_submit_dummy(fd, c[i], 0, s[i]);
		}
		for (int i = 0; i < 4; i++) {
			igt_assert_eq(igt_sgxfive_wait(fd, s[i], 0, WAIT_MS), 0);
			syncobj_destroy(fd, s[i]);
			igt_sgxfive_destroy_ctx(fd, c[i]);
		}
		igt_sgxfive_destroy_vm(fd, v2);
	}

	igt_subtest("wait-needs-a-fence") {
		uint32_t a = syncobj_create(fd, 0), b = syncobj_create(fd, 0);

		/* drm_syncobj_find_fence: a syncobj nobody signals yet is -EINVAL... */
		igt_assert_eq(__igt_sgxfive_submit_dummy(fd, ctx, a, b), -EINVAL);
		igt_assert_eq(igt_sgxfive_wait(fd, b, 0, 0), -ETIME);	/* nothing ran */
		syncobj_destroy(fd, a);
		syncobj_destroy(fd, b);
	}

	igt_subtest("signal-then-wait-in-one-call") {
		/* ...unless an earlier job of the same call signals it. */
		uint32_t a = syncobj_create(fd, 0), b = syncobj_create(fd, 0);
		struct drm_sgxfive_sync_op ops[3] = {
			{ .handle = a, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL },
			{ .handle = a, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_WAIT },
			{ .handle = b, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL },
		};

		jobs[0] = igt_sgxfive_dummy_job(ctx);
		jobs[0].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(1, &ops[0]);
		jobs[1] = igt_sgxfive_dummy_job(ctx);
		jobs[1].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(2, &ops[1]);
		igt_assert_eq(__igt_sgxfive_submit(fd, jobs, 2, NULL), 0);
		igt_assert_eq(igt_sgxfive_wait(fd, b, 0, WAIT_MS), 0);
		syncobj_destroy(fd, a);
		syncobj_destroy(fd, b);
	}

	igt_subtest("timeline-points") {
		uint32_t t = syncobj_create(fd, 0);
		struct drm_sgxfive_sync_op op = {
			.handle = t, .value = 2,
			.flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL |
				 DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ,
		};

		jobs[0] = igt_sgxfive_dummy_job(ctx);
		jobs[0].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(1, &op);
		igt_assert_eq(__igt_sgxfive_submit(fd, jobs, 1, NULL), 0);
		igt_assert_eq(igt_sgxfive_wait(fd, t, 1, WAIT_MS), 0);
		igt_assert_eq(igt_sgxfive_wait(fd, t, 2, WAIT_MS), 0);
		igt_assert_eq(igt_sgxfive_wait(fd, t, 3, 10), -ETIME);

		/* A job waiting on point 2 signals point 4. */
		{
			const uint32_t tl = DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ;
			struct drm_sgxfive_sync_op two[2] = {
				{ .handle = t, .value = 2, .flags = tl },
				{ .handle = t, .value = 4,
				  .flags = tl | DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL },
			};

			jobs[0].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(2, two);
			igt_assert_eq(__igt_sgxfive_submit(fd, jobs, 1, NULL), 0);
		}
		igt_assert_eq(igt_sgxfive_wait(fd, t, 4, WAIT_MS), 0);
		syncobj_destroy(fd, t);
	}

	igt_subtest("job-count") {
		struct drm_sgxfive_job many[64];

		igt_assert(info.max_jobs < ARRAY_SIZE(many));
		for (int i = 0; i < ARRAY_SIZE(many); i++)
			many[i] = igt_sgxfive_dummy_job(ctx);
		igt_assert_eq(__igt_sgxfive_submit(fd, many, 0, NULL), -EINVAL);
		igt_assert_eq(__igt_sgxfive_submit(fd, many, info.max_jobs + 1, NULL), -EINVAL);
		igt_assert_eq(__igt_sgxfive_submit(fd, many, info.max_jobs, NULL), 0);
	}

	igt_subtest("stride") {
		/* A newer, longer job struct is fine if userspace left its tail zero. */
		struct {
			struct drm_sgxfive_job job;
			uint64_t tail;
		} big[2] = {
			{ igt_sgxfive_dummy_job(ctx), 0 },
			{ igt_sgxfive_dummy_job(ctx), 0 },
		};
		struct drm_sgxfive_submit_jobs_args args = {
			.jobs = { .stride = sizeof(big[0]), .count = 2, .array = (uintptr_t)big },
		};

		do_ioctl(fd, DRM_IOCTL_SGXFIVE_SUBMIT_JOBS, &args);
		big[1].tail = 1;
		do_ioctl_err(fd, DRM_IOCTL_SGXFIVE_SUBMIT_JOBS, &args, EINVAL);
		args.jobs.stride = sizeof(big[0].job) - 8;	/* older than the first */
		do_ioctl_err(fd, DRM_IOCTL_SGXFIVE_SUBMIT_JOBS, &args, EINVAL);
	}

	igt_subtest_group() {
		struct drm_sgxfive_transfer_cmd cmd = { };
		struct drm_sgxfive_mem_update upd = { .device_addr = 0x3e000000, .value = 1 };
		struct drm_sgxfive_sync_op op;
		uint32_t s = 0;

		igt_fixture()
			s = syncobj_create(fd, 0);

		/* Job 0 is fine, job 1 breaks one rule: the call fails at index 1. */
#define BAD_SECOND(name, ...)						\
		igt_subtest(name) {					\
			jobs[0] = igt_sgxfive_dummy_job(ctx);		\
			jobs[1] = igt_sgxfive_dummy_job(ctx);		\
			__VA_ARGS__;					\
			failed = 99;					\
			igt_assert_eq(__igt_sgxfive_submit(fd, jobs, 2, &failed), -EINVAL); \
			igt_assert_eq_u32(failed, 1);			\
		}

		BAD_SECOND("bad-type", jobs[1].type = 2)
		BAD_SECOND("bad-context", jobs[1].context_handle = 0xdead)
		BAD_SECOND("bad-flags", jobs[1].flags = 1u << 7)
		BAD_SECOND("reserved-updates",
			   jobs[1].updates = (struct drm_sgxfive_obj_array)
				DRM_SGXFIVE_OBJ_ARRAY(1, &upd))
		BAD_SECOND("reserved-updates-stride",
			   jobs[1].updates = (struct drm_sgxfive_obj_array){ .stride = 16 })
		BAD_SECOND("dummy-with-stream",
			   jobs[1].cmd_stream = (uintptr_t)&cmd;
			   jobs[1].cmd_stream_len = sizeof(cmd))
		BAD_SECOND("short-stream",
			   jobs[1].flags = 0;
			   jobs[1].cmd_stream = (uintptr_t)&cmd;
			   jobs[1].cmd_stream_len = 0x40)
		BAD_SECOND("bad-sync-flags",
			   op = (struct drm_sgxfive_sync_op){ .handle = s, .flags = 1u << 8 };
			   jobs[1].sync_ops = (struct drm_sgxfive_obj_array)
				DRM_SGXFIVE_OBJ_ARRAY(1, &op))
		BAD_SECOND("binary-syncobj-with-point",
			   op = (struct drm_sgxfive_sync_op){
				.handle = s, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL, .value = 5 };
			   jobs[1].sync_ops = (struct drm_sgxfive_obj_array)
				DRM_SGXFIVE_OBJ_ARRAY(1, &op))
#undef BAD_SECOND

		igt_subtest("missing-signal-syncobj") {
			jobs[0] = igt_sgxfive_dummy_job(ctx);
			jobs[1] = igt_sgxfive_dummy_job(ctx);
			op = (struct drm_sgxfive_sync_op){
				.handle = 0xdead, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL };
			jobs[1].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(1, &op);
			failed = 99;
			igt_assert_eq(__igt_sgxfive_submit(fd, jobs, 2, &failed), -ENOENT);
			igt_assert_eq_u32(failed, 1);
		}

		igt_subtest("failed-call-runs-nothing") {
			jobs[0] = igt_sgxfive_dummy_job(ctx);
			op = (struct drm_sgxfive_sync_op){
				.handle = s, .flags = DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL };
			jobs[0].sync_ops = (struct drm_sgxfive_obj_array)DRM_SGXFIVE_OBJ_ARRAY(1, &op);
			jobs[1] = igt_sgxfive_dummy_job(0xdead);
			igt_assert_eq(__igt_sgxfive_submit(fd, jobs, 2, NULL), -EINVAL);
			igt_assert_eq(igt_sgxfive_wait(fd, s, 0, 100), -ETIME);
		}

		igt_subtest("kernel-addresses-rejected") {
			/* A transfer pointing the firmware at the kernel's own heap. */
			cmd.event_pixel_pds_exec =
				igt_sgxfive_heap(fd, DRM_SGXFIVE_HEAP_KERNEL_CODE).base;
			jobs[0] = igt_sgxfive_dummy_job(ctx);
			jobs[0].flags = 0;
			jobs[0].cmd_stream = (uintptr_t)&cmd;
			jobs[0].cmd_stream_len = sizeof(cmd);
			igt_assert_eq(__igt_sgxfive_submit(fd, jobs, 1, NULL), -EINVAL);
		}

		igt_fixture()
			syncobj_destroy(fd, s);
	}

	igt_fixture() {
		igt_sgxfive_destroy_ctx(fd, ctx);
		igt_sgxfive_destroy_vm(fd, vm);
		drm_close_driver(fd);
	}
}
