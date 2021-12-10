// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2015-2016 Freescale Semiconductor, Inc.
 * Copyright 2017,2020 NXP
 */

/*
 * @file
 *  Contains all the functions to handle parsing and loading of PE firmware
 * files.
 */

#include <dm.h>
#include <dm/device-internal.h>
#include <malloc.h>
#include <net/pfe_eth/pfe_eth.h>
#include <net/pfe_eth/pfe_firmware.h>
#include <spi_flash.h>
#ifdef CONFIG_CHAIN_OF_TRUST
#include <fsl_validate.h>
#endif

#define PFE_FIRMWARE_FIT_CNF_NAME	"config@1"

static const void *pfe_fit_addr;
#ifdef CONFIG_CHAIN_OF_TRUST
static const void *pfe_esbc_hdr_addr;
#endif

/*
 * PFE elf firmware loader.
 * Loads an elf firmware image into a list of PE's (specified using a bitmask)
 *
 * @param pe_mask	Mask of PE id's to load firmware to
 * @param pfe_firmware	Pointer to the firmware image
 *
 * @return		0 on success, a negative value on error
 */
static int pfe_load_elf(int pe_mask, uint8_t *pfe_firmware)
{
	Elf32_Ehdr *elf_hdr = (Elf32_Ehdr *)pfe_firmware;
	Elf32_Half sections = be16_to_cpu(elf_hdr->e_shnum);
	Elf32_Shdr *shdr = (Elf32_Shdr *)(pfe_firmware +
						be32_to_cpu(elf_hdr->e_shoff));
	int id, section;
	int ret;

	debug("%s: no of sections: %d\n", __func__, sections);

	/* Some sanity checks */
	if (strncmp((char *)&elf_hdr->e_ident[EI_MAG0], ELFMAG, SELFMAG)) {
		printf("%s: incorrect elf magic number\n", __func__);
		return -1;
	}

	if (elf_hdr->e_ident[EI_CLASS] != ELFCLASS32) {
		printf("%s: incorrect elf class(%x)\n", __func__,
		       elf_hdr->e_ident[EI_CLASS]);
		return -1;
	}

	if (elf_hdr->e_ident[EI_DATA] != ELFDATA2MSB) {
		printf("%s: incorrect elf data(%x)\n", __func__,
		       elf_hdr->e_ident[EI_DATA]);
		return -1;
	}

	if (be16_to_cpu(elf_hdr->e_type) != ET_EXEC) {
		printf("%s: incorrect elf file type(%x)\n", __func__,
		       be16_to_cpu(elf_hdr->e_type));
		return -1;
	}

	for (section = 0; section < sections; section++, shdr++) {
		if (!(be32_to_cpu(shdr->sh_flags) & (SHF_WRITE | SHF_ALLOC |
			SHF_EXECINSTR)))
			continue;
		for (id = 0; id < MAX_PE; id++)
			if (pe_mask & BIT(id)) {
				ret = pe_load_elf_section(id,
							  pfe_firmware, shdr);
				if (ret < 0)
					goto err;
			}
	}
	return 0;

err:
	return ret;
}

/*
 * Get PFE firmware from FIT image
 *
 * @param data pointer to PFE firmware
 * @param size pointer to size of the firmware
 * @param fw_name pfe firmware name, either class or tmu
 *
 * @return 0 on success, a negative value on error
 */
static int pfe_get_fw(const void **data,
		      size_t *size, char *fw_name)
{
	int conf_node_off, fw_node_off;
	char *conf_node_name = NULL;
	char *desc;
	int ret = 0;

	conf_node_name = PFE_FIRMWARE_FIT_CNF_NAME;

	conf_node_off = fit_conf_get_node(pfe_fit_addr, conf_node_name);
	if (conf_node_off < 0) {
		printf("PFE Firmware: %s: no such config\n", conf_node_name);
		return -ENOENT;
	}

	fw_node_off = fit_conf_get_prop_node(pfe_fit_addr, conf_node_off,
					     fw_name);
	if (fw_node_off < 0) {
		printf("PFE Firmware: No '%s' in config\n",
		       fw_name);
		return -ENOLINK;
	}

	if (!(fit_image_verify(pfe_fit_addr, fw_node_off))) {
		printf("PFE Firmware: Bad firmware image (bad CRC)\n");
		return -EINVAL;
	}

	if (fit_image_get_data(pfe_fit_addr, fw_node_off, data, size)) {
		printf("PFE Firmware: Can't get %s subimage data/size",
		       fw_name);
		return -ENOENT;
	}

	ret = fit_get_desc(pfe_fit_addr, fw_node_off, &desc);
	if (ret)
		printf("PFE Firmware: Can't get description\n");
	else
		printf("%s\n", desc);

	return ret;
}

/*
 * Check PFE FIT image
 *
 * @return 0 on success, a negative value on error
 */
static int pfe_fit_check(void)
{
	int ret = 0;

	ret = fdt_check_header(pfe_fit_addr);
	if (ret) {
		printf("PFE Firmware: Bad firmware image (not a FIT image)\n");
		return ret;
	}

	if (!fit_check_format(pfe_fit_addr)) {
		printf("PFE Firmware: Bad firmware image (bad FIT header)\n");
		ret = -1;
		return ret;
	}

	return ret;
}

int pfe_spi_flash_init(void)
{
	struct spi_flash *pfe_flash;
	int ret = 0;
	void *addr = malloc(CONFIG_SYS_LS_PFE_FW_LENGTH);

	if (!addr)
		return -ENOMEM;

#ifdef CONFIG_DM_SPI_FLASH
	struct udevice *new;

	/* speed and mode will be read from DT */
	ret = spi_flash_probe_bus_cs(CONFIG_ENV_SPI_BUS,
				     CONFIG_ENV_SPI_CS, 0, 0, &new);
	if (ret) {
		printf("SF: failed to probe spi\n");
		free(addr);
		device_remove(new, DM_REMOVE_NORMAL);
		return ret;
	}

	pfe_flash = dev_get_uclass_priv(new);
#else
	pfe_flash = spi_flash_probe(CONFIG_ENV_SPI_BUS,
				    CONFIG_ENV_SPI_CS,
				    CONFIG_ENV_SPI_MAX_HZ,
				    CONFIG_ENV_SPI_MODE);
#endif
	if (!pfe_flash) {
		printf("SF: probe for pfe failed\n");
		free(addr);
#ifdef CONFIG_DM_SPI_FLASH
		device_remove(new, DM_REMOVE_NORMAL);
#endif
		return -ENODEV;
	}

	ret = spi_flash_read(pfe_flash,
			     CONFIG_SYS_LS_PFE_FW_ADDR,
			     CONFIG_SYS_LS_PFE_FW_LENGTH,
			     addr);
	if (ret) {
		printf("SF: read for pfe failed\n");
		free(addr);
		spi_flash_free(pfe_flash);
		return ret;
	}

#ifdef CONFIG_CHAIN_OF_TRUST
	void *hdr_addr = malloc(CONFIG_SYS_LS_PFE_ESBC_LENGTH);

	if (!hdr_addr) {
		free(addr);
		spi_flash_free(pfe_flash);
		return -ENOMEM;
	}

	ret = spi_flash_read(pfe_flash,
			     CONFIG_SYS_LS_PFE_ESBC_ADDR,
			     CONFIG_SYS_LS_PFE_ESBC_LENGTH,
			     hdr_addr);
	if (ret) {
		printf("SF: failed to read pfe esbc header\n");
		free(addr);
		free(hdr_addr);
		spi_flash_free(pfe_flash);
		return ret;
	}

	pfe_esbc_hdr_addr = hdr_addr;
#endif

	pfe_fit_addr = addr;
	spi_flash_free(pfe_flash);

	return ret;
}

/*
 * PFE firmware initialization.
 * Loads different firmware files from FIT image.
 * Initializes PE IMEM/DMEM and UTIL-PE DDR
 * Initializes control path symbol addresses (by looking them up in the elf
 * firmware files
 * Takes PE's out of reset
 *
 * @return 0 on success, a negative value on error
 */
int pfe_firmware_init(void)
{
#define PFE_KEY_HASH	NULL
	char *pfe_firmware_name;
	const void *raw_image_addr;
	size_t raw_image_size = 0;
	u8 *pfe_firmware;
#ifdef CONFIG_CHAIN_OF_TRUST
	uintptr_t pfe_esbc_hdr = 0;
	uintptr_t pfe_img_addr = 0;
#endif
	int ret = 0;
	int fw_count, max_fw_count;
	const char *p;

	ret = pfe_spi_flash_init();
	if (ret)
		goto err;

	ret = pfe_fit_check();
	if (ret)
		goto err;

#ifdef CONFIG_CHAIN_OF_TRUST
	pfe_esbc_hdr = (uintptr_t)pfe_esbc_hdr_addr;
	pfe_img_addr = (uintptr_t)pfe_fit_addr;
	if (fsl_check_boot_mode_secure() != 0) {
		/*
		 * In case of failure in validation, fsl_secboot_validate
		 * would not return back in case of Production environment
		 * with ITS=1. In Development environment (ITS=0 and
		 * SB_EN=1), the function may return back in case of
		 * non-fatal failures.
		 */
		ret = fsl_secboot_validate(pfe_esbc_hdr,
					   PFE_KEY_HASH,
					   &pfe_img_addr);
		if (ret != 0)
			printf("PFE firmware(s) validation failed\n");
		else
			printf("PFE firmware(s) validation Successful\n");
	}
#endif

	p = env_get("load_util");
	if (!p) {
		max_fw_count = 2;
	} else {
		max_fw_count = simple_strtoul(p, NULL, 10);
		if (max_fw_count)
			max_fw_count = 3;
		else
			max_fw_count = 2;
	}

	for (fw_count = 0; fw_count < max_fw_count; fw_count++) {
		switch (fw_count) {
		case 0:
			pfe_firmware_name = "class_slowpath";
			break;
		case 1:
			pfe_firmware_name = "tmu_slowpath";
			break;
		case 2:
			pfe_firmware_name = "util_slowpath";
			break;
		}

		if (pfe_get_fw(&raw_image_addr, &raw_image_size,
			       pfe_firmware_name)) {
			printf("%s firmware couldn't be found in FIT image\n",
			       pfe_firmware_name);
			break;
		}
		pfe_firmware = malloc(raw_image_size);
		if (!pfe_firmware)
			return -ENOMEM;
		memcpy((void *)pfe_firmware, (void *)raw_image_addr,
		       raw_image_size);

		switch (fw_count) {
		case 0:
			env_set_addr("class_elf_firmware", pfe_firmware);
			env_set_addr("class_elf_size", (void *)raw_image_size);
			break;
		case 1:
			env_set_addr("tmu_elf_firmware", pfe_firmware);
			env_set_addr("tmu_elf_size", (void *)raw_image_size);
			break;
		case 2:
			env_set_addr("util_elf_firmware", pfe_firmware);
			env_set_addr("util_elf_size", (void *)raw_image_size);
			break;
		}
	}

	raw_image_addr = NULL;
	pfe_firmware = NULL;
	raw_image_size = 0;
	for (fw_count = 0; fw_count < 2; fw_count++) {
		if (fw_count == 0)
			pfe_firmware_name = "class";
		else if (fw_count == 1)
			pfe_firmware_name = "tmu";

		pfe_get_fw(&raw_image_addr, &raw_image_size, pfe_firmware_name);
		pfe_firmware = malloc(raw_image_size);
		if (!pfe_firmware)
			return -ENOMEM;
		memcpy((void *)pfe_firmware, (void *)raw_image_addr,
		       raw_image_size);

		if (fw_count == 0)
			ret = pfe_load_elf(CLASS_MASK, pfe_firmware);
		else if (fw_count == 1)
			ret = pfe_load_elf(TMU_MASK, pfe_firmware);

		if (ret < 0) {
			printf("%s: %s firmware load failed\n", __func__,
			       pfe_firmware_name);
			goto err;
		}
		debug("%s: %s firmware loaded\n", __func__, pfe_firmware_name);
		free(pfe_firmware);
	}

	tmu_enable(0xb);
	class_enable();
	gpi_enable(HGPI_BASE_ADDR);

err:
	return ret;
}

/*
 * PFE firmware cleanup
 * Puts PE's in reset
 */
void pfe_firmware_exit(void)
{
	debug("%s\n", __func__);

	class_disable();
	tmu_disable(0xf);
	hif_tx_disable();
	hif_rx_disable();
}
