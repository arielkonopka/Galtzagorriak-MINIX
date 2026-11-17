/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri GUI (spec R5.2): from the stock firmware
 * applications/services/gui/modules/widget_elements/widget_element.h (GPL-3.0). */
/**
 * @file widget_element_i.h
 * GUI: internal Widget Element API
 */

#pragma once

#include <input/input.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GuiButtonTypeLeft,
    GuiButtonTypeCenter,
    GuiButtonTypeRight,
} GuiButtonType;

typedef void (*ButtonCallback)(GuiButtonType result, InputType type, void* context);

#ifdef __cplusplus
}
#endif
