/* st.c - the modules' result line (spec 7b.3). */
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include "st.h"

void st_result(const char *what, const char *reason)
{
	char line[64];

	strncpy(line, what, sizeof line - 2);
	line[sizeof line - 2] = 0;
	if (reason != NULL && *reason != 0
	    && strlen(line) + 1 + strlen(reason) < sizeof line - 1) {
		strcat(line, " ");
		strcat(line, reason);
	}
	strcat(line, "\n");
	(void) write(1, line, strlen(line));
}
