/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): from the stock firmware
 * applications/services/gui/scene_manager_i.h (GPL-3.0). M*LIB array: src/gui/m-array.h. */
/**
 * @file scene_manager_i.h
 * GUI: internal SceneManager API
 */

#pragma once

#include "scene_manager.h"
#include "m-array.h"

ARRAY_DEF(SceneManagerIdStack, uint32_t, M_DEFAULT_OPLIST); //-V658

typedef struct {
    uint32_t state;
} AppScene;

struct SceneManager {
    SceneManagerIdStack_t scene_id_stack;
    const SceneManagerHandlers* scene_handlers;
    void* context;
    AppScene scene[];
};
