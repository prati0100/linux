// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026 Pratyush Yadav <pratyush@kernel.org>
 *
 * Preserve hugetlb memfds and files on hugetlbfs mounts across a kexec, and
 * retrieve them in the next kernel.
 *
 * Only 1G hugepages are tested. Needs root, and at least 10 1G hugepages in
 * both kernels. The test for a page size mismatch also uses 2M hugepages, and
 * is skipped without them. For example:
 *
 *   hugepagesz=1G hugepages=16 hugepagesz=2M hugepages=16
 *
 * Run with no arguments before kexec, and with -s 2 after.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <linux/memfd.h>

#include <libliveupdate.h>

#define HPAGE			(1UL << 30)

#define STATE_SESSION_NAME	"kexec_hugetlb_state"
#define STATE_TOKEN		999
#define SESSION_NAME		"kexec_hugetlb"

/* Tokens of the preserved files. */
#define TOKEN_MEMFD		1
#define TOKEN_LINKED		2
#define TOKEN_UNLINKED		3
#define TOKEN_ACCOUNTING	4
#define TOKEN_FROZEN		5

/* The memfd and the linked file end in a partial page. */
#define MEMFD_SIZE		(2 * HPAGE - 4096)
#define LINKED_SIZE		(1 * HPAGE + 100)
#define UNLINKED_SIZE		(1 * HPAGE)
#define ACCOUNTING_SIZE		(2 * HPAGE)
#define FROZEN_SIZE		(1 * HPAGE)

#define MEMFD_POS		12345
#define MEMFD_SEALS		(F_SEAL_SHRINK | F_SEAL_GROW)

#define MNT_BASE		"/tmp/luo_hugetlb"
#define MNT_SRC			MNT_BASE "/src"

static char mnt_paths[16][64];
static int mnt_count;

/* Mount a fresh hugetlbfs and return its path, or NULL if that fails. */
static const char *try_mount_hugetlbfs(const char *opts)
{
	char *path;

	if (mnt_count >= 16)
		fail_exit("too many mounts");

	path = mnt_paths[mnt_count];
	snprintf(path, sizeof(mnt_paths[0]), MNT_BASE "/mnt%d", mnt_count++);
	mkdir(MNT_BASE, 0755);
	if (mkdir(path, 0755) && errno != EEXIST)
		fail_exit("mkdir %s", path);
	if (mount("none", path, "hugetlbfs", 0, opts))
		return NULL;

	return path;
}

static const char *mount_hugetlbfs(const char *opts)
{
	const char *path = try_mount_hugetlbfs(opts);

	if (!path)
		fail_exit("mount hugetlbfs with %s", opts);
	return path;
}

/* Unmount everything stage 2 mounted, so the hugepages go back to the pool. */
static void unmount_all(void)
{
	for (int i = 0; i < mnt_count; i++) {
		umount2(mnt_paths[i], MNT_DETACH);
		rmdir(mnt_paths[i]);
	}
	rmdir(MNT_SRC);
	rmdir(MNT_BASE);
}

static unsigned long statfs_free(const char *path)
{
	struct statfs sfs;

	if (statfs(path, &sfs))
		fail_exit("statfs %s", path);
	return sfs.f_bfree;
}

static int open_tmpfile(const char *dir)
{
	int fd = open(dir, O_TMPFILE | O_RDWR, 0600);

	if (fd < 0)
		fail_exit("O_TMPFILE in %s", dir);
	return fd;
}

/*
 * The expected byte at @off. hugetlbfs has no write(), and a writable mmap()
 * grows the file to a full hugepage, so the partial page at the end of a file
 * is left zero.
 */
static unsigned char pattern(int token, unsigned long size, unsigned long off)
{
	if (off >= (size & ~(HPAGE - 1)))
		return 0;
	return (unsigned char)(token * 31 + off / 4096 + 1);
}

/* Allocate all pages of a file of @size and fill it with a pattern. */
static void fill(int fd, int token, unsigned long size)
{
	unsigned long len = size & ~(HPAGE - 1);
	unsigned char *p;

	if (fallocate(fd, 0, 0, size))
		fail_exit("fallocate token %d", token);
	if (!len)
		return;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		fail_exit("mmap token %d", token);
	for (unsigned long off = 0; off < len; off += 4096)
		p[off] = pattern(token, size, off);
	munmap(p, len);
}

static void verify(int fd, int token, unsigned long size)
{
	unsigned long len = (size + HPAGE - 1) & ~(HPAGE - 1);
	unsigned char *p;
	struct stat st;

	if (fstat(fd, &st))
		fail_exit("fstat token %d", token);
	if (st.st_size != size)
		fail_exit("token %d: size %ld, expected %lu", token,
			  (long)st.st_size, size);

	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		fail_exit("mmap token %d", token);
	for (unsigned long off = 0; off < size; off += 4096) {
		if (p[off] != pattern(token, size, off))
			fail_exit("token %d: data mismatch at %#lx", token, off);
	}
	munmap(p, len);
}

static int retrieve(int session_fd, __u64 token, int target)
{
	struct liveupdate_hugetlb_retrieve_arg arg = {
		.size = sizeof(arg),
		.fd = target,
	};
	struct liveupdate_session_retrieve_fd req = {
		.size = sizeof(req),
		.token = token,
	};

	if (target >= 0)
		req.arg = (__u64)(unsigned long)&arg;

	if (ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &req) < 0)
		return -errno;
	return req.fd;
}

static int retrieve_ok(int session_fd, __u64 token, int target)
{
	int fd = retrieve(session_fd, token, target);

	if (fd < 0) {
		errno = -fd;
		fail_exit("retrieve token %llu", (unsigned long long)token);
	}
	return fd;
}

/* Retrieving @token into @target must fail with @err. */
static void expect_fail(int session_fd, __u64 token, int target, int err,
			const char *what)
{
	int ret = retrieve(session_fd, token, target);

	if (ret != -err) {
		errno = ret < 0 ? -ret : 0;
		fail_exit("%s: got %d, expected %d", what, ret, -err);
	}
	ksft_print_msg("[STAGE 2] %s: rejected as expected\n", what);
}

static int create_file(const char *path, mode_t mode, int token,
		       unsigned long size)
{
	int fd = open(path, O_CREAT | O_RDWR, mode);

	if (fd < 0)
		fail_exit("create %s", path);
	fill(fd, token, size);
	return fd;
}

static void preserve(int session_fd, int fd, int token)
{
	if (luo_session_preserve_fd(session_fd, fd, token))
		fail_exit("preserve token %d", token);
}

/* Preserving a file the caller can't write to must fail. */
static void check_preserve_no_write_permission(int session_fd)
{
	int fd, status;
	pid_t pid;

	close(create_file(MNT_SRC "/readonly", 0644, 42, HPAGE));

	fd = open(MNT_SRC "/readonly", O_RDONLY);
	if (fd < 0)
		fail_exit("open readonly file");

	pid = fork();
	if (pid < 0)
		fail_exit("fork");
	if (!pid) {
		if (setresuid(65534, 65534, 65534))
			_exit(2);
		_exit(luo_session_preserve_fd(session_fd, fd, 100) == -EACCES ? 0 : 1);
	}

	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		fail_exit("preserve without write permission: status %#x",
			  status);
	close(fd);
	ksft_print_msg("[STAGE 1] preserve without write permission: rejected\n");
}

static void run_stage_1(int luo_fd)
{
	int session_fd, fd;

	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_TOKEN, 2);

	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_create_session");

	mkdir(MNT_BASE, 0755);
	mkdir(MNT_SRC, 0755);
	if (mount("none", MNT_SRC, "hugetlbfs", 0, "pagesize=1G"))
		fail_exit("mount source hugetlbfs");

	/* A memfd, with seals and a file position. */
	fd = memfd_create("luo_hugetlb", MFD_HUGETLB | MFD_HUGE_1GB |
			  MFD_ALLOW_SEALING);
	if (fd < 0)
		fail_exit("memfd_create");
	fill(fd, TOKEN_MEMFD, MEMFD_SIZE);
	if (fcntl(fd, F_ADD_SEALS, MEMFD_SEALS))
		fail_exit("F_ADD_SEALS");
	if (lseek(fd, MEMFD_POS, SEEK_SET) != MEMFD_POS)
		fail_exit("lseek");
	preserve(session_fd, fd, TOKEN_MEMFD);
	close(fd);

	/* A linked file. */
	fd = create_file(MNT_SRC "/linked", 0600, TOKEN_LINKED, LINKED_SIZE);
	preserve(session_fd, fd, TOKEN_LINKED);
	close(fd);

	/* An unlinked file. */
	fd = open_tmpfile(MNT_SRC);
	fill(fd, TOKEN_UNLINKED, UNLINKED_SIZE);
	preserve(session_fd, fd, TOKEN_UNLINKED);
	close(fd);

	fd = create_file(MNT_SRC "/accounting", 0600, TOKEN_ACCOUNTING,
			 ACCOUNTING_SIZE);
	preserve(session_fd, fd, TOKEN_ACCOUNTING);
	close(fd);

	fd = create_file(MNT_SRC "/frozen", 0600, TOKEN_FROZEN, FROZEN_SIZE);
	preserve(session_fd, fd, TOKEN_FROZEN);

	/* A preserved file can't be resized. */
	if (!ftruncate(fd, 2 * HPAGE) || errno != EPERM)
		fail_exit("ftruncate of preserved file");
	close(fd);

	check_preserve_no_write_permission(session_fd);

	close(luo_fd);
	daemonize_and_wait();
}

static void check_memfd(int session_fd)
{
	const char *mnt = mount_hugetlbfs("pagesize=1G");
	int fd, target;

	target = open_tmpfile(mnt);
	expect_fail(session_fd, TOKEN_MEMFD, target, EINVAL,
		    "memfd with an argument");
	close(target);

	fd = retrieve_ok(session_fd, TOKEN_MEMFD, -1);
	verify(fd, TOKEN_MEMFD, MEMFD_SIZE);
	if (fcntl(fd, F_GET_SEALS) != MEMFD_SEALS)
		fail_exit("memfd seals %#x", fcntl(fd, F_GET_SEALS));
	if (lseek(fd, 0, SEEK_CUR) != MEMFD_POS)
		fail_exit("memfd position");
	close(fd);
	ksft_print_msg("[STAGE 2] memfd: OK\n");
}

/* All the ways a target can be wrong, tried on the linked file. */
static void check_bad_targets(int session_fd)
{
	const char *mnt = mount_hugetlbfs("pagesize=1G");
	const char *mnt_2m = try_mount_hugetlbfs("pagesize=2M");
	const char *mnt_min = mount_hugetlbfs("pagesize=1G,min_size=1G");
	char path[128];
	void *p;
	int fd;

	expect_fail(session_fd, TOKEN_LINKED, -1, EINVAL, "no argument");
	expect_fail(session_fd, TOKEN_LINKED, 1000, EBADF, "bad fd");

	snprintf(path, sizeof(path), "%s/ro", mnt);
	fd = open(path, O_CREAT | O_RDONLY, 0600);
	if (fd < 0)
		fail_exit("create %s", path);
	expect_fail(session_fd, TOKEN_LINKED, fd, EBADF, "read-only target");
	close(fd);

	fd = memfd_create("target", MFD_HUGETLB | MFD_HUGE_1GB);
	if (fd < 0)
		fail_exit("memfd_create");
	expect_fail(session_fd, TOKEN_LINKED, fd, EINVAL, "memfd target");
	close(fd);

	fd = memfd_create("target", 0);
	if (fd < 0)
		fail_exit("memfd_create");
	expect_fail(session_fd, TOKEN_LINKED, fd, EINVAL, "shmem target");
	close(fd);

	if (mnt_2m) {
		fd = open_tmpfile(mnt_2m);
		expect_fail(session_fd, TOKEN_LINKED, fd, EINVAL,
			    "wrong page size");
		close(fd);
	} else {
		ksft_print_msg("[STAGE 2] no 2M hugepages, skipping wrong page size\n");
	}

	fd = open_tmpfile(mnt_min);
	expect_fail(session_fd, TOKEN_LINKED, fd, EINVAL, "min_size mount");
	close(fd);

	fd = open_tmpfile(mnt);
	fill(fd, 7, HPAGE);
	expect_fail(session_fd, TOKEN_LINKED, fd, EINVAL, "target with data");
	close(fd);

	fd = open_tmpfile(mnt);
	if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, HPAGE))
		fail_exit("fallocate");
	expect_fail(session_fd, TOKEN_LINKED, fd, EINVAL,
		    "target with pages past EOF");
	close(fd);

	fd = open_tmpfile(mnt);
	p = mmap(NULL, HPAGE, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		fail_exit("mmap");
	munmap(p, HPAGE);
	expect_fail(session_fd, TOKEN_LINKED, fd, EINVAL,
		    "target with a reservation");
	close(fd);
}

/*
 * Retrieve @token into the O_TMPFILE @target on the mount @mnt, check it, and
 * link it in as @name. Returns the fd LUO handed out.
 */
static int retrieve_and_link(int session_fd, int token, unsigned long size,
			     int target, const char *mnt, const char *name)
{
	char path[128];
	struct stat a, b;
	int fd;

	fd = retrieve_ok(session_fd, token, target);

	if (fstat(fd, &a) || fstat(target, &b))
		fail_exit("fstat");
	if (a.st_ino != b.st_ino || a.st_dev != b.st_dev)
		fail_exit("token %d: retrieved fd is not the target", token);
	verify(fd, token, size);

	snprintf(path, sizeof(path), "%s/%s", mnt, name);
	if (linkat(target, "", AT_FDCWD, path, AT_EMPTY_PATH))
		fail_exit("linkat %s", path);
	close(target);

	target = open(path, O_RDONLY);
	if (target < 0)
		fail_exit("open %s", path);
	verify(target, token, size);
	close(target);

	return fd;
}

/* A file preserved in this kernel can't be a target. */
static void check_frozen_target(int luo_fd, int session_fd, const char *mnt)
{
	int out_fd, target;

	out_fd = luo_create_session(luo_fd, "kexec_hugetlb_out");
	if (out_fd < 0)
		fail_exit("luo_create_session");

	target = open_tmpfile(mnt);
	preserve(out_fd, target, 1);
	expect_fail(session_fd, TOKEN_FROZEN, target, EBUSY, "frozen target");

	/* Closing the session unpreserves the file, and it can be used. */
	close(out_fd);
	close(retrieve_and_link(session_fd, TOKEN_FROZEN, FROZEN_SIZE, target,
				mnt, "frozen"));
	ksft_print_msg("[STAGE 2] frozen target: OK\n");
}

/*
 * Retrieve into a mount with a size limit. Returns the path of the mount, and
 * the two fds of the retrieved file in @fds.
 */
static const char *check_accounting(int session_fd, int fds[2])
{
	const char *small = mount_hugetlbfs("pagesize=1G,size=1G");
	const char *big = mount_hugetlbfs("pagesize=1G,size=4G");
	int target, fd;

	target = open_tmpfile(small);
	expect_fail(session_fd, TOKEN_ACCOUNTING, target, ENOSPC,
		    "mount too small");
	close(target);
	if (statfs_free(small) != 1)
		fail_exit("small mount: %lu pages free after failed retrieve",
			  statfs_free(small));

	target = open_tmpfile(big);
	fd = retrieve_ok(session_fd, TOKEN_ACCOUNTING, target);
	verify(fd, TOKEN_ACCOUNTING, ACCOUNTING_SIZE);

	if (statfs_free(big) != 2)
		fail_exit("big mount: %lu pages free, expected 2",
			  statfs_free(big));

	fds[0] = fd;
	fds[1] = target;
	return big;
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	int session_fd, linked_fd, unlinked_fd, fd, target, stage;
	const char *mnt, *big;
	int acct_fds[2];
	struct stat a, b;

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	if (stage != 2)
		fail_exit("expected stage 2, got %d", stage);

	session_fd = luo_retrieve_session(luo_fd, SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_retrieve_session");

	check_memfd(session_fd);
	check_bad_targets(session_fd);

	mnt = mount_hugetlbfs("pagesize=1G");
	linked_fd = retrieve_and_link(session_fd, TOKEN_LINKED, LINKED_SIZE,
				      open_tmpfile(mnt), mnt, "linked");
	ksft_print_msg("[STAGE 2] linked file: OK\n");

	mnt = mount_hugetlbfs("pagesize=1G");
	unlinked_fd = retrieve_and_link(session_fd, TOKEN_UNLINKED,
					UNLINKED_SIZE, open_tmpfile(mnt), mnt,
					"unlinked");
	ksft_print_msg("[STAGE 2] unlinked file: OK\n");

	/* Retrieving again gives another fd to the same file. */
	fd = luo_session_retrieve_fd(session_fd, TOKEN_LINKED);
	if (fd < 0)
		fail_exit("second retrieve");
	if (fstat(fd, &a) || fstat(linked_fd, &b) || a.st_ino != b.st_ino)
		fail_exit("second retrieve returned a different file");
	close(fd);
	target = open_tmpfile(mnt);
	expect_fail(session_fd, TOKEN_LINKED, target, EBUSY,
		    "second retrieve with an argument");
	close(target);

	/* A retrieved file stays frozen until finish. */
	if (!ftruncate(linked_fd, 0) || errno != EPERM)
		fail_exit("ftruncate of retrieved file before finish");
	check_frozen_target(luo_fd, session_fd, mnt);

	big = check_accounting(session_fd, acct_fds);

	if (luo_session_finish(session_fd) < 0)
		fail_exit("luo_session_finish");
	close(session_fd);

	/* Freeing the retrieved pages gives the charge back to the mount. */
	close(acct_fds[0]);
	close(acct_fds[1]);
	if (statfs_free(big) != 4)
		fail_exit("big mount: %lu pages free after release, expected 4",
			  statfs_free(big));
	ksft_print_msg("[STAGE 2] accounting: OK\n");

	if (ftruncate(linked_fd, 0))
		fail_exit("ftruncate after finish");
	close(linked_fd);
	close(unlinked_fd);

	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	unmount_all();

	ksft_print_msg("\n--- HUGETLB KEXEC TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
