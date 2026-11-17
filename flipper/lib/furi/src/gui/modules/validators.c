/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): from the stock firmware
 * applications/services/gui/modules/validators.c (GPL-3.0). */
#include "validators.h"
#include <sys/stat.h>
#include <strings.h>
#include <furi.h>

struct ValidatorIsFile {
    char* app_path_folder;
    const char* app_extension;
    char* current_name;
};

bool validator_is_file_callback(const char* text, FuriString* error, void* context) {
    furi_check(context);
    ValidatorIsFile* instance = context;

    if(instance->current_name != NULL) {
        if(strcasecmp(instance->current_name, text) == 0) {
            return true;
        }
    }

    FuriString* path = furi_string_alloc_printf(
        "%s/%s%s", instance->app_path_folder, text, instance->app_extension);
    /* libfuri: stat() until the storage record (R5.3) maps /ext, /int */
    struct stat st;
    const bool ret = stat(furi_string_get_cstr(path), &st) != 0;
    if(!ret) {
        furi_string_printf(error, "This name\nexists!\nChoose\nanother one.");
    }
    furi_string_free(path);

    return ret;
}

ValidatorIsFile* validator_is_file_alloc_init(
    const char* app_path_folder,
    const char* app_extension,
    const char* current_name) {
    ValidatorIsFile* instance = malloc(sizeof(ValidatorIsFile));

    instance->app_path_folder = strdup(app_path_folder);
    instance->app_extension = app_extension;
    instance->current_name = current_name != NULL ? strdup(current_name) : NULL;

    return instance;
}

void validator_is_file_free(ValidatorIsFile* instance) {
    furi_check(instance);
    free(instance->app_path_folder);
    free(instance->current_name);
    free(instance);
}
