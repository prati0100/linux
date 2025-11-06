// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME " test: " fmt

#include <linux/init.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include "../../kernel/liveupdate/luo_internal.h"

#define TEST_FLB1_MAGIC 0xFEEDF00DCAFEBEE1ULL
#define TEST_FLB2_MAGIC 0xFEEDF00DCAFEBEE2ULL

/* --- FLB 1 Implementation --- */
static int test_flb1_preserve(struct liveupdate_flb_op_args *argp)
{
	pr_info("test_luo_flb1: preserve was triggered\n");
	argp->data = TEST_FLB1_MAGIC;

	return 0;
}

static void test_flb1_unpreserve(struct liveupdate_flb_op_args *argp)
{
	pr_info("test_luo_flb1: unpreserve was triggered\n");
}

static void test_flb1_retrieve(struct liveupdate_flb_op_args *argp)
{
	if (argp->data == TEST_FLB1_MAGIC) {
		pr_info("test_luo_flb1: found flb data from the previous boot\n");
		argp->obj = (void *)argp->data;
	} else {
		pr_err("test_luo_flb1: ERROR - incorrect data handle: %llx\n",
		       argp->data);
	}
}

static void test_flb1_finish(struct liveupdate_flb_op_args *argp)
{
	if (argp->obj == (void *)TEST_FLB1_MAGIC)
		pr_info("test_luo_flb1: finish was triggered\n");
	else
		pr_err("test_luo_flb1: ERROR - finish called with invalid object\n");
}

static const struct liveupdate_flb_ops test_flb1_ops = {
	.preserve = test_flb1_preserve,
	.unpreserve = test_flb1_unpreserve,
	.retrieve = test_flb1_retrieve,
	.finish = test_flb1_finish,
};

static struct liveupdate_flb test_flb1 = {
	.ops = &test_flb1_ops,
	.compatible = "test-flb-v1",
};

/* --- FLB 2 (identical logic, different compatible string and magic) --- */
static int test_flb2_preserve(struct liveupdate_flb_op_args *argp)
{
	pr_info("test_luo_flb2: preserve was triggered\n");
	argp->data = TEST_FLB2_MAGIC;

	return 0;
}

static void test_flb2_unpreserve(struct liveupdate_flb_op_args *argp)
{
	pr_info("test_luo_flb2: unpreserve was triggered\n");
}

static void test_flb2_retrieve(struct liveupdate_flb_op_args *argp)
{
	if (argp->data == TEST_FLB2_MAGIC) {
		pr_info("test_luo_flb2: found flb data from the previous boot\n");
		argp->obj = (void *)argp->data;
	} else {
		pr_err("test_luo_flb2: ERROR - incorrect data handle: %llx\n",
		       argp->data);
	}
}

static void test_flb2_finish(struct liveupdate_flb_op_args *argp)
{
	if (argp->obj == (void *)TEST_FLB2_MAGIC)
		pr_info("test_luo_flb2: finish was triggered\n");
	else
		pr_err("test_luo_flb2: ERROR - finish called with invalid object\n");
}

static const struct liveupdate_flb_ops test_flb2_ops = {
	.preserve = test_flb2_preserve,
	.unpreserve = test_flb2_unpreserve,
	.retrieve = test_flb2_retrieve,
	.finish = test_flb2_finish,
};

static struct liveupdate_flb test_flb2 = {
	.ops = &test_flb2_ops,
	.compatible = "test-flb-v2",
};

extern struct liveupdate_file_handler memfd_luo_handler;

static int __init liveupdate_test_early_init(void)
{
	void *obj;
	int err;

	liveupdate_init_flb(&test_flb1);
	liveupdate_init_flb(&test_flb2);

	err = liveupdate_flb_incoming_locked(&test_flb1, &obj);
	if (!err) {
		liveupdate_flb_incoming_unlock(&test_flb1, obj);
	} else if (err != -ENODATA) {
		pr_err("liveupdate_flb_incoming_locked flb1 failed: %pe\n",
		       ERR_PTR(err));
	}

	err = liveupdate_flb_incoming_locked(&test_flb2, &obj);
	if (!err) {
		liveupdate_flb_incoming_unlock(&test_flb2, obj);
	} else if (err != -ENODATA) {
		pr_err("liveupdate_flb_incoming_locked flb2 failed: %pe\n",
		       ERR_PTR(err));
	}

	return 0;
}
early_initcall(liveupdate_test_early_init);

void liveupdate_test_register(struct liveupdate_file_handler *h)
{
	int err;

	err = liveupdate_register_flb(h, &test_flb1);
	if (err)
		pr_err("Failed to register flb1 %pe\n", ERR_PTR(err));

	err = liveupdate_register_flb(h, &test_flb2);
	if (err)
		pr_err("Failed to register flb2 %pe\n", ERR_PTR(err));

	err = liveupdate_register_flb(h, &test_flb1);
	if (!err || err != -EEXIST) {
		pr_err("Failed: flb1 should be already registered: %pe\n",
		       ERR_PTR(err));
	}

	pr_info("Registered with file handler: [%s]\n", h->compatible);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pasha Tatashin <pasha.tatashin@soleen.com>");
MODULE_DESCRIPTION("In-kernel test for LUO mechanism");
