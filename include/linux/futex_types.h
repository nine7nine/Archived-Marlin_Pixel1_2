#ifndef _LINUX_FUTEX_TYPES_H
#define _LINUX_FUTEX_TYPES_H

struct futex_hash_bucket;

struct futex_hash {
	struct raw_spinlock		lock;
	unsigned int			hash_bits;
	struct futex_hash_bucket	*hash;
};

#endif
