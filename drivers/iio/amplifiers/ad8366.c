/*
 * AD8366 SPI Dual-Digital Variable Gain Amplifier (VGA)
 *
 * Copyright 2012 Analog Devices Inc.
 *
 * Licensed under the GPL-2.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/spi/spi.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/bitrev.h>
#include <linux/of.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

enum ad8366_type {
	ID_AD8366,
	ID_ADA4961,
	ID_ADL5240,
	ID_HMC271,
};

struct ad8366_state {
	struct spi_device	*spi;
	struct regulator		*reg;
	struct gpio_desc		*reset_gpio;
	unsigned char		ch[2];
	enum ad8366_type	type;

	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	unsigned char		data[2] ____cacheline_aligned;
};

static int ad8366_write(struct iio_dev *indio_dev,
			unsigned char ch_a, unsigned char ch_b)
{
	struct ad8366_state *st = iio_priv(indio_dev);
	int ret;

	switch (st->type) {
	case ID_AD8366:
		ch_a = bitrev8(ch_a & 0x3F);
		ch_b = bitrev8(ch_b & 0x3F);

		st->data[0] = ch_b >> 4;
		st->data[1] = (ch_b << 4) | (ch_a >> 2);
		break;
	case ID_ADA4961:
		st->data[0] = ch_a & 0x1F;
		break;
	case ID_ADL5240:
		st->data[0] = (ch_a & 0x3F);
		break;
	case ID_HMC271:
		st->data[0] = bitrev8(ch_a & 0x1F) >> 3;
		break;
	}


	ret = spi_write(st->spi, st->data, indio_dev->num_channels);
	if (ret < 0)
		dev_err(&indio_dev->dev, "write failed (%d)", ret);

	return ret;
}

static int ad8366_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long m)
{
	struct ad8366_state *st = iio_priv(indio_dev);
	int ret;
	int code;

	mutex_lock(&indio_dev->mlock);
	switch (m) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		code = st->ch[chan->channel];

		switch (st->type) {
		case ID_AD8366:
			code = code * 253 + 4500;
			break;
		case ID_ADA4961:
			code = 15000 - code * 1000;
			break;
		case ID_ADL5240:
			code = 20000 - 31500 + code * 500;
			break;
		case ID_HMC271:
			code = -31000 + code * 1000;
			break;
		}

		/* Values in dB */
		*val = code / 1000;
		*val2 = (code % 1000) * 1000;

		ret = IIO_VAL_INT_PLUS_MICRO_DB;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret;
};

static int ad8366_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val,
			    int val2,
			    long mask)
{
	struct ad8366_state *st = iio_priv(indio_dev);
	int code;
	int ret;
	/* Values in dB */
	if (val < 0)
		code = (((s8)val * 1000) - ((s32)val2 / 1000));
	else
		code = (((s8)val * 1000) + ((s32)val2 / 1000));

	switch (st->type) {
	case ID_AD8366:
		if (code > 20500 || code < 4500)
			return -EINVAL;
		code = (code - 4500) / 253;
		break;
	case ID_ADA4961:
		if (code > 15000 || code < -6000)
			return -EINVAL;
		code = (15000 - code) / 1000;
		break;
	case ID_ADL5240:
		if (code < -11500 || code > 20000)
			return -EINVAL;
		code = ((code - 500 - 20000) / 500) & 0x3F;
		break;
	case ID_HMC271:
		if (code < -31000 || code > 0)
			return -EINVAL;
		code = ((code - 1000) / 1000) & 0x1F;
		break;
	}

	mutex_lock(&indio_dev->mlock);
	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		st->ch[chan->channel] = code;
		ret = ad8366_write(indio_dev, st->ch[0], st->ch[1]);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static const struct iio_info ad8366_info = {
	.read_raw = &ad8366_read_raw,
	.write_raw = &ad8366_write_raw,
	.driver_module = THIS_MODULE,
};

#define AD8366_CHAN(_channel) {				\
	.type = IIO_VOLTAGE,				\
	.output = 1,					\
	.indexed = 1,					\
	.channel = _channel,				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN),\
}

static const struct iio_chan_spec ad8366_channels[] = {
	AD8366_CHAN(0),
	AD8366_CHAN(1),
};

static const struct iio_chan_spec ada4961_channels[] = {
	AD8366_CHAN(0),
};

static int ad8366_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct ad8366_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (indio_dev == NULL)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	st->reg = devm_regulator_get(&spi->dev, "vcc");
	if (!IS_ERR(st->reg)) {
		ret = regulator_enable(st->reg);
		if (ret)
			return ret;
	}

	spi_set_drvdata(spi, indio_dev);
	st->spi = spi;

	indio_dev->dev.parent = &spi->dev;

	/* try to get a unique name */
	if (spi->dev.platform_data)
		indio_dev->name = spi->dev.platform_data;
	else if (spi->dev.of_node)
		indio_dev->name = spi->dev.of_node->name;
	else
		indio_dev->name = spi_get_device_id(spi)->name;

	st->type = spi_get_device_id(spi)->driver_data;
	switch (st->type) {
	case ID_AD8366:
		indio_dev->channels = ad8366_channels;
		indio_dev->num_channels = ARRAY_SIZE(ad8366_channels);
		break;
	case ID_ADA4961:
	case ID_ADL5240:
	case ID_HMC271:

		st->reset_gpio = devm_gpiod_get(&spi->dev, "reset",
			GPIOD_OUT_HIGH);

		indio_dev->channels = ada4961_channels;
		indio_dev->num_channels = ARRAY_SIZE(ada4961_channels);
		break;
	default:
		dev_err(&spi->dev, "Invalid device ID\n");
		return -EINVAL;
	}

	indio_dev->info = &ad8366_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = ad8366_write(indio_dev, 0 , 0);
	if (ret < 0)
		goto error_disable_reg;

	ret = iio_device_register(indio_dev);
	if (ret)
		goto error_disable_reg;

	return 0;

error_disable_reg:
	if (!IS_ERR(st->reg))
		regulator_disable(st->reg);

	return ret;
}

static int ad8366_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct ad8366_state *st = iio_priv(indio_dev);
	struct regulator *reg = st->reg;

	iio_device_unregister(indio_dev);

	if (!IS_ERR(reg))
		regulator_disable(reg);

	return 0;
}

static const struct spi_device_id ad8366_id[] = {
	{"ad8366", ID_AD8366},
	{"ada4961", ID_ADA4961},
	{"adl5240", ID_ADL5240},
	{"hmc271", ID_HMC271},
	{}
};
MODULE_DEVICE_TABLE(spi, ad8366_id);

static struct spi_driver ad8366_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
	},
	.probe		= ad8366_probe,
	.remove		= ad8366_remove,
	.id_table	= ad8366_id,
};

module_spi_driver(ad8366_driver);

MODULE_AUTHOR("Michael Hennerich <hennerich@blackfin.uclinux.org>");
MODULE_DESCRIPTION("Analog Devices AD8366 VGA");
MODULE_LICENSE("GPL v2");
