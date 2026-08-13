/*
 * nvt_ts_jdn.c -- minimal Novatek touchscreen driver for the Huawei
 *                 MediaPad T2 8.0 Pro (JDN / jordan), kernel 3.10.
 *
 * WHY THIS EXISTS RATHER THAN A PORT OF THE VENDOR DRIVER
 *   jdn's digitiser is a Novatek part on i2c bus 5. It ACKs at 0x01 and 0x62,
 *   but its chip ID cannot be read: the NT36xxx trim sequence returns all
 *   zeros because 0x3F004 is an NT36xxx-era address and this is a 2016 chip.
 *   Porting the 85 KB vendor driver would therefore probably fail at exactly
 *   that identification step, after a lot of work.
 *
 *   That turned out not to matter. Watching the bus while the screen was
 *   touched showed the controller ALREADY REPORTING in the standard Novatek
 *   point format -- its firmware is resident and running. So the chip does not
 *   need identifying, initialising, or flashing. It needs reading.
 *
 * THE PACKET FORMAT, decoded from live captures on this exact hardware.
 *   Read from i2c address 0x01, offset 0x00. Six bytes per finger, up to five
 *   fingers, no header byte. Per finger at pos = i * 6:
 *
 *     id     = buf[pos] >> 3            1-based; 0 or > max means empty slot
 *     status = buf[pos] & 0x07          1 or 2 means finger down
 *     x      = (buf[pos+1] << 4) | (buf[pos+3] >> 4)
 *     y      = (buf[pos+2] << 4) | (buf[pos+3] & 0x0F)
 *
 *   All-FF is the idle state and decodes to nothing (id 31 > max), so it needs
 *   no special case. Coordinates come out already in display pixels: captured
 *   samples span x 187..1069 and y 152..1708 against a 1200x1920 panel.
 *
 * POLLING BY DEFAULT, ON PURPOSE
 *   Polling is what was proven to work on this hardware. The DTS declares
 *   `interrupts = <0xd 0x2008>` and that trigger encoding is not something I
 *   can verify without another flash cycle, so the interrupt path is opt-in
 *   via the `use_irq` parameter. In a recovery environment a 66 Hz poll costs
 *   nothing that matters.
 *
 * POWER
 *   Two GPIO-controlled rails, both required -- verified live, because raising
 *   only one leaves the chip silent:
 *     novatek,vdd-gpio    pin 112
 *     novatek,avdd-gpio   pin 117
 *   plus novatek,rst-gpio pin 12. The driver drives all three itself, which is
 *   why no regulator-fixed DTS node is needed.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define NVT_NAME		"nvt_ts_jdn"

#define NVT_MAX_FINGERS		5
#define NVT_BYTES_PER_FINGER	6
#define NVT_READ_LEN		(NVT_MAX_FINGERS * NVT_BYTES_PER_FINGER)

/* Defaults match the INX_NT51021_8_1200P_VIDEO panel this device ships. */
static int max_x = 1200;
static int max_y = 1920;
static int poll_ms = 15;	/* ~66 Hz */
static bool use_irq;

module_param(max_x, int, 0644);
module_param(max_y, int, 0644);
module_param(poll_ms, int, 0644);
module_param(use_irq, bool, 0644);
MODULE_PARM_DESC(poll_ms, "poll interval in ms (ignored when use_irq=1)");
MODULE_PARM_DESC(use_irq, "use the DTS interrupt instead of polling");

struct nvt_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work poll_work;
	int vdd_gpio;
	int avdd_gpio;
	int rst_gpio;
	int irq_gpio;
	unsigned long slots_down;	/* bitmask of slots currently reported */
	u8 buf[NVT_READ_LEN];
};

/*
 * A combined write-offset-then-read in ONE transfer. The repeated START this
 * produces is required: an offset write followed by a separate read
 * transaction returns the byte just written instead of the data, which is why
 * i2cget could never see touch packets from the shell.
 */
static int nvt_read_block(struct nvt_ts *ts, u8 offset, u8 *buf, int len)
{
	struct i2c_msg msgs[2];
	u8 out = offset;
	int ret;

	msgs[0].addr = ts->client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &out;

	msgs[1].addr = ts->client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	ret = i2c_transfer(ts->client->adapter, msgs, 2);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;

	return 0;
}

static void nvt_report(struct nvt_ts *ts)
{
	unsigned long seen = 0;
	int i, id, status, x, y;
	u8 *p;

	if (nvt_read_block(ts, 0x00, ts->buf, NVT_READ_LEN))
		return;

	for (i = 0; i < NVT_MAX_FINGERS; i++) {
		p = ts->buf + i * NVT_BYTES_PER_FINGER;

		id = p[0] >> 3;
		if (id == 0 || id > NVT_MAX_FINGERS)
			continue;	/* covers the all-FF idle pattern too */

		status = p[0] & 0x07;
		if (status != 1 && status != 2)
			continue;

		x = (p[1] << 4) | (p[3] >> 4);
		y = (p[2] << 4) | (p[3] & 0x0F);

		if (x >= max_x || y >= max_y)
			continue;	/* refuse to report off-panel garbage */

		/* Slots are 0-based, the wire protocol's ids are 1-based. */
		input_mt_slot(ts->input, id - 1);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_abs(ts->input, ABS_MT_POSITION_X, x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
		seen |= 1UL << (id - 1);
	}

	/*
	 * Release every slot that was down last time and is not in this frame.
	 * Done explicitly rather than via input_mt_sync_frame(), which is not
	 * available on 3.10.
	 */
	for (i = 0; i < NVT_MAX_FINGERS; i++) {
		if ((seen & (1UL << i)) || !(ts->slots_down & (1UL << i)))
			continue;
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	ts->slots_down = seen;

	/* Keeps single-touch consumers working alongside the MT protocol. */
	input_mt_report_pointer_emulation(ts->input, false);
	input_sync(ts->input);
}

static void nvt_poll_work(struct work_struct *work)
{
	struct nvt_ts *ts = container_of(to_delayed_work(work),
					 struct nvt_ts, poll_work);

	nvt_report(ts);
	schedule_delayed_work(&ts->poll_work, msecs_to_jiffies(poll_ms));
}

static irqreturn_t nvt_irq_thread(int irq, void *dev_id)
{
	nvt_report((struct nvt_ts *)dev_id);
	return IRQ_HANDLED;
}

static int nvt_power_on(struct nvt_ts *ts)
{
	int ret;

	/* Both rails are mandatory: with only vdd raised the chip stays mute. */
	if (gpio_is_valid(ts->vdd_gpio)) {
		ret = devm_gpio_request_one(&ts->client->dev, ts->vdd_gpio,
					    GPIOF_OUT_INIT_HIGH, "nvt-vdd");
		if (ret)
			return ret;
	}
	if (gpio_is_valid(ts->avdd_gpio)) {
		ret = devm_gpio_request_one(&ts->client->dev, ts->avdd_gpio,
					    GPIOF_OUT_INIT_HIGH, "nvt-avdd");
		if (ret)
			return ret;
	}
	msleep(20);

	if (gpio_is_valid(ts->rst_gpio)) {
		ret = devm_gpio_request_one(&ts->client->dev, ts->rst_gpio,
					    GPIOF_OUT_INIT_HIGH, "nvt-rst");
		if (ret)
			return ret;
		/* Pulse low then high: verified to bring the chip out of its
		 * uninitialised state, where every register read returns 0xAA. */
		gpio_set_value(ts->rst_gpio, 0);
		msleep(10);
		gpio_set_value(ts->rst_gpio, 1);
	}

	/* The firmware needs time to come up before it answers sensibly. */
	msleep(150);
	return 0;
}

static int nvt_ts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device_node *np = client->dev.of_node;
	struct nvt_ts *ts;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "adapter lacks plain I2C support\n");
		return -ENODEV;
	}

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->vdd_gpio  = np ? of_get_named_gpio(np, "novatek,vdd-gpio", 0)  : -1;
	ts->avdd_gpio = np ? of_get_named_gpio(np, "novatek,avdd-gpio", 0) : -1;
	ts->rst_gpio  = np ? of_get_named_gpio(np, "novatek,rst-gpio", 0)  : -1;
	ts->irq_gpio  = np ? of_get_named_gpio(np, "novatek,irq-gpio", 0)  : -1;

	dev_info(&client->dev,
		 "addr 0x%02x vdd %d avdd %d rst %d irq %d\n",
		 client->addr, ts->vdd_gpio, ts->avdd_gpio,
		 ts->rst_gpio, ts->irq_gpio);

	ret = nvt_power_on(ts);
	if (ret) {
		dev_err(&client->dev, "power-on failed: %d\n", ret);
		return ret;
	}

	/* One read decides whether anything is actually out there. All-FF is a
	 * perfectly good answer -- it is the idle pattern -- so only a transfer
	 * error counts as absent. */
	ret = nvt_read_block(ts, 0x00, ts->buf, NVT_READ_LEN);
	if (ret) {
		dev_err(&client->dev, "no response at 0x%02x: %d\n",
			client->addr, ret);
		return -ENODEV;
	}
	dev_info(&client->dev, "first read: %02X %02X %02X %02X ...\n",
		 ts->buf[0], ts->buf[1], ts->buf[2], ts->buf[3]);

	ts->input = devm_input_allocate_device(&client->dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "novatek-ts";
	ts->input->id.bustype = BUS_I2C;
	ts->input->dev.parent = &client->dev;

	__set_bit(EV_ABS, ts->input->evbit);
	__set_bit(EV_KEY, ts->input->evbit);
	__set_bit(BTN_TOUCH, ts->input->keybit);
	__set_bit(INPUT_PROP_DIRECT, ts->input->propbit);

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, max_x - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, max_y - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_X, 0, max_x - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_Y, 0, max_y - 1, 0, 0);

	ret = input_mt_init_slots(ts->input, NVT_MAX_FINGERS, 0);
	if (ret)
		return ret;

	ret = input_register_device(ts->input);
	if (ret)
		return ret;

	if (use_irq && gpio_is_valid(ts->irq_gpio)) {
		ret = devm_gpio_request_one(&client->dev, ts->irq_gpio,
					    GPIOF_IN, "nvt-irq");
		if (ret)
			return ret;
		client->irq = gpio_to_irq(ts->irq_gpio);
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, nvt_irq_thread,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						NVT_NAME, ts);
		if (ret) {
			dev_err(&client->dev, "irq %d failed: %d\n",
				client->irq, ret);
			return ret;
		}
		dev_info(&client->dev, "using irq %d\n", client->irq);
	} else {
		INIT_DELAYED_WORK(&ts->poll_work, nvt_poll_work);
		schedule_delayed_work(&ts->poll_work,
				      msecs_to_jiffies(poll_ms));
		dev_info(&client->dev, "polling every %d ms\n", poll_ms);
	}

	dev_info(&client->dev, "ready, %dx%d\n", max_x, max_y);
	return 0;
}

static int nvt_ts_remove(struct i2c_client *client)
{
	struct nvt_ts *ts = i2c_get_clientdata(client);

	if (!use_irq)
		cancel_delayed_work_sync(&ts->poll_work);

	return 0;
}

static const struct i2c_device_id nvt_ts_id[] = {
	{ NVT_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nvt_ts_id);

#ifdef CONFIG_OF
static const struct of_device_id nvt_ts_of_match[] = {
	{ .compatible = "novatek,NVT-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt_ts_of_match);
#endif

static struct i2c_driver nvt_ts_driver = {
	.probe  = nvt_ts_probe,
	.remove = nvt_ts_remove,
	.id_table = nvt_ts_id,
	.driver = {
		.name = NVT_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(nvt_ts_of_match),
	},
};

module_i2c_driver(nvt_ts_driver);

MODULE_DESCRIPTION("Minimal Novatek touchscreen driver for Huawei JDN");
MODULE_LICENSE("GPL v2");
