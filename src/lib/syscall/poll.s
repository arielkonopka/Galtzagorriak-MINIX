.sect .text
.extern	__poll
.define	_poll
.align 2

_poll:
	jmp	__poll
