/* Berry configuration for Galtzagorriak MINIX (spec Phase 7c, R7c.1).
 * Upstream: default/berry_conf.h (MIT, see LICENSE).  One file for the
 * device and for the host tool `berryc' (BE_HOST, tools/berryc): the
 * host build keeps the compiler and the bytecode saver (R7c.2), the
 * device build keeps only what scripts need.  RAM is the tight
 * constraint (strategy.md section 5): 32-bit ints, single float, small
 * stack, no file/os/time/json/debug/solidify/introspect modules.
 */
#ifndef BERRY_CONF_H
#define BERRY_CONF_H

#include <assert.h>

#define BE_DEBUG			0
#define BE_INTGER_TYPE			0	/* int: 32 bit on host and device,
						 * so host bytecode loads */
#define BE_USE_SINGLE_FLOAT		1	/* the FPU is single precision */
#define BE_BYTES_MAX_SIZE		(4*1024)
#define BE_USE_PRECOMPILED_OBJECT	1	/* const tables in flash */
#define BE_DEBUG_SOURCE_FILE		0
#define BE_DEBUG_RUNTIME_INFO		1	/* line numbers in errors */
#define BE_DEBUG_VAR_INFO		0
/* The instruction budget (R7c.4): the VM counts instructions and calls
 * the observability hook every 2^(N-1) of them (be_vm.c VM_HEARTBEAT);
 * bemin.c raises an error there when the budget is spent. */
#define BE_USE_PERF_COUNTERS		1
#define BE_VM_OBSERVABILITY_SAMPLING	11	/* hook every 1024 ins */
#define BE_STACK_TOTAL_MAX		600	/* slots, 8 B each: 4.8 KB max */
#define BE_STACK_FREE_MIN		10
#define BE_STACK_START			40
#define BE_CONST_SEARCH_SIZE		50
#define BE_USE_STR_HASH_CACHE		0
#define BE_USE_FILE_SYSTEM		1	/* load scripts and bytecode */
#ifdef BE_HOST
#define BE_USE_SCRIPT_COMPILER		1
#define BE_USE_BYTECODE_SAVER		1
#else
/* The compiler stays on the device for now (the REPL, R7c.5, and
 * source scripts); BE_NO_COMPILER=1 leaves it out once every script is
 * installed as bytecode (R7c.2). */
#ifdef BE_NO_COMPILER
#define BE_USE_SCRIPT_COMPILER		0
#else
#define BE_USE_SCRIPT_COMPILER		1
#endif
#define BE_USE_BYTECODE_SAVER		0
#endif
#define BE_USE_BYTECODE_LOADER		1
#define BE_USE_SHARED_LIB		0
#define BE_USE_OVERLOAD_HASH		1
#define BE_MAX_PARSER_DEPTH		12
#define BE_USE_DEBUG_HOOK		0
#define BE_USE_DEBUG_GC			0
#define BE_USE_DEBUG_STACK		0
#define BE_USE_MEM_ALIGNED		0

#define BE_USE_STRING_MODULE		1
#define BE_USE_MATH_MODULE		0	/* no float libm yet (gzmath.h) */
#define BE_USE_GLOBAL_MODULE		1
#define BE_USE_SYS_MODULE		1
#define BE_USE_GC_MODULE		1
#define BE_USE_JSON_MODULE		0
#define BE_USE_TIME_MODULE		0
#define BE_USE_OS_MODULE		0
#define BE_USE_DEBUG_MODULE		0
#define BE_USE_INTROSPECT_MODULE	0
#define BE_USE_STRICT_MODULE		0
/* solidify (bytecode as C constants in flash, R7c.2) comes with the
 * shared library slot; coc's tables must match on host and device. */
#define BE_USE_SOLIDIFY_MODULE		0

/* The per-script heap limit (R7c.4) is a check in be_realloc()
 * (src/be_mem.c, be_heap_limit), not an allocator of its own. */
#define BE_EXPLICIT_ABORT		abort
#define BE_EXPLICIT_EXIT		exit
#define BE_EXPLICIT_MALLOC		malloc
#define BE_EXPLICIT_FREE		free
#define BE_EXPLICIT_REALLOC		realloc
#include <stddef.h>
extern size_t be_heap_limit;

#define be_assert(expr)			((void)0)
#endif
