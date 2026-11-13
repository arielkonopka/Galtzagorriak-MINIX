/* Tasks for device classes this machine does not have (printer, floppy),
 * and the winchester task until the SD card driver exists.  They answer
 * every request with ENXIO, so the fixed task numbers of <minix/com.h> stay
 * valid.
 */
#include "../kernel.h"
#include <minix/callnr.h>
#include <minix/com.h>

PUBLIC void nodev_task()
{
  message m;

  while (TRUE) {
	receive(ANY, &m);
	if (m.m_source == HARDWARE) continue;
	m.REP_PROC_NR = m.PROC_NR;
	m.REP_STATUS = ENXIO;
	m.m_type = TASK_REPLY;
	send(m.m_source, &m);
  }
}
