// SPDX-License-Identifier: GPL-2.0
/*
 * task_lister.c — Linux Kernel Module: Process Task Lister
 *
 * Traverses the kernel's task_struct list and exposes process information
 * via /proc/task_lister entries. Supports optional filtering by process
 * state via a module parameter.
 *
 * Usage after insmod:
 *   cat /proc/task_lister/tasks       → list all tasks
 *   cat /proc/task_lister/tree        → list tasks as parent→child tree
 *   echo R > /proc/task_lister/filter → filter by state (R/S/D/Z/T)
 *
 * Author: thunderstruckdude
 * Kernel: 5.x / 6.x / 7.x (Arch Linux)
 */

#include <linux/module.h>       /* MODULE_* macros, module_init/exit        */
#include <linux/kernel.h>       /* pr_info(), pr_err(), KERN_* log levels   */
#include <linux/init.h>         /* __init / __exit section markers           */
#include <linux/proc_fs.h>      /* proc_create(), proc_remove(), PDE_DATA() */
#include <linux/seq_file.h>     /* seq_file API: seq_printf, seq_open, etc. */
#include <linux/sched.h>        /* task_struct, for_each_process()           */
#include <linux/sched/signal.h> /* for_each_process macro (kernel ≥ 4.11)   */
#include <linux/sched/task.h>   /* get_task_state()                          */
#include <linux/mm.h>           /* mm_struct, get_task_mm(), mmput()         */
#include <linux/pid.h>          /* pid_t                                     */
#include <linux/cred.h>         /* task_uid(), current_uid()                 */
#include <linux/uaccess.h>      /* copy_from_user()                          */
#include <linux/string.h>       /* strlen(), strncmp()                       */
#include <linux/jiffies.h>      /* jiffies_to_msecs(), HZ                   */
#include <linux/rcupdate.h>     /* rcu_read_lock() / rcu_read_unlock()       */
#include <linux/nsproxy.h>      /* task->nsproxy                             */

/* ─── Module metadata ────────────────────────────────────────────────────── */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Project");
MODULE_DESCRIPTION("Kernel module that lists all tasks via /proc interface");
MODULE_VERSION("1.0.0");

/* ─── Module parameters ──────────────────────────────────────────────────── */
/*
 * filter_state: single-character state filter passed at insmod time.
 *   insmod task_lister.ko filter_state=R   → show only running tasks
 *   insmod task_lister.ko filter_state=S   → show only sleeping tasks
 *   '\0' (default) means show everything.
 *
 * MODULE_PARM_DESC documents the parameter in modinfo output.
 *
 * The custom parameter parser accepts either the state character or its
 * numeric byte value. Or simply write to /proc/task_lister/filter at runtime.
 */
static char filter_state = '\0';

static bool valid_filter_state(char c)
{
	return c == 'R' || c == 'S' || c == 'D' || c == 'Z' ||
	       c == 'T' || c == 't' || c == 'X';
}

static int filter_state_set(const char *val, const struct kernel_param *kp)
{
	unsigned int numeric_value;
	char c;
	int ret;

	(void)kp;

	if (strlen(val) == 1 &&
	    (val[0] == '-' || valid_filter_state(val[0]))) {
		c = val[0];
		if (c == '-')
			c = '\0';
	} else {
		ret = kstrtouint(val, 0, &numeric_value);
		if (ret || numeric_value > 255)
			return -EINVAL;
		c = (char)numeric_value;
		if (c != '\0' && !valid_filter_state(c))
			return -EINVAL;
	}

	WRITE_ONCE(filter_state, c);
	return 0;
}

static int filter_state_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;

	return sysfs_emit(buffer, "%u\n",
			  (unsigned int)(unsigned char)READ_ONCE(filter_state));
}

static const struct kernel_param_ops filter_state_ops = {
	.set = filter_state_set,
	.get = filter_state_get,
};

module_param_cb(filter_state, &filter_state_ops, &filter_state, 0644);
MODULE_PARM_DESC(filter_state,
	"Filter tasks by state char: R=running S=sleeping D=disk-sleep Z=zombie T=stopped (default: show all)");

/* ─── /proc directory and file handles ───────────────────────────────────── */
static struct proc_dir_entry *proc_dir;        /* /proc/task_lister/         */
static struct proc_dir_entry *proc_flat;       /* /proc/task_lister/tasks    */
static struct proc_dir_entry *proc_tree;       /* /proc/task_lister/tree     */
static struct proc_dir_entry *proc_filter;     /* /proc/task_lister/filter   */
static struct proc_dir_entry *proc_stats;      /* /proc/task_lister/stats    */

/* ─── State helpers ──────────────────────────────────────────────────────── */

#define TASK_TREE_MAX_DEPTH 64

/*
 * task_state_char() — map task->__state bitmask to a human-readable char.
 *
 * The kernel stores process state as a bitmask in task_struct.__state:
 *   TASK_RUNNING         = 0   → 'R' (on run queue)
 *   TASK_INTERRUPTIBLE   = 1   → 'S' (sleeping, can be woken by signal)
 *   TASK_UNINTERRUPTIBLE = 2   → 'D' (disk sleep, cannot be interrupted)
 *   __TASK_STOPPED       = 4   → 'T' (stopped, e.g. by SIGSTOP)
 *   __TASK_TRACED        = 8   → 't' (being traced by ptrace)
 *   EXIT_ZOMBIE          = 16  → 'Z' (exited but not yet reaped)
 *   EXIT_DEAD            = 32  → 'X' (fully dead)
 *
 * task_struct also has an exit_state field for zombie/dead transitions.
 */
static char task_state_char(struct task_struct *t)
{
	/* Check exit state first (zombie / dead) */
	if (t->exit_state & EXIT_ZOMBIE)
		return 'Z';
	if (t->exit_state & EXIT_DEAD)
		return 'X';

	/* Running = on a CPU or on a run queue waiting for one */
	if (t->__state == TASK_RUNNING)
		return 'R';

	if (t->__state & TASK_UNINTERRUPTIBLE)
		return 'D';
	if (t->__state & TASK_INTERRUPTIBLE)
		return 'S';
	if (t->__state & __TASK_STOPPED)
		return 'T';
	if (t->__state & __TASK_TRACED)
		return 't';

	return '?';
}

/*
 * task_state_name() — full English name for display in the flat list.
 */
static const char *task_state_name(char c)
{
	switch (c) {
	case 'R': return "Running        ";
	case 'S': return "Sleeping       ";
	case 'D': return "Disk-Sleep     ";
	case 'T': return "Stopped        ";
	case 't': return "Traced         ";
	case 'Z': return "Zombie         ";
	case 'X': return "Dead           ";
	default:  return "Unknown        ";
	}

}

/*
 * task_vmrss_kb() — resident set size (RSS) in kilobytes.
 *
 * We safely obtain the mm_struct with get_task_mm() which increments
 * mm->mm_users under the task lock, ensuring it won't be freed while
 * we read it. We then release it with mmput().
 *
 * mm->rss_stat[] tracks resident pages per category (file, anon, shmem…).
 * get_mm_rss() sums the relevant counters and returns a page count;
 * we convert to KB by multiplying by PAGE_SIZE / 1024.
 */
static unsigned long task_vmrss_kb(struct task_struct *t)
{
	struct mm_struct *mm;
	unsigned long rss = 0;

	mm = get_task_mm(t);
	if (!mm)
		return 0; /* kernel thread — no user address space */

	rss = get_mm_rss(mm) * (PAGE_SIZE / 1024UL);
	mmput(mm);
	return rss;
}

/*
 * task_uptime_ms() — approximate CPU time used (user + kernel) in ms.
 *
 * task_struct keeps utime (user mode) and stime (kernel mode) as
 * 64-bit nanosecond values (u64 in nanoseconds via cputime_to_nsecs).
 * We sum both and convert to milliseconds.
 */
static unsigned long long task_cputime_ms(struct task_struct *t)
{
	u64 total_ns = t->utime + t->stime;

	return (unsigned long long)div_u64(total_ns, 1000000ULL);
}

/* ─── Flat listing (seq_file) ────────────────────────────────────────────── */

/*
 * seq_file is the kernel's standard iterator API for /proc files that
 * produce multi-line output. It works in three phases per output chunk:
 *
 *   start() → returns the first "position" token (or NULL = empty)
 *   show()  → formats one record from that token into the seq buffer
 *   next()  → advances to the next token
 *   stop()  → cleanup after iteration ends or is aborted
 *
 * The kernel may call start/stop multiple times if the user reads in
 * small chunks (e.g., head -5). The seq buffer is flushed to userspace
 * between calls; we never deal with copy_to_user() ourselves.
 */

/*
 * flat_start() — begin iteration.
 *
 * We use the global init_task as the starting point. init_task (PID 1's
 * ancestor, actually PID 0 — the idle task) is the head of the doubly
 * linked list of all task_structs via task_struct.tasks.
 *
 * *pos == 0 means "print the header first"; we return SEQ_START_TOKEN
 * as a sentinel so show() knows to emit the header line.
 */
static void *flat_start(struct seq_file *m, loff_t *pos)
{
	/* Lock RCU so task_structs are stable while we walk the list */
	rcu_read_lock();

	if (*pos == 0)
		return SEQ_START_TOKEN; /* triggers header in show() */

	/* Otherwise return the (*pos - 1)-th task in the list */
	{
		struct task_struct *t = &init_task;
		loff_t i = 0;

		for_each_process(t) {
			if (i++ == *pos - 1)
				return t;
		}
	}
	return NULL; /* past end */
}

/*
 * flat_next() — advance to the next task in the global list.
 *
 * next_task(t) follows t->tasks.next back to the containing task_struct.
 * When we wrap around to init_task we've seen everything → return NULL.
 */
static void *flat_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct task_struct *t;

	(*pos)++;

	if (v == SEQ_START_TOKEN) {
		/* After header: return the very first real process */
		t = next_task(&init_task);
		return (t == &init_task) ? NULL : t;
	}

	t = next_task((struct task_struct *)v);
	return (t == &init_task) ? NULL : t;
}

/* flat_stop() — release the RCU read lock we took in start(). */
static void flat_stop(struct seq_file *m, void *v)
{
	rcu_read_unlock();
}

/*
 * flat_show() — format one task as a table row.
 *
 * seq_printf() works exactly like snprintf() but writes into the
 * seq_file's internal buffer; the kernel drains that buffer to userspace
 * automatically in PAGE_SIZE-sized chunks.
 */
static int flat_show(struct seq_file *m, void *v)
{
	struct task_struct *t;
	char state;
	char filter;
	uid_t uid;
	unsigned long rss;
	unsigned long long cpu_ms;

	/* Print the column header when v is the sentinel token */
	if (v == SEQ_START_TOKEN) {
		seq_puts(m, "╔══════╦════════╦════════════════════════════╦═════════════════╦══════════╦══════════╗\n");
		seq_puts(m, "║  PID ║  PPID  ║ NAME                       ║ STATE           ║  RSS(KB) ║  CPU(ms) ║\n");
		seq_puts(m, "╠══════╬════════╬════════════════════════════╬═════════════════╬══════════╬══════════╣\n");
		return 0;
	}

	t = (struct task_struct *)v;
	state = task_state_char(t);
	filter = READ_ONCE(filter_state);

	/* Apply state filter if one is set */
	if (filter != '\0' && state != filter)
		return 0;

	uid   = from_kuid_munged(&init_user_ns, task_uid(t));
	rss   = task_vmrss_kb(t);
	cpu_ms = task_cputime_ms(t);

	seq_printf(m, "║%6d║%8d║ %-26.26s ║ %c — %-11s║%10lu║%10llu║\n",
		   t->pid,
		   t->real_parent ? t->real_parent->pid : 0,
		   t->comm,         /* process name (truncated to TASK_COMM_LEN=16) */
		   state,
		   task_state_name(state),
		   rss,
		   cpu_ms);

	return 0;
}

/* Wire the four callbacks into a seq_operations struct */
static const struct seq_operations flat_seq_ops = {
	.start = flat_start,
	.next  = flat_next,
	.stop  = flat_stop,
	.show  = flat_show,
};

/* open() handler: connects the seq_operations to the file descriptor */
static int flat_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &flat_seq_ops);
}

static const struct proc_ops flat_fops = {
	.proc_open    = flat_open,
	.proc_read    = seq_read,    /* kernel-provided: drains seq buffer */
	.proc_lseek   = seq_lseek,  /* kernel-provided: handles seeking    */
	.proc_release = seq_release, /* kernel-provided: frees seq state    */
};

/* ─── Tree listing ───────────────────────────────────────────────────────── */

/*
 * print_task_tree() — recursively print a process and all its children.
 *
 * Every task_struct has a ->children list_head which is the head of a
 * doubly-linked list of its direct children. list_for_each_entry()
 * walks that list and gives us each child as a task_struct via the
 * ->sibling member (the list_head embedded in each child task_struct).
 *
 * We use 'depth' to indent and draw ASCII tree connectors.
 *
 * NOTE: Called under rcu_read_lock() — we must not sleep or block.
 */
static void print_task_tree(struct seq_file *m,
			    struct task_struct *task,
			    int depth)
{
	struct task_struct *child;
	char state = task_state_char(task);
	int i;

	if (depth >= TASK_TREE_MAX_DEPTH) {
		for (i = 0; i < depth; i++)
			seq_puts(m, "  │  ");
		seq_puts(m, "  └─ ... children omitted (depth limit) ...\n");
		return;
	}

	/* Indentation + tree connector */
	for (i = 0; i < depth; i++)
		seq_puts(m, "  │  ");
	if (depth > 0)
		seq_puts(m, "  ├─ ");

	seq_printf(m, "[%c] %-16s  pid=%-6d  ppid=%d\n",
		   state,
		   task->comm,
		   task->pid,
		   task->real_parent ? task->real_parent->pid : 0);

	/* Recurse into children */
	list_for_each_entry(child, &task->children, sibling) {
		print_task_tree(m, child, depth + 1);
	}
}

/*
 * tree_show_all() — walk top-level processes and print each subtree.
 *
 * for_each_process() iterates the global process list (not threads).
 * We only print tasks whose parent is init_task (PID 0) or whose
 * parent PID equals 1 (systemd/init) so we don't duplicate entries;
 * the recursion above handles the rest.
 *
 * Actually the cleanest approach: print only tasks where
 * real_parent == &init_task OR real_parent->pid == 1 (kthreadd/init).
 * Everything else is printed recursively through the children lists.
 */
static int tree_show(struct seq_file *m, void *v)
{
	seq_puts(m, "\n=== Process Tree ===\n\n");

	rcu_read_lock();

	/*
	 * Start from init_task (swapper/0, PID 0).
	 * Its children include kthreadd (PID 2) and init/systemd (PID 1).
	 */
	print_task_tree(m, &init_task, 0);

	rcu_read_unlock();

	seq_puts(m, "\n");
	return 0;
}

/*
 * tree_open() — use single_open() since we have one monolithic show function.
 *
 * single_open() is simpler than seq_open(); it calls show() exactly once
 * and is suitable when we don't need start/next/stop iteration.
 */
static int tree_open(struct inode *inode, struct file *file)
{
	return single_open(file, tree_show, NULL);
}

static const struct proc_ops tree_fops = {
	.proc_open    = tree_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release, /* matches single_open */
};

/* ─── Filter write interface ─────────────────────────────────────────────── */

/*
 * filter_read() — show the current filter state character.
 */
static int filter_show(struct seq_file *m, void *v)
{
	char filter = READ_ONCE(filter_state);

	if (filter == '\0')
		seq_puts(m, "none (showing all tasks)\n");
	else
		seq_printf(m, "%c (%s)\n", filter, task_state_name(filter));
	return 0;
}

static int filter_open(struct inode *inode, struct file *file)
{
	return single_open(file, filter_show, NULL);
}

/*
 * filter_write() — let userspace update the filter at runtime.
 *
 *   echo R > /proc/task_lister/filter   → show only Running tasks
 *   echo -  > /proc/task_lister/filter  → clear filter (show all)
 *
 * copy_from_user() safely copies from the user's buffer into kernel space.
 * We only need 1 byte.
 */
static ssize_t filter_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	char buf[4] = {0};

	if (count == 0)
		return 0;

	if (copy_from_user(buf, ubuf, min(count, sizeof(buf) - 1)))
		return -EFAULT;

	/* Strip newline if echoed by shell */
	if (buf[0] == '\n' || buf[0] == '-' || buf[0] == '\0') {
		WRITE_ONCE(filter_state, '\0');
		pr_info("task_lister: filter cleared — showing all tasks\n");
	} else {
		char c = buf[0];

		if (!valid_filter_state(c)) {
			pr_warn("task_lister: invalid filter state '%c'. "
				"Use R/S/D/Z/T/t/X or - to clear.\n", c);
			return -EINVAL;
		}
		WRITE_ONCE(filter_state, c);
		pr_info("task_lister: filter set to '%c' (%s)\n",
			c, task_state_name(c));
	}

	return count;
}

static const struct proc_ops filter_fops = {
	.proc_open    = filter_open,
	.proc_read    = seq_read,
	.proc_write   = filter_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ─── Stats summary ──────────────────────────────────────────────────────── */

/*
 * stats_show() — aggregate counts: total tasks, per-state breakdown,
 *                total RSS, heaviest process.
 */
static int stats_show(struct seq_file *m, void *v)
{
	struct task_struct *t;
	int total = 0;
	int cnt_R = 0, cnt_S = 0, cnt_D = 0, cnt_Z = 0;
	int cnt_T = 0, cnt_other = 0;
	unsigned long total_rss = 0;
	unsigned long max_rss = 0;
	char max_name[TASK_COMM_LEN] = "<none>";
	pid_t max_pid = 0;

	rcu_read_lock();

	for_each_process(t) {
		char s = task_state_char(t);
		unsigned long rss = task_vmrss_kb(t);

		total++;
		total_rss += rss;

		if (rss > max_rss) {
			max_rss = rss;
			max_pid = t->pid;
			strscpy(max_name, t->comm, TASK_COMM_LEN);
		}

		switch (s) {
		case 'R': cnt_R++;     break;
		case 'S': cnt_S++;     break;
		case 'D': cnt_D++;     break;
		case 'Z': cnt_Z++;     break;
		case 'T': cnt_T++;     break;
		default:  cnt_other++; break;
		}
	}

	rcu_read_unlock();

	seq_puts(m,  "╔══════════════════════════════════════╗\n");
	seq_puts(m,  "║       Task Lister — Statistics       ║\n");
	seq_puts(m,  "╠══════════════════════════════════════╣\n");
	seq_printf(m,"║  Total tasks        : %-14d║\n", total);
	seq_puts(m,  "║──────────────────────────────────────║\n");
	seq_printf(m,"║  Running      (R)   : %-14d║\n", cnt_R);
	seq_printf(m,"║  Sleeping     (S)   : %-14d║\n", cnt_S);
	seq_printf(m,"║  Disk-Sleep   (D)   : %-14d║\n", cnt_D);
	seq_printf(m,"║  Stopped      (T)   : %-14d║\n", cnt_T);
	seq_printf(m,"║  Zombie       (Z)   : %-14d║\n", cnt_Z);
	seq_printf(m,"║  Other              : %-14d║\n", cnt_other);
	seq_puts(m,  "║──────────────────────────────────────║\n");
	seq_printf(m,"║  Total RSS          : %-9lu KB   ║\n", total_rss);
	seq_printf(m,"║  Heaviest process   : %-14s║\n", max_name);
	seq_printf(m,"║    PID              : %-14d║\n", max_pid);
	seq_printf(m,"║    RSS              : %-9lu KB   ║\n", max_rss);
	seq_puts(m,  "╚══════════════════════════════════════╝\n");

	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, stats_show, NULL);
}

static const struct proc_ops stats_fops = {
	.proc_open    = stats_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ─── Module init / exit ─────────────────────────────────────────────────── */

/*
 * task_lister_init() — called when the module is loaded (insmod).
 *
 * __init tells the linker to place this function in a special section
 * (.init.text) that the kernel frees after the module is fully loaded,
 * saving memory since init code is never called again.
 *
 * We create:
 *   /proc/task_lister/           (directory)
 *   /proc/task_lister/tasks      (flat table, readable)
 *   /proc/task_lister/tree       (parent→child tree, readable)
 *   /proc/task_lister/filter     (read/write state filter)
 *   /proc/task_lister/stats      (summary statistics)
 */
static int __init task_lister_init(void)
{
	pr_info("task_lister: loading module v1.0.0\n");

	/* Create /proc/task_lister/ directory */
	proc_dir = proc_mkdir("task_lister", NULL);
	if (!proc_dir) {
		pr_err("task_lister: failed to create /proc/task_lister/\n");
		return -ENOMEM;
	}

	/* /proc/task_lister/tasks — flat list, world-readable */
	proc_flat = proc_create("tasks", 0444, proc_dir, &flat_fops);
	if (!proc_flat) {
		pr_err("task_lister: failed to create tasks entry\n");
		goto err_flat;
	}

	/* /proc/task_lister/tree — process tree, world-readable */
	proc_tree = proc_create("tree", 0444, proc_dir, &tree_fops);
	if (!proc_tree) {
		pr_err("task_lister: failed to create tree entry\n");
		goto err_tree;
	}

	/* /proc/task_lister/filter — read by all, write by root (0644) */
	proc_filter = proc_create("filter", 0644, proc_dir, &filter_fops);
	if (!proc_filter) {
		pr_err("task_lister: failed to create filter entry\n");
		goto err_filter;
	}

	/* /proc/task_lister/stats — statistics summary, world-readable */
	proc_stats = proc_create("stats", 0444, proc_dir, &stats_fops);
	if (!proc_stats) {
		pr_err("task_lister: failed to create stats entry\n");
		goto err_stats;
	}

	pr_info("task_lister: module loaded successfully.\n");
	pr_info("task_lister: /proc/task_lister/{tasks,tree,filter,stats} created.\n");

	if (READ_ONCE(filter_state) != '\0')
		pr_info("task_lister: initial filter: '%c'\n",
			READ_ONCE(filter_state));

	return 0; /* success */

	/* Unwind on partial failure (reverse order of creation) */
err_stats:
	proc_remove(proc_filter);
err_filter:
	proc_remove(proc_tree);
err_tree:
	proc_remove(proc_flat);
err_flat:
	proc_remove(proc_dir);
	return -ENOMEM;
}

/*
 * task_lister_exit() — called when the module is unloaded (rmmod).
 *
 * __exit marks the function for placement in .exit.text — the kernel
 * discards it if the module is built-in (not a .ko), since built-in
 * modules can never be unloaded.
 *
 * proc_remove() on the directory removes all children automatically
 * in modern kernels (≥ 3.10), but we remove children explicitly for
 * clarity and correctness on older-API builds.
 */
static void __exit task_lister_exit(void)
{
	proc_remove(proc_stats);
	proc_remove(proc_filter);
	proc_remove(proc_tree);
	proc_remove(proc_flat);
	proc_remove(proc_dir);

	pr_info("task_lister: module unloaded. /proc/task_lister removed.\n");
}

/* Register init and exit hooks with the kernel module subsystem */
module_init(task_lister_init);
module_exit(task_lister_exit);
