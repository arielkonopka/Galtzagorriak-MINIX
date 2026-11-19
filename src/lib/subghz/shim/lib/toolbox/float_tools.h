/* libsubghz: lib/toolbox/float_tools.h (stock), the one function used. */
#pragma once
#include <stdbool.h>
static inline bool float_is_equal(float a, float b) {
    float d = a - b;
    return (d < 0 ? -d : d) < 1.1920928955078125e-07f;
}
