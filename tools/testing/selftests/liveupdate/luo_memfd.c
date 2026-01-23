// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

/*
 * Selftests for memfd preservation via LUO.
 *
 * TODO
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/liveupdate.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"

#include "luo_test_utils.h"

#define TEST_MEMFD_DATA "hello kexec world"

#define STATE_SESSION_NAME "luo-state"
#define STATE_MEMFD_TOKEN 1

#define SEALS_MEMFD_TOKEN 2

#define LIVEUPDATE_DEV "/dev/liveupdate"
static int luo_fd = -1, stage;

/* TODO: docs */
TEST(seals)
{
	int memfd, session;
	int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL;
	struct liveupdate_session_preserve_fd arg = { .size = sizeof(arg) };

	if (stage == 1) {
		int err;

		session = luo_create_session(luo_fd, "seals");
		ASSERT_GE(session, 0);

		memfd = memfd_create("test", MFD_ALLOW_SEALING);
		ASSERT_GE(memfd, 0);
		err = fcntl(memfd, F_ADD_SEALS, seals);
		ASSERT_GE(err, 0);

		arg.fd = memfd;
		arg.token = SEALS_MEMFD_TOKEN;
		ASSERT_GE(ioctl(session, LIVEUPDATE_SESSION_PRESERVE_FD, &arg), 0);
		daemonize_and_wait();
	} else if (stage == 2) {
		struct liveupdate_session_retrieve_fd arg = { .size = sizeof(arg) };

		session = luo_retrieve_session(luo_fd, "seals");
		ASSERT_GE(session, 0);

		arg.token = SEALS_MEMFD_TOKEN;
		ASSERT_GE(ioctl(session, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg), 0);

		memfd = arg.fd;
		ASSERT_GE(memfd, 0);
		ASSERT_EQ(fcntl(memfd, F_GET_SEALS), seals);
	} else {
		TH_LOG("Unknown stage %d\n", stage);

		/* TODO: Any better way to unconditionally fail? */
		ASSERT_FALSE(true);
	}
}

int main(int argc, char *argv[])
{
	int session;

	luo_fd = luo_open_device();
	if (luo_fd < 0)
		ksft_exit_skip("Failed to open %s. Is the luo module loaded?\n",
			       LUO_DEVICE);

	session = luo_retrieve_session(luo_fd, STATE_SESSION_NAME);
	if (session == -ENOENT)
		stage = 1;
	else if (session >= 0)
		stage = 2;
	else
		fail_exit("Failed to check for state session");

	if (stage == 1)
		create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

	test_harness_run(argc, argv);

	daemonize_and_wait();
}
