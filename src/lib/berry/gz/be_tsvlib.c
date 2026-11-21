/* Berry module tsv (spec K.1, R7c.3, issue #385): the structured text of
 * the commands' -t flag at the edge of a script.  Tab-separated fields,
 * one header line; escapes \t \n \\ inside fields.
 *   tsv.split(line)   -> list of fields (unescaped; a trailing \n dropped)
 *   tsv.join(list)    -> line without \n (fields through str(), escaped)
 *   tsv.records(text) -> list of maps, keyed by the header line's names
 */
#include "be_object.h"
#include "be_strlib.h"
#include <string.h>

/* Push the fields of s[0..n) as strings onto the list at the stack top
 * (or, with keys >= 0, as map entries keyed by the list data at keys;
 * fields past its nkeys names are dropped). */
static void fields(bvm *vm, const char *s, size_t n, int keys, int nkeys)
{
	char buf[128];
	size_t i = 0, k = 0;
	int col = 0;

	for (;;) {
		int end = i >= n || s[i] == '\t';
		if (!end && k < sizeof(buf)) {
			char c = s[i];
			if (c == '\\' && i + 1 < n) {
				c = s[++i];
				c = c == 't' ? '\t' : c == 'n' ? '\n' : c;
			}
			buf[k++] = c;
		}
		if (end) {
			if (keys >= 0 && col >= nkeys) {
				/* no name for it */
			} else if (keys >= 0) {
				be_pushint(vm, col);
				be_getindex(vm, keys);	/* the header name */
				be_remove(vm, -2);
				be_pushnstring(vm, buf, k);
				be_setindex(vm, -3);
				be_pop(vm, 2);
			} else {
				be_pushnstring(vm, buf, k);
				be_data_push(vm, -2);
				be_pop(vm, 1);
			}
			k = 0;
			col++;
			if (i >= n) break;
		}
		i++;
	}
}

static size_t linelen(const char *s, size_t n)
{
	if (n > 0 && s[n - 1] == '\n') n--;
	if (n > 0 && s[n - 1] == '\r') n--;
	return n;
}

static int m_split(bvm *vm)
{
	const char *s;

	if (be_top(vm) < 1 || !be_isstring(vm, 1))
		be_raise(vm, "type_error", "tsv.split(line)");
	s = be_tostring(vm, 1);
	be_newobject(vm, "list");
	fields(vm, s, linelen(s, be_strlen(vm, 1)), -1, 0);
	be_pop(vm, 1);
	be_return(vm);
}

static int m_join(bvm *vm)
{
	int i, n;

	if (be_top(vm) < 1 || !be_islistinstance(vm, 1))
		be_raise(vm, "type_error", "tsv.join(list)");
	be_pushstring(vm, "");			/* the result, index 2 */
	be_getmember(vm, 1, ".p");		/* the list's data */
	n = be_data_size(vm, -1);
	be_pop(vm, 1);
	for (i = 0; i < n; i++) {
		const char *f;
		char buf[64];
		size_t k = 0;

		be_getmember(vm, 1, ".p");
		be_pushint(vm, i);
		be_getindex(vm, -2);
		f = be_tostring(vm, -1);	/* str() of the field */
		if (i > 0) buf[k++] = '\t';
		for (; *f; f++) {
			if (k + 2 >= sizeof(buf)) {
				be_pushnstring(vm, buf, k);
				be_strconcat(vm, 2);
				be_pop(vm, 1);
				k = 0;
			}
			if (*f == '\t' || *f == '\n' || *f == '\\') {
				buf[k++] = '\\';
				buf[k++] = *f == '\t' ? 't' : *f == '\n' ? 'n' : '\\';
			} else buf[k++] = *f;
		}
		be_pushnstring(vm, buf, k);
		be_strconcat(vm, 2);
		be_pop(vm, 4);
	}
	be_pushvalue(vm, 2);
	be_return(vm);
}

static int m_records(bvm *vm)
{
	const char *s, *e, *end;
	int hdr, nk;

	if (be_top(vm) < 1 || !be_isstring(vm, 1))
		be_raise(vm, "type_error", "tsv.records(text)");
	s = be_tostring(vm, 1);
	end = s + be_strlen(vm, 1);
	be_newobject(vm, "list");		/* the header: 2, its data 3 */
	hdr = be_absindex(vm, -1);
	e = memchr(s, '\n', (size_t)(end - s));
	if (e == NULL) e = end;
	fields(vm, s, linelen(s, (size_t)(e - s)), -1, 0);
	nk = be_data_size(vm, hdr);
	be_newobject(vm, "list");		/* the result: 4, its data 5 */
	s = e < end ? e + 1 : end;
	while (s < end) {
		e = memchr(s, '\n', (size_t)(end - s));
		if (e == NULL) e = end;
		if (e > s) {
			be_newobject(vm, "map");	/* 6, its data 7 */
			fields(vm, s, linelen(s, (size_t)(e - s)), hdr, nk);
			be_pop(vm, 1);
			be_data_push(vm, -2);
			be_pop(vm, 1);
		}
		s = e < end ? e + 1 : end;
	}
	be_pop(vm, 1);
	be_return(vm);
}

/* @const_object_info_begin
module tsv (scope: global) {
    split, func(m_split)
    join, func(m_join)
    records, func(m_records)
}
@const_object_info_end */
#include "../generate/be_fixed_tsv.h"
