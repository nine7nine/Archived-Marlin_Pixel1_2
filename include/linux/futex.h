#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

#include <linux/futex_types.h>
#include <uapi/linux/futex.h>

struct inode;
struct mm_struct;
struct task_struct;
union ktime;

long do_futex(u32 __user *uaddr, int op, u32 val, union ktime *timeout,
	      u32 __user *uaddr2, u32 val2, u32 val3);

extern int
handle_futex_death(u32 __user *uaddr, struct task_struct *curr, int pi);

/*
 * Futexes are matched on equal values of this key.
 * The key type depends on whether it's a shared or private mapping.
 * Don't rearrange members without looking at hash_futex().
 *
 * offset is aligned to a multiple of sizeof(u32) (== 4) by definition.
 * We use the two low order bits of offset to tell what is the kind of key :
 *  00 : Private process futex (PTHREAD_PROCESS_PRIVATE) using process private
 *	 hash (no reference on an inode or mm)
 *  01 : Shared futex (PTHREAD_PROCESS_SHARED)
 *	mapped on a file (reference on the underlying inode)
 *  10 : Shared futex (PTHREAD_PROCESS_SHARED)
 *       (but private mapping on an mm, and reference taken on it)
 *  11 : Private process futex (PTHREAD_PROCESS_PRIVATE) using global hash
 *	 (no reference on an inode or mm)
*/

#define FUT_OFF_INODE		0x01 /* Key has a reference on inode */
#define FUT_OFF_MMSHARED	0x02 /* Key has a reference on mm */
#define FUT_OFF_PRIVATE		0x03 /* Key has no ref on inode/mm */

union futex_key {
	struct {
		unsigned long pgoff;
		struct inode *inode;
		int offset;
	} shared;
	struct {
		unsigned long address;
		struct mm_struct *mm;
		int offset;
	} private;
	struct {
		unsigned long word;
		void *ptr;
		int offset;
	} both;
};

#define FUTEX_KEY_INIT (union futex_key) { .both = { .ptr = NULL } }

#ifdef CONFIG_FUTEX
extern void exit_robust_list(struct task_struct *curr);
extern void exit_pi_state_list(struct task_struct *curr);
#ifdef CONFIG_HAVE_FUTEX_CMPXCHG
#define futex_cmpxchg_enabled 1
#else
extern int futex_cmpxchg_enabled;
#endif

#else
static inline void exit_robust_list(struct task_struct *curr) { }
static inline void exit_pi_state_list(struct task_struct *curr) { }
#endif

#ifdef CONFIG_FUTEX_PRIVATE_HASH
/* Process private hash data for futexes */

extern unsigned int futex_default_hash_bits;
extern unsigned int futex_max_hash_bits;
extern unsigned int futex_sysmax_hash_bits;

extern void futex_mm_hash_exit(struct mm_struct *mm);

static inline void futex_mm_hash_init(struct mm_struct *mm)
{
	raw_spin_lock_init(&mm->futex_hash.lock);
	mm->futex_hash.hash = NULL;
}

#else

static inline void futex_mm_hash_init(struct mm_struct *mm) { }
static inline void futex_mm_hash_exit(struct mm_struct *mm) { }
#endif

#endif
