// SPDX-License-Identifier: GPL-3.0
/*
 * crashtest: a module for crashing the kernel in many different ways
 *
 * NOTE: most of the code is based on drivers/misc/lkdtm.c
 *
 * Copyright (C) 2019 Andrea Righi <andrea.righi@canonical.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>

enum ctype {
	CT_NONE,
	CT_PANIC,
	CT_BUG,
	CT_EXCEPTION,
	CT_LOOP,
	CT_OVERFLOW,
	CT_CORRUPT_STACK,
	CT_UNALIGNED_LOAD_STORE_WRITE,
	CT_OVERWRITE_ALLOCATION,
	CT_WRITE_AFTER_FREE,
	CT_SOFTLOCKUP,
	CT_HARDLOCKUP,
	CT_HUNG_TASK,
	CT_SCHEDULING_WHILE_ATOMIC,
	CT_DEADLOCK,
};

static char *ct_type[] = {
	"PANIC",
	"BUG",
	"EXCEPTION",
	"LOOP",
	"OVERFLOW",
	"CORRUPT_STACK",
	"UNALIGNED_LOAD_STORE_WRITE",
	"OVERWRITE_ALLOCATION",
	"WRITE_AFTER_FREE",
	"SOFTLOCKUP",
	"HARDLOCKUP",
	"HUNG_TASK",
	"SCHEDULING_WHILE_ATOMIC",
	"DEADLOCK",
};

static const char procfs_name[] = "crashtest";

static struct proc_dir_entry *procfs_file;

static enum ctype parse_ct_type(const char *what)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ct_type); i++) {
		if (!strcmp(what, ct_type[i]))
			return i + 1;
	}
	return CT_NONE;
}

static void scheduling_while_atomic(void)
{
	static DECLARE_RWSEM(sleep_lock);
	static DEFINE_RWLOCK(atomic_lock);

	down_read(&sleep_lock);
	read_lock(&atomic_lock);
	schedule_timeout_interruptible(1);
	read_unlock(&atomic_lock);
	up_read(&sleep_lock);
}

static void deadlock_splat(void)
{
	static DEFINE_SPINLOCK(lock1);
	static DEFINE_SPINLOCK(lock2);

	/* lock1 -> lock2 */
	spin_lock(&lock1);
	spin_lock(&lock2);
	spin_unlock(&lock2);
	spin_unlock(&lock1);

	/* lock2 -> lock1 */
	spin_lock(&lock2);
	spin_lock(&lock1);
	spin_unlock(&lock1);
	spin_unlock(&lock2);
}

#define BUFSIZE (THREAD_SIZE / 8)

static noinline int recursive_loop(int a)
{
	static int recur_count = 40;
	char buf[BUFSIZE];

	memset(buf, 0xff, sizeof(buf));
	recur_count--;
	if (!recur_count)
		return 0;
	else
		return recursive_loop(a);
}

static noinline void corrupt_stack(void *stack)
{
	memset(stack, 0xff, 64);
}

static void do_crash(enum ctype which)
{
	switch (which) {
	case CT_PANIC:
		panic("have a nice day... ;-)");
		break;
	case CT_BUG:
		BUG();
		break;
	case CT_EXCEPTION:
		*((int *)NULL) = 0;
		break;
	case CT_LOOP:
		for (;;)
			;
		break;
	case CT_OVERFLOW:
		(void)recursive_loop(0);
		break;
	case CT_CORRUPT_STACK: {
		char data[8] __aligned(sizeof(void *));
		corrupt_stack(&data);
		break;
	}
	case CT_UNALIGNED_LOAD_STORE_WRITE: {
		static u8 align_data[5] __attribute__((aligned(4))) = {1, 2, 3,
								       4, 5};
		u32 *p;
		u32 val = 0x12345678;

		p = (u32 *)(align_data + 1);
		if (*p == 0)
			val = 0x87654321;
		*p = val;
		 break;
	}
	case CT_OVERWRITE_ALLOCATION: {
		size_t len = 1020;
		unsigned long *data = kmalloc(len, GFP_KERNEL);

		data[1024 / sizeof(unsigned long)] = 0x12345678;
		kfree(data);
		break;
	}
	case CT_WRITE_AFTER_FREE: {
		size_t len = 1024;
		u32 *data = kmalloc(len, GFP_KERNEL);

		kfree(data);
		schedule();
		memset(data, 0x78, len);
		break;
	}
	case CT_SOFTLOCKUP:
		preempt_disable();
		for (;;)
			cpu_relax();
		break;
	case CT_HARDLOCKUP:
		local_irq_disable();
		for (;;)
			cpu_relax();
		break;
	case CT_HUNG_TASK:
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule();
		break;
	case CT_SCHEDULING_WHILE_ATOMIC:
		scheduling_while_atomic();
		break;
	case CT_DEADLOCK:
		deadlock_splat();
		break;
	case CT_NONE:
	default:
		break;
	}
}

static int procfs_read(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ct_type); i++)
		seq_printf(m, "%s\n", ct_type[i]);
	return 0;
}

static ssize_t procfs_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *pos)
{
        char buf[32] = {};

	if (count > sizeof(buf) - 1)
		return -E2BIG;
        if (copy_from_user(buf, ubuf, count))
                return -EFAULT;

	/* NULL-terminate and remove newline at the end */
	buf[count] = '\0';
	strim(buf);

	/* Execute crash routine */
	do_crash(parse_ct_type(buf));

	return count;
}

static int procfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, procfs_read, NULL);
}

static const struct proc_ops procfs_fops = {
	.proc_open	= procfs_open,
	.proc_read	= seq_read,
	.proc_write	= procfs_write,
	.proc_release	= seq_release,
};

static int __init test_init(void)
{
	procfs_file = proc_create(procfs_name, 0666, NULL, &procfs_fops);
	if (unlikely(!procfs_file))
		return -ENOMEM;
	return 0;
}

static void __exit test_exit(void)
{
	remove_proc_entry(procfs_name, NULL);
}

module_init(test_init);
module_exit(test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrea Righi <andrea.righi@canonical.com>");
MODULE_DESCRIPTION("crash the kernel in many different ways");
