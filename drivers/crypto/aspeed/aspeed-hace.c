/*
 * Crypto driver for the Aspeed SoC
 *
 * Copyright (C) ASPEED Technology Inc.
 * Ryan Chen <ryan_chen@aspeedtech.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include "aspeed-hace.h"

// #define ASPEED_CRYPTO_DEBUG

#ifdef ASPEED_CRYPTO_DEBUG
//#define CRYPTO_DBUG(fmt, args...) printk(KERN_DEBUG "%s() " fmt, __FUNCTION__, ## args)
#define CRYPTO_DBUG(fmt, args...) printk("%s() " fmt, __FUNCTION__, ## args)
#else
#define CRYPTO_DBUG(fmt, args...)
#endif

static irqreturn_t aspeed_crypto_irq(int irq, void *dev)
{
	struct aspeed_hace_dev *hace_dev = (struct aspeed_hace_dev *)dev;
	struct aspeed_engine_crypto *crypto_engine = &hace_dev->crypto_engine;
	struct aspeed_engine_hash *hash_engine = &hace_dev->hash_engine;
	u32 sts = aspeed_hace_read(hace_dev, ASPEED_HACE_STS);
	int handle = IRQ_NONE;

	CRYPTO_DBUG("aspeed_crypto_irq sts %x \n", sts);
	aspeed_hace_write(hace_dev, sts, ASPEED_HACE_STS);

	if (sts & HACE_CRYPTO_ISR) {
		if (crypto_engine->flags & CRYPTO_FLAGS_BUSY)
			tasklet_schedule(&crypto_engine->done_task);
		else
			dev_warn(hace_dev->dev, "CRYPTO interrupt when no active requests.\n");
		handle = IRQ_HANDLED;
	}

	if (sts & HACE_HASH_ISR) {
		if (hash_engine->flags & CRYPTO_FLAGS_BUSY)
			tasklet_schedule(&hash_engine->done_task);
		else
			dev_warn(hace_dev->dev, "CRYPTO interrupt when no active requests.\n");
		handle = IRQ_HANDLED;
	}
	// if (sts & HACE_RSA_ISR) {
	// 	aspeed_hace_write(hace_dev, 0, ASPEED_HACE_RSA_CMD);
	// 	if (hace_dev->flags & CRYPTO_FLAGS_BUSY)
	// 		tasklet_schedule(&hace_dev->done_task);
	// 	else
	// 		dev_warn(hace_dev->dev, "CRYPTO interrupt when no active requests.\n");
	// 	handle = IRQ_HANDLED;
	// }
	return handle;
}

static void aspeed_crypto_sk_done_task(unsigned long data)
{
	struct aspeed_hace_dev *hace_dev = (struct aspeed_hace_dev *)data;
	struct aspeed_engine_crypto *crypto_engine = &hace_dev->crypto_engine;

	crypto_engine->is_async = true;
	(void)crypto_engine->resume(hace_dev);
}

static void aspeed_crypto_ahash_done_task(unsigned long data)
{
	struct aspeed_hace_dev *hace_dev = (struct aspeed_hace_dev *)data;
	struct aspeed_engine_hash *hash_engine = &hace_dev->hash_engine;

	CRYPTO_DBUG("\n");

	(void)hash_engine->resume(hace_dev);
}

// static void aspeed_crypto_ahash_queue_task(unsigned long data)
// {
// 	struct aspeed_hace_dev *hace_dev = (struct aspeed_hace_dev *)data;

// 	CRYPTO_DBUG("\n");
// 	aspeed_hace_ahash_handle_queue(hace_dev, NULL);
// }

static int aspeed_crypto_register(struct aspeed_hace_dev *hace_dev)
{
	aspeed_register_skcipher_algs(hace_dev);
	aspeed_register_ahash_algs(hace_dev);
	// aspeed_register_akcipher_algs(hace_dev);

	return 0;
}

// static void aspeed_crypto_unregister(void)
// {
// #if 0
// 	unsigned int i;

// 	for (i = 0; i < ARRAY_SIZE(aspeed_cipher_algs); i++) {
// 		if (aspeed_cipher_algs[i]->type == ALG_TYPE_CIPHER)
// 			crypto_unregister_alg(&aspeed_cipher_algs[i]->alg.crypto);
// 		else
// 			crypto_unregister_ahash(&aspeed_cipher_algs[i]->alg.hash);
// 	}
// #endif
// }

static const struct of_device_id aspeed_crypto_of_matches[] = {
	{ .compatible = "aspeed,ast2400-crypto", .data = (void *) 0,},
	{ .compatible = "aspeed,ast2500-crypto", .data = (void *) 5,},
	{ .compatible = "aspeed,ast2600-crypto", .data = (void *) 6,},
	{},
};

static int aspeed_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aspeed_hace_dev *hace_dev;
	const struct of_device_id *crypto_dev_id;
	struct aspeed_engine_crypto *crypto_engine;
	struct aspeed_engine_hash *hash_engine;
	int err;


	hace_dev = devm_kzalloc(&pdev->dev, sizeof(struct aspeed_hace_dev), GFP_KERNEL);
	if (!hace_dev) {
		dev_err(dev, "unable to alloc data struct.\n");
		return -ENOMEM;
	}

	crypto_dev_id = of_match_device(aspeed_crypto_of_matches, &pdev->dev);
	if (!crypto_dev_id)
		return -EINVAL;

	hace_dev->dev = dev;
	hace_dev->version = (unsigned long)crypto_dev_id->data;
	crypto_engine = &hace_dev->crypto_engine;
	hash_engine = &hace_dev->hash_engine;

	platform_set_drvdata(pdev, hace_dev);
	spin_lock_init(&crypto_engine->lock);
	tasklet_init(&crypto_engine->done_task, aspeed_crypto_sk_done_task, (unsigned long)hace_dev);
	crypto_init_queue(&crypto_engine->queue, 50);

	spin_lock_init(&hash_engine->lock);
	tasklet_init(&hash_engine->done_task, aspeed_crypto_ahash_done_task, (unsigned long)hace_dev);
	// tasklet_init(&hash_engine->queue_task, aspeed_crypto_ahash_queue_task, (unsigned long)hace_dev);
	crypto_init_queue(&hash_engine->queue, 50);

	hace_dev->regs = of_iomap(pdev->dev.of_node, 0);
	if (!(hace_dev->regs)) {
		dev_err(dev, "can't ioremap\n");
		return -ENOMEM;
	}

	// hace_dev->rsa_buff = of_iomap(pdev->dev.of_node, 1);
	// if (!(hace_dev->rsa_buff)) {
	// 	dev_err(dev, "can't rsa ioremap\n");
	// 	return -ENOMEM;
	// }

	hace_dev->irq = platform_get_irq(pdev, 0);
	if (!hace_dev->irq) {
		dev_err(&pdev->dev, "no memory/irq resource for hace_dev\n");
		return -ENXIO;
	}

	if (devm_request_irq(&pdev->dev, hace_dev->irq, aspeed_crypto_irq, 0, dev_name(&pdev->dev), hace_dev)) {
		dev_err(dev, "unable to request aes irq.\n");
		return -EBUSY;
	}

	hace_dev->yclk = devm_clk_get(&pdev->dev, "yclk");
	if (IS_ERR(hace_dev->yclk)) {
		dev_err(&pdev->dev, "no yclk clock defined\n");
		return -ENODEV;
	}

	clk_prepare_enable(hace_dev->yclk);

	hace_dev->rsaclk = devm_clk_get(&pdev->dev, "rsaclk");
	if (IS_ERR(hace_dev->rsaclk)) {
		dev_err(&pdev->dev, "no rsaclk clock defined\n");
		return -ENODEV;
	}

	clk_prepare_enable(hace_dev->rsaclk);



	// if (of_device_is_compatible(pdev->dev.of_node,
	// 			    "aspeed,ast2600-crypto")) {
	// 	hace_dev->version = ASPEED_CRYPTO_G6;
	// 	hace_dev->rsa_max_buf_len = ASPEED_CRYPTO_G6_RSA_BUFF_SIZE;
	// } else {
	// 	hace_dev->version = 0;
	// 	hace_dev->rsa_max_buf_len = ASPEED_CRYPTO_RSA_BUFF_SIZE;
	// }

	// 8-byte aligned
	crypto_engine->cipher_addr = dma_alloc_coherent(&pdev->dev, 0xa000,
				 &crypto_engine->cipher_dma_addr, GFP_KERNEL);

	if (!crypto_engine->cipher_addr) {
		printk("error buff allocation\n");
		return -ENOMEM;
	}

	hash_engine->ahash_src_addr = dma_alloc_coherent(&pdev->dev,
				       APSEED_CRYPTO_SRC_DMA_BUF_LEN,
				       &hash_engine->ahash_src_dma_addr, GFP_KERNEL);
	if (!hash_engine->ahash_src_addr) {
		printk("error buff allocation\n");
		return -ENOMEM;
	}
	if (hace_dev->version == 6) {
		crypto_engine->dst_sg_addr = dma_alloc_coherent(&pdev->dev,
					 APSEED_CRYPTO_DST_DMA_BUF_LEN,
					 &crypto_engine->dst_sg_dma_addr, GFP_KERNEL);
		if (!crypto_engine->dst_sg_addr) {
			printk("error buff allocation\n");
			return -ENOMEM;
		}
	}
	err = aspeed_crypto_register(hace_dev);
	if (err) {
		dev_err(dev, "err in register alg");
		return err;
	}

	printk("ASPEED Crypto Accelerator successfully registered \n");

	return 0;
}

static int aspeed_crypto_remove(struct platform_device *pdev)
{
	// struct aspeed_hace_dev *hace_dev = platform_get_drvdata(pdev);

	//aspeed_crypto_unregister();
	// tasklet_kill(&hace_dev->done_task);
	// tasklet_kill(&hace_dev->queue_task);
	return 0;
}

#ifdef CONFIG_PM
static int aspeed_crypto_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct aspeed_hace_dev *hace_dev = platform_get_drvdata(pdev);

	/*
	 * We only support standby mode. All we have to do is gate the clock to
	 * the spacc. The hardware will preserve state until we turn it back
	 * on again.
	 */
//	clk_disable(hace_dev->clk);

	return 0;
}

static int aspeed_crypto_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct aspeed_hace_dev *hace_dev = platform_get_drvdata(pdev);
	return 0;
//	return clk_enable(hace_dev->clk);
}

#endif /* CONFIG_PM */

MODULE_DEVICE_TABLE(of, aspeed_crypto_of_matches);

static struct platform_driver aspeed_crypto_driver = {
	.probe 		= aspeed_crypto_probe,
	.remove		= aspeed_crypto_remove,
#ifdef CONFIG_PM
	.suspend	= aspeed_crypto_suspend,
	.resume 	= aspeed_crypto_resume,
#endif
	.driver         = {
		.name   = KBUILD_MODNAME,
		.of_match_table = aspeed_crypto_of_matches,
	},
};

module_platform_driver(aspeed_crypto_driver);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED Crypto driver");
MODULE_LICENSE("GPL2");
