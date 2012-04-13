/* This module saves the information about the events it receives from the 
 * core to a file in debugfs. The parameters of the module control which
 * types of events to report this way.
 * 
 * The module can operate in two modes, depending on the value of 
 * 'target_function' parameter:
 * - if the parameter has an empty value, all events allowed by "report_*"
 * parameters will be reported;
 * - if the parameter has a non-empty value (name of the function), only the
 * events starting from the first entry to the function and up to the exit
 * from that function in the same thread will be reported (and only the 
 * events from that thread will be reported) if enabled by "report_*".
 * 
 * Note that in the second mode, the module cannot handle the targets where
 * that function is called recursively (the reporter must not crash but the 
 * report itself is likely to contain less data than expected).
 * 
 * Format of the output records is as follows if 'resolve_symbols' parameter
 * has a non-zero value (the leading spaces are only for readability). 
 * - Format of the records for function entry events:
 *	TID=<tid,0x%lx> FENTRY name="<%pf for the function>"
 * - Format of the records for function exit events:
 *	TID=<tid,0x%lx> FEXIT name="<%pf for the function>"
 * - Format of the records for "call pre" events:
 *	TID=<tid,0x%lx> CALL_PRE pc=<pc,%pS> name="<%pf for the callee>"
 * - Format of the records for "call post" events:
 *	TID=<tid,0x%lx> CALL_POST pc=<pc,%pS> name="<%pf for the callee>"
 * - Format of the records for memory access events:
 *	TID=<tid,0x%lx> <type> pc=<pc,%pS> addr=<addr,%pS> size=<%lu>
 * <type> is the type of the access, namely READ, WRITE or UPDATE.
 * - Format of the records for locked update events:
 *	TID=<tid,0x%lx> LOCKED <type> pc=<pc,%pS> addr=<addr,%pS> size=<%lu>
 * - Format of the records for I/O memory events:
 *	TID=<tid,0x%lx> IO_MEM <type> pc=<pc,%pS> addr=<addr,%pS> size=<%lu>
 * - Format of the records for pre-/post- memory barrier events: 
 * 	TID=<tid,0x%lx> BARRIER <btype> PRE pc=<pc,%pS>
 * 	TID=<tid,0x%lx> BARRIER <btype> POST pc=<pc,%pS>
 * <btype> is the type of the barrier, namely FULL, LOAD or STORE.
 *
 * If 'resolve_symbols' parameter is 0, the format is almost the same as 
 * described above except the following:
 * - "%p" specifier is used instead of "%pf" and "%pS";
 * - "name=" and the double quotes following it are not output. 
 * Symbol resolution takes time, so if its overhead is significant, it can 
 * be helpful to set 'resolve_symbols' to 0.
 * 
 * If 'zero_unknown' parameter is non-zero, the addresses that are 
 * unresolved after "%pS" and additional symbol table lookup has been used 
 * for them will be replaced with "0x0". This can be used to simplify 
 * testing if the exact values of the unresolved addresses are not 
 * important. 
 * Note that 'zero_unknown' does affects only the addresses to be output 
 * using "%pS" specifier. For example, the function names output using "%pf"
 * are not affected.
 *
 * The user may pass an additional symbol table to the reporter by writing 
 * it to the file "kedr_test_reporter/symbol_table" in debugfs. It will be 
 * used only if symbol resolution is turned on. The lookup in the additional
 * symbol table happens first and only if the symbol is not found there, 
 * the standard kallsyms-based mechanism is used.
 * This can be helpful when resolving data symbols if CONFIG_KALLSYMS_ALL 
 * kernel parameter is 'n' (the default kernel on Debian 6 is a common 
 * example).
 *
 * [NB] The reporter does not report the events that occur during the 
 * initialization of the target module if 'resolve_symbols' parameter has a
 * non-zero value. For these events, symbol resolution is unsafe and may 
 * sometimes lead to system crashes. Symbol resolution is used, for example, 
 * when printing with "%pf", "%pS" or similar specifiers.
 * After the init function of the target module has finished, the module 
 * loader changes some fields in struct module that kallsyms subsystem uses
 * (e.g. the pointers to the string table and the symbol table). It is 
 * better to avoid going into a race condition on these fields. So it is
 * safer not to use symbol resolution (and kallsyms in general) in such 
 * conditions. */

/* ========================================================================
 * Copyright (C) 2012, KEDR development team
 * Authors: 
 *      Eugene A. Shatokhin <spectre@ispras.ru>
 *      Andrey V. Tsyvarev  <tsyvarev@ispras.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 ======================================================================== */
 
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/kallsyms.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <asm/uaccess.h>

#include <kedr/kedr_mem/core_api.h>
#include <kedr/object_types.h>

#include "debug_util.h"
/* ====================================================================== */

#define KEDR_MSG_PREFIX "[kedr_test_reporter] "
/* ====================================================================== */

MODULE_AUTHOR("Eugene A. Shatokhin");
MODULE_LICENSE("GPL");
/* ====================================================================== */

/* The name of the function to report the events for. */
char *target_function = "";
module_param(target_function, charp, S_IRUGO);

/* The maximum number of events that can be reported in a single session
 * (from loading to unloading of the target or from the function entry to 
 * the function exit). After this number of events has been reported, the 
 * following events in this session will be skipped. 
 * The greater this parameter is, the more memory the module needs to 
 * contain the report. */
unsigned int max_events = 65536;
module_param(max_events, uint, S_IRUGO);

/* If non-zero, call pre/post and function entry/exit events will be 
 * reported. */
int report_calls = 0;
module_param(report_calls, int, S_IRUGO);

/* If non-zero, memory access events as well as memory barrier events
 * (incl. xFENCE, locked updates as well as other serializing operations)
 * will be reported. */
int report_mem = 0;
module_param(report_mem, int, S_IRUGO);

/* If non-zero, the reporter will try to resolve the memory addresses in
 * the report, i.e. determine the names of the corresponding symbols, 
 * etc. */
int resolve_symbols = 1;
module_param(resolve_symbols, int, S_IRUGO);

/* If symbol resolution is enabled and this parameter is non-zero, "(null)"
 * will be output instead of the unresolved addresses.
 * Has no effect if symbol resolution is disabled. */
int zero_unknown = 0;
module_param(zero_unknown, int, S_IRUGO);
/* ====================================================================== */

/* A directory for the module in debugfs. */
static struct dentry *debugfs_dir_dentry = NULL;
const char *debugfs_dir_name = "kedr_test_reporter";
/* ====================================================================== */

/* A single-threaded (ordered) workqueue where the requests to handle the 
 * events are placed. The requests are guaranteed to be serviced strictly 
 * one-by-one, in FIFO order. 
 *
 * When the target has executed its cleanup function and is about to unload,
 * the workqueue should be flushed and our on_target_unload() handler would 
 * therefore wait for all pending requests to be processed. */
static struct workqueue_struct *wq = NULL; 

/* The name of the workqueue. */
static const char *wq_name = "kedr_rp_wq";

/* A spinlock that protects the top half of event handling, that is, adding
 * elements to the workqueue. The bottom half (processing these elements) is
 * taken care of by the workqueue itself. The workqueue is ordered, so no
 * additional synchronization is needed there. */
static DEFINE_SPINLOCK(wq_lock);
/* ====================================================================== */

/* This flag specifies if the events should be reported. */
static int within_target_func = 0;

/* Restrict the reported events to a span from the first entry to the given
 * function up to the exit from that function in the same thread. In 
 * addition, if this flag is non-zero, only the events from the same thread
 * as for that function entry will be reported.
 * If this flag is 0, no such restrictions are imposed. That is, if 
 * "report_*" parameters indicate that a given type of events should be 
 * reported, all events of that type will be reported no matter in which 
 * function and in which thread they occur. */
static int restrict_to_func = 0;

/* The number of events reported in the current session so far. */
static unsigned int ecount = 0;

/* The start address of the target function. */
static unsigned long target_start = 0;

#define KEDR_ALL_THREADS ((unsigned long)(-1))

/* The ID of the thread to report the events for. If it is KEDR_ALL_THREADS,
 * no restriction on thread ID is imposed. */
static unsigned long target_tid = KEDR_ALL_THREADS;

/* The target. */
static struct module *target_module = NULL;
/* ====================================================================== */

/* A file in debugfs that a user may write an additional symbol table to. 
 * The format is as follows. For each symbol of interest, a line should be
 * written that contains the following fields (in order) separated by a 
 * single space each time:
 * 	<name> <size> <section_address> <offset> 
 * <name> - name of the symbol, a string;
 * <size> - size of the symbol, a decimal number;
 * <section_address> - the address of the section the symbol belongs to,
 *   a hex number, possibly prefixed with "0x";
 * <offset> - offset of the symbol in the section, a hex number, possibly 
 * prefixed with "0x". */
static struct dentry *symtab_file = NULL;
static const char *symtab_file_name = "symbol_table";

/* Non-zero if the symbol table file is currently open, 0 otherwise.
 * Trying to open the file while it is already open should result in an 
 * error (EBUSY). 
 * This variable should be accessed with symtab_mutex locked. */
static int symtab_file_is_open = 0;

/* A mutex to protect symtab-related data. */
static DEFINE_MUTEX(symtab_mutex);

/* The initial size of the buffer. */
#define KR_SYMTAB_BUF_SIZE 4096

/* The access to this structure should be protected by 'symtab_mutex'. */
struct kr_input_buffer
{
	/* the buffer itself */
	char *buf;
	
	/* the current size of the buffer */
	size_t size;
};
static struct kr_input_buffer krib = {
	.buf = NULL,
	.size = 0,
};

/* The additional symbol table is actually a list of 'struct kr_symbol' 
 * instances. */
struct kr_symbol
{
	struct list_head list;
	
	/* Pointer to the name of the symbol in the input buffer. */
	const char *name;
	
	/* Start address of the symbol. */
	unsigned long addr;
	
	/* Size of the symbol. */
	unsigned long size;
};

/* The additional symbol table. Should be accessed only with 'symtab_mutex'
 * locked. */
static LIST_HEAD(symbol_list);
/* ====================================================================== */

/* Call this function to (re)initialize the input buffer (most likely, in
 * open() handler for the file). */
static int
input_buffer_init(struct kr_input_buffer *ib)
{
	BUG_ON(ib == NULL);
	BUG_ON(ib->buf != NULL);
	
	ib->buf = vmalloc(KR_SYMTAB_BUF_SIZE);
	if (ib->buf == NULL)
		return -ENOMEM;
	
	memset(ib->buf, 0, KR_SYMTAB_BUF_SIZE);
	
	ib->size = KR_SYMTAB_BUF_SIZE;
	return 0;
}

/* Call this function to clean up the input buffer (most likely, in the 
 * cleanup function for this module). */
static void
input_buffer_cleanup(struct kr_input_buffer *ib)
{
	vfree(ib->buf);
	ib->buf = NULL;
	ib->size = 0;
}

/* Enlarge the buffer to make it at least 'new_size' bytes in size.
 * If 'new_size' is less than or equal to 'ib->size', the function does 
 * nothing.
 * If there is not enough memory, the function outputs an error to 
 * the system log, leaves the buffer intact and returns -ENOMEM.
 * 0 is returned in case of success. */
static int
input_buffer_resize(struct kr_input_buffer *ib, size_t new_size)
{
	size_t size;
	void *p;
	
	BUG_ON(ib == NULL || ib->buf == NULL);

	if (ib->size >= new_size)
		return 0;
	
	/* Allocate memory in the multiples of the default size. */
	size = (new_size / KR_SYMTAB_BUF_SIZE + 1) * KR_SYMTAB_BUF_SIZE;
	p = vmalloc(size);
	if (p == NULL) {
		pr_warning(KEDR_MSG_PREFIX "input_buffer_resize: "
	"not enough memory to resize the buffer to %zu bytes\n",
			size);
		return -ENOMEM;
	}
	
	memset(p, 0, size);
	memcpy(p, ib->buf, ib->size);
	
	vfree(ib->buf);
	ib->buf = (char *)p;
	ib->size = size;

	return 0;
}

static void 
symbol_list_destroy(struct list_head *sl)
{
	struct kr_symbol *sym;
	struct kr_symbol *tmp;
	
	BUG_ON(sl == NULL);
	
	list_for_each_entry_safe(sym, tmp, sl, list) {
		list_del(&sym->list);
		kfree(sym);
	}
}

static void
symtab_cleanup(void)
{
	symbol_list_destroy(&symbol_list);
	input_buffer_cleanup(&krib);
}

/* Parse the data contained in 'krib', create the elements of the symbol 
 * list. Should be called with 'symtab_mutex' locked. */
static int
load_symbol_list(void)
{
	/* Loosen the format a bit, for simplicity: allow spaces, tabs and
	 * newlines between the fields. */
	const char *ws = " \t\n\r";
	size_t pos;
	int ret = 0;
		
	BUG_ON(krib.buf == NULL || krib.size == 0);
	
	if (krib.buf[0] == 0)
		return 0; /* No symbol table, nothing to do. */
	
	pos = strspn(krib.buf, ws);
	while (pos < krib.size - 1 && krib.buf[pos] != 0) {
		char *name;
		char *str_end;
		unsigned long addr;
		unsigned long size;
		unsigned long offset;
		size_t num;
		struct kr_symbol *sym;
		
		/* <name> */
		num = strcspn(&krib.buf[pos], ws);
		if (num == 0) {
			ret = -EINVAL;
			goto out;
		}
		name = &krib.buf[pos];
		krib.buf[pos + num] = 0;
		pos += num + 1;
		
		pos += strspn(&krib.buf[pos], ws);
		if (pos >= krib.size - 1) {
			ret = -EINVAL;
			goto out;
		}

		/* <size> */
		size = simple_strtoul(&krib.buf[pos], &str_end, 10);
		num = strspn(str_end, ws);
		pos = str_end - krib.buf + num;
		if (num == 0 || pos >= krib.size - 1) {
			ret = -EINVAL;
			goto out;
		}
		
		/* <section_address> */
		addr = simple_strtoul(&krib.buf[pos], &str_end, 16);
		num = strspn(str_end, ws);
		pos = str_end - krib.buf + num;
		if (num == 0 || pos >= krib.size - 1) {
			ret = -EINVAL;
			goto out;
		}
		
		/* <offset> */
		offset = simple_strtoul(&krib.buf[pos], &str_end, 16);
		num = strspn(str_end, ws);
		pos = str_end - krib.buf + num;
		if (num == 0) {
			ret = -EINVAL;
			goto out;
		}
		
		sym = kzalloc(sizeof(*sym), GFP_KERNEL);
		if (sym == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		sym->name = name;
		sym->addr = addr + offset;
		sym->size = size;
		list_add_tail(&sym->list, &symbol_list);
	}
	return 0;
	
out:
	symbol_list_destroy(&symbol_list);
	return ret;
}

/* Checks if the address 'addr' belongs to a symbol in the list. 
 * Returns a pointer to the corresponding 'kr_symbol' if found, NULL 
 * otherwise. Should be called with 'symtab_mutex' locked. */
static const struct kr_symbol *
kr_symbol_lookup(unsigned long addr)
{
	struct kr_symbol *sym;
	list_for_each_entry(sym, &symbol_list, list) {
		if (addr >= sym->addr && addr < sym->addr + sym->size)
			return sym;
	}
	return NULL;
}
/* ====================================================================== */

static int 
symtab_file_open(struct inode *inode, struct file *filp)
{
	int ret = 0;
	
	if (mutex_lock_killable(&symtab_mutex) != 0)
	{
		pr_warning(KEDR_MSG_PREFIX "symtab_file_open: "
			"got a signal while trying to acquire a mutex.\n");
		return -EINTR;
	}
	
	/* It is not allowed to have this file opened by several threads at
	 * the same time. */
	if (symtab_file_is_open) {
		ret = -EBUSY;
		goto out;
	}
	
	/* Remove the previous contents of the symbol table and then 
	 * reinitialize it. */
	symtab_cleanup();
	ret = input_buffer_init(&krib);
	if (ret != 0)
		goto out;
	
	symtab_file_is_open = 1;
	mutex_unlock(&symtab_mutex);
	return nonseekable_open(inode, filp);
out:	
	mutex_unlock(&symtab_mutex);
	return ret;
}

static int
symtab_file_release(struct inode *inode, struct file *filp)
{
	int ret = 0;
	
	if (mutex_lock_killable(&symtab_mutex) != 0)
	{
		pr_warning(KEDR_MSG_PREFIX "symtab_file_release: "
			"got a signal while trying to acquire a mutex.\n");
		return -EINTR;
	}
	
	BUG_ON(!symtab_file_is_open);
		
	/* Parse the data written to so far, create the symbol list. */
	ret = load_symbol_list();
	if (ret != 0) {
		pr_warning(KEDR_MSG_PREFIX "symtab_file_release: "
		"failed to load the symbols table from the buffer.\n");
		pr_warning(KEDR_MSG_PREFIX 
		"The buffer contains the following:\n%s\n", 
			krib.buf);
		goto out;
	}
	
	symtab_file_is_open = 0;
	mutex_unlock(&symtab_mutex);
	return 0;

out:	
	mutex_unlock(&symtab_mutex);
	return ret;
}

static ssize_t 
symtab_file_write(struct file *filp, const char __user *buf, size_t count,
	loff_t *f_pos)
{
	ssize_t ret = 0;
	loff_t pos = *f_pos;
	size_t write_to;
	
	if (mutex_lock_killable(&symtab_mutex) != 0)
	{
		pr_warning(KEDR_MSG_PREFIX "symtab_file_write: "
			"got a signal while trying to acquire a mutex.\n");
		return -EINTR;
	}
	
	BUG_ON(!symtab_file_is_open);
	
	if (pos < 0) {
		ret = -EINVAL;
		goto out;
	}
	
	/* 0 bytes requested, nothing to do */
	if (count == 0)
		goto out;
	
	/* Make sure the buffer has enough space for the data, including the
	 * terminating 0. */
	write_to = (size_t)pos + count;
	ret = (ssize_t)input_buffer_resize(&krib, write_to + 1);
	if (ret != 0)
		goto out;
	
	if (copy_from_user(&krib.buf[pos], buf, count) != 0) {
		ret = -EFAULT;
		goto out;
	}
	
	mutex_unlock(&symtab_mutex);
	*f_pos += count;
	return count;
	
out:
	mutex_unlock(&symtab_mutex);
	return ret;
}

static const struct file_operations symtab_file_ops = {
	.owner = THIS_MODULE,
	.open = symtab_file_open,
	.release = symtab_file_release,
	.write = symtab_file_write,
};
/* ====================================================================== */

static const char* mem_event_type[] = {
	"READ", "WRITE", "UPDATE", "*UNKNOWN*"
};

static const char* mem_barrier_type[] = {
	"FULL", "LOAD", "STORE", "*UNKNOWN*"
};

static const char *
type_to_string(enum kedr_memory_event_type t)
{
	switch (t) {
	case KEDR_ET_MREAD: 
		return mem_event_type[0];
	case KEDR_ET_MWRITE:
		return mem_event_type[1];
	case KEDR_ET_MUPDATE:
		return mem_event_type[2];
	default:
		return mem_event_type[3];
	}
}

static const char *
barrier_type_to_string(enum kedr_barrier_type bt)
{
	switch (bt) {
	case KEDR_BT_FULL:
		return mem_barrier_type[0];
	case KEDR_BT_LOAD:
		return mem_barrier_type[1];
	case KEDR_BT_STORE:
		return mem_barrier_type[2];
	default:
		return mem_barrier_type[3];
	}
}
/* ====================================================================== */

/* The structures containing the data to be passed to the workqueue. 
 * See core_api.h for the description of the fields (except 'work'). */

/* Data for function entry/exit events. */
struct kr_work_on_func
{
	struct work_struct work;
	unsigned long tid;
	void *func;
};

/* Data for call pre/post events. */
struct kr_work_on_call
{
	struct work_struct work;
	unsigned long tid;
	void *pc;
	void *func;
};

struct kr_mem_event_internal
{
	unsigned long tid;
	enum kedr_memory_event_type type; 
	void *pc;
	void *addr;
	unsigned long size;
};

struct kr_work_mem_events
{
	struct work_struct work;
	
	/* Number of the actually happened events. This is also the number
	 * of the elements of events[] array that should be processed. */
	unsigned long events_happened; 
	struct kr_mem_event_internal events[1];
};

struct kr_work_on_barrier
{
	struct work_struct work;
	unsigned long tid;
	enum kedr_barrier_type btype;
	void *pc;
	int is_post; /* "barrier pre" if 0, "barrier post" otherwise */
};
/* ====================================================================== */

/* This function will be called for each symbol known to the system.
 * We need to find only the particular function in the target module.
 *
 * 'data' should be the pointer to the struct module for the target.
 *
 * If this function returns 0, kallsyms_on_each_symbol() will continue
 * walking the symbols. If non-zero, it will stop. */
static int
symbol_walk_callback(void *data, const char *name, struct module *mod, 
	unsigned long addr)
{
	/* Skip the symbol if it does not belong to the target module. */
	if (mod != (struct module *)data) 
		return 0;
	
	if (strcmp(name, target_function) == 0) {
		target_start = addr;
		return 1; /* no need to search further */
	}
	return 0;
}
/* ====================================================================== */

/* Prepares a string representation of an address in a buffer and returns 
 * the pointer to that buffer. The pointer should be kfree'd when it is no
 * longer needed. 
 * NULL is returned if there is not enough memory.
 * 
 * Should only be called for the addresses to be resolved to 
 * "symbol+offset [...]" like printing with "%pS" specifier does. 
 *
 * The function must not be called in atomic context. */
static char *
print_address(void *addr)
{
	unsigned long val;
	char *buf;
	char *str_end;
	int len;
	const struct kr_symbol *sym;
	
	BUG_ON(!resolve_symbols);
	
	/* First, lookup the the symbol in 'symbol_list'. */
	if (mutex_lock_killable(&symtab_mutex) != 0)
	{
		pr_warning(KEDR_MSG_PREFIX "print_address: "
			"got a signal while trying to acquire a mutex.\n");
		return NULL;
	}
	
	sym = kr_symbol_lookup((unsigned long)addr);
	if (sym != NULL) {
		const char *fmt = "%s+0x%lx";
		unsigned long offset = (unsigned long)addr - sym->addr;
		len = snprintf(NULL, 0, fmt, sym->name, offset) + 1;
		
		buf = kzalloc((size_t)len, GFP_KERNEL);
		if (buf != NULL)
			snprintf(buf, len, fmt, sym->name, offset);

		mutex_unlock(&symtab_mutex);
		return buf; 
	}
	mutex_unlock(&symtab_mutex);
	
	/* Still unresolved, try to resolve via kallsyms. 
	 * [NB] +1 - for the terminating 0 and +3 more - for "0x0" in case
	 * it is necessary. */
	len = snprintf(NULL, 0, "%pS", addr) + 4;
	buf = kzalloc((size_t)len, GFP_KERNEL);
	if (buf == NULL)
		return NULL;
	
	snprintf(buf, len, "%pS", addr);
	if (!zero_unknown)
		return buf;
	
	/* Check if the symbol has been resolved. */
	val = simple_strtoul(&buf[0], &str_end, 16);
	if (val == (unsigned long)addr) {
		/* Still unresolved */
		strcpy(&buf[0], "0x0");
	}
	return buf;
}

/* Clears the output. 
 * 'work' is not expected to be contained in any other structure. */
static void 
work_func_clear(struct work_struct *work)
{
	debug_util_clear();
	kfree(work);
}

/* Reports function entry event.
 * 'work' should be &kr_work_on_func::work. */
static void 
work_func_entry(struct work_struct *work)
{
	static const char *fmt = "TID=0x%lx FENTRY name=\"%pf\"\n";
	static const char *fmt_no_sym = "TID=0x%lx FENTRY %p\n";

	int ret;
	struct kr_work_on_func *wof = container_of(work, 
		struct kr_work_on_func, work);
	
	ret = debug_util_print((resolve_symbols ? fmt : fmt_no_sym), 
		wof->tid, wof->func);
	if (ret < 0)
		pr_warning(KEDR_MSG_PREFIX 
		"work_func_entry(): output failed, error code: %d.\n", 
			ret);
	kfree(wof);
}

/* Reports function exit event.
 * 'work' should be &kr_work_on_func::work. */
static void 
work_func_exit(struct work_struct *work)
{
	static const char *fmt = "TID=0x%lx FEXIT name=\"%pf\"\n";
	static const char *fmt_no_sym = "TID=0x%lx FEXIT %p\n";
	
	int ret;
	struct kr_work_on_func *wof = container_of(work, 
		struct kr_work_on_func, work);
	
	ret = debug_util_print((resolve_symbols ? fmt : fmt_no_sym), 
		wof->tid, wof->func);
	if (ret < 0)
		pr_warning(KEDR_MSG_PREFIX 
		"work_func_exit(): output failed, error code: %d.\n", 
			ret);
	kfree(wof);
}

/* Reports "call pre" event.
 * 'work' should be &kr_work_on_call::work. */
static void 
work_func_call_pre(struct work_struct *work)
{
	static const char *fmt = "TID=0x%lx CALL_PRE pc=%s name=\"%pf\"\n";
	static const char *fmt_no_sym = "TID=0x%lx CALL_PRE pc=%p %p\n";

	char *str_addr;
	int ret = 0;	
	struct kr_work_on_call *woc = container_of(work, 
		struct kr_work_on_call, work);
	
	if (resolve_symbols) {
		str_addr = print_address(woc->pc);
		if (str_addr == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		ret = debug_util_print(fmt, woc->tid, str_addr, woc->func);
		kfree(str_addr);
	}
	else {
		ret = debug_util_print(fmt_no_sym, woc->tid, woc->pc, 
			woc->func);
	}
	
out:
	if (ret < 0)
		pr_warning(KEDR_MSG_PREFIX 
		"work_func_call_pre(): output failed, error code: %d.\n", 
			ret);
	kfree(woc);
}

/* Reports "call post" event.
 * 'work' should be &kr_work_on_call::work. */
static void 
work_func_call_post(struct work_struct *work)
{
	static const char *fmt = 
		"TID=0x%lx CALL_POST pc=%s name=\"%pf\"\n";
	static const char *fmt_no_sym = 
		"TID=0x%lx CALL_POST pc=%p %p\n";
	
	char *str_addr;
	int ret = 0;
	struct kr_work_on_call *woc = container_of(work, 
		struct kr_work_on_call, work);
	
	if (resolve_symbols) {
		str_addr = print_address(woc->pc);
		if (str_addr == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		ret = debug_util_print(fmt, woc->tid, str_addr, woc->func);
		kfree(str_addr);
	}
	else {
		ret = debug_util_print(fmt_no_sym, woc->tid, woc->pc, 
			woc->func);
	}
	
out:
	if (ret < 0)
		pr_warning(KEDR_MSG_PREFIX 
		"work_func_call_post(): output failed, error code: %d.\n", 
			ret);
	kfree(woc);
}

/* Reports a group of memory access events. 
 * 'work' should be &kr_work_mem_events::work. */
static void
work_func_mem_events(struct work_struct *work)
{
	static const char *fmt_with_sym = 
		"TID=0x%lx %s pc=%s addr=%s size=%lu\n";
	static const char *fmt_no_sym = 
		"TID=0x%lx %s pc=%p addr=%p size=%lu\n";
	
	char *str_pc;
	char *str_addr;
	unsigned long i;
	int ret = 0;
	struct kr_work_mem_events *wme = container_of(work, 
		struct kr_work_mem_events, work);
	
	for (i = 0; i < wme->events_happened; ++i) {
		if (resolve_symbols) {
			str_pc = print_address(wme->events[i].pc);
			if (str_pc == NULL) {
				ret = -ENOMEM;
				break;
			}
			str_addr = print_address(wme->events[i].addr);
			if (str_addr == NULL) {
				ret = -ENOMEM;
				kfree(str_pc);
				break;
			}
			
			ret = debug_util_print(fmt_with_sym, 
				wme->events[i].tid, 
				type_to_string(wme->events[i].type), str_pc,
				str_addr, wme->events[i].size);
			kfree(str_pc);
			kfree(str_addr);
		}
		else {
			ret = debug_util_print(fmt_no_sym, 
				wme->events[i].tid, 
				type_to_string(wme->events[i].type), 
				wme->events[i].pc, wme->events[i].addr, 
				wme->events[i].size);
		}

		if (ret < 0)
			break;
	}
	
	if (ret < 0) {
		pr_warning(KEDR_MSG_PREFIX 
	"work_mem_events(): output failed, error code: %d.\n", 
			ret);
	}
	kfree(wme);
}

/* Reports a locked update.
 * 'work' should be &kr_work_mem_events::work. 
 * Only kr_work_mem_events::events[0] is used. */
static void
work_func_locked_op(struct work_struct *work)
{
	static const char *fmt_with_sym = 
		"TID=0x%lx LOCKED %s pc=%s addr=%s size=%lu\n";
	static const char *fmt_no_sym = 
		"TID=0x%lx LOCKED %s pc=%p addr=%p size=%lu\n";
	
	char *str_pc;
	char *str_addr;
	int ret = 0;
	struct kr_work_mem_events *wme = container_of(work, 
		struct kr_work_mem_events, work);
	
	if (resolve_symbols) {
		str_pc = print_address(wme->events[0].pc);
		if (str_pc == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		str_addr = print_address(wme->events[0].addr);
		if (str_addr == NULL) {
			kfree(str_pc);
			ret = -ENOMEM;
			goto out;
		}
		
		ret = debug_util_print(fmt_with_sym, wme->events[0].tid, 
			type_to_string(wme->events[0].type), str_pc, 
			str_addr, wme->events[0].size);
		kfree(str_pc);
		kfree(str_addr);
	}
	else {
		ret = debug_util_print(fmt_no_sym, wme->events[0].tid, 
			type_to_string(wme->events[0].type), 
			wme->events[0].pc, wme->events[0].addr, 
			wme->events[0].size);
	}
out:
	if (ret < 0) {
		pr_warning(KEDR_MSG_PREFIX 
	"work_func_locked_update(): output failed, error code: %d.\n", 
			ret);
	}
	kfree(wme);
}

/* Reports an I/O operation accessing memory.
 * 'work' should be &kr_work_mem_events::work. 
 * Only kr_work_mem_events::events[0] is used. */
static void
work_func_io_mem(struct work_struct *work)
{
	static const char *fmt_with_sym = 
		"TID=0x%lx IO_MEM %s pc=%s addr=%s size=%lu\n";
	static const char *fmt_no_sym = 
		"TID=0x%lx IO_MEM %s pc=%p addr=%p size=%lu\n";
	
	char *str_pc;
	char *str_addr;
	int ret = 0;
	struct kr_work_mem_events *wme = container_of(work, 
		struct kr_work_mem_events, work);
	
	if (resolve_symbols) {
		str_pc = print_address(wme->events[0].pc);
		if (str_pc == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		str_addr = print_address(wme->events[0].addr);
		if (str_addr == NULL) {
			kfree(str_pc);
			ret = -ENOMEM;
			goto out;
		}
		
		ret = debug_util_print(fmt_with_sym, wme->events[0].tid, 
			type_to_string(wme->events[0].type), str_pc, 
			str_addr, wme->events[0].size);
		kfree(str_pc);
		kfree(str_addr);
	}
	else {
		ret = debug_util_print(fmt_no_sym, wme->events[0].tid, 
			type_to_string(wme->events[0].type), 
			wme->events[0].pc, wme->events[0].addr,
			wme->events[0].size);
	}
out:
	if (ret < 0) {
		pr_warning(KEDR_MSG_PREFIX 
		"work_func_io_mem(): output failed, error code: %d.\n", 
			ret);
	}
	kfree(wme);
}

/* Reports "barrier pre" and "barrier post" events.
 * 'work' should be &kr_work_on_barrier::work. */
static void 
work_func_barrier(struct work_struct *work)
{
	static const char *fmt = "TID=0x%lx BARRIER %s %s pc=%s\n";
	static const char *fmt_no_sym = "TID=0x%lx BARRIER %s %s pc=%p\n";

	char *str_pc;
	int ret = 0;	
	struct kr_work_on_barrier *wob = container_of(work, 
		struct kr_work_on_barrier, work);
	
	if (resolve_symbols) {
		str_pc = print_address(wob->pc);
		if (str_pc == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		
		ret = debug_util_print((resolve_symbols ? fmt : fmt_no_sym), 
			wob->tid, barrier_type_to_string(wob->btype), 
			(wob->is_post ? "POST" : "PRE"), str_pc);
		kfree(str_pc);
	}
	else {
		ret = debug_util_print(fmt_no_sym, wob->tid, 
			barrier_type_to_string(wob->btype), 
			(wob->is_post ? "POST" : "PRE"), wob->pc);
	}
out:
	if (ret < 0)
		pr_warning(KEDR_MSG_PREFIX 
		"work_func_barrier(): output failed, error code: %d.\n", 
			ret);
	kfree(wob);
}
/* ====================================================================== */

/* If the function is called not from on_load/on_unload handlers, 'wq_lock' 
 * must be held. */
static void
reset_counters(void)
{
	target_start = 0;
	target_tid = KEDR_ALL_THREADS;
	ecount = 0;
}

/* If the function is called not from on_load/on_unload handlers, 'wq_lock' 
 * must be held. 
 * 
 * Returns non-zero if it is allowed to report the event with a given TID
 * provided "report_*" parameters also allow that. 0 if the event should not 
 * be reported. */
static int
report_event_allowed(unsigned long tid)
{
	if (ecount >= max_events)
		return 0;
	
	if (target_module == NULL || 
	    (target_module->module_init != NULL && resolve_symbols != 0))
		return 0;
	
	if (!restrict_to_func)
		return 1;
	
	return (within_target_func && (tid == target_tid));
}
/* ====================================================================== */

static void 
on_load(struct kedr_event_handlers *eh, struct module *mod)
{
	int ret;
	
	reset_counters();
	debug_util_clear();
	target_module = mod;
	
	if (!restrict_to_func)
		return;
	
	ret = kallsyms_on_each_symbol(symbol_walk_callback, mod);
	if (ret < 0) {
		pr_warning(KEDR_MSG_PREFIX 
			"Failed to search for the function \"%s\".\n", 
			target_function);
	}
	else if (ret == 0) {
		pr_info(KEDR_MSG_PREFIX 
			"The function \"%s\" was not found in \"%s\".\n", 
			target_function, module_name(mod));
	}
	else { /* Must have found the target function. */
		BUG_ON(target_start == 0);
	}
}

static void 
on_unload(struct kedr_event_handlers *eh, struct module *mod)
{
	flush_workqueue(wq);
	/* Reporting must have been finished for all the previous events
	 * by now, so it is safe to reset 'target_module'. */
	target_module = NULL;
}

static void 
on_function_entry(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long func)
{
	unsigned long irq_flags;
	struct work_struct *work = NULL;
	struct kr_work_on_func *wof = NULL;

	spin_lock_irqsave(&wq_lock, irq_flags);
	if (func == target_start) {
		/* Another entry to the target function detected but the
		 * previous invocation of that function has not exited yet.
		 * May be a recursive call or a call from another thread. 
		 * The report may contain less data than expected. */
		WARN_ON_ONCE(within_target_func != 0);
		within_target_func = 1;
		target_tid = tid;
		ecount = 0;
		
		/* Add a command to the wq to clear the output */
		work = kzalloc(sizeof(*work), GFP_ATOMIC);
		if (work == NULL) {
			pr_warning(KEDR_MSG_PREFIX 
			"on_function_entry(): out of memory.\n");
			goto out;
		}
		INIT_WORK(work, work_func_clear);
		queue_work(wq, work);
	}
	if (!report_calls || !report_event_allowed(tid))
		goto out;
		
	wof = kzalloc(sizeof(*wof), GFP_ATOMIC);
	if (wof == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_function_entry(): no memory for \"wof\".\n");
			goto out;
	}
	
	++ecount;
	wof->tid = tid;
	wof->func = (void *)func;
	INIT_WORK(&wof->work, work_func_entry);
	queue_work(wq, &wof->work);

out:	
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void 
on_function_exit(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long func)
{
	unsigned long irq_flags;
	struct kr_work_on_func *wof = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_calls || !report_event_allowed(tid))
		goto out;
		
	wof = kzalloc(sizeof(*wof), GFP_ATOMIC);
	if (wof == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_function_exit(): out of memory.\n");
			goto out;
	}
	
	++ecount;
	wof->tid = tid;
	wof->func = (void *)func;
	INIT_WORK(&wof->work, work_func_exit);
	queue_work(wq, &wof->work);

out:	
	if (func == target_start && tid == target_tid) {
		/* Warn if it is an exit from the target function but no 
		 * entry event has been received for it. */
		WARN_ON_ONCE(within_target_func == 0);
		within_target_func = 0;
		target_tid = KEDR_ALL_THREADS;
	}
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void 
on_call_pre(struct kedr_event_handlers *eh, unsigned long tid,
	unsigned long pc, unsigned long func)
{
	unsigned long irq_flags;
	struct kr_work_on_call *woc = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_calls || !report_event_allowed(tid))
		goto out;
		
	woc = kzalloc(sizeof(*woc), GFP_ATOMIC);
	if (woc == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_call_pre(): out of memory.\n");
			goto out;
	}
	
	++ecount;
	woc->tid = tid;
	woc->pc = (void *)pc;
	woc->func = (void *)func;
	INIT_WORK(&woc->work, work_func_call_pre);
	queue_work(wq, &woc->work);
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void 
on_call_post(struct kedr_event_handlers *eh, unsigned long tid,
	unsigned long pc, unsigned long func)
{
	unsigned long irq_flags;
	struct kr_work_on_call *woc = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_calls || !report_event_allowed(tid))
		goto out;
	
	woc = kzalloc(sizeof(*woc), GFP_ATOMIC);
	if (woc == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_call_post(): out of memory.\n");
			goto out;
	}
	
	++ecount;
	woc->tid = tid;
	woc->pc = (void *)pc;
	woc->func = (void *)func;
	INIT_WORK(&woc->work, work_func_call_post);
	queue_work(wq, &woc->work);
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void 
begin_memory_events(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long num_events, void **pdata)
{
	unsigned long irq_flags;
	struct kr_work_mem_events *wme = NULL;
	
	BUG_ON(num_events == 0);
	BUG_ON(pdata == NULL);
	*pdata = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_mem || !report_event_allowed(tid))
		goto out;
	
	wme = kzalloc(sizeof(struct kr_work_mem_events) + 
		(num_events - 1) * sizeof(struct kr_mem_event_internal), 
		GFP_ATOMIC);
	if (wme == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"begin_memory_events(): out of memory.\n");
			goto out;
	}
	*pdata = wme; /* to be filled in on_memory_event() */
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);	
}

static void 
on_memory_event(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long pc, unsigned long addr, unsigned long size, 
	enum kedr_memory_event_type type,
	void *data)
{
	unsigned long irq_flags;
	struct kr_work_mem_events *wme = (struct kr_work_mem_events *)data;
	struct kr_mem_event_internal *e;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (wme == NULL || !report_mem || !report_event_allowed(tid) || 
	    addr == 0)
		goto out;
		
	++ecount;
	
	e = &wme->events[wme->events_happened];
	++wme->events_happened;
	
	e->tid = tid;
	e->pc = (void *)pc;
	e->addr = (void *)addr;
	e->size = size;
	e->type = type;
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void
end_memory_events(struct kedr_event_handlers *eh, unsigned long tid, 
	void *data)
{
	unsigned long irq_flags;
	struct kr_work_mem_events *wme = (struct kr_work_mem_events *)data;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (wme == NULL || !report_mem || !report_event_allowed(tid)) {
		kfree(wme); /* just in case */
		goto out;
	}
	
	INIT_WORK(&wme->work, work_func_mem_events);
	queue_work(wq, &wme->work);
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void 
on_locked_op_pre(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long pc, void **pdata)
{
	unsigned long irq_flags;
	struct kr_work_mem_events *wme = NULL;
	
	BUG_ON(pdata == NULL);
	*pdata = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_mem || !report_event_allowed(tid))
		goto out;
		
	wme = kzalloc(sizeof(*wme), GFP_ATOMIC);
	if (wme == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_locked_op_pre(): out of memory.\n");
			goto out;
	}
	wme->events[0].tid = tid;
	wme->events[0].pc = (void *)pc;
	wme->events_happened = 1;
	
	*pdata = wme;
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);	
}

static void
on_locked_op_post(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long pc, unsigned long addr, unsigned long size, 
	enum kedr_memory_event_type type, void *data)
{
	unsigned long irq_flags;
	struct kr_work_mem_events *wme = (struct kr_work_mem_events *)data;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (wme == NULL || !report_mem || !report_event_allowed(tid)) {
		kfree(wme); /* just in case */
		goto out;
	}
	
	if (wme->events[0].tid != tid || wme->events[0].pc != (void *)pc) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_locked_op_post(): mismatch in tid or pc.\n");
		kfree(wme);
		goto out;
	}
	
	++ecount;
	
	wme->events[0].addr = (void *)addr;
	wme->events[0].size = size;
	wme->events[0].type = type;
	
	INIT_WORK(&wme->work, work_func_locked_op);
	queue_work(wq, &wme->work);
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void 
on_io_mem_op_pre(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long pc, void **pdata)
{
	unsigned long irq_flags;
	struct kr_work_mem_events *wme = NULL;
	
	BUG_ON(pdata == NULL);
	*pdata = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_mem || !report_event_allowed(tid))
		goto out;
	
	wme = kzalloc(sizeof(*wme), GFP_ATOMIC);
	if (wme == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_io_mem_op_pre(): out of memory.\n");
			goto out;
	}
	wme->events[0].tid = tid;
	wme->events[0].pc = (void *)pc;
	wme->events_happened = 1;
	
	*pdata = wme;
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);	
}

static void
on_io_mem_op_post(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long pc, unsigned long addr, unsigned long size, 
	enum kedr_memory_event_type type, void *data)
{
	unsigned long irq_flags;
	struct kr_work_mem_events *wme = (struct kr_work_mem_events *)data;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (wme == NULL || !report_mem || !report_event_allowed(tid)) {
		kfree(wme); /* just in case */
		goto out;
	}
	
	if (wme->events[0].tid != tid || wme->events[0].pc != (void *)pc) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_locked_op_post(): mismatch in tid or pc.\n");
		kfree(wme);
		goto out;
	}
	
	++ecount;
	
	wme->events[0].addr = (void *)addr;
	wme->events[0].size = size;
	wme->events[0].type = type;
	
	INIT_WORK(&wme->work, work_func_io_mem);
	queue_work(wq, &wme->work);
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void
on_memory_barrier_pre(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long pc, enum kedr_barrier_type type)
{
	unsigned long irq_flags;
	struct kr_work_on_barrier *wob = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_mem || !report_event_allowed(tid))
		goto out;
		
	wob = kzalloc(sizeof(*wob), GFP_ATOMIC);
	if (wob == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_memory_barrier_pre(): out of memory.\n");
			goto out;
	}
	
	++ecount;
	wob->tid = tid;
	wob->pc = (void *)pc;
	wob->btype = type;
	INIT_WORK(&wob->work, work_func_barrier);
	queue_work(wq, &wob->work);
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

static void
on_memory_barrier_post(struct kedr_event_handlers *eh, unsigned long tid, 
	unsigned long pc, enum kedr_barrier_type type)
{
	unsigned long irq_flags;
	struct kr_work_on_barrier *wob = NULL;
	
	spin_lock_irqsave(&wq_lock, irq_flags);
	if (!report_mem || !report_event_allowed(tid))
		goto out;
		
	wob = kzalloc(sizeof(*wob), GFP_ATOMIC);
	if (wob == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"on_memory_barrier_post(): out of memory.\n");
			goto out;
	}
	
	++ecount;
	wob->tid = tid;
	wob->pc = (void *)pc;
	wob->btype = type;
	wob->is_post = 1;
	INIT_WORK(&wob->work, work_func_barrier);
	queue_work(wq, &wob->work);
out:
	spin_unlock_irqrestore(&wq_lock, irq_flags);
}

struct kedr_event_handlers eh = {
	.owner = THIS_MODULE,
	.on_target_loaded = on_load,
	.on_target_about_to_unload = on_unload,
	.on_function_entry = on_function_entry,
	.on_function_exit = on_function_exit,
	.on_call_pre = on_call_pre,
	.on_call_post = on_call_post,
	.begin_memory_events = begin_memory_events,
	.end_memory_events = end_memory_events,
	.on_memory_event = on_memory_event,
	.on_locked_op_pre = on_locked_op_pre,
	.on_locked_op_post = on_locked_op_post,
	.on_io_mem_op_pre = on_io_mem_op_pre,
	.on_io_mem_op_post = on_io_mem_op_post,
	.on_memory_barrier_pre = on_memory_barrier_pre,
	.on_memory_barrier_post = on_memory_barrier_post,
	/* [NB] Add more handlers here if necessary. */
};
/* ====================================================================== */

static void __exit
test_cleanup_module(void)
{
	kedr_unregister_event_handlers(&eh);
	
	destroy_workqueue(wq);
	debugfs_remove(symtab_file);
	debug_util_fini();
	debugfs_remove(debugfs_dir_dentry);
	symtab_cleanup();
	return;
}

static int __init
test_init_module(void)
{
	int ret = 0;
	
	restrict_to_func = (target_function[0] != 0);
	
	/* [NB] Add checking of other report_* parameters here as needed. */
	if (!report_calls && !report_mem) {
		pr_warning(KEDR_MSG_PREFIX 
	"At least one of \"report_*\" parameters should be non-zero.\n");
		return -EINVAL;
	}
	
	debugfs_dir_dentry = debugfs_create_dir(debugfs_dir_name, NULL);
	if (debugfs_dir_dentry == NULL) {
		pr_warning(KEDR_MSG_PREFIX
			"Failed to create a directory in debugfs\n");
		ret = -EINVAL;
		goto out;
	}
	if (IS_ERR(debugfs_dir_dentry)) {
		pr_warning(KEDR_MSG_PREFIX "Debugfs is not supported\n");
		ret = -ENODEV;
		goto out;
	}

	ret = debug_util_init(debugfs_dir_dentry);
	if (ret != 0)
		goto out_rmdir;
	
	symtab_file = debugfs_create_file(symtab_file_name, 
		S_IWUSR | S_IWGRP, debugfs_dir_dentry, NULL, 
		&symtab_file_ops);
	if (symtab_file == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"Failed to create a file in debugfs (\"%s\").\n",
			symtab_file_name);
		ret = -ENOMEM;
		goto out_clean_debug_util;
	}
	
	wq = create_singlethread_workqueue(wq_name);
	if (wq == NULL) {
		pr_warning(KEDR_MSG_PREFIX 
			"Failed to create workqueue \"%s\"\n",
			wq_name);
		ret = -ENOMEM;
		goto out_del_symtab_file;
	}
	
	/* [NB] Register event handlers only after everything else has 
	 * been initialized. */
	ret = kedr_register_event_handlers(&eh);
	if (ret != 0)
		goto out_clean_all;
	
	return 0;

out_clean_all:
	destroy_workqueue(wq);
out_del_symtab_file:
	debugfs_remove(symtab_file);
out_clean_debug_util:	
	debug_util_fini();
out_rmdir:
	debugfs_remove(debugfs_dir_dentry);
out:
	/* Just in case something has triggered initialization of the 
	 * "symtab" facilities already (e.g. some process has opened 
	 * "symbol_table" file in debugfs). */
	symtab_cleanup();
	return ret;
}

module_init(test_init_module);
module_exit(test_cleanup_module);
/* ====================================================================== */
