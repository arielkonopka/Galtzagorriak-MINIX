#ifndef _CONFIG_H
#define _CONFIG_H

/* Minix release and version numbers. */
#define OS_RELEASE "2.0"
#define OS_VERSION "0"

/* This file sets configuration parameters for the MINIX kernel, FS, and MM.
 * It is divided up into two main sections.  The first section contains
 * user-settable parameters.  In the second section, various internal system
 * parameters are set based on the user-settable parameters.
 */

/*===========================================================================*
 *		This section contains user-settable parameters		     *
 *===========================================================================*/
#ifndef MACHINE
#define MACHINE       IBM_PC	/* Must be one of the names listed below */
#endif

#define IBM_PC             1	/* any  8088 or 80x86-based system */
#define SUN_4             40	/* any Sun SPARC-based system */
#define SUN_4_60	  40	/* Sun-4/60 (aka SparcStation 1 or Campus) */
#define ATARI             60	/* ATARI ST/STe/TT (68000/68030) */
#define AMIGA             61	/* Commodore Amiga (68000) */
#define MACINTOSH         62	/* Apple Macintosh (68000) */
#define FLIPPER           70	/* Flipper Zero (STM32WB55, Cortex-M4) */

/* Word size in bytes (a constant equal to sizeof(int)). */
#if __ACK__
#define _WORD_SIZE	_EM_WSIZE
#endif
#if __GNUC__
#define _WORD_SIZE	__SIZEOF_INT__
#endif


/* If ROBUST is set to 1, writes of i-node, directory, and indirect blocks
 * from the cache happen as soon as the blocks are modified.  This gives a more
 * robust, but slower, file system.  If it is set to 0, these blocks are not
 * given any special treatment, which may cause problems if the system crashes.
 */
#define ROBUST             0	/* 0 for speed, 1 for robustness */

/* Number of slots in the process table for user processes. */
#if (MACHINE == FLIPPER)
#define NR_PROCS          16	/* RAM is tight: ~212 KB in total */
#else
#define NR_PROCS          32
#endif

/* The buffer cache should be made as large as you can afford. */
#if (MACHINE == IBM_PC && _WORD_SIZE == 2)
#define NR_BUFS           40	/* # blocks in the buffer cache */
#define NR_BUF_HASH       64	/* size of buf hash table; MUST BE POWER OF 2*/
#endif

#if (MACHINE == IBM_PC && _WORD_SIZE == 4)
#define NR_BUFS           80	/* # blocks in the buffer cache */
#define NR_BUF_HASH      128	/* size of buf hash table; MUST BE POWER OF 2*/
#endif

#if (MACHINE == SUN_4_60)
#define NR_BUFS		 512	/* # blocks in the buffer cache (<=1536) */
#define NR_BUF_HASH	 512	/* size of buf hash table; MUST BE POWER OF 2*/
#endif

#if (MACHINE == FLIPPER)
#define NR_BUFS           18	/* # blocks in the buffer cache (SRAM2) */
#define NR_BUF_HASH       32	/* size of buf hash table; MUST BE POWER OF 2*/
#endif

#if (MACHINE == ATARI)
#define NR_BUFS		1536	/* # blocks in the buffer cache (<=1536) */
#define NR_BUF_HASH	2048	/* size of buf hash table; MUST BE POWER OF 2*/
#endif

/* Defines for kernel configuration. */
#define AUTO_BIOS          0	/* xt_wini.c - use Western's autoconfig BIOS */
#define LINEWRAP           1	/* console.c - wrap lines at column 80 */
#define ALLOW_GAP_MESSAGES 1	/* proc.c - allow messages in the gap between
				 * the end of bss and lowest stack address */

/* Enable or disable the second level file system cache on the RAM disk. */
#if (MACHINE == FLIPPER)
#define ENABLE_CACHE2      0
#else
#define ENABLE_CACHE2      1
#endif

/* Include or exclude device drivers.  Set to 1 to include, 0 to exclude. */
#if (MACHINE == FLIPPER)
#define ENABLE_NETWORKING  0	/* no network hardware */
#define ENABLE_SDCARD      1	/* microSD over SPI, the WINCHESTER task */
#define ENABLE_MITSUMI_CDROM 0
#define ENABLE_SB_AUDIO    0
#define ENABLE_ADAPTEC_SCSI 0
#else
#define ENABLE_NETWORKING  0	/* enable TCP/IP code */
#define ENABLE_AT_WINI     1	/* enable AT winchester driver */
#define ENABLE_BIOS_WINI   1	/* enable BIOS winchester driver */
#define ENABLE_ESDI_WINI   1	/* enable ESDI winchester driver */
#define ENABLE_XT_WINI     0	/* enable XT winchester driver */
#define ENABLE_ADAPTEC_SCSI 1	/* enable ADAPTEC SCSI driver */
#define ENABLE_MITSUMI_CDROM 0	/* enable Mitsumi CD-ROM driver */
#define ENABLE_SB_AUDIO    0	/* enable Soundblaster audio driver */
#endif /* MACHINE == FLIPPER */

/* DMA_SECTORS may be increased to speed up DMA based drivers. */
#define DMA_SECTORS        1	/* DMA buffer size (must be >= 1) */

/* Include or exclude backwards compatibility code. */
#define ENABLE_BINCOMPAT   0	/* for binaries using obsolete calls */
#define ENABLE_SRCCOMPAT   0	/* for sources using obsolete calls */

/* Determine which device to use for pipes. */
#define PIPE_DEV    ROOT_DEV	/* put pipes on root device */

/* NR_CONS, NR_RS_LINES, and NR_PTYS determine the number of terminals the
 * system can handle.
 */
#if (MACHINE == FLIPPER)
#define NR_CONS            1	/* serial (USART1/USB) + LCD console */
#define	NR_RS_LINES	   0
#define	NR_PTYS		   0
#else
#define NR_CONS            2	/* # system consoles (1 to 8) */
#define	NR_RS_LINES	   2	/* # rs232 terminals (0, 1, or 2) */
#define	NR_PTYS		   0	/* # pseudo terminals (0 to 64) */
#endif

#if (MACHINE == ATARI)
/* The next define says if you have an ATARI ST or TT */
#define ATARI_TYPE	  TT
#define ST		   1	/* all ST's and Mega ST's */
#define STE		   2	/* all STe and Mega STe's */
#define TT		   3

/* if SCREEN is set to 1 graphical screen operations are possible */
#define SCREEN             1	

/* This define says whether the keyboard generates VT100 or IBM_PC escapes. */
#define KEYBOARD       VT100	/* either VT100 or IBM_PC */
#define VT100		 100

/* The next define determines the kind of partitioning. */
#define PARTITIONING   SUPRA	/* one of the following or ATARI */
#define SUPRA		   1	/*ICD, SUPRA and BMS are all the same */
#define BMS		   1
#define ICD		   1
#define CBHD		   2
#define EICKMANN	   3

/* Define the number of hard disk drives on your system. */
#define NR_ACSI_DRIVES	   3	/* typically 0 or 1 */
#define NR_SCSI_DRIVES	   1	/* typically 0 (ST, STe) or 1 (TT) */

/* Some systems need to have a little delay after each winchester
 * commands. These systems need FAST_DISK set to 0. Other disks do not
 * need this delay, and thus can have FAST_DISK set to 1 to avoid this delay.
 */
#define FAST_DISK	   1	/* 0 or 1 */

/* Note: if you want to make your kernel smaller, you can set NR_FD_DRIVES
 * to 0. You will still be able to boot minix.img from floppy. However, you
 * MUST fetch both the root and usr filesystem from a hard disk
 */

/* Define the number of floppy disk drives on your system. */
#define NR_FD_DRIVES	   1	/* 0, 1, 2 */

/* This configuration define controls parallel printer code. */
#define PAR_PRINTER	   1	/* disable (0) / enable (1) parallel printer */

/* This configuration define controls disk controller clock code. */
#define HD_CLOCK	   1	/* disable (0) / enable (1) hard disk clock */

#endif


/*===========================================================================*
 *	There are no user-settable parameters after this line		     *
 *===========================================================================*/
/* Set the CHIP type based on the machine selected. The symbol CHIP is actually
 * indicative of more than just the CPU.  For example, machines for which
 * CHIP == INTEL are expected to have 8259A interrrupt controllers and the
 * other properties of IBM PC/XT/AT/386 types machines in general. */
#define INTEL             1	/* CHIP type for PC, XT, AT, 386 and clones */
#define M68000            2	/* CHIP type for Atari, Amiga, Macintosh    */
#define SPARC             3	/* CHIP type for SUN-4 (e.g. SPARCstation)  */
#define ARM_CM            4	/* CHIP type for ARM Cortex-M (no MMU)      */

/* Set the FP_FORMAT type based on the machine selected, either hw or sw    */
#define FP_NONE		  0	/* no floating point support                */
#define FP_IEEE		  1	/* conform IEEE floating point standard     */

#if (MACHINE == IBM_PC)
#define CHIP          INTEL
#define SHADOWING	  0
#define ENABLE_WINI	(ENABLE_AT_WINI || ENABLE_BIOS_WINI || \
			ENABLE_ESDI_WINI || ENABLE_XT_WINI)
#define ENABLE_SCSI	(ENABLE_ADAPTEC_SCSI)
#define ENABLE_CDROM	(ENABLE_MITSUMI_CDROM)
#define ENABLE_AUDIO	(ENABLE_SB_AUDIO)
#endif

#if (MACHINE == ATARI) || (MACHINE == AMIGA) || (MACHINE == MACINTOSH)
#define CHIP         M68000
#define SHADOWING	  1
#endif

#if (MACHINE == FLIPPER)
#define CHIP          ARM_CM
#define SHADOWING	  1	/* no MMU: fork via shadowing, as on the ST */
#define ENABLE_WINI	(ENABLE_SDCARD)
#define ENABLE_SCSI	0
#define ENABLE_CDROM	0
#define ENABLE_AUDIO	0
#endif

#if (MACHINE == SUN_4) || (MACHINE == SUN_4_60)
#define CHIP          SPARC
#define FP_FORMAT   FP_IEEE
#define SHADOWING	  0
#endif

#if (MACHINE == ATARI) || (MACHINE == SUN_4)
#define ASKDEV            1	/* ask for boot device */
#define FASTLOAD          1	/* use multiple block transfers to init ram */
#endif

#if (MACHINE == ATARI) && (ATARI_TYPE == TT) /* and all other 68030's */
/* (Without the MACHINE test this was true on every non-Atari machine, since
 * the undefined ATARI_TYPE and TT both evaluate to 0.) */
#define FPP
#undef SHADOWING
#define SHADOWING 0
#endif

#ifndef FP_FORMAT
#define FP_FORMAT   FP_NONE
#endif

/* The file buf.h uses MAYBE_WRITE_IMMED. */
#if ROBUST
#define MAYBE_WRITE_IMMED  WRITE_IMMED	/* slower but perhaps safer */
#else
#define MAYBE_WRITE_IMMED 0		/* faster */
#endif

#ifndef MACHINE
error "In <minix/config.h> please define MACHINE"
#endif

#ifndef CHIP
error "In <minix/config.h> please define MACHINE to have a legal value"
#endif

#if (MACHINE == 0)
error "MACHINE has incorrect value (0)"
#endif

#endif /* _CONFIG_H */
