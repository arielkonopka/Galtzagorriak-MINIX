/* Berry module pipe (spec K.1, R7c.3, issue #385): list operations for
 * records inside one script (one heap, no serialisation).
 *   pipe.where(list, f)    -> the items for which f(item) is true
 *   pipe.map(list, f)      -> f(item) of each item
 *   pipe.head(list, n)     -> the first n items
 *   pipe.sortby(list, key) -> a copy sorted (stably) by key: a function
 *                             f(item), or a name for maps (records)
 * Numbers compare as numbers, everything else as str().
 */
#include "be_object.h"
#include <string.h>

static void need_list(bvm *vm, const char *what)
{
	if (be_top(vm) < 2 || !be_islistinstance(vm, 1))
		be_raise(vm, "type_error", what);
}

/* items of the list at 1 into a new list; f: 0 copy, 1 where, 2 map */
static int each(bvm *vm, int how)
{
	int i, n;

	be_getmember(vm, 1, ".p");		/* 3: the input data */
	n = be_data_size(vm, 3);
	be_newobject(vm, "list");		/* 4, its data 5 */
	for (i = 0; i < n; i++) {
		be_pushint(vm, i);
		be_getindex(vm, 3);		/* item at the top */
		be_remove(vm, -2);
		if (how) {
			be_pushvalue(vm, 2);
			be_pushvalue(vm, -2);
			be_call(vm, 1);
			be_pop(vm, 1);		/* f, result at the top */
			if (how == 2) {
				be_remove(vm, -2);
			} else {
				int keep = be_tobool(vm, -1);
				be_pop(vm, 1);
				if (!keep) { be_pop(vm, 1); continue; }
			}
		}
		be_data_push(vm, 5);
		be_pop(vm, 1);
	}
	be_pop(vm, 1);
	be_return(vm);
}

static int m_where(bvm *vm) { need_list(vm, "pipe.where(list, f)"); return each(vm, 1); }
static int m_map(bvm *vm) { need_list(vm, "pipe.map(list, f)"); return each(vm, 2); }

static int m_head(bvm *vm)
{
	int i, n, k;

	need_list(vm, "pipe.head(list, n)");
	k = be_toint(vm, 2);
	be_getmember(vm, 1, ".p");
	n = be_data_size(vm, 3);
	if (k < n) n = k < 0 ? 0 : k;
	be_newobject(vm, "list");
	for (i = 0; i < n; i++) {
		be_pushint(vm, i);
		be_getindex(vm, 3);
		be_remove(vm, -2);
		be_data_push(vm, 5);
		be_pop(vm, 1);
	}
	be_pop(vm, 1);
	be_return(vm);
}

/* the key of the item at idx, pushed */
static void key(bvm *vm, int idx)
{
	if (be_isfunction(vm, 2) || be_isclosure(vm, 2)) {
		be_pushvalue(vm, 2);
		be_pushvalue(vm, idx);
		be_call(vm, 1);
		be_pop(vm, 1);
	} else {
		be_getmember(vm, idx, ".p");	/* the map's data */
		be_pushvalue(vm, 2);
		if (!be_getindex(vm, -2)) {
			be_pop(vm, 1);
			be_pushnil(vm);
		}
		be_remove(vm, -2);
		be_remove(vm, -2);
	}
}

/* <0, 0, >0: key at a against key at b */
static int cmp(bvm *vm, int a, int b)
{
	if (be_isnumber(vm, a) && be_isnumber(vm, b)) {
		breal x = be_toreal(vm, a), y = be_toreal(vm, b);
		return x < y ? -1 : x > y;
	}
	if (be_isnil(vm, a) || be_isnil(vm, b))
		return be_isnil(vm, b) - be_isnil(vm, a);
	be_pushvalue(vm, a);
	be_pushvalue(vm, b);
	{
		int r = strcmp(be_tostring(vm, -2), be_tostring(vm, -1));
		be_pop(vm, 2);
		return r;
	}
}

/* Insertion sort of a copy (stable; lists in scripts are short):
 * the keys are computed once, in a parallel list. */
static int m_sortby(bvm *vm)
{
	int i, j, n;

	need_list(vm, "pipe.sortby(list, key)");
	be_getmember(vm, 1, ".p");		/* 3 */
	n = be_data_size(vm, 3);
	be_newobject(vm, "list");		/* 4, data 5: the items */
	be_newobject(vm, "list");		/* 6, data 7: their keys */
	for (i = 0; i < n; i++) {
		be_pushint(vm, i);
		be_getindex(vm, 3);
		be_remove(vm, -2);		/* item: 8 */
		key(vm, 8);			/* its key: 9 */
		/* find its place among the sorted 0..i-1 */
		for (j = i; j > 0; j--) {
			int c;
			be_pushint(vm, j - 1);
			be_getindex(vm, 7);
			be_remove(vm, -2);
			c = cmp(vm, -1, 9);
			be_pop(vm, 1);
			if (c <= 0) break;
		}
		be_pushvalue(vm, 8);
		be_data_push(vm, 5);
		be_pop(vm, 1);
		be_pushvalue(vm, 9);
		be_data_push(vm, 7);
		be_pop(vm, 1);
		/* move the new item from i down to j */
		{
			int k;
			for (k = i; k > j; k--) {
				int d;
				for (d = 5; d <= 7; d += 2) {
					be_pushint(vm, k - 1);
					be_getindex(vm, d);
					be_remove(vm, -2);
					be_pushint(vm, k);
					be_pushvalue(vm, -2);
					be_setindex(vm, d);
					be_pop(vm, 3);
				}
			}
			for (k = 0; k < 2; k++) {
				be_pushint(vm, j);
				be_pushvalue(vm, 8 + k);
				be_setindex(vm, k ? 7 : 5);
				be_pop(vm, 2);
			}
		}
		be_pop(vm, 2);
	}
	be_pop(vm, 3);				/* back to the items at 4 */
	be_return(vm);
}

/* @const_object_info_begin
module pipe (scope: global) {
    where, func(m_where)
    map, func(m_map)
    head, func(m_head)
    sortby, func(m_sortby)
}
@const_object_info_end */
#include "../generate/be_fixed_pipe.h"
