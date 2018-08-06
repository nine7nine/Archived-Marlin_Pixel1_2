/*
 * Copyright (C) 2018, Jordan Johnston <johnstonljordan@gmail.com>
 *
 * Boostbox is based on "cpu_input_boost" driver, authored by;
 *
 * Copyright (C) 2018, Sultan Alsawaf <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef _BOOSTBOX_H_
#define _BOOSTBOX_H_

#ifdef CONFIG_BOOSTBOX
void boostbox_gfx_kick(void);
void boostbox_idle_boost(void);
void boostbox_kick_max(unsigned int duration_ms);
#else
static inline void boostbox_gfx_kick(void)
{
}
static inline void boostbox_idle_boost(void)
{
}
static inline void boostbox_kick_max(unsigned int duration_ms)
{
}
#endif

#endif /* _boostbox_H_ */

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
void dsb_audio_boost_kick(void);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */
