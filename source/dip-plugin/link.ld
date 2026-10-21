OUTPUT_FORMAT("elf32-bigarm")
OUTPUT_ARCH(arm)
ENTRY(_start)
 
/* Sections area */ 
/*
 * d2x-cios-ftp: the code region ("exe") was 0x2000 bytes upstream and
 * the network backend (net.c) doesn't fit in it, so it grows to 0x3000
 * and the data region ("ram") starts 0x1000 later. The end of the plugin
 * memory is unchanged (0x13842000), so nothing else moves: the FFS plugin
 * starts at 0x13843000. RAM use is ~0x3C900 of 0x3F000 (the fraglist is
 * most of it, and net.c reuses it as its receive buffer).
 */
MEMORY {
	table		: ORIGIN = 0x0, LENGTH = 0x4000
	exe(rwx)	: ORIGIN = 0x13800000, LENGTH = 0x3000
	ram(rw)		: ORIGIN = 0x13803000, LENGTH = 0x3F000
}

__exe_start_phys__		= 0x13800000;
__ram_start_phys__		= 0x13803000;

SECTIONS {
	.ios_info_table : {
		KEEP (*(.ios_info_table))
	} > table

	.init : AT (__exe_start_phys__) {
		*(.init)
		. = ALIGN(4);
	} > exe

	.text : { 
		*(.text*) 
		*(.gnu.warning) 
		*(.gnu.linkonce.t.*) 
		*(.init) 
		*(.glue_7) 
		*(.glue_7t)
		. = ALIGN(4);
	} > exe

	.data : AT (__ram_start_phys__) {
		*(.data*)
		*(.data.*)
		*(.gnu.linkonce.d.*)
		. = ALIGN(4);
	} > ram

	.rodata : { 
		*(.rodata)
		*all.rodata*(*)
		*(.roda)
		*(.rodata.*) 
		*(.gnu.linkonce.r.*)
		. = ALIGN(4);
	} > ram

	.bss : {
		*(.dynsbss)
		*(.gnu.linkonce.sb.*)
		*(.bss*)
		*(COMMON)
		KEEP(*(.ios_bss))
		. = ALIGN(4);
	} > ram
}
