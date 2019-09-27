/*
 * Copyright (c) 2018 naehrwert
 *
 * Copyright (c) 2018-2019 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "config/config.h"
#include "gfx/di.h"
#include "gfx/gfx.h"
#include "gfx/tui.h"
#include "hos/pkg1.h"
#include "libs/fatfs/ff.h"
#include "mem/heap.h"
#include "mem/minerva.h"
#include "power/max77620.h"
#include "rtc/max77620-rtc.h"
#include "soc/bpmp.h"
#include "soc/hw_init.h"
#include "storage/emummc.h"
#include "storage/nx_emmc.h"
#include "storage/sdmmc.h"
#include "utils/sprintf.h"
#include "utils/util.h"

#include "keys/keys.h"

sdmmc_t sd_sdmmc;
sdmmc_storage_t sd_storage;
__attribute__ ((aligned (16))) FATFS sd_fs;
static bool sd_mounted;

hekate_config h_cfg;
boot_cfg_t __attribute__((section ("._boot_cfg"))) b_cfg;

bool sd_mount()
{
    if (sd_mounted)
        return true;

    if (!sdmmc_storage_init_sd(&sd_storage, &sd_sdmmc, SDMMC_1, SDMMC_BUS_WIDTH_4, 11))
    {
        EPRINTF("Failed to init SD card.\nMake sure that it is inserted.\nOr that SD reader is properly seated!");
    }
    else
    {
        int res = 0;
        res = f_mount(&sd_fs, "sd:", 1);
        if (res == FR_OK)
        {
            sd_mounted = 1;
            return true;
        }
        else
        {
            EPRINTFARGS("Failed to mount SD card (FatFS Error %d).\nMake sure that a FAT partition exists..", res);
        }
    }

    return false;
}

void sd_unmount()
{
    if (sd_mounted)
    {
        f_mount(NULL, "sd:", 1);
        sdmmc_storage_end(&sd_storage);
        sd_mounted = false;
    }
}

void *sd_file_read(const char *path, u32 *fsize)
{
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK)
        return NULL;

    u32 size = f_size(&fp);
    if (fsize)
        *fsize = size;

    void *buf = malloc(size);

    if (f_read(&fp, buf, size, NULL) != FR_OK)
    {
        free(buf);
        f_close(&fp);

        return NULL;
    }

    f_close(&fp);

    return buf;
}

int sd_save_to_file(void *buf, u32 size, const char *filename)
{
    FIL fp;
    u32 res = 0;
    res = f_open(&fp, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (res)
    {
        EPRINTFARGS("Error (%d) creating file\n%s.\n", res, filename);
        return res;
    }

    f_write(&fp, buf, size, NULL);
    f_close(&fp);

    return 0;
}

// This is a safe and unused DRAM region for our payloads.
#define RELOC_META_OFF      0x7C
#define PATCHED_RELOC_SZ    0x94
#define PATCHED_RELOC_STACK 0x40007000
#define COREBOOT_ADDR       (0xD0000000 - 0x100000)
#define CBFS_DRAM_EN_ADDR   0x4003e000
#define  CBFS_DRAM_MAGIC    0x4452414D // "DRAM"

void reloc_patcher(u32 payload_dst, u32 payload_src, u32 payload_size)
{
    memcpy((u8 *)payload_src, (u8 *)IPL_LOAD_ADDR, PATCHED_RELOC_SZ);

    volatile reloc_meta_t *relocator = (reloc_meta_t *)(payload_src + RELOC_META_OFF);

    relocator->start = payload_dst - ALIGN(PATCHED_RELOC_SZ, 0x10);
    relocator->stack = PATCHED_RELOC_STACK;
    relocator->end   = payload_dst + payload_size;
    relocator->ep    = payload_dst;

    if (payload_size == 0x7000)
    {
        memcpy((u8 *)(payload_src + ALIGN(PATCHED_RELOC_SZ, 0x10)), (u8 *)COREBOOT_ADDR, 0x7000); //Bootblock
        *(vu32 *)CBFS_DRAM_EN_ADDR = CBFS_DRAM_MAGIC;
    }
}

void dump_sysnand()
{
    h_cfg.emummc_force_disable = true;
    b_cfg.extra_cfg &= ~EXTRA_CFG_DUMP_EMUMMC;
    dump_keys();
}

void dump_emunand()
{
    if (h_cfg.emummc_force_disable)
        return;
    emu_cfg.enabled = 1;
    b_cfg.extra_cfg |= EXTRA_CFG_DUMP_EMUMMC;
    dump_keys();
}

ment_t ment_top[] = {
    MDEF_HANDLER("Dump from SysNAND | Key generation: unk", dump_sysnand, COLOR_RED),
    MDEF_HANDLER("Dump from EmuNAND | Key generation: unk", dump_emunand, COLOR_ORANGE),
    MDEF_CAPTION("---------------", COLOR_YELLOW),
    MDEF_HANDLER("Reboot (Normal)", reboot_normal, COLOR_GREEN),
    MDEF_HANDLER("Reboot (RCM)", reboot_rcm, COLOR_BLUE),
    MDEF_HANDLER("Power off", power_off, COLOR_VIOLET),
    MDEF_END()
};

menu_t menu_top = { ment_top, NULL, 0, 0 };

void _get_key_generations(char *sysnand_label, char *emunand_label) {
    sdmmc_t sdmmc;
    sdmmc_storage_t storage;
    sdmmc_storage_init_mmc(&storage, &sdmmc, SDMMC_4, SDMMC_BUS_WIDTH_8, 4);
    u8 *pkg1 = (u8 *)malloc(NX_EMMC_BLOCKSIZE);
    sdmmc_storage_set_mmc_partition(&storage, 1);
    sdmmc_storage_read(&storage, 0x100000 / NX_EMMC_BLOCKSIZE, 1, pkg1);
    const pkg1_id_t *pkg1_id = pkg1_identify(pkg1);
    sdmmc_storage_end(&storage);

    if (pkg1_id)
        sprintf(sysnand_label + 36, "% 3d", pkg1_id->kb);
    ment_top[0].caption = sysnand_label;
    if (h_cfg.emummc_force_disable) {
        free(pkg1);
        return;
    }

    emummc_storage_init_mmc(&storage, &sdmmc);
    memset(pkg1, 0, NX_EMMC_BLOCKSIZE);
    emummc_storage_set_mmc_partition(&storage, 1);
    emummc_storage_read(&storage, 0x100000 / NX_EMMC_BLOCKSIZE, 1, pkg1);
    pkg1_id = pkg1_identify(pkg1);
    emummc_storage_end(&storage);

    if (pkg1_id)
        sprintf(emunand_label + 36, "% 3d", pkg1_id->kb);
    free(pkg1);
    ment_top[1].caption = emunand_label;
}

#define IPL_STACK_TOP  0x90010000
#define IPL_HEAP_START 0x90020000

extern void pivot_stack(u32 stack_top);

void ipl_main()
{
    config_hw();
    pivot_stack(IPL_STACK_TOP);
    heap_init(IPL_HEAP_START);

    set_default_configuration();

    sd_mount();
    minerva_init();
    minerva_change_freq(FREQ_1600);

    display_init();
    u32 *fb = display_init_framebuffer();
    gfx_init_ctxt(fb, 720, 1280, 720);
    gfx_con_init();
    display_backlight_pwm_init();

    bpmp_clk_rate_set(BPMP_CLK_SUPER_BOOST);

    h_cfg.emummc_force_disable = emummc_load_cfg();

    if (b_cfg.boot_cfg & BOOT_CFG_SEPT_RUN)
    {
        if (!(b_cfg.extra_cfg & EXTRA_CFG_DUMP_EMUMMC))
            h_cfg.emummc_force_disable = true;
        dump_keys();
    }

    if (h_cfg.emummc_force_disable)
    {
        ment_top[1].type = MENT_CAPTION;
        ment_top[1].color = 0xFF555555;
        ment_top[1].handler = NULL;
    }

    _get_key_generations((char *)ment_top[0].caption, (char *)ment_top[1].caption);

    while (true)
        tui_do_menu(&menu_top);

    while (true)
        bpmp_halt();
}
