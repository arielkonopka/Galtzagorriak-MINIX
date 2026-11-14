#include <lib.h>
#define execl	_execl
#define execle	_execle
#define execv	_execv
#define execve	_execve

#include <minix/minlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

#define	PTRSIZE	(sizeof(char *))

/* execl() and execle() used to pass their va_list straight on as the
 * argv array.  That only works if all arguments lie on the stack in order,
 * which is not true when arguments are passed in registers (ARM, and gcc's
 * va_list in general).  Copy them into a real array instead.
 */
#define MAX_LARGS	256		/* max arguments to execl/execle */

PUBLIC int execl(const char *name, const char *arg, ...)
{
  va_list argp;
  char *argv[MAX_LARGS + 1];
  int n = 0;

  va_start(argp, arg);
  argv[n] = (char *) arg;
  while (argv[n] != NIL_PTR) {
	if (++n == MAX_LARGS) { va_end(argp); errno = E2BIG; return(-1); }
	argv[n] = va_arg(argp, char *);
  }
  va_end(argp);
  return(execve(name, argv, environ));
}


PUBLIC int execle(const char *name, const char *arg, ...)
{
  va_list argp;
  char *argv[MAX_LARGS + 1];
  char **envp;
  int n = 0;

  va_start(argp, arg);
  argv[n] = (char *) arg;
  while (argv[n] != NIL_PTR) {
	if (++n == MAX_LARGS) { va_end(argp); errno = E2BIG; return(-1); }
	argv[n] = va_arg(argp, char *);
  }
  envp = va_arg(argp, char **);		/* follows the NULL */
  va_end(argp);
  return(execve(name, argv, envp));
}


PUBLIC int execv(name, argv)
_CONST char *name;
char * _CONST argv[];
{
  return(execve(name, argv, environ));
}


PUBLIC int execve(path, argv, envp)
_CONST char *path;		/* pointer to name of file to be executed */
char * _CONST argv[];		/* pointer to argument array */
char * _CONST envp[];		/* pointer to environment */
{
  int i, j;

  /* Count the argument pointers and environment pointers. */
  i = 0;
  if (argv != NULL)
  {
	while (argv[i] != NULL) i++;
  }
  j = 0;
  if (envp != NULL)
  {
	while (envp[j] != NULL) j++;
  }

  return(__execve(path, argv, envp, i, j));
}


PUBLIC int __execve(path, argv, envp, nargs, nenvps)
_CONST char *path;		/* pointer to name of file to be executed */
char * _CONST argv[];		/* pointer to argument array */
char * _CONST envp[];		/* pointer to environment */
int nargs;			/* number of args */
int nenvps;			/* number of environment strings */
{
/* This is split off from execve to be called from execvp, so execvp does not
 * have to allocate up to ARG_MAX bytes just to prepend "sh" to the arg array.
 */

  char **ap;
  int i, stackbytes, npointers, overflow, temp;
  message m;

  /* Decide how big a stack is needed. Be paranoid about overflow. */
  overflow = FALSE;
  npointers = 1 + nargs + 1 + nenvps + 1;	/* 1's for argc and NULLs */
  stackbytes = nargs + nenvps;		/* 1 byte for each null in strings */
  if (nargs < 0 || nenvps < 0 || stackbytes < nargs || npointers < stackbytes)
	overflow = TRUE;
  for (i = PTRSIZE; i != 0; i--) {
	temp = stackbytes + npointers;
	if (temp < stackbytes) overflow = TRUE;
	stackbytes = temp;
  }
  ap = (char **) argv;
  for (i = 0; i < nargs; i++) {
	temp = stackbytes + strlen(*ap++);
	if (temp < stackbytes) overflow = TRUE;
	stackbytes = temp;
  }
  ap = (char **) envp;
  for (i = 0; i < nenvps; i++) {
	temp = stackbytes + strlen(*ap++);
	if (temp < stackbytes) overflow = TRUE;
	stackbytes = temp;
  }
  temp = stackbytes + PTRSIZE - 1;
  if (temp < stackbytes) overflow = TRUE;
  stackbytes = (temp / PTRSIZE) * PTRSIZE;

  /* Check for overflow. */
  if (overflow) {
	errno = E2BIG;
	return(-1);
  }

  /* MM gathers the arguments and the environment from argv and envp
   * itself (mm/exec.c, gather()), so no stack image is built here: a
   * process short of memory (a shell after a big glob) can still exec.
   * stackbytes is only a size check for MM.
   */
  m.m1_i1 = strlen(path) + 1;
  m.m1_i2 = stackbytes;
  m.m1_i3 = (int) envp;
  m.m1_p1 = (char *) path;
  m.m1_p2 = NULL;
  m.m1_p3 = (char *) argv;
  (void) _syscall(MM, EXEC, &m);

  /* The exec failed. */
  return(m.m_type);
}
