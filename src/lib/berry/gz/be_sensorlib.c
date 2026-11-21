/* Berry module sensor (spec R7c.3, H.3, issue #385): a stub over the
 * files in /dev/sensor (the drivers are lane:local work).  The normal file
 * permissions apply: a script reads what its user may read.
 *   sensor.list()     -> the names in /dev/sensor ([] if there is none)
 *   sensor.read(name) -> the file's first line, without \n; nil if it
 *                        does not exist or cannot be read
 */
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include "be_object.h"

#define SENSOR_DIR	"/dev/sensor"

static int m_list(bvm *vm)
{
	DIR *d = opendir(SENSOR_DIR);
	struct dirent *e;

	be_newobject(vm, "list");
	if (d != NULL) {
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			be_pushstring(vm, e->d_name);
			be_data_push(vm, -2);
			be_pop(vm, 1);
		}
		closedir(d);
	}
	be_pop(vm, 1);
	be_return(vm);
}

static int m_read(bvm *vm)
{
	char path[64], line[96];
	const char *n;
	FILE *f;
	size_t k;

	if (be_top(vm) < 1 || !be_isstring(vm, 1))
		be_raise(vm, "type_error", "sensor.read(name)");
	n = be_tostring(vm, 1);
	if (strchr(n, '/') != NULL || strlen(n) > sizeof(path) - 13)
		be_return_nil(vm);
	strcpy(path, SENSOR_DIR "/");
	strcat(path, n);
	f = fopen(path, "r");
	if (f == NULL) be_return_nil(vm);
	if (fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		be_return_nil(vm);
	}
	fclose(f);
	k = strlen(line);
	while (k > 0 && (line[k - 1] == '\n' || line[k - 1] == '\r')) k--;
	be_pushnstring(vm, line, k);
	be_return(vm);
}

/* @const_object_info_begin
module sensor (scope: global) {
    list, func(m_list)
    read, func(m_read)
}
@const_object_info_end */
#include "../generate/be_fixed_sensor.h"
