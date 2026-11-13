/* _brksize: the current break, initially the end of the bss.  _brk() and
 * _sbrk() (src/lib/other/_brk.c) move it. */
	.data
	.p2align 2
	.global	_brksize
_brksize:
	.word	_end
