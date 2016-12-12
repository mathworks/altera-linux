/*
 * ADI-AIM ADC Interface Module
 *
 * Copyright 2012 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/spinlock.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/dma-buffer.h>
#include <linux/iio/dmaengine.h>
#include "cf_axi_adc.h"

static int axiadc_hw_submit_block(void *data, struct iio_dma_buffer_block *block)
{
	struct iio_dev *indio_dev = data;
	struct axiadc_state *st = iio_priv(indio_dev);

	block->block.bytes_used = block->block.size;

	iio_dmaengine_buffer_submit_block(block, DMA_FROM_DEVICE);

	axiadc_write(st, ADI_REG_STATUS, ~0);
	axiadc_write(st, ADI_REG_DMA_STATUS, ~0);

	if (!st->has_fifo_interface) {
		axiadc_write(st, ADI_REG_DMA_CNTRL, 0);
		axiadc_write(st, ADI_REG_DMA_COUNT, block->block.bytes_used);
		axiadc_write(st, ADI_REG_DMA_CNTRL, ADI_DMA_START | 2);
	}

	return 0;
}

static const struct iio_dma_buffer_ops axiadc_dma_buffer_ops = {
	.submit_block = axiadc_hw_submit_block,
};

int axiadc_configure_ring_stream(struct iio_dev *indio_dev,
	const char *dma_name)
{
	struct iio_buffer *buffer;

	if (dma_name == NULL)
		dma_name = "rx";

	buffer = iio_dmaengine_buffer_alloc(indio_dev->dev.parent, dma_name,
			&axiadc_dma_buffer_ops, indio_dev);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	indio_dev->modes |= INDIO_BUFFER_HARDWARE;
	iio_device_attach_buffer(indio_dev, buffer);

	return 0;
}
EXPORT_SYMBOL_GPL(axiadc_configure_ring_stream);

void axiadc_unconfigure_ring_stream(struct iio_dev *indio_dev)
{
	iio_dmaengine_buffer_free(indio_dev->buffer);
}
EXPORT_SYMBOL_GPL(axiadc_unconfigure_ring_stream);
