/*
 * TI LM36923 backlight -- interface to the mdss panel driver.
 *
 * Installed as include/linux/lm36923_bl.h.
 *
 * The stub below is what makes the mdss side safe to patch unconditionally:
 * with CONFIG_LM36923_BL off, mdss_dsi_panel_bl_ctrl()'s call compiles away to
 * nothing rather than failing to link. That keeps the board buildable with the
 * driver disabled, which matters for bisecting a boot failure -- flip one
 * defconfig line instead of reverting a source patch.
 */

#ifndef __LINUX_LM36923_BL_H
#define __LINUX_LM36923_BL_H

#include <linux/types.h>

#ifdef CONFIG_LM36923_BL

/*
 * Set panel brightness. bl_level is the value mdss already clamped to
 * [bl_min .. bl_max] = [4 .. 255] for this panel, and maps 1:1 onto the
 * chip's brightness MSB register. Level 0 is handled without touching i2c:
 * the chip is unpowered by then.
 *
 * Safe to call before the driver has bound, or if the chip never answered --
 * it returns silently. May sleep; callers must be in process context (the
 * mdss backlight path holds mfd->bl_lock, a mutex, so this holds).
 */
void lm36923_set_backlight(u32 bl_level);

#else /* !CONFIG_LM36923_BL */

static inline void lm36923_set_backlight(u32 bl_level) { }

#endif /* CONFIG_LM36923_BL */

#endif /* __LINUX_LM36923_BL_H */
