/*
 * (C) Copyright 2009-2010
 * Nokia Siemens Networks, michael.lawnick.ext@nsn.com
 *
 * Portions Copyright (C) 2010 - 2016 Cavium, Inc.
 *
 * This is a driver for the i2c adapter in Cavium Networks' OCTEON processors.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/of.h>

#include <asm/octeon/octeon.h>

#define DRV_NAME "i2c-octeon"

/* Register offsets */
#define SW_TWSI			0x00
#define TWSI_INT		0x10
#define SW_TWSI_EXT		0x18

/* Controller command patterns */
#define SW_TWSI_V		BIT_ULL(63)
#define SW_TWSI_EIA		BIT_ULL(61)
#define SW_TWSI_OP_TWSI_CLK	BIT_ULL(59)
#define SW_TWSI_OP_7		(0ULL << 57)
#define SW_TWSI_OP_7_IA		BIT_ULL(57)
#define SW_TWSI_OP_10		(2ULL << 57)
#define SW_TWSI_OP_10_IA	(3ULL << 57)
#define SW_TWSI_R		BIT_ULL(56)
#define SW_TWSI_SOVR		BIT_ULL(55)
#define SW_TWSI_SIZE_SHIFT	52
#define SW_TWSI_A_SHIFT		40
#define SW_TWSI_IA_SHIFT	32
#define SW_TWSI_EOP_TWSI_DATA	0x0C00000100000000ULL
#define SW_TWSI_EOP_TWSI_CTL	0x0C00000200000000ULL
#define SW_TWSI_EOP_TWSI_CLKCTL	0x0C00000300000000ULL
#define SW_TWSI_EOP_TWSI_STAT	0x0C00000300000000ULL
#define SW_TWSI_EOP_TWSI_RST	0x0C00000700000000ULL

/* Controller command and status bits */
#define TWSI_CTL_CE		0x80	/* High level controller enable */
#define TWSI_CTL_ENAB		0x40	/* Bus enable */
#define TWSI_CTL_STA		0x20	/* Master-mode start, HW clears when done */
#define TWSI_CTL_STP		0x10	/* Master-mode stop, HW clears when done */
#define TWSI_CTL_IFLG		0x08	/* HW event, SW writes 0 to ACK */
#define TWSI_CTL_AAK		0x04	/* Assert ACK */

/* Some status values */
#define STAT_ERROR		0x00
#define STAT_START		0x08
#define STAT_RSTART		0x10
#define STAT_TXADDR_ACK		0x18
#define STAT_TXADDR_NAK		0x20
#define STAT_TXDATA_ACK		0x28
#define STAT_TXDATA_NAK		0x30
#define STAT_LOST_ARB_38	0x38
#define STAT_RXADDR_ACK		0x40
#define STAT_RXADDR_NAK		0x48
#define STAT_RXDATA_ACK		0x50
#define STAT_RXDATA_NAK		0x58
#define STAT_SLAVE_60		0x60
#define STAT_LOST_ARB_68	0x68
#define STAT_SLAVE_70		0x70
#define STAT_LOST_ARB_78	0x78
#define STAT_SLAVE_80		0x80
#define STAT_SLAVE_88		0x88
#define STAT_GENDATA_ACK	0x90
#define STAT_GENDATA_NAK	0x98
#define STAT_SLAVE_A0		0xA0
#define STAT_SLAVE_A8		0xA8
#define STAT_LOST_ARB_B0	0xB0
#define STAT_SLAVE_LOST		0xB8
#define STAT_SLAVE_NAK		0xC0
#define STAT_SLAVE_ACK		0xC8
#define STAT_AD2W_ACK		0xD0
#define STAT_AD2W_NAK		0xD8
#define STAT_IDLE		0xF8

/* TWSI_INT values */
#define TWSI_INT_ST_INT		BIT_ULL(0)
#define TWSI_INT_TS_INT		BIT_ULL(1)
#define TWSI_INT_CORE_INT	BIT_ULL(2)
#define TWSI_INT_ST_EN		BIT_ULL(4)
#define TWSI_INT_TS_EN		BIT_ULL(5)
#define TWSI_INT_CORE_EN	BIT_ULL(6)
#define TWSI_INT_SDA_OVR	BIT_ULL(8)
#define TWSI_INT_SCL_OVR	BIT_ULL(9)
#define TWSI_INT_SDA		BIT_ULL(10)
#define TWSI_INT_SCL		BIT_ULL(11)

struct octeon_i2c {
	wait_queue_head_t queue;
	struct i2c_adapter adap;
	int irq;
	u32 twsi_freq;
	int sys_freq;
	void __iomem *twsi_base;
	struct device *dev;
	bool hlc_enabled;
};

static int reset_how;

static int timeout = 2;
module_param(timeout, int, 0444);
MODULE_PARM_DESC(timeout, "Low-level device timeout (ms)");

/**
 * octeon_i2c_write_sw - write an I2C core register
 * @i2c: The struct octeon_i2c
 * @eop_reg: Register selector
 * @data: Value to be written
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static void octeon_i2c_write_sw(struct octeon_i2c *i2c, u64 eop_reg, u32 data)
{
	u64 tmp;

	__raw_writeq(SW_TWSI_V | eop_reg | data, i2c->twsi_base + SW_TWSI);
	do {
		tmp = __raw_readq(i2c->twsi_base + SW_TWSI);
	} while ((tmp & SW_TWSI_V) != 0);
}

/**
 * octeon_i2c_read_sw64 - read an I2C core register
 * @i2c: The struct octeon_i2c
 * @eop_reg: Register selector
 *
 * Returns the data.
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static u64 octeon_i2c_read_sw64(struct octeon_i2c *i2c, u64 eop_reg)
{
	u64 tmp;

	__raw_writeq(SW_TWSI_V | eop_reg | SW_TWSI_R, i2c->twsi_base + SW_TWSI);
	do {
		tmp = __raw_readq(i2c->twsi_base + SW_TWSI);
	} while ((tmp & SW_TWSI_V) != 0);

	return tmp;
}

/**
 * octeon_i2c_read_sw - read lower bits of an I2C core register
 * @i2c: The struct octeon_i2c
 * @eop_reg: Register selector
 *
 * Returns the data.
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static u8 octeon_i2c_read_sw(struct octeon_i2c *i2c, u64 eop_reg)
{
	return octeon_i2c_read_sw64(i2c, eop_reg) & 0xFF;
}

/**
 * octeon_i2c_write_int - write the TWSI_INT register
 * @i2c: The struct octeon_i2c
 * @data: Value to be written
 */
static void octeon_i2c_write_int(struct octeon_i2c *i2c, u64 data)
{
	__raw_writeq(data, i2c->twsi_base + TWSI_INT);
	__raw_readq(i2c->twsi_base + TWSI_INT);
}

/**
 * octeon_i2c_int_enable - enable the CORE interrupt
 * @i2c: The struct octeon_i2c
 *
 * The interrupt will be asserted when there is non-STAT_IDLE state in
 * the SW_TWSI_EOP_TWSI_STAT register.
 */
static void octeon_i2c_int_enable(struct octeon_i2c *i2c)
{
	octeon_i2c_write_int(i2c, TWSI_INT_CORE_EN);
}

/* disable the CORE interrupt */
static void octeon_i2c_int_disable(struct octeon_i2c *i2c)
{
	/* clear TS/ST/IFLG events */
	octeon_i2c_write_int(i2c, 0);
}

static void octeon_i2c_disable_hlc(struct octeon_i2c *i2c)
{
	if (!i2c->hlc_enabled)
		return;

	i2c->hlc_enabled = false;
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);
}

/**
 * octeon_i2c_unblock - unblock the bus
 * @i2c: The struct octeon_i2c
 *
 * If there was a reset while a device was driving 0 to bus, bus is blocked.
 * We toggle it free manually by some clock cycles and send a stop.
 */
static void octeon_i2c_unblock(struct octeon_i2c *i2c)
{
	int state, i;

	octeon_i2c_disable_hlc(i2c);
	dev_dbg(i2c->dev, "%s\n", __func__);

	/* cycle 8+1 clocks with SDA high */
	for (i = 0; i < 9; i++) {
		octeon_i2c_write_int(i2c, 0);
		udelay(5);
		state = __raw_readq(i2c->twsi_base + TWSI_INT);
		if (state & (TWSI_INT_SDA | TWSI_INT_SCL))
			break;
		octeon_i2c_write_int(i2c, TWSI_INT_SCL_OVR);
		udelay(5);
	}
	/* hand-crank a STOP */
	octeon_i2c_write_int(i2c, TWSI_INT_SDA_OVR | TWSI_INT_SCL_OVR);
	udelay(5);
	octeon_i2c_write_int(i2c, TWSI_INT_SDA_OVR);
	udelay(5);
	octeon_i2c_write_int(i2c, 0);
}

/* interrupt service routine */
static irqreturn_t octeon_i2c_isr(int irq, void *dev_id)
{
	struct octeon_i2c *i2c = dev_id;

	octeon_i2c_int_disable(i2c);
	wake_up(&i2c->queue);

	return IRQ_HANDLED;
}

static u64 octeon_i2c_read_ctl(struct octeon_i2c *i2c)
{
	return octeon_i2c_read_sw64(i2c, SW_TWSI_EOP_TWSI_CTL);
}

static int octeon_i2c_test_iflg(struct octeon_i2c *i2c)
{
	return (octeon_i2c_read_ctl(i2c) & TWSI_CTL_IFLG) != 0;
}

/**
 * octeon_i2c_wait - wait for the IFLG to be set
 * @i2c: The struct octeon_i2c
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_wait(struct octeon_i2c *i2c)
{
	long time_left;

	octeon_i2c_int_enable(i2c);
	time_left = wait_event_timeout(i2c->queue, octeon_i2c_test_iflg(i2c),
				       i2c->adap.timeout);
	octeon_i2c_int_disable(i2c);
	if (!time_left) {
		dev_dbg(i2c->dev, "%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * Cleanup low-level state & enable high-level.
 *
 * Returns -EAGAIN if low-level state could not be cleaned
 */
static int octeon_i2c_enable_hlc(struct octeon_i2c *i2c)
{
	int try = 0, ret = 0;
	u64 val;

	if (i2c->hlc_enabled)
		return 0;
	i2c->hlc_enabled = true;

	while (1) {
		val = octeon_i2c_read_ctl(i2c) & (TWSI_CTL_STA | TWSI_CTL_STP);
		if (!val)
			break;

		/* clear IFLG event */
		if (val & TWSI_CTL_IFLG)
			octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

		if (try++ > 100) {
			pr_err("%s: giving up\n", __func__);
			ret = -EAGAIN;
			break;
		}

		/* spin until any start/stop has finished */
		udelay(10);
	}

	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
			    TWSI_CTL_CE | TWSI_CTL_AAK | TWSI_CTL_ENAB);
	return ret;
}

static int octeon_i2c_lost_arb(u8 code, int final_read)
{
	switch (code) {
	/* Arbitration lost */
	case STAT_LOST_ARB_38:
	case STAT_LOST_ARB_68:
	case STAT_LOST_ARB_78:
	case STAT_LOST_ARB_B0:
		return -EAGAIN;

	/* Being addressed as slave, should back off & listen */
	case STAT_SLAVE_60:
	case STAT_SLAVE_70:
	case STAT_GENDATA_ACK:
	case STAT_GENDATA_NAK:
		return -EIO;

	/* Core busy as slave */
	case STAT_SLAVE_80:
	case STAT_SLAVE_88:
	case STAT_SLAVE_A0:
	case STAT_SLAVE_A8:
	case STAT_SLAVE_LOST:
	case STAT_SLAVE_NAK:
	case STAT_SLAVE_ACK:
		return -EIO;

	/* ACK allowed on pre-terminal bytes only */
	case STAT_RXDATA_ACK:
		if (!final_read)
			return 0;
		return -EAGAIN;

	/* NAK allowed on terminal byte only */
	case STAT_RXDATA_NAK:
		if (final_read)
			return 0;
		return -EAGAIN;
	case STAT_TXDATA_NAK:
	case STAT_TXADDR_NAK:
	case STAT_RXADDR_NAK:
	case STAT_AD2W_NAK:
		return -EAGAIN;
	}
	return 0;
}

static int check_arb(struct octeon_i2c *i2c, int final_read)
{
	return octeon_i2c_lost_arb(octeon_i2c_read_sw(i2c,
			SW_TWSI_EOP_TWSI_STAT),	final_read);
}

/* send STOP to the bus */
static void octeon_i2c_stop(struct octeon_i2c *i2c)
{
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
			    TWSI_CTL_ENAB | TWSI_CTL_STP);
}

static bool octeon_i2c_hlc_test_ready(struct octeon_i2c *i2c)
{
	u64 val = __raw_readq(i2c->twsi_base + SW_TWSI);

	return (val & SW_TWSI_V) == 0;
}

static void octeon_i2c_hlc_int_enable(struct octeon_i2c *i2c)
{
	octeon_i2c_write_int(i2c, TWSI_INT_ST_EN);
}

static void octeon_i2c_hlc_int_clear(struct octeon_i2c *i2c)
{
	/* clear ST/TS events, listen for neither */
	octeon_i2c_write_int(i2c, TWSI_INT_ST_INT | TWSI_INT_TS_INT);
}

/**
 * octeon_i2c_hlc_wait - wait for an HLC operation to complete
 * @i2c: The struct octeon_i2c
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_hlc_wait(struct octeon_i2c *i2c)
{
	int result;

	octeon_i2c_hlc_int_enable(i2c);
	result = wait_event_interruptible_timeout(i2c->queue,
						  octeon_i2c_hlc_test_ready(i2c),
						  i2c->adap.timeout);
	octeon_i2c_int_disable(i2c);
	if (!result)
		octeon_i2c_hlc_int_clear(i2c);
	if (result < 0) {
		dev_dbg(i2c->dev, "%s: wait interrupted\n", __func__);
		return result;
	} else if (!result) {
		dev_dbg(i2c->dev, "%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}
	return 0;
}

/* high-level-controller pure read of up to 8 bytes */
static int octeon_i2c_simple_read(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	int i, j, ret = 0;
	u64 cmd;

	octeon_i2c_enable_hlc(i2c);
	cmd = SW_TWSI_V | SW_TWSI_R | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64) (msgs[0].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64) (msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10;
	else
		cmd |= SW_TWSI_OP_7;

	octeon_i2c_hlc_int_clear(i2c);
	__raw_writeq(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = __raw_readq(i2c->twsi_base + SW_TWSI);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;

	for (i = 0, j = msgs[0].len - 1; i  < msgs[0].len && i < 4; i++, j--)
		msgs[0].buf[j] = (cmd >> (8 * i)) & 0xff;

	if (msgs[0].len > 4) {
		cmd = __raw_readq(i2c->twsi_base + SW_TWSI_EXT);
		for (i = 0; i  < msgs[0].len - 4 && i < 4; i++, j--)
			msgs[0].buf[j] = (cmd >> (8 * i)) & 0xff;
	}

err:
	return ret;
}

/* high-level-controller pure write of up to 8 bytes */
static int octeon_i2c_simple_write(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	int i, j, ret = 0;
	u64 cmd;

	octeon_i2c_enable_hlc(i2c);
	octeon_i2c_hlc_int_clear(i2c);

	ret = check_arb(i2c, false);
	if (ret)
		goto err;

	cmd = SW_TWSI_V | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64) (msgs[0].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64) (msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10;
	else
		cmd |= SW_TWSI_OP_7;

	for (i = 0, j = msgs[0].len - 1; i  < msgs[0].len && i < 4; i++, j--)
		cmd |= (u64)msgs[0].buf[j] << (8 * i);

	if (msgs[0].len > 4) {
		u64 ext = 0;

		for (i = 0; i < msgs[0].len - 4 && i < 4; i++, j--)
			ext |= (u64) msgs[0].buf[j] << (8 * i);
		__raw_writeq(ext, i2c->twsi_base + SW_TWSI_EXT);
	}

	__raw_writeq(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = __raw_readq(i2c->twsi_base + SW_TWSI);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;

	ret = check_arb(i2c, false);

err:
	return ret;
}

/* high-level-controller composite write+read, msg0=addr, msg1=data */
static int octeon_i2c_ia_read(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	int i, j, ret = 0;
	u64 cmd;

	octeon_i2c_enable_hlc(i2c);

	cmd = SW_TWSI_V | SW_TWSI_R | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64)(msgs[1].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64)(msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10_IA;
	else
		cmd |= SW_TWSI_OP_7_IA;

	if (msgs[0].len == 2) {
		u64 ext = 0;

		cmd |= SW_TWSI_EIA;
		ext = (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;
		cmd |= (u64) msgs[0].buf[1] << SW_TWSI_IA_SHIFT;
		__raw_writeq(ext, i2c->twsi_base + SW_TWSI_EXT);
	} else
		cmd |= (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;

	octeon_i2c_hlc_int_clear(i2c);
	__raw_writeq(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = __raw_readq(i2c->twsi_base + SW_TWSI);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;

	for (i = 0, j = msgs[1].len - 1; i  < msgs[1].len && i < 4; i++, j--)
		msgs[1].buf[j] = (cmd >> (8 * i)) & 0xff;

	if (msgs[1].len > 4) {
		cmd = __raw_readq(i2c->twsi_base + SW_TWSI_EXT);
		for (i = 0; i  < msgs[1].len - 4 && i < 4; i++, j--)
			msgs[1].buf[j] = (cmd >> (8 * i)) & 0xff;
	}

err:
	return ret;
}

/* high-level-controller composite write+write, m[0]len<=2, m[1]len<=8 */
static int octeon_i2c_ia_write(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	bool set_ext = false;
	int i, j, ret = 0;
	u64 cmd, ext = 0;

	octeon_i2c_enable_hlc(i2c);

	cmd = SW_TWSI_V | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64) (msgs[1].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64) (msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10_IA;
	else
		cmd |= SW_TWSI_OP_7_IA;

	if (msgs[0].len == 2) {
		cmd |= SW_TWSI_EIA;
		ext |= (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;
		set_ext = true;
		cmd |= (u64) msgs[0].buf[1] << SW_TWSI_IA_SHIFT;
	} else
		cmd |= (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;

	for (i = 0, j = msgs[1].len - 1; i  < msgs[1].len && i < 4; i++, j--)
		cmd |= (u64) msgs[1].buf[j] << (8 * i);

	if (msgs[1].len > 4) {
		for (i = 0; i < msgs[1].len - 4 && i < 4; i++, j--)
			ext |= (u64)msgs[1].buf[j] << (8 * i);
		set_ext = true;
	}
	if (set_ext)
		__raw_writeq(ext, i2c->twsi_base + SW_TWSI_EXT);

	octeon_i2c_hlc_int_clear(i2c);
	__raw_writeq(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = octeon_i2c_read_sw64(i2c, SW_TWSI_EOP_TWSI_STAT);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;
	ret = octeon_i2c_lost_arb(cmd, false);

err:
	return ret;
}

/* calculate and set clock divisors */
static void octeon_i2c_setclock(struct octeon_i2c *i2c)
{
	int tclk, thp_base, inc, thp_idx, mdiv_idx, ndiv_idx, foscl, diff;
	int thp = 0x18, mdiv = 2, ndiv = 0, delta_hz = 1000000;

	for (ndiv_idx = 0; ndiv_idx < 8 && delta_hz != 0; ndiv_idx++) {
		/*
		 * An mdiv value of less than 2 seems to not work well
		 * with ds1337 RTCs, so we constrain it to larger values.
		 */
		for (mdiv_idx = 15; mdiv_idx >= 2 && delta_hz != 0; mdiv_idx--) {
			/*
			 * For given ndiv and mdiv values check the
			 * two closest thp values.
			 */
			tclk = i2c->twsi_freq * (mdiv_idx + 1) * 10;
			tclk *= (1 << ndiv_idx);
			thp_base = (i2c->sys_freq / (tclk * 2)) - 1;

			for (inc = 0; inc <= 1; inc++) {
				thp_idx = thp_base + inc;
				if (thp_idx < 5 || thp_idx > 0xff)
					continue;

				foscl = i2c->sys_freq / (2 * (thp_idx + 1));
				foscl = foscl / (1 << ndiv_idx);
				foscl = foscl / (mdiv_idx + 1) / 10;
				diff = abs(foscl - i2c->twsi_freq);
				if (diff < delta_hz) {
					delta_hz = diff;
					thp = thp_idx;
					mdiv = mdiv_idx;
					ndiv = ndiv_idx;
				}
			}
		}
	}
	octeon_i2c_write_sw(i2c, SW_TWSI_OP_TWSI_CLK, thp);
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CLKCTL, (mdiv << 3) | ndiv);
}

static int octeon_i2c_initlowlevel(struct octeon_i2c *i2c)
{
	u8 status = 0;
	int tries;

	/* reset controller */
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_RST, 0);

	for (tries = 10; tries && status != STAT_IDLE; tries--) {
		udelay(1);
		status = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
	}

	if (status != STAT_IDLE) {
		dev_err(i2c->dev, "%s: TWSI_RST failed! (0x%x)\n",
			__func__, status);
		return -EIO;
	}

	/* toggle twice to force both teardowns */
	octeon_i2c_enable_hlc(i2c);
	octeon_i2c_disable_hlc(i2c);
	return 0;
}

/*
 * TWSI state seems stuck. Not sure if it's TWSI-engine state or something
 * else on bus. The initial _stop() is always harmless, it just resets state
 * machine, does not _transmit_ STOP unless engine was active.
 */
static int start_unstick(struct octeon_i2c *i2c)
{
	octeon_i2c_stop(i2c);

	/*
	 * Response is escalated over successive calls,
	 * as EAGAIN provokes retries from i2c/core.
	 */
	switch (reset_how++ % 4) {
	case 0:
		/* just the stop above */
		break;
	case 1:
		/*
		 * Controller refused to send start flag. May be a
		 * client is holding SDA low? Let's try to free it.
		 */
		octeon_i2c_unblock(i2c);
		break;
	case 2:
		/* re-init our TWSI hardware */
		octeon_i2c_initlowlevel(i2c);
		break;
	default:
		/* retry in caller */
		reset_how = 0;
		return -EAGAIN;
	}
	return 0;
}

/**
 * octeon_i2c_start - send START to the bus
 * @i2c: The struct octeon_i2c
 * @first: first msg in combined operation?
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_start(struct octeon_i2c *i2c, int first)
{
	int result;
	u8 data;

	octeon_i2c_disable_hlc(i2c);

	while (1) {
		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
				    TWSI_CTL_ENAB | TWSI_CTL_STA);

		result = octeon_i2c_wait(i2c);
		data = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);

		switch (data) {
		case STAT_START:
		case STAT_RSTART:
			if (!first)
				return -EAGAIN;
			reset_how = 0;
			return 0;
		case STAT_RXADDR_ACK:
			if (first)
				return -EAGAIN;
			return start_unstick(i2c);
		/*
		 * case STAT_IDLE:
		 * case STAT_ERROR:
		 */
		default:
			if (!first)
				return -EAGAIN;
			start_unstick(i2c);
		}
	}
	return 0;
}

/**
 * octeon_i2c_read - receive data from the bus via low-level controller
 * @i2c: The struct octeon_i2c
 * @target: Target address
 * @data: Pointer to the location to store the data
 * @rlength: Length of the data
 * @phase: which phase of a combined operation.
 * @recv_len: flag for length byte
 *
 * The address is sent over the bus, then the data is read.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_read(struct octeon_i2c *i2c, int target, u8 *data,
			   u16 *rlength, bool first, bool last, bool recv_len)
{
	u8 ctl = TWSI_CTL_ENAB | TWSI_CTL_AAK;
	int i, result, length = *rlength;
	u8 tmp;

	if (length < 1)
		return -EINVAL;

	result = octeon_i2c_start(i2c, first);
	if (result)
		return result;

	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, (target << 1) | 1);

	for (i = 0; i < length; ) {
		tmp = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
		result = octeon_i2c_lost_arb(tmp, !(ctl & TWSI_CTL_AAK));
		if (result)
			return result;

		switch (tmp) {
		case STAT_RXDATA_ACK:
		case STAT_RXDATA_NAK:
			data[i++] = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_DATA);
		}


		/* NAK last recv'd byte, as a no-more-please */
		if (last && i == length - 1)
			ctl &= ~TWSI_CTL_AAK;

		/* clr iflg to allow next event */
		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, ctl);
		result = octeon_i2c_wait(i2c);
		if (result)
			return result;
		if (recv_len && i == 0) {
			if (data[i] > I2C_SMBUS_BLOCK_MAX + 1) {
				dev_err(i2c->dev,
					"%s: read len > I2C_SMBUS_BLOCK_MAX %d\n",
					__func__, data[i]);
				return -EIO;
			}
			length += data[i];
		}
	}
	*rlength = length;
	return 0;
}

/**
 * octeon_i2c_write - send data to the bus via low-level controller
 * @i2c: The struct octeon_i2c
 * @target: Target address
 * @data: Pointer to the data to be sent
 * @length: Length of the data
 * @last: is last msg in combined operation?
 *
 * The address is sent over the bus, then the data.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_write(struct octeon_i2c *i2c, int target,
			    const u8 *data, int length, int first, int last)
{
	int i, result;

	result = octeon_i2c_start(i2c, first);
	if (result)
		return result;

	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, target << 1);
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

	result = octeon_i2c_wait(i2c);
	if (result)
		return result;

	for (i = 0; i < length; i++) {
		result = check_arb(i2c, false);
		if (result)
			return result;

		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, data[i]);
		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

		result = octeon_i2c_wait(i2c);
		if (result)
			return result;
		result = check_arb(i2c, false);
		if (result)
			return result;
	}

	return 0;
}

/**
 * octeon_i2c_xfer - The driver's master_xfer function
 * @adap: Pointer to the i2c_adapter structure
 * @msgs: Pointer to the messages to be processed
 * @num: Length of the MSGS array
 *
 * Returns the number of messages processed, or a negative errno on failure.
 */
static int octeon_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct octeon_i2c *i2c = i2c_get_adapdata(adap);
	int i, ret = 0;

	if (num == 1) {
		if (msgs[0].len > 0 && msgs[0].len <= 8) {
			if (msgs[0].flags & I2C_M_RD)
				ret = octeon_i2c_simple_read(i2c, msgs);
			else
				ret = octeon_i2c_simple_write(i2c, msgs);
			goto out;
		}
	} else if (num == 2) {
		if ((msgs[0].flags & I2C_M_RD) == 0 &&
		    (msgs[1].flags & I2C_M_RECV_LEN) == 0 &&
		    msgs[0].len > 0 && msgs[0].len <= 2 &&
		    msgs[1].len > 0 && msgs[1].len <= 8 &&
		    msgs[0].addr == msgs[1].addr) {
			if (msgs[1].flags & I2C_M_RD)
				ret = octeon_i2c_ia_read(i2c, msgs);
			else
				ret = octeon_i2c_ia_write(i2c, msgs);
			goto out;
		}
	}

	for (i = 0; ret == 0 && i < num; i++) {
		struct i2c_msg *pmsg = &msgs[i];
		bool last = (i == (num - 1));

		dev_dbg(i2c->dev,
			"Doing %s %d byte(s) to/from 0x%02x - %d of %d messages\n",
			 pmsg->flags & I2C_M_RD ? "read" : "write",
			 pmsg->len, pmsg->addr, i + 1, num);
		if (pmsg->flags & I2C_M_RD)
			ret = octeon_i2c_read(i2c, pmsg->addr, pmsg->buf,
					      &pmsg->len, !i, last,
					      pmsg->flags & I2C_M_RECV_LEN);
		else
			ret = octeon_i2c_write(i2c, pmsg->addr, pmsg->buf,
					       pmsg->len, !i, last);
	}
	octeon_i2c_stop(i2c);

out:
	return ret ? -EAGAIN : num;
}

static u32 octeon_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm octeon_i2c_algo = {
	.master_xfer = octeon_i2c_xfer,
	.functionality = octeon_i2c_functionality,
};

static struct i2c_adapter octeon_i2c_ops = {
	.owner = THIS_MODULE,
	.name = "OCTEON adapter",
	.algo = &octeon_i2c_algo,
};

static int octeon_i2c_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *res_mem;
	struct octeon_i2c *i2c;
	int irq, result = 0;

	/* All adaptors have an irq.  */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c) {
		result = -ENOMEM;
		goto out;
	}
	i2c->dev = &pdev->dev;

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (res_mem == NULL) {
		dev_err(i2c->dev, "found no memory resource\n");
		result = -ENXIO;
		goto out;
	}

	/*
	 * "clock-rate" is a legacy binding, the official binding is
	 * "clock-frequency".  Try the official one first and then
	 * fall back if it doesn't exist.
	 */
	if (of_property_read_u32(node, "clock-frequency", &i2c->twsi_freq) &&
	    of_property_read_u32(node, "clock-rate", &i2c->twsi_freq)) {
		dev_err(i2c->dev,
			"no I2C 'clock-rate' or 'clock-frequency' property\n");
		result = -ENXIO;
		goto out;
	}

	i2c->sys_freq = octeon_get_io_clock_rate();

	if (!devm_request_mem_region(&pdev->dev, res_mem->start,
				     resource_size(res_mem), res_mem->name)) {
		dev_err(i2c->dev, "request_mem_region failed\n");
		goto out;
	}
	i2c->twsi_base = devm_ioremap(&pdev->dev, res_mem->start,
				      resource_size(res_mem));

	init_waitqueue_head(&i2c->queue);

	i2c->irq = irq;

	result = devm_request_irq(&pdev->dev, i2c->irq,
				  octeon_i2c_isr, 0, DRV_NAME, i2c);
	if (result < 0) {
		dev_err(i2c->dev, "failed to attach interrupt\n");
		goto out;
	}

	result = octeon_i2c_initlowlevel(i2c);
	if (result) {
		dev_err(i2c->dev, "init low level failed\n");
		goto  out;
	}

	octeon_i2c_setclock(i2c);

	i2c->adap = octeon_i2c_ops;
	i2c->adap.timeout = msecs_to_jiffies(timeout);
	i2c->adap.retries = 10;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.dev.of_node = node;
	i2c_set_adapdata(&i2c->adap, i2c);
	platform_set_drvdata(pdev, i2c);

	result = i2c_add_adapter(&i2c->adap);
	if (result < 0) {
		dev_err(i2c->dev, "failed to add adapter\n");
		goto out;
	}
	dev_info(i2c->dev, "probed\n");
	return 0;

out:
	return result;
};

static int octeon_i2c_remove(struct platform_device *pdev)
{
	struct octeon_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
	return 0;
};

static const struct of_device_id octeon_i2c_match[] = {
	{	.compatible = "cavium,octeon-3860-twsi",	},
	{},
};
MODULE_DEVICE_TABLE(of, octeon_i2c_match);

static struct platform_driver octeon_i2c_driver = {
	.probe		= octeon_i2c_probe,
	.remove		= octeon_i2c_remove,
	.driver		= {
		.name	= DRV_NAME,
		.of_match_table = octeon_i2c_match,
	},
};

module_platform_driver(octeon_i2c_driver);

MODULE_AUTHOR("Michael Lawnick <michael.lawnick.ext@nsn.com>");
MODULE_DESCRIPTION("I2C-Bus adapter for Cavium OCTEON processors");
MODULE_LICENSE("GPL");
