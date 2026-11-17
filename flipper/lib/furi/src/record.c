/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): records (stock furi/core/record.h), the
 * named services of one process. Semantics as stock: open waits for
 * create; destroy fails while the record is held. */
#include <stdlib.h>
#include <string.h>
#include "furi_i.h"

typedef struct Record {
    struct Record* next;
    void* data;
    uint32_t holders;
    FuriWaitq wq;
    char name[]; /* NUL-terminated */
} Record;

static Record* records;

static Record* find(const char* name) {
    Record* r;

    for(r = records; r; r = r->next)
        if(strcmp(r->name, name) == 0) return r;
    return NULL;
}

static Record* find_or_add(const char* name) {
    Record* r = find(name);

    if(r) return r;
    r = calloc(1, sizeof(Record) + strlen(name) + 1);
    furi_check(r, "furi_record: out of memory");
    strcpy(r->name, name);
    r->next = records;
    records = r;
    return r;
}

bool furi_record_exists(const char* name) {
    Record* r;

    furi_check(name);
    r = find(name);
    return r != NULL && r->data != NULL;
}

void furi_record_create(const char* name, void* data) {
    Record* r;

    furi_check(name && data);
    r = find_or_add(name);
    furi_check(r->data == NULL, "furi_record_create: record exists");
    r->data = data;
    fsched_wake_all(&r->wq);
}

bool furi_record_destroy(const char* name) {
    Record **pp, *r;

    furi_check(name);
    for(pp = &records; (r = *pp) != NULL; pp = &r->next) {
        if(strcmp(r->name, name) != 0) continue;
        if(r->holders != 0) return false;
        *pp = r->next;
        free(r);
        return true;
    }
    return false;
}

void* furi_record_open(const char* name) {
    Record* r;

    furi_check(name);
    r = find_or_add(name);
    r->holders++;
    while(r->data == NULL)
        fsched_block(&r->wq, FuriWaitForever);
    return r->data;
}

void furi_record_close(const char* name) {
    Record* r;

    furi_check(name);
    r = find(name);
    furi_check(r && r->holders > 0, "furi_record_close: not open");
    r->holders--;
}
