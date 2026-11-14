/* exit() for the boot servers (MM, FS, INIT): they never register atexit()
 * functions or use stdio streams that need flushing, so they skip the C
 * library's exit and its 128-byte __functab (spec R2a.4). */
#include <lib.h>
#include <unistd.h>

void exit(int status)
{
	_exit(status);
}
