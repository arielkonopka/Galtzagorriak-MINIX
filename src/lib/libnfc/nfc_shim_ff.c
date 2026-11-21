/* nfc_shim_ff.c - read-only FlipperFormat over stdio for the parsers'
 * lookup files (#412).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412 (D.3).
 *
 * The parsers that name stations, figures or EMV codes (plantain,
 * sevppk_tk, sk_tk, szppk_so, skylanders, emv) look them up in Flipper
 * "resources" files, as stock does from /ext/nfc/assets.  No such file is
 * shipped (D.3); the user puts their own in /home/flipper/nfc/assets, and
 * a missing file makes the lookup fail, never the parse.  Only reading:
 * read_header, read_string (from the current position on, as stock), and
 * rewind.  One FILE and one line at a time, nothing kept resident.
 */
#include <furi.h>
#include <flipper_format/flipper_format.h>
#include <storage/storage.h>

const char* nfcshim_ext_dir = "/home/flipper/";

struct FlipperFormat {
    FILE* f;
};

const char* nfcshim_ext_path(const char* path, char* buf, size_t size) {
    if(!strncmp(path, "/ext/", 5))
        snprintf(buf, size, "%s%s", nfcshim_ext_dir, path + 5);
    else
        snprintf(buf, size, "%s", path);
    return buf;
}

FlipperFormat* flipper_format_file_alloc(Storage* storage) {
    (void)storage;
    return nfcshim_malloc(sizeof(FlipperFormat));
}

bool flipper_format_file_open_existing(FlipperFormat* ff, const char* path) {
    char p[256];
    if(!ff) return false;
    if(ff->f) fclose(ff->f);
    ff->f = fopen(nfcshim_ext_path(path, p, sizeof p), "r");
    return ff->f != NULL;
}

void flipper_format_free(FlipperFormat* ff) {
    if(!ff) return;
    if(ff->f) fclose(ff->f);
    free(ff);
}

bool flipper_format_rewind(FlipperFormat* ff) {
    if(!ff || !ff->f) return false;
    rewind(ff->f);
    return true;
}

/* Next "key: value" line from the current position; comments skipped.
 * Lines longer than the buffer are skipped whole. */
static bool ff_find(FlipperFormat* ff, const char* key, FuriString* value) {
    char line[256];
    size_t kl = strlen(key);
    if(!ff || !ff->f) return false;
    while(fgets(line, sizeof line, ff->f)) {
        size_t n = strlen(line);
        if(n && line[n - 1] != '\n' && !feof(ff->f)) {
            int c;
            while((c = fgetc(ff->f)) != EOF && c != '\n') {
            }
            continue;
        }
        while(n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if(line[0] == '#') continue;
        if(strncmp(line, key, kl) || line[kl] != ':') continue;
        const char* v = line + kl + 1;
        while(*v == ' ') v++;
        if(value) furi_string_set_str(value, v);
        return true;
    }
    return false;
}

bool flipper_format_read_string(FlipperFormat* ff, const char* key, FuriString* d) {
    return ff_find(ff, key, d);
}

bool flipper_format_read_header(FlipperFormat* ff, FuriString* type, uint32_t* version) {
    FuriString* v = furi_string_alloc();
    bool ok = flipper_format_rewind(ff) && ff_find(ff, "Filetype", type) && ff_find(ff, "Version", v);
    if(ok) *version = (uint32_t)strtoul(furi_string_get_cstr(v), NULL, 10);
    furi_string_free(v);
    return ok;
}
