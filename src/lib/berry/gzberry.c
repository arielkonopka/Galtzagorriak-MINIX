/* berry: the Berry launcher for Galtzagorriak MINIX (spec Phase 7c,
 * R7c.1, R7c.4, R7c.5).  Replaces upstream default/berry.c.
 *
 *   berry [-m heap] [-n budget] script.be|script.bec [args...]
 *   berry [-m heap] [-n budget] -e 'code'
 *   berry -i                      the REPL on the console (R7c.5)
 *   berryc -c out.bec script.be   host only (BE_HOST): bytecode (R7c.2)
 *
 * -m: heap limit in bytes (R7c.4; default 8192, 0 = none; the package
 *     manifest's value is passed here by the launcher line).
 * -n: instruction budget in units of 1024 VM instructions per run
 *     (default 4096, about 4 M instructions; 0 = none).  A runaway loop
 *     is stopped with the error "budget_error", the process exits with
 *     status 3, nothing else is affected.
 * Exit status: 0 ok, 1 exception, 2 usage, 3 budget, 4 heap, 5 i/o.
 */
#include "berry.h"
#include "be_repl.h"
#include "be_vm.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEF_HEAP	8192
#define DEF_BUDGET	4096

static unsigned long budget, left;
static int over;

/* The VM calls this every 1024 instructions (BE_VM_OBSERVABILITY_
 * SAMPLING in berry_conf.h).  No system call, no time read: a counter. */
static void obs(bvm *vm, int event, ...)
{
	if (event != BE_OBS_VM_HEARTBEAT || budget == 0) return;
	if (left > 0) { left--; return; }
	over = 1;
	be_raise(vm, "budget_error", "instruction budget spent");
}

static int result(bvm *vm, int res)
{
	switch (res) {
	case BE_OK: return 0;
	case BE_EXIT: return be_toindex(vm, -1);
	case BE_EXCEPTION:
		be_dumpexcept(vm);
		return over ? 3 : 1;
	case BE_MALLOC_FAIL:
		fputs("berry: heap limit reached\n", stderr);
		return 4;
	case BE_IO_ERROR:
		fprintf(stderr, "berry: %s\n", be_tostring(vm, -1));
		return 5;
	default: return 1;
	}
}

static int run(bvm *vm, int res)
{
	if (res == BE_OK) {
		left = budget; over = 0;
		res = be_pcall(vm, 0);
	}
	return result(vm, res);
}

#if BE_USE_SCRIPT_COMPILER
static char line[128];
static char *getl(const char *prompt)
{
	size_t n;

	fputs(prompt, stdout);
	fflush(stdout);
	if (fgets(line, sizeof(line), stdin) == NULL) return NULL;
	n = strlen(line);
	if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
	left = budget; over = 0;	/* a fresh budget per line */
	return line;
}
static void freel(char *p) { (void)p; }
#endif

static void args(bvm *vm, int argc, char **argv)
{
	be_newobject(vm, "list");
	while (argc-- > 0) {
		be_pushstring(vm, *argv++);
		be_data_push(vm, -2);
		be_pop(vm, 1);
	}
	be_pop(vm, 1);
	be_setglobal(vm, "_argv");
	be_pop(vm, 1);
}

static int usage(void)
{
	fputs("usage: berry [-m heap] [-n budget] [-i] [-e code] "
#ifdef BE_HOST
	      "[-c out.bec] "
#endif
	      "[script [args]]\n", stderr);
	return 2;
}

int main(int argc, char **argv)
{
	bvm *vm;
	const char *code = NULL, *out = NULL;
	int i, repl = 0, res = 0;

	be_heap_limit = DEF_HEAP;
	budget = DEF_BUDGET;
	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		char o = argv[i][1];
		if (o == 'i') { repl = 1; continue; }
		if (i + 1 >= argc) return usage();
		switch (o) {
		case 'm': be_heap_limit = strtoul(argv[++i], NULL, 0); break;
		case 'n': budget = strtoul(argv[++i], NULL, 0); break;
		case 'e': code = argv[++i]; break;
#ifdef BE_HOST
		case 'c': out = argv[++i]; break;
#endif
		default: return usage();
		}
	}
	if (out && i >= argc) return usage();
	if (!code && !repl && i >= argc) return usage();

	vm = be_vm_new();
	if (vm == NULL) {
		fputs("berry: no memory\n", stderr);
		return 4;
	}
	be_set_obs_hook(vm, obs);
	args(vm, argc - i, argv + i);
#ifdef BE_HOST
	if (out) {
		res = be_loadmode(vm, argv[i], 0);
		if (res == BE_OK) res = be_savecode(vm, out);
		res = result(vm, res);
		be_vm_delete(vm);
		return res;
	}
#endif
#if BE_USE_SCRIPT_COMPILER
	if (code) res = run(vm, be_loadstring(vm, code));
#else
	if (code) { fputs("berry: no compiler\n", stderr); res = 2; }
#endif
	if (res == 0 && i < argc)
		res = run(vm, be_loadmode(vm, argv[i], 0));
#if BE_USE_SCRIPT_COMPILER
	if (res == 0 && repl) {
		puts("Berry " BERRY_VERSION " (-m " "heap, -n budget); Ctrl-D ends");
		if (be_repl(vm, getl, freel) == -BE_MALLOC_FAIL) res = 4;
	}
#endif
	be_vm_delete(vm);
	return res;
}
