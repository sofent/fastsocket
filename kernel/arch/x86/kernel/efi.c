/*
 * Common EFI (Extensible Firmware Interface) support functions
 * Based on Extensible Firmware Interface Specification version 1.0
 *
 * Copyright (C) 1999 VA Linux Systems
 * Copyright (C) 1999 Walt Drummond <drummond@valinux.com>
 * Copyright (C) 1999-2002 Hewlett-Packard Co.
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *	Stephane Eranian <eranian@hpl.hp.com>
 * Copyright (C) 2005-2008 Intel Co.
 *	Fenghua Yu <fenghua.yu@intel.com>
 *	Bibo Mao <bibo.mao@intel.com>
 *	Chandramouli Narayanan <mouli@linux.intel.com>
 *	Huang Ying <ying.huang@intel.com>
 *
 * Copied from efi_32.c to eliminate the duplicated code between EFI
 * 32/64 support code. --ying 2007-10-26
 *
 * All EFI Runtime Services are not implemented yet as EFI only
 * supports physical mode addressing on SoftSDV. This is to be fixed
 * in a future version.  --drummond 1999-07-20
 *
 * Implemented EFI runtime services and virtual mode calls.  --davidm
 *
 * Goutham Rao: <goutham.rao@intel.com>
 *	Skip non-WB memory and ignore empty memory ranges.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/efi.h>
#include <linux/bootmem.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/time.h>
#include <linux/io.h>
#include <linux/reboot.h>
#include <linux/bcd.h>

#include <asm/setup.h>
#include <asm/efi.h>
#include <asm/time.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/x86_init.h>

#define EFI_DEBUG	1
#define PFX 		"EFI: "

int efi_enabled;
EXPORT_SYMBOL(efi_enabled);

struct efi efi;
EXPORT_SYMBOL(efi);

struct efi_memory_map memmap;

static struct efi efi_phys __initdata;
static efi_system_table_t efi_systab __initdata;
static efi_runtime_services_t phys_runtime;

static int __init setup_noefi(char *arg)
{
	efi_enabled = 0;
	return 0;
}
early_param("noefi", setup_noefi);

int add_efi_memmap;
EXPORT_SYMBOL(add_efi_memmap);

static int __init setup_add_efi_memmap(char *arg)
{
	add_efi_memmap = 1;
	return 0;
}
early_param("add_efi_memmap", setup_add_efi_memmap);


static efi_status_t virt_efi_get_time(efi_time_t *tm, efi_time_cap_t *tc)
{
	return efi_call_virt2(get_time, tm, tc);
}

static efi_status_t virt_efi_set_time(efi_time_t *tm)
{
	return efi_call_virt1(set_time, tm);
}

static efi_status_t virt_efi_get_wakeup_time(efi_bool_t *enabled,
					     efi_bool_t *pending,
					     efi_time_t *tm)
{
	return efi_call_virt3(get_wakeup_time,
			      enabled, pending, tm);
}

static efi_status_t virt_efi_set_wakeup_time(efi_bool_t enabled, efi_time_t *tm)
{
	return efi_call_virt2(set_wakeup_time,
			      enabled, tm);
}

static efi_status_t virt_efi_get_variable(efi_char16_t *name,
					  efi_guid_t *vendor,
					  u32 *attr,
					  unsigned long *data_size,
					  void *data)
{
	return efi_call_virt5(get_variable,
			      name, vendor, attr,
			      data_size, data);
}

static efi_status_t virt_efi_get_next_variable(unsigned long *name_size,
					       efi_char16_t *name,
					       efi_guid_t *vendor)
{
	return efi_call_virt3(get_next_variable,
			      name_size, name, vendor);
}

static efi_status_t virt_efi_set_variable(efi_char16_t *name,
					  efi_guid_t *vendor,
					  u32 attr,
					  unsigned long data_size,
					  void *data)
{
	return efi_call_virt5(set_variable,
			      name, vendor, attr,
			      data_size, data);
}

static efi_status_t virt_efi_query_variable_info(u32 attr,
						 u64 *storage_space,
						 u64 *remaining_space,
						 u64 *max_variable_size)
{
	if (efi.runtime_version < EFI_2_00_SYSTEM_TABLE_REVISION)
		return EFI_UNSUPPORTED;

	return efi_call_virt4(query_variable_info, attr, storage_space,
			      remaining_space, max_variable_size);
}

static efi_status_t virt_efi_get_next_high_mono_count(u32 *count)
{
	return efi_call_virt1(get_next_high_mono_count, count);
}

static void virt_efi_reset_system(int reset_type,
				  efi_status_t status,
				  unsigned long data_size,
				  efi_char16_t *data)
{
	efi_call_virt4(reset_system, reset_type, status,
		       data_size, data);
}

static efi_status_t virt_efi_update_capsule(efi_capsule_header_t **capsules,
					    unsigned long count,
					    unsigned long sg_list)
{
	if (efi.runtime_version < EFI_2_00_SYSTEM_TABLE_REVISION)
		return EFI_UNSUPPORTED;

	return efi_call_virt3(update_capsule, capsules, count, sg_list);
}

static efi_status_t virt_efi_query_capsule_caps(efi_capsule_header_t **capsules,
						unsigned long count,
						u64 *max_size,
						int *reset_type)
{
	if (efi.runtime_version < EFI_2_00_SYSTEM_TABLE_REVISION)
		return EFI_UNSUPPORTED;

	return efi_call_virt4(query_capsule_caps, capsules, count, max_size,
			      reset_type);
}

static efi_status_t __init phys_efi_set_virtual_address_map(
	unsigned long memory_map_size,
	unsigned long descriptor_size,
	u32 descriptor_version,
	efi_memory_desc_t *virtual_map)
{
	efi_status_t status;

	efi_call_phys_prelog();
	status = efi_call_phys4(efi_phys.set_virtual_address_map,
				memory_map_size, descriptor_size,
				descriptor_version, virtual_map);
	efi_call_phys_epilog();
	return status;
}

static efi_status_t __init phys_efi_get_time_early(efi_time_t *tm,
					     efi_time_cap_t *tc)
{
	efi_status_t status;

	efi_call_phys_prelog();
	status = efi_call_phys2(efi_phys.get_time, tm, tc);
	efi_call_phys_epilog();
	return status;
}

static efi_status_t phys_efi_get_time(efi_time_t *tm,
				      efi_time_cap_t *tc)
{
	efi_status_t status;

	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys2((void*)phys_runtime.get_time, tm, tc);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static efi_status_t __init phys_efi_set_time(efi_time_t *tm)
{
	efi_status_t status;

	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys1((void*)phys_runtime.set_time, tm);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static efi_status_t phys_efi_get_wakeup_time(efi_bool_t *enabled,
                                             efi_bool_t *pending,
                                             efi_time_t *tm)
{
	efi_status_t status;

	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys3((void*)phys_runtime.get_wakeup_time, enabled,
				pending, tm);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static efi_status_t phys_efi_set_wakeup_time(efi_bool_t enabled, efi_time_t *tm)
{
	efi_status_t status;
	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys2((void*)phys_runtime.set_wakeup_time, enabled,
				tm);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static efi_status_t phys_efi_get_variable(efi_char16_t *name,
					  efi_guid_t *vendor,
					  u32 *attr,
					  unsigned long *data_size,
					  void *data)
{
	efi_status_t status;
	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys5((void*)phys_runtime.get_variable, name, vendor,
				attr, data_size, data);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static efi_status_t phys_efi_get_next_variable(unsigned long *name_size,
					       efi_char16_t *name,
					       efi_guid_t *vendor)
{
	efi_status_t status;

	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys3((void*)phys_runtime.get_next_variable,
				name_size, name, vendor);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static efi_status_t phys_efi_set_variable(efi_char16_t *name,
					  efi_guid_t *vendor,
					  u32 attr,
					  unsigned long data_size,
					  void *data)
{
	efi_status_t status;
	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys5((void*)phys_runtime.set_variable, name,
				vendor, attr, data_size, data);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static efi_status_t phys_efi_get_next_high_mono_count(u32 *count)
{
	efi_status_t status;
	efi_call_phys_prelog_in_physmode();
	status = efi_call_phys1((void*)phys_runtime.get_next_high_mono_count,
				count);
	efi_call_phys_epilog_in_physmode();
	return status;
}

static void phys_efi_reset_system(int reset_type,
                                  efi_status_t status,
                                  unsigned long data_size,
                                  efi_char16_t *data)
{
	efi_call_phys_prelog_in_physmode();
	efi_call_phys4((void*)phys_runtime.reset_system, reset_type, status,
				data_size, data);
	efi_call_phys_epilog_in_physmode();
}

int efi_set_rtc_mmss(unsigned long nowtime)
{
	int real_seconds, real_minutes;
	efi_status_t 	status;
	efi_time_t 	eft;
	efi_time_cap_t 	cap;

	status = efi.get_time(&eft, &cap);
	if (status != EFI_SUCCESS) {
		printk(KERN_ERR "Oops: efitime: can't read time!\n");
		return -1;
	}

	real_seconds = nowtime % 60;
	real_minutes = nowtime / 60;
	if (((abs(real_minutes - eft.minute) + 15)/30) & 1)
		real_minutes += 30;
	real_minutes %= 60;
	eft.minute = real_minutes;
	eft.second = real_seconds;

	status = efi.set_time(&eft);
	if (status != EFI_SUCCESS) {
		printk(KERN_ERR "Oops: efitime: can't write time!\n");
		return -1;
	}
	return 0;
}

unsigned long efi_get_time(void)
{
	efi_status_t status;
	efi_time_t eft;
	efi_time_cap_t cap;

	status = efi.get_time(&eft, &cap);
	if (status != EFI_SUCCESS)
		printk(KERN_ERR "Oops: efitime: can't read time!\n");

	return mktime(eft.year, eft.month, eft.day, eft.hour,
		      eft.minute, eft.second);
}

/*
 * Tell the kernel about the EFI memory map.  This might include
 * more than the max 128 entries that can fit in the e820 legacy
 * (zeropage) memory map.
 */

static void __init do_add_efi_memmap(void)
{
	void *p;

	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		efi_memory_desc_t *md = p;
		unsigned long long start = md->phys_addr;
		unsigned long long size = md->num_pages << EFI_PAGE_SHIFT;
		int e820_type;

		switch (md->type) {
		case EFI_LOADER_CODE:
		case EFI_LOADER_DATA:
		case EFI_BOOT_SERVICES_CODE:
		case EFI_BOOT_SERVICES_DATA:
		case EFI_CONVENTIONAL_MEMORY:
			if (md->attribute & EFI_MEMORY_WB)
				e820_type = E820_RAM;
			else
				e820_type = E820_RESERVED;
			break;
		case EFI_ACPI_RECLAIM_MEMORY:
			e820_type = E820_ACPI;
			break;
		case EFI_ACPI_MEMORY_NVS:
			e820_type = E820_NVS;
			break;
		case EFI_UNUSABLE_MEMORY:
			e820_type = E820_UNUSABLE;
			break;
		default:
			/*
			 * EFI_RESERVED_TYPE EFI_RUNTIME_SERVICES_CODE
			 * EFI_RUNTIME_SERVICES_DATA EFI_MEMORY_MAPPED_IO
			 * EFI_MEMORY_MAPPED_IO_PORT_SPACE EFI_PAL_CODE
			 */
			e820_type = E820_RESERVED;
			break;
		}
		e820_add_region(start, size, e820_type);
		e820_saved_add_region(start, size, e820_type);
	}
	sanitize_e820_map(e820.map, ARRAY_SIZE(e820.map), &e820.nr_map);
}

void __init efi_reserve_early(void)
{
	unsigned long pmap;

#ifdef CONFIG_X86_32
	pmap = boot_params.efi_info.efi_memmap;
#else
	pmap = (boot_params.efi_info.efi_memmap |
		((__u64)boot_params.efi_info.efi_memmap_hi<<32));
#endif
	memmap.phys_map = (void *)pmap;
	memmap.nr_map = boot_params.efi_info.efi_memmap_size /
		boot_params.efi_info.efi_memdesc_size;
	memmap.desc_version = boot_params.efi_info.efi_memdesc_version;
	memmap.desc_size = boot_params.efi_info.efi_memdesc_size;
	reserve_early(pmap, pmap + memmap.nr_map * memmap.desc_size,
		      "EFI memmap");
}

#if EFI_DEBUG
static void __init print_efi_memmap(void)
{
	efi_memory_desc_t *md;
	void *p;
	int i;

	for (p = memmap.map, i = 0;
	     p < memmap.map_end;
	     p += memmap.desc_size, i++) {
		md = p;
		printk(KERN_INFO PFX "mem%02u: type=%u, attr=0x%llx, "
			"range=[0x%016llx-0x%016llx) (%lluMB)\n",
			i, md->type, md->attribute, md->phys_addr,
			md->phys_addr + (md->num_pages << EFI_PAGE_SHIFT),
			(md->num_pages >> (20 - EFI_PAGE_SHIFT)));
	}
}
#endif  /*  EFI_DEBUG  */

void __init efi_reserve_boot_services(void)
{
	void *p;

	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		efi_memory_desc_t *md = p;
		u64 start = md->phys_addr;
		u64 size = md->num_pages << EFI_PAGE_SHIFT;

		if (md->type != EFI_BOOT_SERVICES_CODE &&
		    md->type != EFI_BOOT_SERVICES_DATA)
			continue;

		/* Only reserve where possible:
		 * - Not within any already allocated areas
		 * - Not over any memory area (really needed, if above?)
		 * - Not within any part of the kernel
		 * - Not the bios reserved area
		 */

		if ((start+size >= virt_to_phys(_text) &&
		     start <= virt_to_phys(_end)) ||
		    !e820_all_mapped(start, start+size, E820_RAM)) {
			/* Could not reserve, skip it */
			md->num_pages = 0;
		} else {
			if (reserve_bootmem_generic(start, size,
						    BOOTMEM_EXCLUSIVE)) {
				md->num_pages = 0;
			}
		}
	}
}

static void __init efi_free_boot_services(void)
{
	void *p;

	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		efi_memory_desc_t *md = p;
		unsigned long long start = md->phys_addr;
		unsigned long long size = md->num_pages << EFI_PAGE_SHIFT;

		if (md->type != EFI_BOOT_SERVICES_CODE &&
		    md->type != EFI_BOOT_SERVICES_DATA)
			continue;

		free_bootmem_late(start, size);
	}
}

void __init efi_init(void)
{
	efi_config_table_t *config_tables;
	efi_runtime_services_t *runtime;
	efi_char16_t *c16;
	char vendor[100] = "unknown";
	int i = 0;
	void *tmp;

#ifdef CONFIG_X86_32
	efi_phys.systab = (efi_system_table_t *)boot_params.efi_info.efi_systab;
#else
	efi_phys.systab = (efi_system_table_t *)
		(boot_params.efi_info.efi_systab |
		 ((__u64)boot_params.efi_info.efi_systab_hi<<32));
#endif

	efi.systab = early_ioremap((unsigned long)efi_phys.systab,
				   sizeof(efi_system_table_t));
	if (efi.systab == NULL)
		printk(KERN_ERR "Couldn't map the EFI system table!\n");
	memcpy(&efi_systab, efi.systab, sizeof(efi_system_table_t));
	early_iounmap(efi.systab, sizeof(efi_system_table_t));
	efi.systab = &efi_systab;

	/*
	 * Verify the EFI Table
	 */
	if (efi.systab->hdr.signature != EFI_SYSTEM_TABLE_SIGNATURE)
		printk(KERN_ERR "EFI system table signature incorrect!\n");
	if ((efi.systab->hdr.revision >> 16) == 0)
		printk(KERN_ERR "Warning: EFI system table version "
		       "%d.%02d, expected 1.00 or greater!\n",
		       efi.systab->hdr.revision >> 16,
		       efi.systab->hdr.revision & 0xffff);

	/*
	 * Show what we know for posterity
	 */
	c16 = tmp = early_ioremap(efi.systab->fw_vendor, 2);
	if (c16) {
		for (i = 0; i < sizeof(vendor) - 1 && *c16; ++i)
			vendor[i] = *c16++;
		vendor[i] = '\0';
	} else
		printk(KERN_ERR PFX "Could not map the firmware vendor!\n");
	early_iounmap(tmp, 2);

	printk(KERN_INFO "EFI v%u.%.02u by %s\n",
	       efi.systab->hdr.revision >> 16,
	       efi.systab->hdr.revision & 0xffff, vendor);

	/*
	 * Let's see what config tables the firmware passed to us.
	 */
	config_tables = early_ioremap(
		efi.systab->tables,
		efi.systab->nr_tables * sizeof(efi_config_table_t));
	if (config_tables == NULL)
		printk(KERN_ERR "Could not map EFI Configuration Table!\n");

	printk(KERN_INFO);
	for (i = 0; i < efi.systab->nr_tables; i++) {
		if (!efi_guidcmp(config_tables[i].guid, MPS_TABLE_GUID)) {
			efi.mps = config_tables[i].table;
			printk(" MPS=0x%lx ", config_tables[i].table);
		} else if (!efi_guidcmp(config_tables[i].guid,
					ACPI_20_TABLE_GUID)) {
			efi.acpi20 = config_tables[i].table;
			printk(" ACPI 2.0=0x%lx ", config_tables[i].table);
		} else if (!efi_guidcmp(config_tables[i].guid,
					ACPI_TABLE_GUID)) {
			efi.acpi = config_tables[i].table;
			printk(" ACPI=0x%lx ", config_tables[i].table);
		} else if (!efi_guidcmp(config_tables[i].guid,
					SMBIOS_TABLE_GUID)) {
			efi.smbios = config_tables[i].table;
			printk(" SMBIOS=0x%lx ", config_tables[i].table);
#ifdef CONFIG_X86_UV
		} else if (!efi_guidcmp(config_tables[i].guid,
					UV_SYSTEM_TABLE_GUID)) {
			efi.uv_systab = config_tables[i].table;
			printk(" UVsystab=0x%lx ", config_tables[i].table);
#endif
		} else if (!efi_guidcmp(config_tables[i].guid,
					HCDP_TABLE_GUID)) {
			efi.hcdp = config_tables[i].table;
			printk(" HCDP=0x%lx ", config_tables[i].table);
		} else if (!efi_guidcmp(config_tables[i].guid,
					UGA_IO_PROTOCOL_GUID)) {
			efi.uga = config_tables[i].table;
			printk(" UGA=0x%lx ", config_tables[i].table);
		}
	}
	printk("\n");
	early_iounmap(config_tables,
			  efi.systab->nr_tables * sizeof(efi_config_table_t));

	/*
	 * Check out the runtime services table. We need to map
	 * the runtime services table so that we can grab the physical
	 * address of several of the EFI runtime functions, needed to
	 * set the firmware into virtual mode.
	 */
	runtime = early_ioremap((unsigned long)efi.systab->runtime,
				sizeof(efi_runtime_services_t));
	if (runtime != NULL) {
		/*
		 * We will only need *early* access to the following
		 * two EFI runtime services before set_virtual_address_map
		 * is invoked.
		 */
		efi_phys.get_time = (efi_get_time_t *)runtime->get_time;
		efi_phys.set_virtual_address_map =
			(efi_set_virtual_address_map_t *)
			runtime->set_virtual_address_map;
		/*
		 * Make efi_get_time can be called before entering
		 * virtual mode.
		 */
		efi.get_time = phys_efi_get_time_early;

		memcpy(&phys_runtime, runtime, sizeof(efi_runtime_services_t));
	} else
		printk(KERN_ERR "Could not map the EFI runtime service "
		       "table!\n");
	early_iounmap(runtime, sizeof(efi_runtime_services_t));

	/* Map the EFI memory map */
	memmap.map = early_ioremap((unsigned long)memmap.phys_map,
				   memmap.nr_map * memmap.desc_size);
	if (memmap.map == NULL)
		printk(KERN_ERR "Could not map the EFI memory map!\n");
	memmap.map_end = memmap.map + (memmap.nr_map * memmap.desc_size);

	if (add_efi_memmap)
		do_add_efi_memmap();

#ifdef CONFIG_X86_32
	x86_platform.get_wallclock = efi_get_time;
	x86_platform.set_wallclock = efi_set_rtc_mmss;
#endif

#if EFI_DEBUG
	print_efi_memmap();
#endif

#ifndef CONFIG_X86_64
	/*
	 * Only x86_64 supports physical mode as of now. Use virtual mode
	 * forcibly.
	 */
	usevirtefi = 1;
#endif
}

void __init efi_set_executable(efi_memory_desc_t *md, bool executable)
{
	u64 addr, npages;

	if (md->phys_addr == 0)
		return;

	addr = md->virt_addr;
	npages = md->num_pages;

	memrange_efi_to_native(&addr, &npages);

	if (executable)
		set_memory_x(addr, npages);
	else
		set_memory_nx(addr, npages);
}

static void __init runtime_code_page_mkexec(void)
{
	efi_memory_desc_t *md;
	void *p;

	/* Make EFI runtime service code area executable */
	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		md = p;

		if (md->type != EFI_RUNTIME_SERVICES_CODE)
			continue;

		efi_set_executable(md, true);
	}
}

/*
 * This function will switch the EFI runtime services to virtual mode.
 * Essentially, look through the EFI memmap and map every region that
 * has the runtime attribute bit set in its memory descriptor and update
 * that memory descriptor with the virtual address obtained from ioremap().
 * This enables the runtime services to be called without having to
 * thunk back into physical mode for every invocation.
 */
void __init efi_enter_virtual_mode(void)
{
	efi_memory_desc_t *md, *prev_md = NULL;
	efi_status_t status;
	unsigned long size;
	u64 end, systab, addr, npages, end_pfn;
	void *p, *va;

	efi.systab = NULL;

	/* Merge contiguous regions of the same type and attribute */
	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		u64 prev_size;
		md = p;

		if (!prev_md) {
			prev_md = md;
			continue;
		}

		if (prev_md->type != md->type ||
		    prev_md->attribute != md->attribute) {
			prev_md = md;
			continue;
		}

		prev_size = prev_md->num_pages << EFI_PAGE_SHIFT;

		if (md->phys_addr == (prev_md->phys_addr + prev_size)) {
			prev_md->num_pages += md->num_pages;
			md->type = EFI_RESERVED_TYPE;
			md->attribute = 0;
			continue;
		}
		prev_md = md;
	}

	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		md = p;
		if (!(md->attribute & EFI_MEMORY_RUNTIME) &&
		    md->type != EFI_BOOT_SERVICES_CODE &&
		    md->type != EFI_BOOT_SERVICES_DATA)
			continue;

		size = md->num_pages << EFI_PAGE_SHIFT;
		end = md->phys_addr + size;

		end_pfn = PFN_UP(end);
		if (end_pfn <= max_low_pfn_mapped
		    || (end_pfn > (1UL << (32 - PAGE_SHIFT))
			&& end_pfn <= max_pfn_mapped))
			va = __va(md->phys_addr);
		else
			va = efi_ioremap(md->phys_addr, size, md->type);

		md->virt_addr = (u64) (unsigned long) va;

		if (!va) {
			printk(KERN_ERR PFX "ioremap of 0x%llX failed!\n",
			       (unsigned long long)md->phys_addr);
			continue;
		}

		if ((md->attribute & EFI_MEMORY_UC) &&
		    !(md->attribute & EFI_MEMORY_WB)) {
			addr = md->virt_addr;
			npages = md->num_pages;
			memrange_efi_to_native(&addr, &npages);
			set_memory_uc(addr, npages);
		}

		systab = (u64) (unsigned long) efi_phys.systab;
		if (md->phys_addr <= systab && systab < end) {
			systab += md->virt_addr - md->phys_addr;
			efi.systab = (efi_system_table_t *) (unsigned long) systab;
		}
	}

	BUG_ON(!efi.systab);

	status = phys_efi_set_virtual_address_map(
		memmap.desc_size * memmap.nr_map,
		memmap.desc_size,
		memmap.desc_version,
		memmap.phys_map);

	if (status != EFI_SUCCESS) {
		printk(KERN_ALERT "Unable to switch EFI into virtual mode "
		       "(status=%lx)!\n", status);
		panic("EFI call to SetVirtualAddressMap() failed!");
	}

	/*
	 * Thankfully, it does seem that no runtime services other than
	 * SetVirtualAddressMap() will touch boot services code, so we can
	 * get rid of it all at this point
	 */
	efi_free_boot_services();

	/*
	 * Now that EFI is in virtual mode, update the function
	 * pointers in the runtime service table to the new virtual addresses.
	 *
	 * Call EFI services through wrapper functions.
	 */
	efi.runtime_version = efi_systab.hdr.revision;
	efi.get_time = virt_efi_get_time;
	efi.set_time = virt_efi_set_time;
	efi.get_wakeup_time = virt_efi_get_wakeup_time;
	efi.set_wakeup_time = virt_efi_set_wakeup_time;
	efi.get_variable = virt_efi_get_variable;
	efi.get_next_variable = virt_efi_get_next_variable;
	efi.set_variable = virt_efi_set_variable;
	efi.get_next_high_mono_count = virt_efi_get_next_high_mono_count;
	efi.reset_system = virt_efi_reset_system;
	efi.set_virtual_address_map = NULL;
	efi.query_variable_info = virt_efi_query_variable_info;
	efi.update_capsule = virt_efi_update_capsule;
	efi.query_capsule_caps = virt_efi_query_capsule_caps;
	if (__supported_pte_mask & _PAGE_NX)
		runtime_code_page_mkexec();
	early_iounmap(memmap.map, memmap.nr_map * memmap.desc_size);
	memmap.map = NULL;
}

void __init efi_setup_physical_mode(void)
{
#ifdef CONFIG_X86_64
	efi_pagetable_init();
#endif
	efi.get_time = phys_efi_get_time;
	efi.set_time = phys_efi_set_time;
	efi.get_wakeup_time = phys_efi_get_wakeup_time;
	efi.set_wakeup_time = phys_efi_set_wakeup_time;
	efi.get_variable = phys_efi_get_variable;
	efi.get_next_variable = phys_efi_get_next_variable;
	efi.set_variable = phys_efi_set_variable;
	efi.get_next_high_mono_count =
		phys_efi_get_next_high_mono_count;
	efi.reset_system = phys_efi_reset_system;
	efi.set_virtual_address_map = NULL; /* Not needed */

	early_iounmap(memmap.map, memmap.nr_map * memmap.desc_size);
	memmap.map = NULL;
}

/*
 * Convenience functions to obtain memory types and attributes
 */
u32 efi_mem_type(unsigned long phys_addr)
{
	efi_memory_desc_t *md;
	void *p;

	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		md = p;
		if ((md->phys_addr <= phys_addr) &&
		    (phys_addr < (md->phys_addr +
				  (md->num_pages << EFI_PAGE_SHIFT))))
			return md->type;
	}
	return 0;
}

u64 efi_mem_attributes(unsigned long phys_addr)
{
	efi_memory_desc_t *md;
	void *p;

	for (p = memmap.map; p < memmap.map_end; p += memmap.desc_size) {
		md = p;
		if ((md->phys_addr <= phys_addr) &&
		    (phys_addr < (md->phys_addr +
				  (md->num_pages << EFI_PAGE_SHIFT))))
			return md->attribute;
	}
	return 0;
}
