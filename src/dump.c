/*
 * dump.c - render runtime values in a readable form for the qrun tool and
 * diagnostics. Immediates print literally; objects print a short summary.
 */
#include "dump.h"

#include <inttypes.h>

#include "obj.h"

/* Print a string object's bytes (up to 40) quoted, escaping non-printables. */
static void dump_string(FILE *f, const vl_string *s) {
    uint32_t limit = s->len < 40u ? s->len : 40u;
    uint32_t i;
    fputc('"', f);
    for (i = 0; i < limit; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if (c >= 0x20 && c < 0x7f) {
            fputc((int)c, f);
        } else {
            fprintf(f, "\\x%02X", c);
        }
    }
    fputc('"', f);
}

/* Print a single runtime value (nil/bool/int/real, or an object summary). */
void vl_dump_value(FILE *f, vl_value v) {
    switch (v.tag) {
    case VL_NIL:
        fputs("nil", f);
        break;
    case VL_BOOL:
        fputs(v.as.i ? "true" : "false", f);
        break;
    case VL_INT:
        fprintf(f, "%lld", (long long)v.as.i);
        break;
    case VL_REAL:
        fprintf(f, "%g", v.as.d);
        break;
    case VL_OBJ: {
        vl_obj *o = v.as.o;
        if (o == NULL) {
            fputs("<obj>", f);
            break;
        }
        switch (o->otype) {
        case VL_OBJ_STRING:
            dump_string(f, (const vl_string *)o);
            break;
        case VL_OBJ_ARRAY:
            fprintf(f, "[array len=%u]", ((const vl_array *)o)->len);
            break;
        case VL_OBJ_MAP:
            fprintf(f, "{map count=%u}", ((const vl_map *)o)->count);
            break;
        case VL_OBJ_CLOSURE:
            fprintf(f, "<closure fn=%u>",
                    (unsigned)((const vl_closure *)o)->func);
            break;
        case VL_OBJ_UPVALUE:
            fputs("<upvalue>", f);
            break;
        default:
            fputs("<obj>", f);
            break;
        }
        break;
    }
    default:
        fputs("<obj>", f);
        break;
    }
}
