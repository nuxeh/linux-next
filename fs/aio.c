/*
 *	An async IO implementation for Linux
 *	Written by Benjamin LaHaise <bcrl@kvack.org>
 *
 *	Implements an efficient asynchronous io interface.
 *
 *	Copyright 2000, 2001, 2002 Red Hat, Inc.  All Rights Reserved.
 *
 *	See ../COPYING for licensing terms.
 */
#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/aio_abi.h>
#include <linux/export.h>
#include <linux/syscalls.h>
#include <linux/backing-dev.h>
#include <linux/uio.h>

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmu_context.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/aio.h>
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/security.h>
#include <linux/eventfd.h>
#include <linux/blkdev.h>
#include <linux/compat.h>
#include <linux/migrate.h>
#include <linux/ramfs.h>
#include <linux/percpu-refcount.h>
#include <linux/mount.h>
#include <linux/fdtable.h>
#include <linux/fs_struct.h>
#include <linux/fsnotify.h>
#include <linux/namei.h>
#include <../mm/internal.h>

#include <asm/kmap_types.h>
#include <asm/uaccess.h>

#include "internal.h"

#define AIO_RING_MAGIC			0xa10a10a1
#define AIO_RING_COMPAT_FEATURES	1
#define AIO_RING_COMPAT_THREADED	2
#define AIO_RING_INCOMPAT_FEATURES	0
struct aio_ring {
	unsigned	id;	/* kernel internal index number */
	unsigned	nr;	/* number of io_events */
	unsigned	head;	/* Written to by userland or under ring_lock
				 * mutex by aio_read_events_ring(). */
	unsigned	tail;

	unsigned	magic;
	unsigned	compat_features;
	unsigned	incompat_features;
	unsigned	header_length;	/* size of aio_ring */


	struct io_event		io_events[0];
}; /* 128 bytes + ring size */

#define AIO_RING_PAGES	8

struct kioctx_table {
	struct rcu_head	rcu;
	unsigned	nr;
	struct kioctx	*table[];
};

struct kioctx_cpu {
	unsigned		reqs_available;
};

struct ctx_rq_wait {
	struct completion comp;
	atomic_t count;
};

struct kioctx {
	struct percpu_ref	users;
	atomic_t		dead;

	struct percpu_ref	reqs;

	unsigned long		user_id;

	struct __percpu kioctx_cpu *cpu;

	/*
	 * For percpu reqs_available, number of slots we move to/from global
	 * counter at a time:
	 */
	unsigned		req_batch;
	/*
	 * This is what userspace passed to io_setup(), it's not used for
	 * anything but counting against the global max_reqs quota.
	 *
	 * The real limit is nr_events - 1, which will be larger (see
	 * aio_setup_ring())
	 */
	unsigned		max_reqs;

	/* Size of ringbuffer, in units of struct io_event */
	unsigned		nr_events;

	unsigned long		mmap_base;
	unsigned long		mmap_size;

	struct page		**ring_pages;
	long			nr_pages;

	struct work_struct	free_work;

	/*
	 * signals when all in-flight requests are done
	 */
	struct ctx_rq_wait	*rq_wait;

	struct {
		/*
		 * This counts the number of available slots in the ringbuffer,
		 * so we avoid overflowing it: it's decremented (if positive)
		 * when allocating a kiocb and incremented when the resulting
		 * io_event is pulled off the ringbuffer.
		 *
		 * We batch accesses to it with a percpu version.
		 */
		atomic_t	reqs_available;
	} ____cacheline_aligned_in_smp;

	struct {
		spinlock_t	ctx_lock;
		struct list_head active_reqs;	/* used for cancellation */
	} ____cacheline_aligned_in_smp;

	struct {
		struct mutex	ring_lock;
		wait_queue_head_t wait;
	} ____cacheline_aligned_in_smp;

	struct {
		unsigned	tail;
		unsigned	completed_events;
		spinlock_t	completion_lock;
	} ____cacheline_aligned_in_smp;

	struct page		*internal_pages[AIO_RING_PAGES];
	struct file		*aio_ring_file;

	unsigned		id;
	struct mm_struct	*mm;
};

struct aio_kiocb;
typedef long (*aio_thread_work_fn_t)(struct aio_kiocb *iocb);
typedef void (*aio_destruct_fn_t)(struct aio_kiocb *iocb);

/*
 * We use ki_cancel == KIOCB_CANCELLED to indicate that a kiocb has been either
 * cancelled or completed (this makes a certain amount of sense because
 * successful cancellation - io_cancel() - does deliver the completion to
 * userspace).
 *
 * And since most things don't implement kiocb cancellation and we'd really like
 * kiocb completion to be lockless when possible, we use ki_cancel to
 * synchronize cancellation and completion - we only set it to KIOCB_CANCELLED
 * with xchg() or cmpxchg(), see batch_complete_aio() and kiocb_cancel().
 */
#define KIOCB_CANCELLED		((void *) (~0ULL))

#define AIO_THREAD_NEED_TASK	0x0001	/* Need aio_kiocb->ki_submit_task */
#define AIO_THREAD_NEED_FS	0x0002	/* Need aio_kiocb->ki_fs */
#define AIO_THREAD_NEED_FILES	0x0004	/* Need aio_kiocb->ki_files */
#define AIO_THREAD_NEED_CRED	0x0008	/* Need aio_kiocb->ki_cred */
#define AIO_THREAD_NEED_MM	0x0010	/* Need the mm context */

struct aio_kiocb {
	struct kiocb		common;

	struct kioctx		*ki_ctx;
	kiocb_cancel_fn		*ki_cancel;

	struct iocb __user	*ki_user_iocb;	/* user's aiocb */
	__u64			ki_user_data;	/* user's data for completion */

	struct list_head	ki_list;	/* the aio core uses this
						 * for cancellation */

	/*
	 * If the aio_resfd field of the userspace iocb is not zero,
	 * this is the underlying eventfd context to deliver events to.
	 */
	struct eventfd_ctx	*ki_eventfd;

	struct iov_iter		ki_iter;
	struct iovec		*ki_iovec;
	struct iovec		ki_inline_vecs[UIO_FASTIOV];

	/* Fields used for threaded aio helper. */
	struct task_struct	*ki_submit_task;
#if IS_ENABLED(CONFIG_AIO_THREAD)
	struct task_struct	*ki_cancel_task;
	unsigned long		ki_data;
	unsigned long		ki_data2;
	unsigned long		ki_rlimit_fsize;
	unsigned		ki_thread_flags;	/* AIO_THREAD_NEED... */
	aio_thread_work_fn_t	ki_work_fn;
	struct work_struct	ki_work;
	struct fs_struct	*ki_fs;
	struct files_struct	*ki_files;
	const struct cred	*ki_cred;
	aio_destruct_fn_t	ki_destruct_fn;
#endif
};

/*------ sysctl variables----*/
static DEFINE_SPINLOCK(aio_nr_lock);
unsigned long aio_nr;		/* current system wide number of aio requests */
unsigned long aio_max_nr = 0x10000; /* system wide maximum number of aio requests */
#if IS_ENABLED(CONFIG_AIO_THREAD)
unsigned long aio_auto_threads;	/* Currently disabled by default */
#endif
/*----end sysctl variables---*/

static struct kmem_cache	*kiocb_cachep;
static struct kmem_cache	*kioctx_cachep;

static struct vfsmount *aio_mnt;

static const struct file_operations aio_ring_fops;
static const struct address_space_operations aio_ctx_aops;

static void aio_complete(struct kiocb *kiocb, long res, long res2);

typedef long (*do_foo_at_t)(int fd, const char *filename, int flags, int mode);

static __always_inline bool aio_may_use_threads(void)
{
#if IS_ENABLED(CONFIG_AIO_THREAD)
	return !!(aio_auto_threads & 1);
#else
	return false;
#endif
}

static struct file *aio_private_file(struct kioctx *ctx, loff_t nr_pages)
{
	struct qstr this = QSTR_INIT("[aio]", 5);
	struct file *file;
	struct path path;
	struct inode *inode = alloc_anon_inode(aio_mnt->mnt_sb);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	inode->i_mapping->a_ops = &aio_ctx_aops;
	inode->i_mapping->private_data = ctx;
	inode->i_size = PAGE_SIZE * nr_pages;

	path.dentry = d_alloc_pseudo(aio_mnt->mnt_sb, &this);
	if (!path.dentry) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	path.mnt = mntget(aio_mnt);

	d_instantiate(path.dentry, inode);
	file = alloc_file(&path, FMODE_READ | FMODE_WRITE, &aio_ring_fops);
	if (IS_ERR(file)) {
		path_put(&path);
		return file;
	}

	file->f_flags = O_RDWR;
	return file;
}

static struct dentry *aio_mount(struct file_system_type *fs_type,
				int flags, const char *dev_name, void *data)
{
	static const struct dentry_operations ops = {
		.d_dname	= simple_dname,
	};
	return mount_pseudo(fs_type, "aio:", NULL, &ops, AIO_RING_MAGIC);
}

/* aio_setup
 *	Creates the slab caches used by the aio routines, panic on
 *	failure as this is done early during the boot sequence.
 */
static int __init aio_setup(void)
{
	static struct file_system_type aio_fs = {
		.name		= "aio",
		.mount		= aio_mount,
		.kill_sb	= kill_anon_super,
	};
	aio_mnt = kern_mount(&aio_fs);
	if (IS_ERR(aio_mnt))
		panic("Failed to create aio fs mount.");

	kiocb_cachep = KMEM_CACHE(aio_kiocb, SLAB_HWCACHE_ALIGN|SLAB_PANIC);
	kioctx_cachep = KMEM_CACHE(kioctx,SLAB_HWCACHE_ALIGN|SLAB_PANIC);

	pr_debug("sizeof(struct page) = %zu\n", sizeof(struct page));

	return 0;
}
__initcall(aio_setup);

static void put_aio_ring_file(struct kioctx *ctx)
{
	struct file *aio_ring_file = ctx->aio_ring_file;
	if (aio_ring_file) {
		truncate_setsize(aio_ring_file->f_inode, 0);

		/* Prevent further access to the kioctx from migratepages */
		spin_lock(&aio_ring_file->f_inode->i_mapping->private_lock);
		aio_ring_file->f_inode->i_mapping->private_data = NULL;
		ctx->aio_ring_file = NULL;
		spin_unlock(&aio_ring_file->f_inode->i_mapping->private_lock);

		fput(aio_ring_file);
	}
}

static void aio_free_ring(struct kioctx *ctx)
{
	int i;

	/* Disconnect the kiotx from the ring file.  This prevents future
	 * accesses to the kioctx from page migration.
	 */
	put_aio_ring_file(ctx);

	for (i = 0; i < ctx->nr_pages; i++) {
		struct page *page;
		pr_debug("pid(%d) [%d] page->count=%d\n", current->pid, i,
				page_count(ctx->ring_pages[i]));
		page = ctx->ring_pages[i];
		if (!page)
			continue;
		ctx->ring_pages[i] = NULL;
		put_page(page);
	}

	if (ctx->ring_pages && ctx->ring_pages != ctx->internal_pages) {
		kfree(ctx->ring_pages);
		ctx->ring_pages = NULL;
	}
}

static int aio_ring_mremap(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;
	struct mm_struct *mm = vma->vm_mm;
	struct kioctx_table *table;
	int i, res = -EINVAL;

	spin_lock(&mm->ioctx_lock);
	rcu_read_lock();
	table = rcu_dereference(mm->ioctx_table);
	for (i = 0; i < table->nr; i++) {
		struct kioctx *ctx;

		ctx = table->table[i];
		if (ctx && ctx->aio_ring_file == file) {
			if (!atomic_read(&ctx->dead)) {
				ctx->user_id = ctx->mmap_base = vma->vm_start;
				res = 0;
			}
			break;
		}
	}

	rcu_read_unlock();
	spin_unlock(&mm->ioctx_lock);
	return res;
}

static const struct vm_operations_struct aio_ring_vm_ops = {
	.mremap		= aio_ring_mremap,
#if IS_ENABLED(CONFIG_MMU)
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
#endif
};

static int aio_ring_mmap(struct file *file, struct vm_area_struct *vma)
{
	vma->vm_flags |= VM_DONTEXPAND;
	vma->vm_ops = &aio_ring_vm_ops;
	return 0;
}

static const struct file_operations aio_ring_fops = {
	.mmap = aio_ring_mmap,
};

#if IS_ENABLED(CONFIG_MIGRATION)
static int aio_migratepage(struct address_space *mapping, struct page *new,
			struct page *old, enum migrate_mode mode)
{
	struct kioctx *ctx;
	unsigned long flags;
	pgoff_t idx;
	int rc;

	rc = 0;

	/* mapping->private_lock here protects against the kioctx teardown.  */
	spin_lock(&mapping->private_lock);
	ctx = mapping->private_data;
	if (!ctx) {
		rc = -EINVAL;
		goto out;
	}

	/* The ring_lock mutex.  The prevents aio_read_events() from writing
	 * to the ring's head, and prevents page migration from mucking in
	 * a partially initialized kiotx.
	 */
	if (!mutex_trylock(&ctx->ring_lock)) {
		rc = -EAGAIN;
		goto out;
	}

	idx = old->index;
	if (idx < (pgoff_t)ctx->nr_pages) {
		/* Make sure the old page hasn't already been changed */
		if (ctx->ring_pages[idx] != old)
			rc = -EAGAIN;
	} else
		rc = -EINVAL;

	if (rc != 0)
		goto out_unlock;

	/* Writeback must be complete */
	BUG_ON(PageWriteback(old));
	get_page(new);

	rc = migrate_page_move_mapping(mapping, new, old, NULL, mode, 1);
	if (rc != MIGRATEPAGE_SUCCESS) {
		put_page(new);
		goto out_unlock;
	}

	/* Take completion_lock to prevent other writes to the ring buffer
	 * while the old page is copied to the new.  This prevents new
	 * events from being lost.
	 */
	spin_lock_irqsave(&ctx->completion_lock, flags);
	migrate_page_copy(new, old);
	BUG_ON(ctx->ring_pages[idx] != old);
	ctx->ring_pages[idx] = new;
	spin_unlock_irqrestore(&ctx->completion_lock, flags);

	/* The old page is no longer accessible. */
	put_page(old);

out_unlock:
	mutex_unlock(&ctx->ring_lock);
out:
	spin_unlock(&mapping->private_lock);
	return rc;
}
#endif

static const struct address_space_operations aio_ctx_aops = {
	.set_page_dirty = __set_page_dirty_no_writeback,
#if IS_ENABLED(CONFIG_MIGRATION)
	.migratepage	= aio_migratepage,
#endif
};

static int aio_setup_ring(struct kioctx *ctx)
{
	struct aio_ring *ring;
	unsigned nr_events = ctx->max_reqs;
	struct mm_struct *mm = current->mm;
	unsigned long size, unused;
	int nr_pages;
	int i;
	struct file *file;

	/* Compensate for the ring buffer's head/tail overlap entry */
	nr_events += 2;	/* 1 is required, 2 for good luck */

	size = sizeof(struct aio_ring);
	size += sizeof(struct io_event) * nr_events;

	nr_pages = PFN_UP(size);
	if (nr_pages < 0)
		return -EINVAL;

	file = aio_private_file(ctx, nr_pages);
	if (IS_ERR(file)) {
		ctx->aio_ring_file = NULL;
		return -ENOMEM;
	}

	ctx->aio_ring_file = file;
	nr_events = (PAGE_SIZE * nr_pages - sizeof(struct aio_ring))
			/ sizeof(struct io_event);

	ctx->ring_pages = ctx->internal_pages;
	if (nr_pages > AIO_RING_PAGES) {
		ctx->ring_pages = kcalloc(nr_pages, sizeof(struct page *),
					  GFP_KERNEL);
		if (!ctx->ring_pages) {
			put_aio_ring_file(ctx);
			return -ENOMEM;
		}
	}

	for (i = 0; i < nr_pages; i++) {
		struct page *page;
		page = find_or_create_page(file->f_inode->i_mapping,
					   i, GFP_HIGHUSER | __GFP_ZERO);
		if (!page)
			break;
		pr_debug("pid(%d) page[%d]->count=%d\n",
			 current->pid, i, page_count(page));
		SetPageUptodate(page);
		unlock_page(page);

		ctx->ring_pages[i] = page;
	}
	ctx->nr_pages = i;

	if (unlikely(i != nr_pages)) {
		aio_free_ring(ctx);
		return -ENOMEM;
	}

	ctx->mmap_size = nr_pages * PAGE_SIZE;
	pr_debug("attempting mmap of %lu bytes\n", ctx->mmap_size);

	down_write(&mm->mmap_sem);
	ctx->mmap_base = do_mmap_pgoff(ctx->aio_ring_file, 0, ctx->mmap_size,
				       PROT_READ | PROT_WRITE,
				       MAP_SHARED, 0, &unused);
	up_write(&mm->mmap_sem);
	if (IS_ERR((void *)ctx->mmap_base)) {
		ctx->mmap_size = 0;
		aio_free_ring(ctx);
		return -ENOMEM;
	}

	pr_debug("mmap address: 0x%08lx\n", ctx->mmap_base);

	ctx->user_id = ctx->mmap_base;
	ctx->nr_events = nr_events; /* trusted copy */

	ring = kmap_atomic(ctx->ring_pages[0]);
	ring->nr = nr_events;	/* user copy */
	ring->id = ~0U;
	ring->head = ring->tail = 0;
	ring->magic = AIO_RING_MAGIC;
	ring->compat_features = AIO_RING_COMPAT_FEATURES;
	if (aio_may_use_threads())
		ring->compat_features |= AIO_RING_COMPAT_THREADED;
	ring->incompat_features = AIO_RING_INCOMPAT_FEATURES;
	ring->header_length = sizeof(struct aio_ring);
	kunmap_atomic(ring);
	flush_dcache_page(ctx->ring_pages[0]);

	return 0;
}

#define AIO_EVENTS_PER_PAGE	(PAGE_SIZE / sizeof(struct io_event))
#define AIO_EVENTS_FIRST_PAGE	((PAGE_SIZE - sizeof(struct aio_ring)) / sizeof(struct io_event))
#define AIO_EVENTS_OFFSET	(AIO_EVENTS_PER_PAGE - AIO_EVENTS_FIRST_PAGE)

void kiocb_set_cancel_fn(struct kiocb *iocb, kiocb_cancel_fn *cancel)
{
	struct aio_kiocb *req = container_of(iocb, struct aio_kiocb, common);
	struct kioctx *ctx = req->ki_ctx;
	unsigned long flags;

	spin_lock_irqsave(&ctx->ctx_lock, flags);

	if (!req->ki_list.next)
		list_add(&req->ki_list, &ctx->active_reqs);

	req->ki_cancel = cancel;

	spin_unlock_irqrestore(&ctx->ctx_lock, flags);
}
EXPORT_SYMBOL(kiocb_set_cancel_fn);

static int kiocb_cancel(struct aio_kiocb *kiocb)
{
	kiocb_cancel_fn *old, *cancel;

	/*
	 * Don't want to set kiocb->ki_cancel = KIOCB_CANCELLED unless it
	 * actually has a cancel function, hence the cmpxchg()
	 */

	cancel = ACCESS_ONCE(kiocb->ki_cancel);
	do {
		if (!cancel || cancel == KIOCB_CANCELLED)
			return -EINVAL;

		old = cancel;
		cancel = cmpxchg(&kiocb->ki_cancel, old, KIOCB_CANCELLED);
	} while (cancel != old);

	return cancel(&kiocb->common);
}

struct mm_struct *aio_get_mm(struct kiocb *req)
{
	if (req->ki_complete == aio_complete) {
		struct aio_kiocb *iocb;

		iocb = container_of(req, struct aio_kiocb, common);
		return iocb->ki_ctx->mm;
	}
	return NULL;
}

struct task_struct *aio_get_task(struct kiocb *req)
{
	if (req->ki_complete == aio_complete) {
		struct aio_kiocb *iocb;

		iocb = container_of(req, struct aio_kiocb, common);
		return iocb->ki_submit_task;
	}
	return current;
}

static void free_ioctx(struct work_struct *work)
{
	struct kioctx *ctx = container_of(work, struct kioctx, free_work);

	pr_debug("freeing %p\n", ctx);

	aio_free_ring(ctx);
	free_percpu(ctx->cpu);
	percpu_ref_exit(&ctx->reqs);
	percpu_ref_exit(&ctx->users);
	kmem_cache_free(kioctx_cachep, ctx);
}

static void free_ioctx_reqs(struct percpu_ref *ref)
{
	struct kioctx *ctx = container_of(ref, struct kioctx, reqs);

	/* At this point we know that there are no any in-flight requests */
	if (ctx->rq_wait && atomic_dec_and_test(&ctx->rq_wait->count))
		complete(&ctx->rq_wait->comp);

	INIT_WORK(&ctx->free_work, free_ioctx);
	schedule_work(&ctx->free_work);
}

/*
 * When this function runs, the kioctx has been removed from the "hash table"
 * and ctx->users has dropped to 0, so we know no more kiocbs can be submitted -
 * now it's safe to cancel any that need to be.
 */
static void free_ioctx_users(struct percpu_ref *ref)
{
	struct kioctx *ctx = container_of(ref, struct kioctx, users);
	struct aio_kiocb *req;

	spin_lock_irq(&ctx->ctx_lock);

	while (!list_empty(&ctx->active_reqs)) {
		req = list_first_entry(&ctx->active_reqs,
				       struct aio_kiocb, ki_list);

		list_del_init(&req->ki_list);
		kiocb_cancel(req);
	}

	spin_unlock_irq(&ctx->ctx_lock);

	percpu_ref_kill(&ctx->reqs);
	percpu_ref_put(&ctx->reqs);
}

static int ioctx_add_table(struct kioctx *ctx, struct mm_struct *mm)
{
	unsigned i, new_nr;
	struct kioctx_table *table, *old;
	struct aio_ring *ring;

	spin_lock(&mm->ioctx_lock);
	table = rcu_dereference_raw(mm->ioctx_table);

	while (1) {
		if (table)
			for (i = 0; i < table->nr; i++)
				if (!table->table[i]) {
					ctx->id = i;
					table->table[i] = ctx;
					spin_unlock(&mm->ioctx_lock);

					/* While kioctx setup is in progress,
					 * we are protected from page migration
					 * changes ring_pages by ->ring_lock.
					 */
					ring = kmap_atomic(ctx->ring_pages[0]);
					ring->id = ctx->id;
					kunmap_atomic(ring);
					return 0;
				}

		new_nr = (table ? table->nr : 1) * 4;
		spin_unlock(&mm->ioctx_lock);

		table = kzalloc(sizeof(*table) + sizeof(struct kioctx *) *
				new_nr, GFP_KERNEL);
		if (!table)
			return -ENOMEM;

		table->nr = new_nr;

		spin_lock(&mm->ioctx_lock);
		old = rcu_dereference_raw(mm->ioctx_table);

		if (!old) {
			rcu_assign_pointer(mm->ioctx_table, table);
		} else if (table->nr > old->nr) {
			memcpy(table->table, old->table,
			       old->nr * sizeof(struct kioctx *));

			rcu_assign_pointer(mm->ioctx_table, table);
			kfree_rcu(old, rcu);
		} else {
			kfree(table);
			table = old;
		}
	}
}

static void aio_nr_sub(unsigned nr)
{
	spin_lock(&aio_nr_lock);
	if (WARN_ON(aio_nr - nr > aio_nr))
		aio_nr = 0;
	else
		aio_nr -= nr;
	spin_unlock(&aio_nr_lock);
}

/* ioctx_alloc
 *	Allocates and initializes an ioctx.  Returns an ERR_PTR if it failed.
 */
static struct kioctx *ioctx_alloc(unsigned nr_events)
{
	struct mm_struct *mm = current->mm;
	struct kioctx *ctx;
	int err = -ENOMEM;

	/*
	 * We keep track of the number of available ringbuffer slots, to prevent
	 * overflow (reqs_available), and we also use percpu counters for this.
	 *
	 * So since up to half the slots might be on other cpu's percpu counters
	 * and unavailable, double nr_events so userspace sees what they
	 * expected: additionally, we move req_batch slots to/from percpu
	 * counters at a time, so make sure that isn't 0:
	 */
	nr_events = max(nr_events, num_possible_cpus() * 4);
	nr_events *= 2;

	/* Prevent overflows */
	if (nr_events > (0x10000000U / sizeof(struct io_event))) {
		pr_debug("ENOMEM: nr_events too high\n");
		return ERR_PTR(-EINVAL);
	}

	if (!nr_events || (unsigned long)nr_events > (aio_max_nr * 2UL))
		return ERR_PTR(-EAGAIN);

	ctx = kmem_cache_zalloc(kioctx_cachep, GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->max_reqs = nr_events;
	ctx->mm = mm;

	spin_lock_init(&ctx->ctx_lock);
	spin_lock_init(&ctx->completion_lock);
	mutex_init(&ctx->ring_lock);
	/* Protect against page migration throughout kiotx setup by keeping
	 * the ring_lock mutex held until setup is complete. */
	mutex_lock(&ctx->ring_lock);
	init_waitqueue_head(&ctx->wait);

	INIT_LIST_HEAD(&ctx->active_reqs);

	if (percpu_ref_init(&ctx->users, free_ioctx_users, 0, GFP_KERNEL))
		goto err;

	if (percpu_ref_init(&ctx->reqs, free_ioctx_reqs, 0, GFP_KERNEL))
		goto err;

	ctx->cpu = alloc_percpu(struct kioctx_cpu);
	if (!ctx->cpu)
		goto err;

	err = aio_setup_ring(ctx);
	if (err < 0)
		goto err;

	atomic_set(&ctx->reqs_available, ctx->nr_events - 1);
	ctx->req_batch = (ctx->nr_events - 1) / (num_possible_cpus() * 4);
	if (ctx->req_batch < 1)
		ctx->req_batch = 1;

	/* limit the number of system wide aios */
	spin_lock(&aio_nr_lock);
	if (aio_nr + nr_events > (aio_max_nr * 2UL) ||
	    aio_nr + nr_events < aio_nr) {
		spin_unlock(&aio_nr_lock);
		err = -EAGAIN;
		goto err_ctx;
	}
	aio_nr += ctx->max_reqs;
	spin_unlock(&aio_nr_lock);

	percpu_ref_get(&ctx->users);	/* io_setup() will drop this ref */
	percpu_ref_get(&ctx->reqs);	/* free_ioctx_users() will drop this */

	err = ioctx_add_table(ctx, mm);
	if (err)
		goto err_cleanup;

	/* Release the ring_lock mutex now that all setup is complete. */
	mutex_unlock(&ctx->ring_lock);

	pr_debug("allocated ioctx %p[%ld]: mm=%p mask=0x%x\n",
		 ctx, ctx->user_id, mm, ctx->nr_events);
	return ctx;

err_cleanup:
	aio_nr_sub(ctx->max_reqs);
err_ctx:
	atomic_set(&ctx->dead, 1);
	if (ctx->mmap_size)
		vm_munmap(ctx->mmap_base, ctx->mmap_size);
	aio_free_ring(ctx);
err:
	mutex_unlock(&ctx->ring_lock);
	free_percpu(ctx->cpu);
	percpu_ref_exit(&ctx->reqs);
	percpu_ref_exit(&ctx->users);
	kmem_cache_free(kioctx_cachep, ctx);
	pr_debug("error allocating ioctx %d\n", err);
	return ERR_PTR(err);
}

/* kill_ioctx
 *	Cancels all outstanding aio requests on an aio context.  Used
 *	when the processes owning a context have all exited to encourage
 *	the rapid destruction of the kioctx.
 */
static int kill_ioctx(struct mm_struct *mm, struct kioctx *ctx,
		      struct ctx_rq_wait *wait)
{
	struct kioctx_table *table;

	spin_lock(&mm->ioctx_lock);
	if (atomic_xchg(&ctx->dead, 1)) {
		spin_unlock(&mm->ioctx_lock);
		return -EINVAL;
	}

	table = rcu_dereference_raw(mm->ioctx_table);
	WARN_ON(ctx != table->table[ctx->id]);
	table->table[ctx->id] = NULL;
	spin_unlock(&mm->ioctx_lock);

	/* percpu_ref_kill() will do the necessary call_rcu() */
	wake_up_all(&ctx->wait);

	/*
	 * It'd be more correct to do this in free_ioctx(), after all
	 * the outstanding kiocbs have finished - but by then io_destroy
	 * has already returned, so io_setup() could potentially return
	 * -EAGAIN with no ioctxs actually in use (as far as userspace
	 *  could tell).
	 */
	aio_nr_sub(ctx->max_reqs);

	if (ctx->mmap_size)
		vm_munmap(ctx->mmap_base, ctx->mmap_size);

	ctx->rq_wait = wait;
	percpu_ref_kill(&ctx->users);
	return 0;
}

/*
 * exit_aio: called when the last user of mm goes away.  At this point, there is
 * no way for any new requests to be submited or any of the io_* syscalls to be
 * called on the context.
 *
 * There may be outstanding kiocbs, but free_ioctx() will explicitly wait on
 * them.
 */
void exit_aio(struct mm_struct *mm)
{
	struct kioctx_table *table = rcu_dereference_raw(mm->ioctx_table);
	struct ctx_rq_wait wait;
	int i, skipped;

	if (!table)
		return;

	atomic_set(&wait.count, table->nr);
	init_completion(&wait.comp);

	skipped = 0;
	for (i = 0; i < table->nr; ++i) {
		struct kioctx *ctx = table->table[i];

		if (!ctx) {
			skipped++;
			continue;
		}

		/*
		 * We don't need to bother with munmap() here - exit_mmap(mm)
		 * is coming and it'll unmap everything. And we simply can't,
		 * this is not necessarily our ->mm.
		 * Since kill_ioctx() uses non-zero ->mmap_size as indicator
		 * that it needs to unmap the area, just set it to 0.
		 */
		ctx->mmap_size = 0;
		kill_ioctx(mm, ctx, &wait);
	}

	if (!atomic_sub_and_test(skipped, &wait.count)) {
		/* Wait until all IO for the context are done. */
		wait_for_completion(&wait.comp);
	}

	RCU_INIT_POINTER(mm->ioctx_table, NULL);
	kfree(table);
}

static void put_reqs_available(struct kioctx *ctx, unsigned nr)
{
	struct kioctx_cpu *kcpu;
	unsigned long flags;

	local_irq_save(flags);
	kcpu = this_cpu_ptr(ctx->cpu);
	kcpu->reqs_available += nr;

	while (kcpu->reqs_available >= ctx->req_batch * 2) {
		kcpu->reqs_available -= ctx->req_batch;
		atomic_add(ctx->req_batch, &ctx->reqs_available);
	}

	local_irq_restore(flags);
}

static bool get_reqs_available(struct kioctx *ctx)
{
	struct kioctx_cpu *kcpu;
	bool ret = false;
	unsigned long flags;

	local_irq_save(flags);
	kcpu = this_cpu_ptr(ctx->cpu);
	if (!kcpu->reqs_available) {
		int old, avail = atomic_read(&ctx->reqs_available);

		do {
			if (avail < ctx->req_batch)
				goto out;

			old = avail;
			avail = atomic_cmpxchg(&ctx->reqs_available,
					       avail, avail - ctx->req_batch);
		} while (avail != old);

		kcpu->reqs_available += ctx->req_batch;
	}

	ret = true;
	kcpu->reqs_available--;
out:
	local_irq_restore(flags);
	return ret;
}

/* refill_reqs_available
 *	Updates the reqs_available reference counts used for tracking the
 *	number of free slots in the completion ring.  This can be called
 *	from aio_complete() (to optimistically update reqs_available) or
 *	from aio_get_req() (the we're out of events case).  It must be
 *	called holding ctx->completion_lock.
 */
static void refill_reqs_available(struct kioctx *ctx, unsigned head,
                                  unsigned tail)
{
	unsigned events_in_ring, completed;

	/* Clamp head since userland can write to it. */
	head %= ctx->nr_events;
	if (head <= tail)
		events_in_ring = tail - head;
	else
		events_in_ring = ctx->nr_events - (head - tail);

	completed = ctx->completed_events;
	if (events_in_ring < completed)
		completed -= events_in_ring;
	else
		completed = 0;

	if (!completed)
		return;

	ctx->completed_events -= completed;
	put_reqs_available(ctx, completed);
}

/* user_refill_reqs_available
 *	Called to refill reqs_available when aio_get_req() encounters an
 *	out of space in the completion ring.
 */
static void user_refill_reqs_available(struct kioctx *ctx)
{
	spin_lock_irq(&ctx->completion_lock);
	if (ctx->completed_events) {
		struct aio_ring *ring;
		unsigned head;

		/* Access of ring->head may race with aio_read_events_ring()
		 * here, but that's okay since whether we read the old version
		 * or the new version, and either will be valid.  The important
		 * part is that head cannot pass tail since we prevent
		 * aio_complete() from updating tail by holding
		 * ctx->completion_lock.  Even if head is invalid, the check
		 * against ctx->completed_events below will make sure we do the
		 * safe/right thing.
		 */
		ring = kmap_atomic(ctx->ring_pages[0]);
		head = ring->head;
		kunmap_atomic(ring);

		refill_reqs_available(ctx, head, ctx->tail);
	}

	spin_unlock_irq(&ctx->completion_lock);
}

/* aio_get_req
 *	Allocate a slot for an aio request.
 * Returns NULL if no requests are free.
 */
static inline struct aio_kiocb *aio_get_req(struct kioctx *ctx)
{
	struct aio_kiocb *req;

	if (!get_reqs_available(ctx)) {
		user_refill_reqs_available(ctx);
		if (!get_reqs_available(ctx))
			return NULL;
	}

	req = kmem_cache_alloc(kiocb_cachep, GFP_KERNEL|__GFP_ZERO);
	if (unlikely(!req))
		goto out_put;

	percpu_ref_get(&ctx->reqs);

	req->ki_ctx = ctx;
	req->ki_iovec = req->ki_inline_vecs;
	return req;
out_put:
	put_reqs_available(ctx, 1);
	return NULL;
}

static void kiocb_free(struct aio_kiocb *req)
{
	if (req->ki_destruct_fn)
		req->ki_destruct_fn(req);
	if (req->common.ki_filp)
		fput(req->common.ki_filp);
	if (req->ki_eventfd != NULL)
		eventfd_ctx_put(req->ki_eventfd);
	if (req->ki_iovec != req->ki_inline_vecs)
		kfree(req->ki_iovec);
	if (req->ki_submit_task)
		put_task_struct(req->ki_submit_task);
	kmem_cache_free(kiocb_cachep, req);
}

static struct kioctx *lookup_ioctx(unsigned long ctx_id)
{
	struct aio_ring __user *ring  = (void __user *)ctx_id;
	struct mm_struct *mm = current->mm;
	struct kioctx *ctx, *ret = NULL;
	struct kioctx_table *table;
	unsigned id;

	if (get_user(id, &ring->id))
		return NULL;

	rcu_read_lock();
	table = rcu_dereference(mm->ioctx_table);

	if (!table || id >= table->nr)
		goto out;

	ctx = table->table[id];
	if (ctx && ctx->user_id == ctx_id) {
		percpu_ref_get(&ctx->users);
		ret = ctx;
	}
out:
	rcu_read_unlock();
	return ret;
}

/* aio_complete
 *	Called when the io request on the given iocb is complete.
 */
static void aio_complete(struct kiocb *kiocb, long res, long res2)
{
	struct aio_kiocb *iocb = container_of(kiocb, struct aio_kiocb, common);
	struct kioctx	*ctx = iocb->ki_ctx;
	struct aio_ring	*ring;
	struct io_event	*ev_page, *event;
	unsigned tail, pos, head;
	unsigned long	flags;

	/*
	 * Special case handling for sync iocbs:
	 *  - events go directly into the iocb for fast handling
	 *  - the sync task with the iocb in its stack holds the single iocb
	 *    ref, no other paths have a way to get another ref
	 *  - the sync task helpfully left a reference to itself in the iocb
	 */
	BUG_ON(is_sync_kiocb(kiocb));

	if (iocb->ki_list.next) {
		unsigned long flags;

		spin_lock_irqsave(&ctx->ctx_lock, flags);
		list_del(&iocb->ki_list);
		spin_unlock_irqrestore(&ctx->ctx_lock, flags);
	}

	/*
	 * Add a completion event to the ring buffer. Must be done holding
	 * ctx->completion_lock to prevent other code from messing with the tail
	 * pointer since we might be called from irq context.
	 */
	spin_lock_irqsave(&ctx->completion_lock, flags);

	tail = ctx->tail;
	pos = tail + AIO_EVENTS_OFFSET;

	if (++tail >= ctx->nr_events)
		tail = 0;

	ev_page = kmap_atomic(ctx->ring_pages[pos / AIO_EVENTS_PER_PAGE]);
	event = ev_page + pos % AIO_EVENTS_PER_PAGE;

	event->obj = (u64)(unsigned long)iocb->ki_user_iocb;
	event->data = iocb->ki_user_data;
	event->res = res;
	event->res2 = res2;

	kunmap_atomic(ev_page);
	flush_dcache_page(ctx->ring_pages[pos / AIO_EVENTS_PER_PAGE]);

	pr_debug("%p[%u]: %p: %p %Lx %lx %lx\n",
		 ctx, tail, iocb, iocb->ki_user_iocb, iocb->ki_user_data,
		 res, res2);

	/* after flagging the request as done, we
	 * must never even look at it again
	 */
	smp_wmb();	/* make event visible before updating tail */

	ctx->tail = tail;

	ring = kmap_atomic(ctx->ring_pages[0]);
	head = ring->head;
	ring->tail = tail;
	kunmap_atomic(ring);
	flush_dcache_page(ctx->ring_pages[0]);

	ctx->completed_events++;
	if (ctx->completed_events > 1)
		refill_reqs_available(ctx, head, tail);
	spin_unlock_irqrestore(&ctx->completion_lock, flags);

	pr_debug("added to ring %p at [%u]\n", iocb, tail);

	/*
	 * Check if the user asked us to deliver the result through an
	 * eventfd. The eventfd_signal() function is safe to be called
	 * from IRQ context.
	 */
	if (iocb->ki_eventfd != NULL)
		eventfd_signal(iocb->ki_eventfd, 1);

	/* everything turned out well, dispose of the aiocb. */
	kiocb_free(iocb);

	/*
	 * We have to order our ring_info tail store above and test
	 * of the wait list below outside the wait lock.  This is
	 * like in wake_up_bit() where clearing a bit has to be
	 * ordered with the unlocked test.
	 */
	smp_mb();

	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);

	percpu_ref_put(&ctx->reqs);
}

/* aio_read_events_ring
 *	Pull an event off of the ioctx's event ring.  Returns the number of
 *	events fetched
 */
static long aio_read_events_ring(struct kioctx *ctx,
				 struct io_event __user *event, long nr)
{
	struct aio_ring *ring;
	unsigned head, tail, pos;
	long ret = 0;
	int copy_ret;

	/*
	 * The mutex can block and wake us up and that will cause
	 * wait_event_interruptible_hrtimeout() to schedule without sleeping
	 * and repeat. This should be rare enough that it doesn't cause
	 * peformance issues. See the comment in read_events() for more detail.
	 */
	sched_annotate_sleep();
	mutex_lock(&ctx->ring_lock);

	/* Access to ->ring_pages here is protected by ctx->ring_lock. */
	ring = kmap_atomic(ctx->ring_pages[0]);
	head = ring->head;
	tail = ring->tail;
	kunmap_atomic(ring);

	/*
	 * Ensure that once we've read the current tail pointer, that
	 * we also see the events that were stored up to the tail.
	 */
	smp_rmb();

	pr_debug("h%u t%u m%u\n", head, tail, ctx->nr_events);

	if (head == tail)
		goto out;

	head %= ctx->nr_events;
	tail %= ctx->nr_events;

	while (ret < nr) {
		long avail;
		struct io_event *ev;
		struct page *page;

		avail = (head <= tail ?  tail : ctx->nr_events) - head;
		if (head == tail)
			break;

		avail = min(avail, nr - ret);
		avail = min_t(long, avail, AIO_EVENTS_PER_PAGE -
			    ((head + AIO_EVENTS_OFFSET) % AIO_EVENTS_PER_PAGE));

		pos = head + AIO_EVENTS_OFFSET;
		page = ctx->ring_pages[pos / AIO_EVENTS_PER_PAGE];
		pos %= AIO_EVENTS_PER_PAGE;

		ev = kmap(page);
		copy_ret = copy_to_user(event + ret, ev + pos,
					sizeof(*ev) * avail);
		kunmap(page);

		if (unlikely(copy_ret)) {
			ret = -EFAULT;
			goto out;
		}

		ret += avail;
		head += avail;
		head %= ctx->nr_events;
	}

	ring = kmap_atomic(ctx->ring_pages[0]);
	ring->head = head;
	kunmap_atomic(ring);
	flush_dcache_page(ctx->ring_pages[0]);

	pr_debug("%li  h%u t%u\n", ret, head, tail);
out:
	mutex_unlock(&ctx->ring_lock);

	return ret;
}

static bool aio_read_events(struct kioctx *ctx, long min_nr, long nr,
			    struct io_event __user *event, long *i)
{
	long ret = aio_read_events_ring(ctx, event + *i, nr - *i);

	if (ret > 0)
		*i += ret;

	if (unlikely(atomic_read(&ctx->dead)))
		ret = -EINVAL;

	if (!*i)
		*i = ret;

	return ret < 0 || *i >= min_nr;
}

static long read_events(struct kioctx *ctx, long min_nr, long nr,
			struct io_event __user *event,
			struct timespec __user *timeout)
{
	ktime_t until = { .tv64 = KTIME_MAX };
	long ret = 0;

	if (timeout) {
		struct timespec	ts;

		if (unlikely(copy_from_user(&ts, timeout, sizeof(ts))))
			return -EFAULT;
		if (!timespec_valid(&ts))
			return -EINVAL;

		until = timespec_to_ktime(ts);
	}

	/*
	 * Note that aio_read_events() is being called as the conditional - i.e.
	 * we're calling it after prepare_to_wait() has set task state to
	 * TASK_INTERRUPTIBLE.
	 *
	 * But aio_read_events() can block, and if it blocks it's going to flip
	 * the task state back to TASK_RUNNING.
	 *
	 * This should be ok, provided it doesn't flip the state back to
	 * TASK_RUNNING and return 0 too much - that causes us to spin. That
	 * will only happen if the mutex_lock() call blocks, and we then find
	 * the ringbuffer empty. So in practice we should be ok, but it's
	 * something to be aware of when touching this code.
	 */
	if (until.tv64 == 0)
		aio_read_events(ctx, min_nr, nr, event, &ret);
	else
		wait_event_interruptible_hrtimeout(ctx->wait,
				aio_read_events(ctx, min_nr, nr, event, &ret),
				until);

	if (!ret && signal_pending(current))
		ret = -EINTR;

	return ret;
}

/* sys_io_setup:
 *	Create an aio_context capable of receiving at least nr_events.
 *	ctxp must not point to an aio_context that already exists, and
 *	must be initialized to 0 prior to the call.  On successful
 *	creation of the aio_context, *ctxp is filled in with the resulting 
 *	handle.  May fail with -EINVAL if *ctxp is not initialized,
 *	if the specified nr_events exceeds internal limits.  May fail 
 *	with -EAGAIN if the specified nr_events exceeds the user's limit 
 *	of available events.  May fail with -ENOMEM if insufficient kernel
 *	resources are available.  May fail with -EFAULT if an invalid
 *	pointer is passed for ctxp.  Will fail with -ENOSYS if not
 *	implemented.
 */
SYSCALL_DEFINE2(io_setup, unsigned, nr_events, aio_context_t __user *, ctxp)
{
	struct kioctx *ioctx = NULL;
	unsigned long ctx;
	long ret;

	ret = get_user(ctx, ctxp);
	if (unlikely(ret))
		goto out;

	ret = -EINVAL;
	if (unlikely(ctx || nr_events == 0)) {
		pr_debug("EINVAL: ctx %lu nr_events %u\n",
		         ctx, nr_events);
		goto out;
	}

	ioctx = ioctx_alloc(nr_events);
	ret = PTR_ERR(ioctx);
	if (!IS_ERR(ioctx)) {
		ret = put_user(ioctx->user_id, ctxp);
		if (ret)
			kill_ioctx(current->mm, ioctx, NULL);
		percpu_ref_put(&ioctx->users);
	}

out:
	return ret;
}

/* sys_io_destroy:
 *	Destroy the aio_context specified.  May cancel any outstanding 
 *	AIOs and block on completion.  Will fail with -ENOSYS if not
 *	implemented.  May fail with -EINVAL if the context pointed to
 *	is invalid.
 */
SYSCALL_DEFINE1(io_destroy, aio_context_t, ctx)
{
	struct kioctx *ioctx = lookup_ioctx(ctx);
	if (likely(NULL != ioctx)) {
		struct ctx_rq_wait wait;
		int ret;

		init_completion(&wait.comp);
		atomic_set(&wait.count, 1);

		/* Pass requests_done to kill_ioctx() where it can be set
		 * in a thread-safe way. If we try to set it here then we have
		 * a race condition if two io_destroy() called simultaneously.
		 */
		ret = kill_ioctx(current->mm, ioctx, &wait);
		percpu_ref_put(&ioctx->users);

		/* Wait until all IO for the context are done. Otherwise kernel
		 * keep using user-space buffers even if user thinks the context
		 * is destroyed.
		 */
		if (!ret)
			wait_for_completion(&wait.comp);

		return ret;
	}
	pr_debug("EINVAL: invalid context id\n");
	return -EINVAL;
}

typedef ssize_t (rw_iter_op)(struct kiocb *, struct iov_iter *);

static int aio_setup_vectored_rw(int rw, char __user *buf, size_t len,
				 struct iovec **iovec,
				 bool compat,
				 struct iov_iter *iter)
{
#ifdef CONFIG_COMPAT
	if (compat)
		return compat_import_iovec(rw,
				(struct compat_iovec __user *)buf,
				len, UIO_FASTIOV, iovec, iter);
#endif
	return import_iovec(rw, (struct iovec __user *)buf,
				len, UIO_FASTIOV, iovec, iter);
}

#if IS_ENABLED(CONFIG_AIO_THREAD)
/* aio_thread_queue_iocb_cancel_early:
 *	Early stage cancellation helper function for threaded aios.  This
 *	is used prior to the iocb being assigned to a worker thread.
 */
static int aio_thread_queue_iocb_cancel_early(struct kiocb *iocb)
{
	return 0;
}

/* aio_thread_queue_iocb_cancel:
 *	Late stage cancellation method for threaded aios.  Once an iocb is
 *	assigned to a worker thread, we use a fatal signal to interrupt an
 *	in-progress operation.
 */
static int aio_thread_queue_iocb_cancel(struct kiocb *kiocb)
{
	struct aio_kiocb *iocb = container_of(kiocb, struct aio_kiocb, common);

	if (iocb->ki_cancel_task) {
		force_sig(SIGKILL, iocb->ki_cancel_task);
		return 0;
	}
	return -EAGAIN;
}

/* aio_thread_fn:
 *	Entry point for worker to perform threaded aio.  Handles issues
 *	arising due to cancellation using signals.
 */
static void aio_thread_fn(struct work_struct *work)
{
	struct aio_kiocb *iocb = container_of(work, struct aio_kiocb, ki_work);
	struct files_struct *old_files = current->files;
	const struct cred *old_cred = current_cred();
	struct fs_struct *old_fs = current->fs;
	kiocb_cancel_fn *old_cancel;
	long ret;

	iocb->ki_cancel_task = current;
	current->kiocb = &iocb->common;		/* For io_send_sig(). */
	WARN_ON(atomic_read(&current->signal->sigcnt) != 1);

	if (iocb->ki_fs)
		current->fs = iocb->ki_fs;
	if (iocb->ki_files)
		current->files = iocb->ki_files;
	if (iocb->ki_cred)
		current->cred = iocb->ki_cred;

	/* Check for early stage cancellation and switch to late stage
	 * cancellation if it has not already occurred.
	 */
	old_cancel = cmpxchg(&iocb->ki_cancel,
			     (kiocb_cancel_fn *)aio_thread_queue_iocb_cancel_early,
			     (kiocb_cancel_fn *)aio_thread_queue_iocb_cancel);
	if (old_cancel != KIOCB_CANCELLED) {
		if (iocb->ki_thread_flags & AIO_THREAD_NEED_MM)
			use_mm(iocb->ki_ctx->mm);
		ret = iocb->ki_work_fn(iocb);
		if (iocb->ki_thread_flags & AIO_THREAD_NEED_MM)
			unuse_mm(iocb->ki_ctx->mm);
	} else
		ret = -EINTR;

	current->kiocb = NULL;
	if (unlikely(ret == -ERESTARTSYS || ret == -ERESTARTNOINTR ||
		     ret == -ERESTARTNOHAND || ret == -ERESTART_RESTARTBLOCK))
		ret = -EINTR;

	/* Completion serializes cancellation by taking ctx_lock, so
	 * aio_complete() will not return until after force_sig() in
	 * aio_thread_queue_iocb_cancel().  This should ensure that
	 * the signal is pending before being flushed in this thread.
	 */
	aio_complete(&iocb->common, ret, 0);
	if (fatal_signal_pending(current))
		flush_signals(current);

	/* Clean up state after aio_complete() since ki_destruct may still
	 * need to access them.
	 */
	if (iocb->ki_cred) {
		current->cred = old_cred;
		put_cred(iocb->ki_cred);
	}
	if (iocb->ki_files) {
		current->files = old_files;
		put_files_struct(iocb->ki_files);
	}
	if (iocb->ki_fs) {
		exit_fs(current);
		current->fs = old_fs;
	}
}

/* aio_thread_queue_iocb
 *	Queues an aio_kiocb for dispatch to a worker thread.  Prepares the
 *	aio_kiocb for cancellation.  The caller must provide a function to
 *	execute the operation in work_fn.  The flags may be provided as an
 *	ored set AIO_THREAD_xxx.
 */
static ssize_t aio_thread_queue_iocb(struct aio_kiocb *iocb,
				     aio_thread_work_fn_t work_fn,
				     unsigned flags)
{
	if (!aio_may_use_threads())
		return -EINVAL;
	INIT_WORK(&iocb->ki_work, aio_thread_fn);
	iocb->ki_thread_flags = flags;
	iocb->ki_work_fn = work_fn;
	if (flags & AIO_THREAD_NEED_TASK) {
		iocb->ki_submit_task = current;
		get_task_struct(iocb->ki_submit_task);
	}
	if (flags & AIO_THREAD_NEED_FS) {
		struct fs_struct *fs = current->fs;

		iocb->ki_fs = fs;
		spin_lock(&fs->lock);
		fs->users++;
		spin_unlock(&fs->lock);
	}
	if (flags & AIO_THREAD_NEED_FILES) {
		iocb->ki_files = current->files;
		atomic_inc(&iocb->ki_files->count);
	}
	if (flags & AIO_THREAD_NEED_CRED)
		iocb->ki_cred = get_current_cred();

	/* Cancellation needs to be always available for operations performed
	 * using helper threads.  Prior to the iocb being assigned to a worker
	 * thread, we need to record that a cancellation has occurred.  We
	 * can do this by having a minimal helper function that is recorded in
	 * ki_cancel.
	 */
	kiocb_set_cancel_fn(&iocb->common, aio_thread_queue_iocb_cancel_early);
	queue_work(system_long_wq, &iocb->ki_work);
	return -EIOCBQUEUED;
}

static long aio_thread_op_read_iter(struct aio_kiocb *iocb)
{
	struct file *filp;
	long ret;

	filp = iocb->common.ki_filp;

	if (filp->f_op->read_iter) {
		struct kiocb sync_kiocb;

		init_sync_kiocb(&sync_kiocb, filp);
		sync_kiocb.ki_pos = iocb->common.ki_pos;
		ret = filp->f_op->read_iter(&sync_kiocb, &iocb->ki_iter);
	} else if (filp->f_op->read)
		ret = do_loop_readv_writev(filp, &iocb->ki_iter,
					   &iocb->common.ki_pos,
					   filp->f_op->read, 0);
	else
		ret = -EINVAL;
	return ret;
}

ssize_t generic_async_read_iter_non_direct(struct kiocb *iocb,
					   struct iov_iter *iter)
{
	if ((iocb->ki_flags & IOCB_DIRECT) ||
	    (iocb->ki_complete != aio_complete))
		return iocb->ki_filp->f_op->read_iter(iocb, iter);
	return generic_async_read_iter(iocb, iter);
}
EXPORT_SYMBOL(generic_async_read_iter_non_direct);

ssize_t generic_async_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct aio_kiocb *req;

	req = container_of(iocb, struct aio_kiocb, common);
	if (iter != &req->ki_iter)
		return -EINVAL;

	return aio_thread_queue_iocb(req, aio_thread_op_read_iter,
				     AIO_THREAD_NEED_TASK | AIO_THREAD_NEED_MM);
}
EXPORT_SYMBOL(generic_async_read_iter);

static long aio_thread_op_write_iter(struct aio_kiocb *iocb)
{
	u64 saved_rlim_fsize;
	struct file *filp;
	long ret;

	filp = iocb->common.ki_filp;
	saved_rlim_fsize = rlimit(RLIMIT_FSIZE);
	current->signal->rlim[RLIMIT_FSIZE].rlim_cur = iocb->ki_rlimit_fsize;

	if (filp->f_op->write_iter) {
		struct kiocb sync_kiocb;

		init_sync_kiocb(&sync_kiocb, filp);
		sync_kiocb.ki_pos = iocb->common.ki_pos;
		ret = filp->f_op->write_iter(&sync_kiocb, &iocb->ki_iter);
	} else if (filp->f_op->write)
		ret = do_loop_readv_writev(filp, &iocb->ki_iter,
					   &iocb->common.ki_pos,
					   (io_fn_t)filp->f_op->write, 0);
	else
		ret = -EINVAL;
	current->signal->rlim[RLIMIT_FSIZE].rlim_cur = saved_rlim_fsize;
	return ret;
}

ssize_t generic_async_write_iter_non_direct(struct kiocb *iocb,
					    struct iov_iter *iter)
{
	if ((iocb->ki_flags & IOCB_DIRECT) ||
	    (iocb->ki_complete != aio_complete))
		return iocb->ki_filp->f_op->write_iter(iocb, iter);
	return generic_async_write_iter(iocb, iter);
}
EXPORT_SYMBOL(generic_async_write_iter_non_direct);

ssize_t generic_async_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct aio_kiocb *req;

	req = container_of(iocb, struct aio_kiocb, common);
	if (iter != &req->ki_iter)
		return -EINVAL;
	req->ki_rlimit_fsize = rlimit(RLIMIT_FSIZE);

	return aio_thread_queue_iocb(req, aio_thread_op_write_iter,
				     AIO_THREAD_NEED_TASK | AIO_THREAD_NEED_MM);
}
EXPORT_SYMBOL(generic_async_write_iter);

static long aio_thread_op_fsync(struct aio_kiocb *iocb)
{
	return vfs_fsync(iocb->common.ki_filp, iocb->ki_data);
}

static long aio_thread_op_poll(struct aio_kiocb *iocb)
{
	struct file *file = iocb->common.ki_filp;
	short events = iocb->ki_data;
	struct poll_wqueues table;
	unsigned int mask;
	ssize_t ret = 0;

	poll_initwait(&table);
	events |= POLLERR | POLLHUP;

	for (;;) {
		mask = DEFAULT_POLLMASK;
		if (file->f_op && file->f_op->poll) {
			table.pt._key = events;
			mask = file->f_op->poll(file, &table.pt);
		}
		/* Mask out unneeded events. */
		mask &= events;
		ret = mask;
		if (mask)
			break;

		ret = -EINTR;
		if (signal_pending(current))
			break;

		poll_schedule_timeout(&table, TASK_INTERRUPTIBLE, NULL, 0);
	}

	poll_freewait(&table);
	return ret;
}

static long aio_poll(struct aio_kiocb *req, struct iocb *user_iocb, bool compat)
{
	if (!req->common.ki_filp->f_op->poll)
		return -EINVAL;
	if ((unsigned short)user_iocb->aio_buf != user_iocb->aio_buf)
		return -EINVAL;
	req->ki_data = user_iocb->aio_buf;
	return aio_thread_queue_iocb(req, aio_thread_op_poll, 0);
}

static long aio_do_unlinkat(int fd, const char *filename, int flags, int mode)
{
	if (flags || mode)
		return -EINVAL;
	return do_unlinkat(fd, filename);
}

static long aio_thread_op_foo_at(struct aio_kiocb *req)
{
	u64 buf, offset;
	long ret;
	u32 fd;

	if (unlikely(get_user(fd, &req->ki_user_iocb->aio_fildes)))
		ret = -EFAULT;
	else if (unlikely(get_user(buf, &req->ki_user_iocb->aio_buf)))
		ret = -EFAULT;
	else if (unlikely(get_user(offset, &req->ki_user_iocb->aio_offset)))
		ret = -EFAULT;
	else {
		do_foo_at_t do_foo_at = (void *)req->ki_data;

		ret = do_foo_at((s32)fd,
				(const char __user *)(long)buf,
				(int)offset,
				(unsigned short)(offset >> 32));
	}
	return ret;
}

static void openat_destruct(struct aio_kiocb *req)
{
	struct filename *filename = req->common.private;
	int fd;

	putname(filename);
	fd = req->ki_data;
	if (fd >= 0)
		put_unused_fd(fd);
}

static long aio_thread_op_openat(struct aio_kiocb *req)
{
	struct filename *filename = req->common.private;
	int mode = req->common.ki_pos >> 32;
	int flags = req->common.ki_pos;
	struct open_flags op;
	struct file *f;
	int dfd = req->ki_data2;

	build_open_flags(flags, mode, &op);
	f = do_filp_open(dfd, filename, &op);
	if (!IS_ERR(f)) {
		int fd = req->ki_data;
		/* Prevent openat_destruct from doing put_unused_fd() */
		req->ki_data = -1;
		fsnotify_open(f);
		fd_install(fd, f);
		return fd;
	}
	return PTR_ERR(f);
}

static long aio_openat(struct aio_kiocb *req, struct iocb *uiocb, bool compat)
{
	int mode = req->common.ki_pos >> 32;
	struct filename *filename;
	struct open_flags op;
	int flags;
	int fd;

	if (force_o_largefile())
		req->common.ki_pos |= O_LARGEFILE;
	flags = req->common.ki_pos;
	fd = build_open_flags(flags, mode, &op);
	if (fd)
		goto out_err;

	filename = getname((const char __user *)(long)uiocb->aio_buf);
	if (IS_ERR(filename)) {
		fd = PTR_ERR(filename);
		goto out_err;
	}
	req->common.private = filename;
	req->ki_destruct_fn = openat_destruct;
	req->ki_data = fd = get_unused_fd_flags(flags);
	if (fd >= 0) {
		struct file *f;
		op.lookup_flags |= LOOKUP_RCU | LOOKUP_NONBLOCK;
		req->ki_data = fd;
		req->ki_data2 = uiocb->aio_fildes;
		f = do_filp_open(uiocb->aio_fildes, filename, &op);
		if (IS_ERR(f) && ((PTR_ERR(f) == -ECHILD) ||
				  (PTR_ERR(f) == -ESTALE) ||
				  (PTR_ERR(f) == -EAGAIN))) {
			int ret;
			ret = aio_thread_queue_iocb(req, aio_thread_op_openat,
						   AIO_THREAD_NEED_TASK |
						   AIO_THREAD_NEED_FILES |
						   AIO_THREAD_NEED_CRED);
			if (ret == -EIOCBQUEUED)
				return ret;
			put_unused_fd(fd);
			fd = ret;
		} else if (IS_ERR(f)) {
			put_unused_fd(fd);
			fd = PTR_ERR(f);
		} else {
			fsnotify_open(f);
			fd_install(fd, f);
		}
	}
out_err:
	aio_complete(&req->common, fd, 0);
	return -EIOCBQUEUED;
}

static long aio_unlink(struct aio_kiocb *req, struct iocb *uiocb, bool compt)
{
	req->ki_data = (unsigned long)(void *)aio_do_unlinkat;
	return aio_thread_queue_iocb(req, aio_thread_op_foo_at,
				     AIO_THREAD_NEED_TASK |
				     AIO_THREAD_NEED_MM |
				     AIO_THREAD_NEED_FILES |
				     AIO_THREAD_NEED_CRED);
}

static int aio_ra_filler(void *data, struct page *page)
{
	struct file *file = data;

	return file->f_mapping->a_ops->readpage(file, page);
}

static long aio_ra_wait_on_pages(struct file *file, pgoff_t start,
				 unsigned long nr)
{
	struct address_space *mapping = file->f_mapping;
	unsigned long i;

	/* Wait on pages starting at the end to holdfully avoid too many
	 * wakeups.
	 */
	for (i = nr; i-- > 0; ) {
		pgoff_t index = start + i;
		struct page *page;

		/* First do the quick check to see if the page is present and
		 * uptodate.
		 */
		rcu_read_lock();
		page = radix_tree_lookup(&mapping->page_tree, index);
		rcu_read_unlock();

		if (page && !radix_tree_exceptional_entry(page) &&
		    PageUptodate(page)) {
			continue;
		}

		page = read_cache_page(mapping, index, aio_ra_filler, file);
		if (IS_ERR(page))
			return PTR_ERR(page);
		page_cache_release(page);
	}
	return 0;
}

static long aio_thread_op_readahead(struct aio_kiocb *iocb)
{
	pgoff_t start, end, nr, offset;
	long ret = 0;

	start = iocb->common.ki_pos >> PAGE_CACHE_SHIFT;
	end = (iocb->common.ki_pos + iocb->ki_data - 1) >> PAGE_CACHE_SHIFT;
	nr = end - start + 1;

	for (offset = 0; offset < nr; ) {
		pgoff_t chunk = nr - offset;
		unsigned long max_chunk = (2 * 1024 * 1024) / PAGE_CACHE_SIZE;

		if (chunk > max_chunk)
			chunk = max_chunk;

		ret = __do_page_cache_readahead(iocb->common.ki_filp->f_mapping,
						iocb->common.ki_filp,
						start + offset, chunk, 0, 1);
		if (ret <= 0)
			break;
		offset += ret;
	}

	if (!offset && ret < 0)
		return ret;

	if (offset > 0) {
		ret = aio_ra_wait_on_pages(iocb->common.ki_filp, start, offset);
		if (ret < 0)
			return ret;
	}

	if (offset == nr)
		return iocb->ki_data;
	if (offset > 0)
		return ((start + offset) << PAGE_CACHE_SHIFT) -
			iocb->common.ki_pos;
	return 0;
}

static long aio_ra(struct aio_kiocb *iocb, struct iocb *uiocb, bool compat)
{
	struct address_space *mapping = iocb->common.ki_filp->f_mapping;
	pgoff_t index, end;
	loff_t epos, isize;
	int do_io = 0;
	size_t len;

	if (!aio_may_use_threads())
		return -EINVAL;
	if (uiocb->aio_buf)
		return -EINVAL;
	if (!mapping || !mapping->a_ops)
		return -EBADF;
	if (!mapping->a_ops->readpage && !mapping->a_ops->readpages)
		return -EBADF;
	len = uiocb->aio_nbytes;
	if (!len)
		return 0;

	epos = iocb->common.ki_pos + len;
	if (epos < 0)
		return -EINVAL;
	isize = i_size_read(mapping->host);
	if (isize < epos) {
		epos = isize - iocb->common.ki_pos;
		if (epos <= 0)
			return 0;
		if ((unsigned long)epos != epos)
			return -EINVAL;
		len = epos;
	}

	index = iocb->common.ki_pos >> PAGE_CACHE_SHIFT;
	end = (iocb->common.ki_pos + len - 1) >> PAGE_CACHE_SHIFT;
	iocb->ki_data = len;
	if (end < index)
		return -EINVAL;

	do {
		struct page *page;

		rcu_read_lock();
		page = radix_tree_lookup(&mapping->page_tree, index);
		rcu_read_unlock();

		if (!page || radix_tree_exceptional_entry(page) ||
		    !PageUptodate(page))
			do_io = 1;
	} while (!do_io && (index++ < end));

	if (do_io)
		return aio_thread_queue_iocb(iocb, aio_thread_op_readahead, 0);
	return len;
}

static long aio_thread_op_renameat(struct aio_kiocb *iocb)
{
	const void * __user user_info = (void * __user)iocb->common.private;
	struct renameat_info info;
	const char * __user old;
	const char * __user new;
	int olddir, newdir;
	unsigned flags;
	long ret;

	if (unlikely(copy_from_user(&info, user_info, sizeof(info)))) {
		ret = -EFAULT;
		goto done;
	}

	old = (const char * __user)(unsigned long)info.oldpath;
	new = (const char * __user)(unsigned long)info.newpath;
	olddir = info.olddirfd;
	newdir = info.newdirfd;
	flags = info.flags;

	if (((unsigned long)old != info.oldpath) ||
	    ((unsigned long)new != info.newpath) ||
	    (olddir != info.olddirfd) ||
	    (newdir != info.newdirfd) ||
	    (flags != info.flags))
		ret = -EINVAL;
	else
		ret = sys_renameat2(olddir, old, newdir, new, flags);
done:
	return ret;
}

static long aio_rename(struct aio_kiocb *iocb, struct iocb *user_iocb, bool c)
{
	if (user_iocb->aio_nbytes != sizeof(struct renameat_info))
		return -EINVAL;
	if (user_iocb->aio_offset)
		return -EINVAL;

	iocb->common.private = (void *)(long)user_iocb->aio_buf;
	return aio_thread_queue_iocb(iocb, aio_thread_op_renameat,
				     AIO_THREAD_NEED_TASK |
				     AIO_THREAD_NEED_MM |
				     AIO_THREAD_NEED_FS |
				     AIO_THREAD_NEED_FILES |
				     AIO_THREAD_NEED_CRED);
}
#endif /* IS_ENABLED(CONFIG_AIO_THREAD) */

long aio_fsync(struct aio_kiocb *req, struct iocb *user_iocb, bool compat)
{
	bool datasync = (user_iocb->aio_lio_opcode == IOCB_CMD_FDSYNC);
	struct file *file = req->common.ki_filp;

	if (file->f_op->aio_fsync)
		return file->f_op->aio_fsync(&req->common, datasync);
#if IS_ENABLED(CONFIG_AIO_THREAD)
	if (file->f_op->fsync) {
		req->ki_data = datasync;
		return aio_thread_queue_iocb(req, aio_thread_op_fsync, 0);
	}
#endif
	return -EINVAL;
}

/*
 * aio_rw:
 *	Implements read/write vectored and non-vectored
 */
static long aio_rw(struct aio_kiocb *req, struct iocb *user_iocb, bool compat)
{
	struct file *file = req->common.ki_filp;
	ssize_t ret = -EINVAL;
	char __user *buf;
	int rw;
	fmode_t mode;
	rw_iter_op *iter_op;

	switch (user_iocb->aio_lio_opcode) {
	case IOCB_CMD_PREAD:
	case IOCB_CMD_PREADV:
		mode	= FMODE_READ;
		rw	= READ;
		iter_op	= file->f_op->async_read_iter;
		if (iter_op)
			goto rw_common;
		if ((aio_may_use_threads()) &&
		    (file->f_op->read_iter || file->f_op->read)) {
			iter_op = generic_async_read_iter;
			goto rw_common;
		}
		iter_op	= file->f_op->read_iter;
		goto rw_common;

	case IOCB_CMD_PWRITE:
	case IOCB_CMD_PWRITEV:
		mode	= FMODE_WRITE;
		rw	= WRITE;
		iter_op	= file->f_op->async_write_iter;
		if (iter_op)
			goto rw_common;
		if ((aio_may_use_threads()) &&
		    (file->f_op->write_iter || file->f_op->write)) {
			iter_op = generic_async_write_iter;
			goto rw_common;
		}
		iter_op	= file->f_op->write_iter;
		goto rw_common;
rw_common:
		if (unlikely(!(file->f_mode & mode)))
			return -EBADF;

		if (!iter_op)
			return -EINVAL;

		buf = (char __user *)(unsigned long)user_iocb->aio_buf;
		if (user_iocb->aio_lio_opcode == IOCB_CMD_PREADV ||
		    user_iocb->aio_lio_opcode == IOCB_CMD_PWRITEV)
			ret = aio_setup_vectored_rw(rw, buf,
						    user_iocb->aio_nbytes,
						    &req->ki_iovec, compat,
						    &req->ki_iter);
		else {
			ret = import_single_range(rw, buf,
						  user_iocb->aio_nbytes,
						  req->ki_iovec,
						  &req->ki_iter);
		}
		if (!ret)
			ret = rw_verify_area(rw, file, &req->common.ki_pos,
					     iov_iter_count(&req->ki_iter));
		if (ret < 0)
			return ret;

		if (rw == WRITE)
			file_start_write(file);

		ret = iter_op(&req->common, &req->ki_iter);

		if (rw == WRITE)
			file_end_write(file);
		break;

	default:
		pr_debug("EINVAL: no operation provided\n");
	}
	return ret;
}

typedef long (*aio_submit_fn_t)(struct aio_kiocb *req, struct iocb *iocb,
				bool compat);

#define NEED_FD			0x0001

struct submit_info {
	aio_submit_fn_t		fn;
	unsigned long		flags;
};

static const struct submit_info aio_submit_info[] = {
	[IOCB_CMD_PREAD]	= { aio_rw,	NEED_FD },
	[IOCB_CMD_PWRITE]	= { aio_rw,	NEED_FD },
	[IOCB_CMD_PREADV]	= { aio_rw,	NEED_FD },
	[IOCB_CMD_PWRITEV]	= { aio_rw,	NEED_FD },
	[IOCB_CMD_FSYNC]	= { aio_fsync,	NEED_FD },
	[IOCB_CMD_FDSYNC]	= { aio_fsync,	NEED_FD },
#if IS_ENABLED(CONFIG_AIO_THREAD)
	[IOCB_CMD_POLL]		= { aio_poll,	NEED_FD },
	[IOCB_CMD_OPENAT]	= { aio_openat,	0 },
	[IOCB_CMD_UNLINKAT]	= { aio_unlink,	0 },
	[IOCB_CMD_READAHEAD]	= { aio_ra,	NEED_FD },
	[IOCB_CMD_RENAMEAT]	= { aio_rename,	0 },
#endif
};

static int io_submit_one(struct kioctx *ctx, struct iocb __user *user_iocb,
			 struct iocb *iocb, bool compat)
{
	const struct submit_info *submit_info;
	struct aio_kiocb *req;
	ssize_t ret;

	/* enforce forwards compatibility on users */
	if (unlikely(iocb->aio_reserved1 || iocb->aio_reserved2)) {
		pr_debug("EINVAL: reserve field set\n");
		return -EINVAL;
	}

	/* prevent overflows */
	if (unlikely(
	    (iocb->aio_buf != (unsigned long)iocb->aio_buf) ||
	    (iocb->aio_nbytes != (size_t)iocb->aio_nbytes) ||
	    ((ssize_t)iocb->aio_nbytes < 0)
	   )) {
		pr_debug("EINVAL: overflow check\n");
		return -EINVAL;
	}

	if (unlikely(iocb->aio_lio_opcode >= ARRAY_SIZE(aio_submit_info)))
		return -EINVAL;
	submit_info = &aio_submit_info[iocb->aio_lio_opcode];
	if (unlikely(!submit_info->fn))
		return -EINVAL;

	req = aio_get_req(ctx);
	if (unlikely(!req))
		return -EAGAIN;

	if (submit_info->flags & NEED_FD) {
		req->common.ki_filp = fget(iocb->aio_fildes);
		if (unlikely(!req->common.ki_filp)) {
			ret = -EBADF;
			goto out_put_req;
		}
		req->common.ki_flags = iocb_flags(req->common.ki_filp);
	}
	req->common.ki_pos = iocb->aio_offset;
	req->common.ki_complete = aio_complete;

	if (iocb->aio_flags & IOCB_FLAG_RESFD) {
		/*
		 * If the IOCB_FLAG_RESFD flag of aio_flags is set, get an
		 * instance of the file* now. The file descriptor must be
		 * an eventfd() fd, and will be signaled for each completed
		 * event using the eventfd_signal() function.
		 */
		req->ki_eventfd = eventfd_ctx_fdget((int) iocb->aio_resfd);
		if (IS_ERR(req->ki_eventfd)) {
			ret = PTR_ERR(req->ki_eventfd);
			req->ki_eventfd = NULL;
			goto out_put_req;
		}

		req->common.ki_flags |= IOCB_EVENTFD;
	}

	ret = put_user(KIOCB_KEY, &user_iocb->aio_key);
	if (unlikely(ret)) {
		pr_debug("EFAULT: aio_key\n");
		goto out_put_req;
	}

	req->ki_user_iocb = user_iocb;
	req->ki_user_data = iocb->aio_data;

	ret = submit_info->fn(req, iocb, compat);
	if (ret != -EIOCBQUEUED) {
		/*
		 * There's no easy way to restart the syscall since other AIO's
		 * may be already running. Just fail this IO with EINTR.
		 */
		if (unlikely(ret == -ERESTARTSYS || ret == -ERESTARTNOINTR ||
			     ret == -ERESTARTNOHAND ||
			     ret == -ERESTART_RESTARTBLOCK))
			ret = -EINTR;
		else if (IS_ERR_VALUE(ret))
			goto out_put_req;
		aio_complete(&req->common, ret, 0);
	}
	return 0;
out_put_req:
	put_reqs_available(ctx, 1);
	percpu_ref_put(&ctx->reqs);
	kiocb_free(req);
	return ret;
}

long do_io_submit(aio_context_t ctx_id, long nr,
		  struct iocb __user *__user *iocbpp, bool compat)
{
	struct kioctx *ctx;
	long ret = 0;
	int i = 0;
	struct blk_plug plug;

	if (unlikely(nr < 0))
		return -EINVAL;

	if (unlikely(nr > LONG_MAX/sizeof(*iocbpp)))
		nr = LONG_MAX/sizeof(*iocbpp);

	if (unlikely(!access_ok(VERIFY_READ, iocbpp, (nr*sizeof(*iocbpp)))))
		return -EFAULT;

	ctx = lookup_ioctx(ctx_id);
	if (unlikely(!ctx)) {
		pr_debug("EINVAL: invalid context id\n");
		return -EINVAL;
	}

	blk_start_plug(&plug);

	/*
	 * AKPM: should this return a partial result if some of the IOs were
	 * successfully submitted?
	 */
	for (i=0; i<nr; i++) {
		struct iocb __user *user_iocb;
		struct iocb tmp;

		if (unlikely(__get_user(user_iocb, iocbpp + i))) {
			ret = -EFAULT;
			break;
		}

		if (unlikely(copy_from_user(&tmp, user_iocb, sizeof(tmp)))) {
			ret = -EFAULT;
			break;
		}

		ret = io_submit_one(ctx, user_iocb, &tmp, compat);
		if (ret)
			break;
	}
	blk_finish_plug(&plug);

	percpu_ref_put(&ctx->users);
	return i ? i : ret;
}

/* sys_io_submit:
 *	Queue the nr iocbs pointed to by iocbpp for processing.  Returns
 *	the number of iocbs queued.  May return -EINVAL if the aio_context
 *	specified by ctx_id is invalid, if nr is < 0, if the iocb at
 *	*iocbpp[0] is not properly initialized, if the operation specified
 *	is invalid for the file descriptor in the iocb.  May fail with
 *	-EFAULT if any of the data structures point to invalid data.  May
 *	fail with -EBADF if the file descriptor specified in the first
 *	iocb is invalid.  May fail with -EAGAIN if insufficient resources
 *	are available to queue any iocbs.  Will return 0 if nr is 0.  Will
 *	fail with -ENOSYS if not implemented.
 */
SYSCALL_DEFINE3(io_submit, aio_context_t, ctx_id, long, nr,
		struct iocb __user * __user *, iocbpp)
{
	return do_io_submit(ctx_id, nr, iocbpp, 0);
}

/* lookup_kiocb
 *	Finds a given iocb for cancellation.
 */
static struct aio_kiocb *
lookup_kiocb(struct kioctx *ctx, struct iocb __user *iocb, u32 key)
{
	struct aio_kiocb *kiocb;

	assert_spin_locked(&ctx->ctx_lock);

	if (key != KIOCB_KEY)
		return NULL;

	/* TODO: use a hash or array, this sucks. */
	list_for_each_entry(kiocb, &ctx->active_reqs, ki_list) {
		if (kiocb->ki_user_iocb == iocb)
			return kiocb;
	}
	return NULL;
}

/* sys_io_cancel:
 *	Attempts to cancel an iocb previously passed to io_submit.  If
 *	the operation is successfully cancelled, the resulting event is
 *	copied into the memory pointed to by result without being placed
 *	into the completion queue and 0 is returned.  May fail with
 *	-EFAULT if any of the data structures pointed to are invalid.
 *	May fail with -EINVAL if aio_context specified by ctx_id is
 *	invalid.  May fail with -EAGAIN if the iocb specified was not
 *	cancelled.  Will fail with -ENOSYS if not implemented.
 */
SYSCALL_DEFINE3(io_cancel, aio_context_t, ctx_id, struct iocb __user *, iocb,
		struct io_event __user *, result)
{
	struct kioctx *ctx;
	struct aio_kiocb *kiocb;
	u32 key;
	int ret;

	ret = get_user(key, &iocb->aio_key);
	if (unlikely(ret))
		return -EFAULT;

	ctx = lookup_ioctx(ctx_id);
	if (unlikely(!ctx))
		return -EINVAL;

	spin_lock_irq(&ctx->ctx_lock);

	kiocb = lookup_kiocb(ctx, iocb, key);
	if (kiocb)
		ret = kiocb_cancel(kiocb);
	else
		ret = -EINVAL;

	spin_unlock_irq(&ctx->ctx_lock);

	if (!ret) {
		/*
		 * The result argument is no longer used - the io_event is
		 * always delivered via the ring buffer. -EINPROGRESS indicates
		 * cancellation is progress:
		 */
		ret = -EINPROGRESS;
	}

	percpu_ref_put(&ctx->users);

	return ret;
}

/* io_getevents:
 *	Attempts to read at least min_nr events and up to nr events from
 *	the completion queue for the aio_context specified by ctx_id. If
 *	it succeeds, the number of read events is returned. May fail with
 *	-EINVAL if ctx_id is invalid, if min_nr is out of range, if nr is
 *	out of range, if timeout is out of range.  May fail with -EFAULT
 *	if any of the memory specified is invalid.  May return 0 or
 *	< min_nr if the timeout specified by timeout has elapsed
 *	before sufficient events are available, where timeout == NULL
 *	specifies an infinite timeout. Note that the timeout pointed to by
 *	timeout is relative.  Will fail with -ENOSYS if not implemented.
 */
SYSCALL_DEFINE5(io_getevents, aio_context_t, ctx_id,
		long, min_nr,
		long, nr,
		struct io_event __user *, events,
		struct timespec __user *, timeout)
{
	struct kioctx *ioctx = lookup_ioctx(ctx_id);
	long ret = -EINVAL;

	if (likely(ioctx)) {
		if (likely(min_nr <= nr && min_nr >= 0))
			ret = read_events(ioctx, min_nr, nr, events, timeout);
		percpu_ref_put(&ioctx->users);
	}
	return ret;
}
