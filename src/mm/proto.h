/* Function prototypes. */

struct mproc;		/* need types outside of parameter list --kub */
struct stat;

/* alloc.c */
_PROTOTYPE( phys_clicks alloc_mem, (phys_clicks clicks)			);
_PROTOTYPE( void free_mem, (phys_clicks base, phys_clicks clicks)	);
_PROTOTYPE( phys_clicks max_hole, (void)				);
_PROTOTYPE( void mem_init, (phys_clicks *total, phys_clicks *free)	);
_PROTOTYPE( phys_clicks mem_left, (void)				);
_PROTOTYPE( int do_brk3, (void)						);

/* break.c */
_PROTOTYPE( int adjust, (struct mproc *rmp,
			vir_clicks data_clicks, vir_bytes sp)		);
_PROTOTYPE( int do_brk, (void)						);
_PROTOTYPE( int size_ok, (int file_type, vir_clicks tc, vir_clicks dc,
			vir_clicks sc, vir_clicks dvir, vir_clicks s_vir) );

/* exec.c */
_PROTOTYPE( int do_exec, (void)						);
_PROTOTYPE( struct mproc *find_share, (struct mproc *mp_ign, Ino_t ino,
			Dev_t dev, ftime32_t ctime)			);

/* forkexit.c */
_PROTOTYPE( int do_fork, (void)						);
_PROTOTYPE( int do_vfork, (void)					);
_PROTOTYPE( void vfork_done, (struct mproc *rmc)			);
_PROTOTYPE( int do_mm_exit, (void)					);
_PROTOTYPE( int do_waitpid, (void)					);
_PROTOTYPE( void mm_exit, (struct mproc *rmp, int exit_status)		);

/* getset.c */
_PROTOTYPE( int do_getset, (void)					);

/* main.c */
_PROTOTYPE( void main, (void)						);

#if (MACHINE == MACINTOSH)
_PROTOTYPE( phys_clicks start_click, (void)				);
#endif

_PROTOTYPE( void reply, (int proc_nr, int result, int res2, char *respt));

/* putk.c */
_PROTOTYPE( void putk, (int c)						);

/* signal.c */
_PROTOTYPE( int do_alarm, (void)					);
_PROTOTYPE( int do_kill, (void)						);
_PROTOTYPE( int do_ksig, (void)						);
_PROTOTYPE( int do_pause, (void)					);
_PROTOTYPE( int set_alarm, (int proc_nr, int sec)			);
_PROTOTYPE( int check_sig, (pid_t proc_id, int signo)			);
_PROTOTYPE( void sig_proc, (struct mproc *rmp, int sig_nr)		);
_PROTOTYPE( int do_sigaction, (void)					);
_PROTOTYPE( int do_sigpending, (void)					);
_PROTOTYPE( int do_sigprocmask, (void)					);
_PROTOTYPE( int do_sigreturn, (void)					);
_PROTOTYPE( int do_sigsuspend, (void)					);
_PROTOTYPE( int do_reboot, (void)					);
#if (CHIP == ARM_CM)
/* fcache.c */
_PROTOTYPE( void fc_init, (void)					);
_PROTOTYPE( unsigned long fc_lookup, (unsigned long crc, unsigned long size,
						unsigned long owner)	);
_PROTOTYPE( int fc_queue, (int fd, unsigned long toff, unsigned long size,
			unsigned long crc, unsigned long owner)		);
_PROTOTYPE( void fc_step, (void)					);
_PROTOTYPE( int fc_ran, (unsigned long crc, unsigned long owner)	);
_PROTOTYPE( unsigned long fc_sync, (int fd, unsigned long size,
	unsigned long crc, unsigned long owner, char *buf, int bufsize)	);
_PROTOTYPE( void fc_info, (struct fcinfo *fi)				);
_PROTOTYPE( unsigned long crc32_more, (unsigned long crc,
			const unsigned char *p, unsigned long n)		);
_PROTOTYPE( int fc_lend, (void)						);
_PROTOTYPE( int fc_busy, (void)						);
/* swap.c */
_PROTOTYPE( void sw_init, (void)					);
_PROTOTYPE( int sw_ctl, (void)						);
_PROTOTYPE( void sw_room, (phys_clicks clicks)				);
_PROTOTYPE( int sw_in, (struct mproc *rmp)				);
_PROTOTYPE( void sw_touch, (int proc_nr)				);
_PROTOTYPE( void sw_after, (void)					);
_PROTOTYPE( void sw_step, (void)					);
_PROTOTYPE( int sw_io, (int op, int proc, unsigned long pos,
					phys_bytes addr, phys_bytes n)	);
_PROTOTYPE( int sw_state, (void)					);
_PROTOTYPE( void sw_kick, (void)					);
_PROTOTYPE( int sw_wbudget, (unsigned kb)				);
/* blk.c */
_PROTOTYPE( void blk_init, (void)					);
_PROTOTYPE( int blk_on, (unsigned long *sz, int nsz)			);
_PROTOTYPE( int blk_busy, (void)					);
_PROTOTYPE( void blk_end, (int proc_nr)					);
_PROTOTYPE( void blk_wrap, (void)					);
_PROTOTYPE( void pid_wrap, (pid_t want)					);
_PROTOTYPE( int blk_req, (void)						);
_PROTOTYPE( int blk_step, (void)					);
_PROTOTYPE( unsigned blk_nwipe, (void)					);
_PROTOTYPE( int blk_wipe, (unsigned step)				);
_PROTOTYPE( int sw_discard, (int minor, unsigned long first,
						unsigned long count)	);
_PROTOTYPE( int sw_dio, (int minor, int op, int proc, unsigned long pos,
					phys_bytes addr, phys_bytes n)	);
_PROTOTYPE( int sw_minor, (void)					);
/* alloc.c */
_PROTOTYPE( int alloc_mem_at, (phys_clicks base, phys_clicks clicks)	);
_PROTOTYPE( phys_clicks hole_end, (phys_clicks pos)			);
_PROTOTYPE( phys_clicks hole_with, (phys_clicks base, phys_clicks clicks));
_PROTOTYPE( phys_clicks mem_free, (void)				);
_PROTOTYPE( int mem_test, (void)					);
/* forkexit.c */
_PROTOTYPE( void wait_retry, (struct mproc *rmp)			);
_PROTOTYPE( void fc_return, (void)					);
/* finst.c */
struct stat;
_PROTOTYPE( void in_init, (void)					);
_PROTOTYPE( unsigned long in_lookup, (struct stat *s, unsigned long size,
						int fd, unsigned long toff)	);
_PROTOTYPE( int do_install, (void)					);
_PROTOTYPE( void in_info, (int *entries, unsigned long *used)		);
_PROTOTYPE( void mem_paint, (struct mproc *rmp)				);
_PROTOTYPE( void mem_report, (struct mproc *rmp)			);
#endif
_PROTOTYPE( int do_meminfo, (void)					);
_PROTOTYPE( phys_clicks alloc_mem_high, (phys_clicks clicks)		);

/* trace.c */
_PROTOTYPE( int do_trace, (void)					);
_PROTOTYPE( void stop_proc, (struct mproc *rmp, int sig_nr)		);

/* utility.c */
_PROTOTYPE( int allowed, (char *name_buf, struct stat *s_buf, int mask)	);
_PROTOTYPE( int no_sys, (void)						);
_PROTOTYPE( void panic, (char *format, int num)				);
_PROTOTYPE( void tell_fs, (int what, int p1, int p2, int p3)		);
