/*
 * dump.h - render runtime values in a readable form, used by the qrun tool and
 * diagnostics.
 */
#ifndef VELLUM_DUMP_H
#define VELLUM_DUMP_H

#include <stdio.h>

#include "internal.h"
#include "value.h"

/* Print a single runtime value (nil/bool/int/real, or an object summary). */
void vl_dump_value(FILE *f, vl_value v);

#endif /* VELLUM_DUMP_H */
