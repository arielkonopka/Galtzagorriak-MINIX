/* nfc_shim_mfc.c - what the vendored MIFARE Classic poller needs past
 * nfc_shim.c (#404): thread flags without threads, and the storage, stream
 * and key-dictionary calls of its dictionary/nested attack modes, which
 * fail here (no file system in the library; libnfc's own mf_attack.c does
 * the attacks).  Its read mode and mf_classic_poller_sync use none of
 * them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <furi.h>
#include <stream/buffered_file_stream.h>
#include <toolbox/keys_dict.h>

static uint32_t nfcshim_flags;

FuriThreadId furi_thread_get_current_id(void) { return &nfcshim_flags; }
uint32_t furi_thread_flags_set(FuriThreadId id, uint32_t flags) {
    furi_check(id == &nfcshim_flags);
    return nfcshim_flags |= flags;
}
uint32_t furi_thread_flags_clear(uint32_t flags) {
    uint32_t was = nfcshim_flags;
    nfcshim_flags &= ~flags;
    return was;
}
uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout) {
    (void)timeout;
    uint32_t got = nfcshim_flags & flags;
    if((options & FuriFlagWaitAll) ? got != flags : got == 0) return FuriFlagErrorTimeout;
    if(!(options & FuriFlagNoClear)) nfcshim_flags &= ~got;
    return got;
}

void furi_string_trim_chars(FuriString* s, const char* chars) {
    size_t a = 0, b = s->len;
    while(a < b && strchr(chars, s->buf[a])) a++;
    while(b > a && strchr(chars, s->buf[b - 1])) b--;
    memmove(s->buf, s->buf + a, b - a);
    s->len = b - a;
    s->buf[s->len] = 0;
}

void* furi_record_open(const char* name) { (void)name; return NULL; }
void furi_record_close(const char* name) { (void)name; }
Stream* buffered_file_stream_alloc(Storage* storage) { (void)storage; return NULL; }
bool buffered_file_stream_open(Stream* s, const char* path, FS_AccessMode a, FS_OpenMode m) {
    (void)s; (void)path; (void)a; (void)m;
    return false;
}
bool buffered_file_stream_close(Stream* s) { (void)s; return false; }
void stream_free(Stream* s) { (void)s; }
size_t stream_write_string(Stream* s, FuriString* str) { (void)s; (void)str; return 0; }

bool keys_dict_check_presence(const char* path) { (void)path; return false; }
KeysDict* keys_dict_alloc(const char* path, KeysDictMode mode, size_t key_size) {
    (void)path; (void)mode; (void)key_size;
    return NULL;
}
void keys_dict_free(KeysDict* d) { (void)d; }
bool keys_dict_rewind(KeysDict* d) { (void)d; return false; }
bool keys_dict_get_next_key(KeysDict* d, uint8_t* key, size_t size) {
    (void)d; (void)key; (void)size;
    return false;
}
