/*
 * vellum/error.h - status codes shared across the Vellum VM.
 *
 * Every fallible entry point returns a vl_status. Zero (VL_OK) is success;
 * negative values describe the failure. vl_status_str turns a code into a
 * short string for diagnostics and the command-line tools.
 */
#ifndef VELLUM_ERROR_H
#define VELLUM_ERROR_H

typedef enum vl_status {
    VL_OK = 0,

    VL_ERR_IO = -1,          /* underlying read/write failed              */
    VL_ERR_TRUNCATED = -2,   /* input ended before a structure completed  */
    VL_ERR_BAD_MAGIC = -3,   /* module magic did not match                */
    VL_ERR_BAD_VERSION = -4, /* unsupported module version                */
    VL_ERR_BAD_MODULE = -5,  /* module header/table malformed             */
    VL_ERR_BAD_CONST = -6,   /* constant pool entry malformed             */
    VL_ERR_BAD_FUNC = -7,    /* function descriptor malformed             */
    VL_ERR_BAD_CODE = -8,    /* bytecode failed verification              */
    VL_ERR_OOM = -9,         /* allocation failed                         */
    VL_ERR_LIMIT = -10,      /* a structural or runtime limit exceeded    */
    VL_ERR_TYPE = -11,       /* runtime type error                        */
    VL_ERR_STACK = -12,      /* value-stack over/underflow                */
    VL_ERR_BUDGET = -13,     /* instruction budget exhausted              */
    VL_ERR_TRAP = -14,       /* runtime trap (bad index, div by zero...)  */
    VL_ERR_CORRUPT = -15,    /* malformed encoding (e.g. bad varint)      */
    VL_ERR_OVERFLOW = -16,   /* an integer computation overflowed         */
    VL_ERR_UNSUPPORTED = -17 /* well-formed but not implemented           */
} vl_status;

/* Returns a static, never-NULL description of the status code. */
const char *vl_status_str(vl_status s);

#endif /* VELLUM_ERROR_H */
