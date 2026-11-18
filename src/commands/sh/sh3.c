#define Extern extern
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <setjmp.h>
#include <stddef.h>
#include <time.h>
#include <sys/times.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include "sh.h"

/* -------- exec.c -------- */
/* #include "sh.h" */

/*
 * execute tree
 */

/* Galtzagorriak: const and in flash (no pointer table in .data);
 * "" where the shell prints nothing. */
static const char signame[][20] = {
	"Signal 0",
	"Hangup",
	"",		/* interrupt */
	"Quit",
	"Illegal instruction",
	"Trace/BPT trap",
	"Abort",
	"EMT trap",
	"Floating exception",
	"Killed",
	"Bus error",
	"Memory fault",
	"Bad system call",
	"",		/* broken pipe */
	"Alarm clock",
	"Terminated",
};
#define	NSIGNAL (sizeof(signame)/sizeof(signame[0]))


_PROTOTYPE(static int forkexec, (struct op *t, int *pin, int *pout, int act, char **wp, int *pforked ));
_PROTOTYPE(static int parent, (void));
_PROTOTYPE(static int vforkable, (struct ioword **iopp));
_PROTOTYPE(int iosetup, (struct ioword *iop, int pipein, int pipeout ));
_PROTOTYPE(static void echo, (char **wp ));
_PROTOTYPE(static struct op **find1case, (struct op *t, char *w ));
_PROTOTYPE(static struct op *findcase, (struct op *t, char *w ));
_PROTOTYPE(static void brkset, (struct brkcon *bc ));
_PROTOTYPE(static int dolabel, (void));
_PROTOTYPE(static int dochdir, (struct op *t ));
_PROTOTYPE(static int doshift, (struct op *t ));
_PROTOTYPE(static int dologin, (struct op *t ));
_PROTOTYPE(static int doumask, (struct op *t ));
_PROTOTYPE(static int doexec, (struct op *t ));
_PROTOTYPE(static int dodot, (struct op *t ));
_PROTOTYPE(static int dowait, (struct op *t ));
_PROTOTYPE(static int doread, (struct op *t ));
_PROTOTYPE(static int doeval, (struct op *t ));
_PROTOTYPE(static int dotrap, (struct op *t ));
_PROTOTYPE(int getsig, (char *s ));
_PROTOTYPE(void setsig, (int n, void (*f)()));
_PROTOTYPE(int getn, (char *as ));
_PROTOTYPE(static int dobreak, (struct op *t ));
_PROTOTYPE(static int docontinue, (struct op *t ));
_PROTOTYPE(static int brkcontin, (char *cp, int val ));
_PROTOTYPE(static int doexit, (struct op *t ));
_PROTOTYPE(static int doexport, (struct op *t ));
_PROTOTYPE(static int doreadonly, (struct op *t ));
_PROTOTYPE(static void rdexp, (char **wp, void (*f)(), int key));
_PROTOTYPE(static void badid, (char *s ));
_PROTOTYPE(static int doset, (struct op *t ));
_PROTOTYPE(void varput, (char *s, int out ));
_PROTOTYPE(static int dotimes, (void));
_PROTOTYPE(static int callbuiltin, (int n, struct op *t ));
#define BEXEC	4	/* inbuilt("exec") */
#define VFROOM	512	/* free heap left for a vfork child's redirections */

int
execute(t, pin, pout, act)
register struct op *t;
int *pin, *pout;
int act;
{
	register struct op *t1;
	int i, pv[2], rv, child, a;
	char *cp, **wp, **wp2;
	struct var *vp;
	struct brkcon bc;

	if (t == NULL)
		return(0);
	rv = 0;
	a = areanum++;
	wp = (wp2 = t->words) != NULL
	     ? eval(wp2, t->type == TCOM ? DOALL : DOALL & ~DOKEY)
	     : NULL;
	if (wp2 != NULL && wp == NULL) {
		/* the words could not be expanded (out of string space,
		 * already reported): fail the command, don't run it */
		rv = setstatus(1);
		goto broken;
	}

	switch(t->type) {
	case TPAREN:
	case TCOM:
		rv = forkexec(t, pin, pout, act, wp, &child);
		if (child) {
			exstat = rv;
			leave();
		}
		break;

	case TPIPE:
		if ((rv = openpipe(pv)) < 0)
			break;
		pv[0] = remap(pv[0]);
		pv[1] = remap(pv[1]);
		(void) execute(t->left, pin, pv, 0);
		rv = execute(t->right, pv, pout, 0);
		break;

	case TLIST:
		(void) execute(t->left, pin, pout, 0);
		rv = execute(t->right, pin, pout, 0);
		break;

	case TASYNC:
		i = parent();
		if (i != 0) {
			if (i != -1) {
				setval(lookup("!"), putn(i));
				if (pin != NULL)
					closepipe(pin);
				if (talking) {
					prs(putn(i));
					prs("\n");
				}
			} else
				rv = -1;
			setstatus(rv);
		} else {
			signal(SIGINT, SIG_IGN);
			signal(SIGQUIT, SIG_IGN);
			if (talking)
				signal(SIGTERM, SIG_DFL);
			talking = 0;
			if (pin == NULL) {
				close(0);
				open("/dev/null", 0);
			}
			exit(execute(t->left, pin, pout, FEXEC));
		}
		break;

	case TOR:
	case TAND:
		rv = execute(t->left, pin, pout, 0);
		if ((t1 = t->right)!=NULL && (rv == 0) == (t->type == TAND))
			rv = execute(t1, pin, pout, 0);
		break;

	case TFOR:
		if (wp == NULL) {
			wp = dolv+1;
			if ((i = dolc) < 0)
				i = 0;
		} else {
			i = -1;
			while (*wp++ != NULL)
				;			
		}
		vp = lookup(t->str);
		while (setjmp(bc.brkpt))
			if (isbreak)
				goto broken;
		brkset(&bc);
		for (t1 = t->left; i-- && *wp != NULL;) {
			setval(vp, *wp++);
			rv = execute(t1, pin, pout, 0);
		}
		brklist = brklist->nextlev;
		break;

	case TWHILE:
	case TUNTIL:
		while (setjmp(bc.brkpt))
			if (isbreak)
				goto broken;
		brkset(&bc);
		t1 = t->left;
		while ((execute(t1, pin, pout, 0) == 0) == (t->type == TWHILE))
			rv = execute(t->right, pin, pout, 0);
		brklist = brklist->nextlev;
		break;

	case TIF:
	case TELIF:
	 	if (t->right != NULL) {
		rv = !execute(t->left, pin, pout, 0) ?
			execute(t->right->left, pin, pout, 0):
			execute(t->right->right, pin, pout, 0);
		}
		break;

	case TCASE:
		if ((cp = evalstr(t->str, DOSUB|DOTRIM)) == 0)
			cp = "";
		if ((t1 = findcase(t->left, cp)) != NULL)
			rv = execute(t1, pin, pout, 0);
		break;

	case TBRACE:
/*
		if (iopp = t->ioact)
			while (*iopp)
				if (iosetup(*iopp++, pin!=NULL, pout!=NULL)) {
					rv = -1;
					break;
				}
*/
		if (rv >= 0 && (t1 = t->left))
			rv = execute(t1, pin, pout, 0);
		break;
	}

broken:
	t->words = wp2;
	isbreak = 0;
	freehere(areanum);
	freearea(areanum);
	areanum = a;
	if (talking && intr) {
		closeall();
		fail();
	}
	if ((i = trapset) != 0) {
		trapset = 0;
		runtrap(i);
	}
	return(rv);
}

static int
forkexec(t, pin, pout, act, wp, pforked)
register struct op *t;
int *pin, *pout;
int act;
char **wp;
int *pforked;
{
	int i, rv, shcom;	/* built-in: inbuilt()'s number, else 0 */
	register int f;
	char *cp;
	struct ioword **iopp;
	int resetsig, vforked;
	char **owp, **envp;

	owp = wp;
	envp = NULL;
	resetsig = 0;
	*pforked = 0;
	shcom = 0;
	rv = -1;	/* system-detected error */
	if (t->type == TCOM) {
		while ((cp = *wp++) != NULL)
			;
		cp = *wp;

		/* strip all initial assignments */
		/* not correct wrt PATH=yyy command  etc */
		if (flag['x'])
			echo (cp ? wp: owp);
		if (cp == NULL && t->ioact == NULL) {
			while ((cp = *owp++) != NULL && assign(cp, COPYV))
				;
			return(setstatus(0));
		}
		else if (cp != NULL)
			shcom = inbuilt(cp);
	}
	t->words = wp;
	f = act;
	vforked = 0;
	if (shcom == 0 && (f & FEXEC) == 0) {
		/* A simple external command: vfork (R2c.2), no image copy.
		 * The child runs in our memory until exec, so it must not
		 * touch our state: no assignments, no here-documents, no
		 * substitutions in redirections; our globals it changes are
		 * put back below; and it never returns from here (_exit). */
		int canv = t->type == TCOM && owp != NULL && *owp == NULL
						&& vforkable(t->ioact);
		int s_talking = talking, s_intr = intr, s_execflg = execflg;
		struct brkcon *s_brklist = brklist;

		int tries = 0, st;

		if (canv) {
			/* Galtzagorriak: what the child takes from the heap
			 * (environment, redirection names) is made or made
			 * room for here: its heap grows in small steps, and
			 * a vfork child cannot grow it (invfork). */
			envp = makenv();
			if (t->ioact != NULL)
				freecell(space(VFROOM));
		}
	again:
		/* #271: no memory hole for the program (exec: EAGAIN) is
		 * not "not found"; it passes once others end: try again.
		 * Only without pipes: we wait for the child here. */
		vfagain = 0;
		vfquiet = canv && pin == NULL && pout == NULL
							&& tries < VFTRIES;
		i = canv ? vfork() : parent();
		if (i != 0) {
			if (canv) {
				invfork = 0;
				talking = s_talking;
				intr = s_intr;
				execflg = s_execflg;
				brklist = s_brklist;
				vfquiet = 0;
				if (i == -1 && errno == EAGAIN
							&& tries++ < VFTRIES) {
					sleep(1);
					goto again;
				}
				if (i == -1)
					warn("try again");
			}
			if (i == -1)
				return(rv);
			if (pin != NULL)
				closepipe(pin);
			if (pout != NULL)
				return(0);
			st = waitfor(i, 0);
			if (canv && vfagain && tries++ < VFTRIES) {
				sleep(1);
				goto again;
			}
			return(setstatus(st));
		}
		vforked = canv;
		invfork = canv;
		if (talking) {
			signal(SIGINT, SIG_IGN);
			signal(SIGQUIT, SIG_IGN);
			resetsig = 1;
		}
		talking = 0;
		intr = 0;
		if (!vforked)
			(*pforked)++;
		brklist = 0;
		execflg = 0;
	}
	if (owp != NULL)
		while ((cp = *owp++) != NULL && assign(cp, COPYV))
			if (shcom == 0)
				export(lookup(cp));
#ifdef COMPIPE
	if ((pin != NULL || pout != NULL) && shcom != 0 && shcom != BEXEC) {
		err("piping to/from shell builtins not yet done");
		return(-1);
	}
#endif
	if (pin != NULL) {
		dup2(pin[0], 0);
		closepipe(pin);
	}
	if (pout != NULL) {
		dup2(pout[1], 1);
		closepipe(pout);
	}
	if ((iopp = t->ioact) != NULL) {
		if (shcom != 0 && shcom != BEXEC) {
			prs(cp);
			err(": cannot redirect shell command");
			return(-1);
		}
		while (*iopp)
			if (iosetup(*iopp++, pin!=NULL, pout!=NULL)) {
				if (vforked)
					_exit(1);	/* not back into our frames */
				return(rv);
			}
	}
	if (shcom)
		return(setstatus(callbuiltin(shcom, t)));
	/* should use FIOCEXCL */
	for (i=FDBASE; i<NOFILE; i++)
		close(i);
	if (resetsig) {
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
	}
	if (t->type == TPAREN)
		exit(execute(t->left, NOPIPE, NOPIPE, FEXEC));
	if (wp[0] == NULL)
		exit(0);
	cp = rexecve(wp[0], wp, envp != NULL? envp: makenv());
	if (vforked && vfquiet && errno == EAGAIN) {
		vfagain = 1;		/* our parent tries again (#271) */
		_exit(126);
	}
	prs(wp[0]); prs(": "); warn(cp);
	if (vforked)
		_exit(exstat);		/* not exit(): stdio and traps are ours */
	if (!execflg)
		trap[0] = NULL;
	leave();
	/* NOTREACHED */
}

/*
 * Redirections a vfork child may do: plain files and dups, names without
 * substitutions (evalstr would run a command for `...`).
 */
static int
vforkable(iopp)
register struct ioword **iopp;
{
	register char *cp;

	if (iopp == NULL)
		return(1);
	for (; *iopp != NULL; iopp++) {
		if ((*iopp)->io_flag & IOHERE)
			return(0);
		for (cp = (*iopp)->io_name; cp != NULL && *cp != '\0'; cp++)
			if (*cp == '`' || *cp == '$')
				return(0);
	}
	return(1);
}

/*
 * common actions when creating a new child
 */
static int
parent()
{
	register int i;

	i = fork();
	if (i != 0) {
		if (i == -1)
			warn("try again");
	}
	return(i);
}

/*
 * 0< 1> are ignored as required
 * within pipelines.
 */
int
iosetup(iop, pipein, pipeout)
register struct ioword *iop;
int pipein, pipeout;
{
	register u;
	char *cp, *msg;

	if (iop->io_unit == IODEFAULT)	/* take default */
		iop->io_unit = iop->io_flag&(IOREAD|IOHERE)? 0: 1;
	if (pipein && iop->io_unit == 0)
		return(0);
	if (pipeout && iop->io_unit == 1)
		return(0);
	msg = iop->io_flag&(IOREAD|IOHERE)? "open": "create";
	if ((iop->io_flag & IOHERE) == 0) {
		cp = iop->io_name;
		if ((cp = evalstr(cp, DOSUB|DOTRIM)) == NULL)
			return(1);
	}
	if (iop->io_flag & IODUP) {
		if (cp[1] || (!digit(*cp) && *cp != '-')) {
			prs(cp);
			err(": illegal >& argument");
			return(1);
		}
		if (*cp == '-')
			iop->io_flag = IOCLOSE;
		iop->io_flag &= ~(IOREAD|IOWRITE);
	}
	switch (iop->io_flag) {
	case IOREAD:
		u = open(cp, 0);
		break;

	case IOHERE:
	case IOHERE|IOXHERE:
		u = herein(iop->io_name, iop->io_flag&IOXHERE);
		cp = "here file";
		break;

	case IOWRITE|IOCAT:
		if ((u = open(cp, 1)) >= 0) {
			lseek(u, (long)0, 2);
			break;
		}
	case IOWRITE:
		u = creat(cp, 0666);
		break;

	case IODUP:
		u = dup2(*cp-'0', iop->io_unit);
		break;

	case IOCLOSE:
		close(iop->io_unit);
		return(0);
	}
	if (u < 0) {
		prs(cp);
		prs(": cannot ");
		warn(msg);
		return(1);
	} else {
		if (u != iop->io_unit) {
			dup2(u, iop->io_unit);
			close(u);
		}
	}
	return(0);
}

static void
echo(wp)
register char **wp;
{
	register i;

	prs("+");
	for (i=0; wp[i]; i++) {
		if (i)
			prs(" ");
		prs(wp[i]);
	}
	prs("\n");
}

static struct op **
find1case(t, w)
struct op *t;
char *w;
{
	register struct op *t1;
	struct op **tp;
	register char **wp, *cp;

	if (t == NULL)
		return((struct op **)NULL);
	if (t->type == TLIST) {
		if ((tp = find1case(t->left, w)) != NULL)
			return(tp);
		t1 = t->right;	/* TPAT */
	} else
		t1 = t;
	for (wp = t1->words; *wp;)
		if ((cp = evalstr(*wp++, DOSUB)) && gmatch(w, cp))
			return(&t1->left);
	return((struct op **)NULL);
}

static struct op *
findcase(t, w)
struct op *t;
char *w;
{
	register struct op **tp;

	return((tp = find1case(t, w)) != NULL? *tp: (struct op *)NULL);
}

/*
 * Enter a new loop level (marked for break/continue).
 */
static void
brkset(bc)
struct brkcon *bc;
{
	bc->nextlev = brklist;
	brklist = bc;
}

/*
 * Wait for the last process created.
 * Print a message for each process found
 * that was killed by a signal.
 * Ignore interrupt signals while waiting
 * unless `canintr' is true.
 */
int
waitfor(lastpid, canintr)
register int lastpid;
int canintr;
{
	register int pid, rv;
	int s;
	int oheedint = heedint;

	heedint = 0;
	rv = 0;
	do {
		pid = wait(&s);
		if (pid == -1) {
			if (errno != EINTR || canintr)
				break;
		} else {
			if ((rv = WAITSIG(s)) != 0) {
				if (rv < NSIGNAL) {
					if (signame[rv][0] != '\0') {
						if (pid != lastpid) {
							prn(pid);
							prs(": ");
						}
						prs((char *)signame[rv]);
					}
				} else {
					if (pid != lastpid) {
						prn(pid);
						prs(": ");
					}
					prs("Signal "); prn(rv); prs(" ");
				}
				if (WAITCORE(s))
					prs(" - core dumped");
				if (rv >= NSIGNAL || signame[rv][0])
					prs("\n");
				rv = -1;
			} else
				rv = WAITVAL(s);
		}
	} while (pid != lastpid);
	heedint = oheedint;
	if (intr)
		if (talking) {
			if (canintr)
				intr = 0;
		} else {
			if (exstat == 0) exstat = rv;
			onintr(0);
		}
	return(rv);
}

int
setstatus(s)
register int s;
{
	exstat = s;
	setval(lookup("?"), putn(s));
	return(s);
}

/*
 * PATH-searching interface to execve.
 * If getenv("PATH") were kept up-to-date,
 * execvp might be used.
 */
char *
rexecve(c, v, envp)
char *c, **v, **envp;
{
	register int i;
	register char *sp, *tp;
	int eacces = 0, asis = 0, tries;

	sp = any('/', c)? "": path->value;
	asis = *sp == '\0';
	while (asis || *sp != '\0') {
		asis = 0;
		tp = e.linep;
		for (; *sp != '\0'; tp++)
			if ((*tp = *sp++) == ':') {
				asis = *sp == '\0';
				break;
			}
		if (tp != e.linep)
			*tp++ = '/';
		for (i = 0; (*tp++ = c[i++]) != '\0';)
			;
		execve(e.linep, v, envp);
		/* #271: no memory hole now (EAGAIN).  A forked child waits
		 * and tries again itself; a vfork child must not (it runs
		 * in its parent's memory): forkexec() does that for it. */
		for (tries = 0; errno == EAGAIN && !invfork && tries < VFTRIES;
								tries++) {
			sleep(1);
			execve(e.linep, v, envp);
		}
		switch (errno) {
		case EAGAIN:
			return("no memory to start it");

		case ENOEXEC:
			*v = e.linep;
			tp = *--v;
			*v = e.linep;
			execve("/bin/sh", v, envp);
			*v = tp;
			return("no Shell");

		case ENOMEM:
			return("program too big");

		case E2BIG:
			return("argument list too long");

		case EACCES:
			eacces++;
			break;
		}
	}
	return(errno==ENOENT ? "not found" : "cannot execute");
}

/*
 * Run the command produced by generator `f'
 * applied to stream `arg'.
 */
int
run(argp, f)
struct ioarg *argp;
int (*f)();
{
	struct op *otree;
	struct wdblock *swdlist;
	struct wdblock *siolist;
	jmp_buf ev, rt;
	xint *ofail;
	int rv;

	areanum++;
	swdlist = wdlist;
	siolist = iolist;
	otree = outtree;
	ofail = failpt;
	rv = -1;
	if (newenv(setjmp(errpt = ev)) == 0) {
		wdlist = 0;
		iolist = 0;
		pushio(argp, f);
		e.iobase = e.iop;
		yynerrs = 0;
		if (setjmp(failpt = rt) == 0 && yyparse() == 0)
			rv = execute(outtree, NOPIPE, NOPIPE, 0);
		quitenv();
	}
	wdlist = swdlist;
	iolist = siolist;
	failpt = ofail;
	outtree = otree;
	freearea(areanum--);
	return(rv);
}

/* -------- do.c -------- */
/* #include "sh.h" */

/*
 * built-in commands: doX
 */

int
dolabel()
{
	return(0);
}

int
dochdir(t)
register struct op *t;
{
	register char *cp, *er;

	if ((cp = t->words[1]) == NULL && (cp = homedir->value) == NULL)
		er = ": no home directory";
	else if(chdir(cp) < 0)
		er = ": bad directory";
	else
		return(0);
	prs(cp != NULL? cp: "cd");
	err(er);
	return(1);
}

int
doshift(t)
register struct op *t;
{
	register n;

	n = t->words[1]? getn(t->words[1]): 1;
	if(dolc < n) {
		err("nothing to shift");
		return(1);
	}
	dolv[n] = dolv[0];
	dolv += n;
	dolc -= n;
	setval(lookup("#"), putn(dolc));
	return(0);
}

/*
 * execute login and newgrp directly
 */
int
dologin(t)
struct op *t;
{
	register char *cp;

	if (talking) {
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
	}
	cp = rexecve(t->words[0], t->words, makenv());
	prs(t->words[0]); prs(": "); err(cp);
	return(1);
}

int
doumask(t)
register struct op *t;
{
	register int i, n;
	register char *cp;

	if ((cp = t->words[1]) == NULL) {
		i = umask(0);
		umask(i);
		for (n=3*4; (n-=3) >= 0;)
			putc('0'+((i>>n)&07));
		putc('\n');
	} else {
		for (n=0; *cp>='0' && *cp<='9'; cp++)
			n = n*8 + (*cp-'0');
		umask(n);
	}
	return(0);
}

int
doexec(t)
register struct op *t;
{
	register i;
	jmp_buf ex;
	xint *ofail;

	t->ioact = NULL;
	for(i = 0; (t->words[i]=t->words[i+1]) != NULL; i++)
		;
	if (i == 0)
		return(1);
	execflg = 1;
	ofail = failpt;
	if (setjmp(failpt = ex) == 0)
		execute(t, NOPIPE, NOPIPE, FEXEC);
	failpt = ofail;
	execflg = 0;
	return(1);
}

int
dodot(t)
struct op *t;
{
	register i;
	register char *sp, *tp;
	char *cp;

	if ((cp = t->words[1]) == NULL)
		return(0);
	sp = any('/', cp)? ":": path->value;
	while (*sp) {
		tp = e.linep;
		while (*sp && (*tp = *sp++) != ':')
			tp++;
		if (tp != e.linep)
			*tp++ = '/';
		for (i = 0; (*tp++ = cp[i++]) != '\0';)
			;
		if ((i = open(e.linep, 0)) >= 0) {
			exstat = 0;
			next(remap(i));
			return(exstat);
		}
	}
	prs(cp);
	err(": not found");
	return(-1);
}

int
dowait(t)
struct op *t;
{
	register i;
	register char *cp;

	if ((cp = t->words[1]) != NULL) {
		i = getn(cp);
		if (i == 0)
			return(0);
	} else
		i = -1;
	setstatus(waitfor(i, 1));
	return(0);
}

int
doread(t)
struct op *t;
{
	register char *cp, **wp;
	register nb;
	register int  nl = 0;

	if (t->words[1] == NULL) {
		err("Usage: read name ...");
		return(1);
	}
	for (wp = t->words+1; *wp; wp++) {
		for (cp = e.linep; !nl && cp < elinep-1; cp++)
			if ((nb = read(0, cp, sizeof(*cp))) != sizeof(*cp) ||
			    (nl = (*cp == '\n')) ||
			    (wp[1] && any(*cp, ifs->value)))
				break;
		*cp = 0;
		if (nb <= 0)
			break;
		setval(lookup(*wp), e.linep);
	}
	return(nb <= 0);
}

int
doeval(t)
register struct op *t;
{
	return(RUN(awordlist, t->words+1, wdchar));
}

int
dotrap(t)
register struct op *t;
{
	register int  n, i;
	register int  resetsig;

	if (t->words[1] == NULL) {
		for (i=0; i<=_NSIG; i++)
			if (trap[i]) {
				prn(i);
				prs(": ");
				prs(trap[i]);
				prs("\n");
			}
		return(0);
	}
	resetsig = digit(*t->words[1]);
	for (i = resetsig ? 1 : 2; t->words[i] != NULL; ++i) {
		n = getsig(t->words[i]);
		xfree(trap[n]);
		trap[n] = 0;
		if (!resetsig) {
			if (*t->words[1] != '\0') {
				trap[n] = strsave(t->words[1], 0);
				setsig(n, sig);
			} else
				setsig(n, SIG_IGN);
		} else {
			if (talking)
				if (n == SIGINT)
					setsig(n, onintr);
				else
					setsig(n, n == SIGQUIT ? SIG_IGN 
							       : SIG_DFL);
			else
				setsig(n, SIG_DFL);
		}
	}
	return(0);
}

int
getsig(s)
char *s;
{
	register int n;

	if ((n = getn(s)) < 0 || n > _NSIG) {
		err("trap: bad signal number");
		n = 0;
	}
	return(n);
}

void
setsig(n, f)
register n;
_PROTOTYPE(void (*f), (int));
{
	if (n == 0)
		return;
	if (signal(n, SIG_IGN) != SIG_IGN || ourtrap[n]) {
		ourtrap[n] = 1;
		signal(n, f);
	}
}

int
getn(as)
char *as;
{
	register char *s;
	register n, m;

	s = as;
	m = 1;
	if (*s == '-') {
		m = -1;
		s++;
	}
	for (n = 0; digit(*s); s++)
		n = (n*10) + (*s-'0');
	if (*s) {
		prs(as);
		err(": bad number");
	}
	return(n*m);
}

int
dobreak(t)
struct op *t;
{
	return(brkcontin(t->words[1], 1));
}

int
docontinue(t)
struct op *t;
{
	return(brkcontin(t->words[1], 0));
}

static int
brkcontin(cp, val)
register char *cp;
int val;
{
	register struct brkcon *bc;
	register nl;

	nl = cp == NULL? 1: getn(cp);
	if (nl <= 0)
		nl = 999;
	do {
		if ((bc = brklist) == NULL)
			break;
		brklist = bc->nextlev;
	} while (--nl);
	if (nl) {
		err("bad break/continue level");
		return(1);
	}
	isbreak = val;
	longjmp(bc->brkpt, 1);
	/* NOTREACHED */
}

int
doexit(t)
struct op *t;
{
	register char *cp;

	execflg = 0;
	if ((cp = t->words[1]) != NULL)
		setstatus(getn(cp));
	leave();
	/* NOTREACHED */
}

int
doexport(t)
struct op *t;
{
	rdexp(t->words+1, export, EXPORT);
	return(0);
}

int
doreadonly(t)
struct op *t;
{
	rdexp(t->words+1, ronly, RONLY);
	return(0);
}

static void
rdexp(wp, f, key)
register char **wp;
void (*f)();
int key;
{
	if (*wp != NULL) {
		for (; *wp != NULL; wp++)
			if (checkname(*wp))
				(*f)(lookup(*wp));
			else
				badid(*wp);
	} else
		putvlist(key, 1);
}

static void
badid(s)
register char *s;
{
	prs(s);
	err(": bad identifier");
}

int
doset(t)
register struct op *t;
{
	register struct var *vp;
	register char *cp;
	register n;

	if ((cp = t->words[1]) == NULL) {
		for (vp = vlist; vp; vp = vp->next)
			varput(vp->name, 1);
		return(0);
	}
	if (*cp == '-') {
		/* bad: t->words++; */
		for(n = 0; (t->words[n]=t->words[n+1]) != NULL; n++)
			;
		if (*++cp == 0)
			flag['x'] = flag['v'] = 0;
		else
			for (; *cp; cp++)
				switch (*cp) {
				case 'e':
					if (!talking)
						flag['e']++;
					break;

				default:
					if (*cp>='a' && *cp<='z')
						flag[*cp]++;
					break;
				}
		setdash();
	}
	if (t->words[1]) {
		t->words[0] = dolv[0];
		for (n=1; t->words[n]; n++)
			setarea((char *)t->words[n], 0);
		dolc = n-1;
		dolv = t->words;
		setval(lookup("#"), putn(dolc));
		setarea((char *)(dolv-1), 0);
	}
	return(0);
}

void
varput(s, out)
register char *s;
int out;
{
	if (letnum(*s)) {
		write(out, s, strlen(s));
		write(out, "\n", 1);
	}
}


#define	SECS	60L
#define	MINS	3600L

int
dotimes()
{
	struct tms tbuf;

	times(&tbuf);

	prn((int)(tbuf.tms_cutime / MINS));
	prs("m");
	prn((int)((tbuf.tms_cutime % MINS) / SECS));
	prs("s ");
	prn((int)(tbuf.tms_cstime / MINS));
	prs("m");
	prn((int)((tbuf.tms_cstime % MINS) / SECS));
	prs("s\n");
	return(0);
}

/*
 * The built-in commands.  Galtzagorriak: a const table of names in flash
 * and a switch that calls the function, so the shell keeps no table of
 * function pointers in RAM (.data, or GOT entries: each relocated at exec).
 * inbuilt() gives the command's number + 1 (0: not built in), BEXEC is
 * exec's.
 */
static const char builtin[][9] = {
	":",
	"cd",
	"shift",
	"exec",
	"wait",
	"read",
	"eval",
	"trap",
	"break",
	"continue",
	"exit",
	"export",
	"readonly",
	"set",
	".",
	"umask",
	"login",
	"newgrp",
	"times",
};
#define NBUILTIN (sizeof(builtin)/sizeof(builtin[0]))

int
inbuilt(s)
register char *s;
{
	register int i;

	for (i = 0; i < NBUILTIN; i++)
		if (strcmp(builtin[i], s) == 0)
			return(i + 1);
	return(0);
}

static int
callbuiltin(n, t)
int n;
struct op *t;
{
	switch (n) {
	case 1:	return(dolabel());
	case 2:	return(dochdir(t));
	case 3:	return(doshift(t));
	case 4:	return(doexec(t));
	case 5:	return(dowait(t));
	case 6:	return(doread(t));
	case 7:	return(doeval(t));
	case 8:	return(dotrap(t));
	case 9:	return(dobreak(t));
	case 10:	return(docontinue(t));
	case 11:	return(doexit(t));
	case 12:	return(doexport(t));
	case 13:	return(doreadonly(t));
	case 14:	return(doset(t));
	case 15:	return(dodot(t));
	case 16:	return(doumask(t));
	case 17:	return(dologin(t));
	case 18:	return(dologin(t));
	case 19:	return(dotimes());
	}
	return(-1);
}

