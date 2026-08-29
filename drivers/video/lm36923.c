/*
 * TI LM36923 backlight LED driver -- Huawei MediaPad T2 8.0 Pro (hwjdn).
 *
 * WHY THIS EXISTS
 * ---------------
 * jdn's INX NT51021 panel is declared "bl_ctrl_dcs" in the board DTS, so stock
 * CAF mdss sends DCS 0x51 on every brightness change. The panel ignores it, and
 * it always did: Huawei's own kernel replaced mdss_dsi_panel_bklt_dcs() with a
 * "_pad" variant that drove THIS chip over i2c instead. The DCS control type in
 * the DTS was therefore correct for their fork and wrong for ours. Their
 * backlight layer is what we did not have; this file is it.
 *
 * Everything below was measured on the tablet, not taken from a datasheet:
 *
 *   - The register map came out of Huawei's stock kernel binary via its own
 *     format strings ("Brightness Register MSB's(0x19) = 0x%x" and friends).
 *   - 0x11 = 0x55 and 0x12 = 0xa3 are the values Huawei's bootloader leaves in
 *     the chip; aboot has explicit error strings for both writes ("set lm36923
 *     backlight control mode failed" / "set lm36923 pwm control mode failed").
 *   - The msb/lsb split was confirmed against Huawei's own log recovered from
 *     this unit's /log partition: "level = 100, bl_msb = 12, bl_lsb = 4", and
 *     (12 << 3) | 4 == 100 exactly.
 *   - The chip powers down with the panel and NACKs every transfer while
 *     blanked, which is why it is re-initialised on each unblank rather than
 *     once at probe -- the same thing Huawei's driver does.
 *
 * It is deliberately NOT a backlight class device. CONFIG_BACKLIGHT_LCD_SUPPORT
 * is off in jdn-64_defconfig, and Android already reaches the panel through
 * /sys/class/leds/lcd-backlight -> mdss -> panel_data.set_backlight. A second
 * unused interface would only add config we do not have.
 *
 * Sleeping here is safe: mdss_fb_set_bl_brightness() takes mutex_lock(&mfd->
 * bl_lock) around this path (mdss_fb.c:263), and a mutex cannot be held in
 * atomic context. That lock IS held across our i2c write, so this code must
 * stay short -- never retry in a loop, never sleep beyond a transfer.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/lm36923_bl.h>

#define LM36923_REG_REVISION		0x00
#define LM36923_REG_SW_RESET		0x01
#define LM36923_REG_ENABLE		0x10
#define LM36923_REG_BL_CONTROL		0x11
#define LM36923_REG_PWM_CONTROL		0x12
#define LM36923_REG_BOOST_CTRL1		0x13
#define LM36923_REG_BRT_LSB		0x18
#define LM36923_REG_BRT_MSB		0x19
#define LM36923_REG_FAULT_CTRL		0x1e
#define LM36923_REG_FAULT_FLAGS		0x1f

/*
 * Read off this tablet with the bootloader's configuration still in place. The
 * chip comes back from a panel blank at its power-on defaults (0x11 = 0x65,
 * 0x12 = 0x73) and brightness still works in that state, so these are about
 * matching the tuning the hardware is known-good with, not about function.
 */
#define LM36923_BL_CONTROL_VAL		0x55
#define LM36923_PWM_CONTROL_VAL		0xa3

/* DT fallbacks -- jdn's node supplies all three, these are for safety only. */
#define LM36923_DEFAULT_CTRL_MODE	0x3
#define LM36923_DEFAULT_ENABLE_REG	0xf
#define LM36923_DEFAULT_PROTECT		0x1

struct lm36923_data {
	struct i2c_client *client;
	struct mutex lock;
	u32 ctrl_mode;
	u32 enable_reg;
	u32 protect_disable;
	bool configured;	/* false whenever the chip may have lost power */
};

/*
 * Single instance. mdss calls in through the exported symbol rather than
 * holding a reference, because panel_data.set_backlight() has no private-data
 * argument to hang one off.
 */
static struct lm36923_data *lm36923_dev;

static int lm36923_write(struct lm36923_data *d, u8 reg, u8 val)
{
	int rc;

	rc = i2c_smbus_write_byte_data(d->client, reg, val);
	if (rc < 0)
		dev_err(&d->client->dev,
			"i2c write reg 0x%02x = 0x%02x failed: %d\n",
			reg, val, rc);
	return rc;
}

/*
 * Restore the configuration the bootloader proved good. Must run after every
 * panel unblank: the chip's rail follows hw_bl_gpio, so a blank resets it.
 */
static int lm36923_chip_init(struct lm36923_data *d)
{
	int rc;

	rc = lm36923_write(d, LM36923_REG_BL_CONTROL, LM36923_BL_CONTROL_VAL);
	if (rc < 0)
		return rc;

	rc = lm36923_write(d, LM36923_REG_PWM_CONTROL, LM36923_PWM_CONTROL_VAL);
	if (rc < 0)
		return rc;

	rc = lm36923_write(d, LM36923_REG_ENABLE, (u8)(d->enable_reg & 0xff));
	if (rc < 0)
		return rc;

	return 0;
}

/*
 * Called from mdss_dsi_panel_bl_ctrl() on every brightness change, with
 * bl_level already clamped by mdss to [bl_min .. bl_max] = [4 .. 255].
 */
void lm36923_set_backlight(u32 bl_level)
{
	struct lm36923_data *d = lm36923_dev;

	if (!d)
		return;

	mutex_lock(&d->lock);

	if (bl_level == 0) {
		/*
		 * mdss has already driven hw_bl_gpio low, cutting this chip's
		 * power -- a read at 0x36 with the panel blanked returns
		 * ENOTCONN. Issuing i2c here would only fill the log with
		 * NACKs. Just remember that the chip needs reconfiguring
		 * before the next non-zero level.
		 */
		d->configured = false;
		goto out;
	}

	if (!d->configured) {
		/*
		 * mdss raised hw_bl_gpio immediately before calling us, so this
		 * chip's rail has only just come up. Huawei's own log leaves
		 * ~250us between the rail rising and their first i2c write; a
		 * NACK here would leave the panel at the wrong level for the
		 * whole screen-on. PRECAUTION, NOT A MEASURED SETTLING TIME --
		 * 2ms is invisible on an unblank and gives generous margin.
		 */
		usleep_range(2000, 3000);

		if (lm36923_chip_init(d) < 0)
			goto out;
		d->configured = true;
	}

	/*
	 * Brightness is 11 bits: LSB 0x18 holds [2:0], MSB 0x19 holds [10:3].
	 * Android's bl_level is 0-255 and 0x19 is 8 bits wide, so the slider
	 * maps onto the chip 1:1 with no arithmetic, covering 0-2040 of 2047.
	 *
	 * 0x18 is deliberately left alone. Writing it produced no visible
	 * change even at the dimmest usable level, where its share of the
	 * total is at its largest -- so it buys nothing and is one less
	 * register to keep in sync.
	 */
	/*
	 * Clamp rather than mask: masking would turn 256 into 0, i.e. a level
	 * above full would blank the screen. mdss should never send more than
	 * bl_max, but a bug here must fail bright, not dark.
	 */
	if (bl_level > 0xff)
		bl_level = 0xff;

	lm36923_write(d, LM36923_REG_BRT_MSB, (u8)bl_level);

out:
	mutex_unlock(&d->lock);
}
EXPORT_SYMBOL(lm36923_set_backlight);

/*
 * Huawei's property names, read from the same node their own driver reads.
 * Missing properties are NOT fatal: a strict parser is what kept the AP3426
 * from binding on this board for a week.
 *
 * HONEST LIMITATION: only enable-reg is applied to hardware (register 0x10).
 * ctrl-mode and protect-disable are parsed and logged for parity with Huawei's
 * driver, but WE DO NOT KNOW which registers they target -- their stock driver
 * logs the values without naming a destination. Whatever ctrl-mode selects is
 * in practice already carried by the 0x11 value the bootloader leaves behind.
 * Do not invent a mapping for them; find it or leave them recorded only.
 */
static void lm36923_parse_dt(struct lm36923_data *d)
{
	struct device_node *np = d->client->dev.of_node;
	u32 val;

	d->ctrl_mode = LM36923_DEFAULT_CTRL_MODE;
	d->enable_reg = LM36923_DEFAULT_ENABLE_REG;
	d->protect_disable = LM36923_DEFAULT_PROTECT;

	if (!np) {
		dev_warn(&d->client->dev,
			 "no device node, using built-in defaults\n");
		return;
	}

	if (!of_property_read_u32(np, "lm36923-ctrl-mode", &val))
		d->ctrl_mode = val;
	if (!of_property_read_u32(np, "enable-reg", &val))
		d->enable_reg = val;
	if (!of_property_read_u32(np, "protect-disable", &val))
		d->protect_disable = val;
}

static int lm36923_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct lm36923_data *d;
	int rc;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "i2c functionality check failed\n");
		return -ENODEV;
	}

	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->client = client;
	mutex_init(&d->lock);
	i2c_set_clientdata(client, d);

	lm36923_parse_dt(d);

	/*
	 * A failed init here is NOT a probe failure. The chip is only powered
	 * while the panel is lit, and probe order relative to mdss is not
	 * guaranteed; refusing to bind would make that accident permanent.
	 * Bind anyway and let the first non-zero brightness retry.
	 */
	rc = lm36923_chip_init(d);
	if (rc < 0) {
		dev_warn(&client->dev,
			 "init failed (%d) -- will retry on first backlight write\n",
			 rc);
		d->configured = false;
	} else {
		d->configured = true;
	}

	lm36923_dev = d;

	dev_info(&client->dev,
		 "lm36923 backlight ready at 0x%02x: ctrl_mode=%u enable_reg=0x%x protect=%u configured=%d\n",
		 client->addr, d->ctrl_mode, d->enable_reg,
		 d->protect_disable, d->configured);

	return 0;
}

static int lm36923_remove(struct i2c_client *client)
{
	lm36923_dev = NULL;
	return 0;
}

static struct of_device_id lm36923_match_table[] = {
	{ .compatible = "ti,lm36923", },
	{ },
};

/*
 * The i2c core derives a DT client's name from the part of the compatible
 * after the comma, so this node's name is "lm36923". Carrying the id_table
 * as well costs nothing and covers a non-DT instantiation.
 */
static const struct i2c_device_id lm36923_id[] = {
	{ "lm36923", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, lm36923_id);

static struct i2c_driver lm36923_driver = {
	.driver = {
		.name = "lm36923",
		.owner = THIS_MODULE,
		.of_match_table = lm36923_match_table,
	},
	.probe = lm36923_probe,
	.remove = lm36923_remove,
	.id_table = lm36923_id,
};

module_i2c_driver(lm36923_driver);

MODULE_DESCRIPTION("TI LM36923 backlight driver for Huawei MediaPad T2 8.0 Pro");
MODULE_LICENSE("GPL v2");
