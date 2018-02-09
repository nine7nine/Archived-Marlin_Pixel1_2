/*
 * Modulo based hash - Global helper functions
 *
 * (C) 2016 Linutronix GmbH, Thomas Gleixner
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public Licence version 2 as published by
 * the Free Software Foundation;
 */

#include <linux/hash.h>
#include <linux/errno,h>
#include <linux/bug.h>
#include <linux/kernel.h>

#define hash_pmul(prime)	((unsigned int)((1ULL << 32) / prime))

static const struct hash_modulo hash_modulo[] = {
	{ .prime =    3, .pmul = hash_pmul(3),    .mask = 0x0003 },
	{ .prime =    7, .pmul = hash_pmul(7),    .mask = 0x0007 },
	{ .prime =   13, .pmul = hash_pmul(13),   .mask = 0x000f },
	{ .prime =   31, .pmul = hash_pmul(31),   .mask = 0x001f },
	{ .prime =   61, .pmul = hash_pmul(61),   .mask = 0x003f },
	{ .prime =  127, .pmul = hash_pmul(127),  .mask = 0x007f },
	{ .prime =  251, .pmul = hash_pmul(251),  .mask = 0x00ff },
	{ .prime =  509, .pmul = hash_pmul(509),  .mask = 0x01ff },
	{ .prime = 1021, .pmul = hash_pmul(1021), .mask = 0x03ff },
	{ .prime = 2039, .pmul = hash_pmul(2039), .mask = 0x07ff },
	{ .prime = 4093, .pmul = hash_pmul(4093), .mask = 0x0fff },
};

/**
 * hash_modulo_params - FIXME
 */
int hash_modulo_params(unsigned int hash_bits, struct hash_modulo *hm)
{
	hash_bits -= 2;

	if (hash_bits >= ARRAY_SIZE(hash_modulo))
		return -EINVAL;

	*hm = hash_modulo[hash_bits];
	return 0;
}
