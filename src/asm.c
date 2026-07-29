/*
 * asm.c - a small line-oriented text assembler for Vellum.
 *
 * Parses assembly source (see asm.h for the grammar) into a .qbc module using
 * the builder. Directives create constants, functions, upvalue descriptors, and
 * set the entry function; labels mark jump targets; every other line is a single
 * instruction. Each function body is processed in two passes: the first collects
 * its instructions (recording label offsets by summing instruction sizes), the
 * second resolves each jump's label to a relative displacement and emits.
 */
#include "asm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "builder.h"
#include "internal.h"
#include "opcodes.h"

/* One collected instruction awaiting emission. */
typedef struct {
    int line;         /* source line, for error messages                  */
    uint8_t op;       /* opcode                                           */
    int size;         /* inline operand size in bytes (0/1/2/4)           */
    int is_jump;      /* 1 if the operand is a label (relative jump)       */
    int64_t imm;      /* parsed integer operand (when !is_jump)            */
    char *label;      /* target label name (when is_jump), heap-owned      */
    uint32_t offset;  /* byte offset of this instruction in the function   */
} ins_t;

/* A label definition within the current function. */
typedef struct {
    char *name;       /* heap-owned label name                            */
    uint32_t offset;  /* code offset the label points at                  */
} label_t;

/* Assembler working state. */
typedef struct {
    vl_builder *b;
    ins_t *ins;
    size_t nins, cap_ins;
    label_t *labels;
    size_t nlabels, cap_labels;
    int cur_func;         /* current function index, or -1 if none         */
    uint32_t cur_offset;  /* running code offset within the current func   */
} asm_state;

/* True for the horizontal whitespace the assembler treats as a separator. */
static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* Write "line N: <msg>" into errbuf if one was provided. */
static void set_err(char *errbuf, size_t errcap, int line, const char *msg) {
    if (errbuf != NULL && errcap > 0) {
        snprintf(errbuf, errcap, "line %d: %s", line, msg);
    }
}

/* Compare a (possibly non-terminated) token against a literal string. */
static int tok_eq(const char *tok, size_t len, const char *lit) {
    return strlen(lit) == len && memcmp(tok, lit, len) == 0;
}

/* Copy a byte range into a fresh NUL-terminated string (NULL on OOM). */
static char *dup_range(const char *s, size_t n) {
    char *p;
    size_t bytes;
    if (!vl_size_add(n, 1, &bytes)) {
        return NULL;
    }
    p = vl_malloc(bytes);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/*
 * Read the next whitespace-delimited token from [*p, e). On return *p advances
 * past the token; tok and toklen describe it (toklen == 0 when none remain).
 */
static void next_token(const char **p, const char *e, const char **tok,
                       size_t *toklen) {
    const char *q = *p;
    const char *start;
    while (q < e && is_ws(*q)) {
        q++;
    }
    start = q;
    while (q < e && !is_ws(*q)) {
        q++;
    }
    *tok = start;
    *toklen = (size_t)(q - start);
    *p = q;
}

/* Parse a base-10 signed integer from a token. Returns 1 on success. */
static int parse_int(const char *tok, size_t len, int64_t *out) {
    char buf[32];
    char *end;
    long long v;
    if (len == 0 || len >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, tok, len);
    buf[len] = '\0';
    errno = 0;
    v = strtoll(buf, &end, 10);
    if (end != buf + len || errno == ERANGE) {
        return 0;
    }
    *out = (int64_t)v;
    return 1;
}

/* Parse a floating-point literal from a token. Returns 1 on success. */
static int parse_real(const char *tok, size_t len, double *out) {
    char buf[64];
    char *end;
    double v;
    if (len == 0 || len >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, tok, len);
    buf[len] = '\0';
    errno = 0;
    v = strtod(buf, &end);
    if (end != buf + len) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Append one byte to a growable buffer. Returns a vl_status. */
static vl_status buf_push(uint8_t **buf, size_t *cap, size_t *n, uint8_t c) {
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        uint8_t *p = vl_realloc(*buf, ncap);
        if (p == NULL) {
            return VL_ERR_OOM;
        }
        *buf = p;
        *cap = ncap;
    }
    (*buf)[(*n)++] = c;
    return VL_OK;
}

/*
 * Parse a double-quoted string literal starting at *p == '"'. Recognizes the
 * escapes \n \t \\ and \". On success stores a heap buffer in *out (caller
 * frees) with *outlen bytes and advances *p past the closing quote.
 */
static vl_status parse_string_lit(const char **p, const char *e, uint8_t **out,
                                  uint32_t *outlen) {
    const char *q = *p;
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    vl_status rc;

    q++; /* skip the opening quote */
    while (q < e) {
        char c = *q;
        uint8_t b;
        if (c == '"') {
            q++;
            *p = q;
            *out = buf;
            *outlen = (uint32_t)n;
            return VL_OK;
        }
        if (c == '\\') {
            if (q + 1 >= e) {
                vl_free(buf);
                return VL_ERR_BAD_CODE;
            }
            switch (q[1]) {
            case 'n': b = (uint8_t)'\n'; break;
            case 't': b = (uint8_t)'\t'; break;
            case '\\': b = (uint8_t)'\\'; break;
            case '"': b = (uint8_t)'"'; break;
            default:
                vl_free(buf);
                return VL_ERR_BAD_CODE;
            }
            q += 2;
        } else {
            b = (uint8_t)c;
            q++;
        }
        rc = buf_push(&buf, &cap, &n, b);
        if (rc != VL_OK) {
            vl_free(buf);
            return rc;
        }
    }
    /* Ran off the line without a closing quote. */
    vl_free(buf);
    return VL_ERR_BAD_CODE;
}

/* Ensure room for one more collected instruction. Returns a vl_status. */
static vl_status ensure_ins(asm_state *st) {
    if (st->nins == st->cap_ins) {
        size_t ncap = st->cap_ins ? st->cap_ins * 2 : 16;
        size_t bytes;
        ins_t *p;
        if (!vl_size_mul(ncap, sizeof(ins_t), &bytes)) {
            return VL_ERR_OVERFLOW;
        }
        p = vl_realloc(st->ins, bytes);
        if (p == NULL) {
            return VL_ERR_OOM;
        }
        st->ins = p;
        st->cap_ins = ncap;
    }
    return VL_OK;
}

/* Ensure room for one more label. Returns a vl_status. */
static vl_status ensure_labels(asm_state *st) {
    if (st->nlabels == st->cap_labels) {
        size_t ncap = st->cap_labels ? st->cap_labels * 2 : 8;
        size_t bytes;
        label_t *p;
        if (!vl_size_mul(ncap, sizeof(label_t), &bytes)) {
            return VL_ERR_OVERFLOW;
        }
        p = vl_realloc(st->labels, bytes);
        if (p == NULL) {
            return VL_ERR_OOM;
        }
        st->labels = p;
        st->cap_labels = ncap;
    }
    return VL_OK;
}

/* Look up a label by name; returns 1 and stores its offset when found. */
static int find_label(asm_state *st, const char *name, uint32_t *offset) {
    size_t i;
    for (i = 0; i < st->nlabels; i++) {
        if (strcmp(st->labels[i].name, name) == 0) {
            *offset = st->labels[i].offset;
            return 1;
        }
    }
    return 0;
}

/* Drop all collected instructions and labels, freeing their names. */
static void reset_pending(asm_state *st) {
    size_t i;
    for (i = 0; i < st->nins; i++) {
        vl_free(st->ins[i].label);
    }
    for (i = 0; i < st->nlabels; i++) {
        vl_free(st->labels[i].name);
    }
    vl_free(st->ins);
    vl_free(st->labels);
    st->ins = NULL;
    st->labels = NULL;
    st->nins = st->cap_ins = 0;
    st->nlabels = st->cap_labels = 0;
    st->cur_offset = 0;
}

/*
 * Second pass for the current function: resolve every jump's label to a
 * relative displacement and emit all collected instructions into the builder.
 * Clears the pending buffers afterward.
 */
static vl_status st_flush(asm_state *st, char *errbuf, size_t errcap) {
    size_t i;
    vl_status rc = VL_OK;

    for (i = 0; i < st->nins; i++) {
        ins_t *in = &st->ins[i];
        if (in->is_jump) {
            uint32_t target;
            int64_t rel;
            if (!find_label(st, in->label, &target)) {
                set_err(errbuf, errcap, in->line, "undefined label");
                rc = VL_ERR_BAD_CODE;
                break;
            }
            rel = (int64_t)target - (int64_t)(in->offset + 1 + (uint32_t)in->size);
            if (rel < -32768 || rel > 32767) {
                set_err(errbuf, errcap, in->line, "jump out of range");
                rc = VL_ERR_BAD_CODE;
                break;
            }
            rc = vl_builder_emit_i16(st->b, st->cur_func, in->op, (int16_t)rel);
        } else {
            switch (in->size) {
            case 0:
                rc = vl_builder_emit(st->b, st->cur_func, in->op);
                break;
            case 1:
                rc = vl_builder_emit_u8(st->b, st->cur_func, in->op,
                                        (uint8_t)in->imm);
                break;
            case 2:
                rc = vl_builder_emit_u16(st->b, st->cur_func, in->op,
                                         (uint16_t)in->imm);
                break;
            case 4:
                rc = vl_builder_emit_i32(st->b, st->cur_func, in->op,
                                         (int32_t)in->imm);
                break;
            default:
                rc = VL_ERR_BAD_CODE;
                break;
            }
        }
        if (rc != VL_OK) {
            set_err(errbuf, errcap, in->line, "emit failed");
            break;
        }
    }

    reset_pending(st);
    return rc;
}

/* Handle a label definition line ("name:"). Returns a vl_status. */
static vl_status do_label(asm_state *st, const char *s, const char *e, int line,
                          char *errbuf, size_t errcap) {
    const char *ne = e - 1; /* drop the trailing ':' */
    const char *scan;
    char *name;
    vl_status rc;
    uint32_t dummy;

    while (ne > s && is_ws(*(ne - 1))) {
        ne--;
    }
    if (ne <= s) {
        set_err(errbuf, errcap, line, "empty label");
        return VL_ERR_BAD_CODE;
    }
    for (scan = s; scan < ne; scan++) {
        if (is_ws(*scan)) {
            set_err(errbuf, errcap, line, "bad label");
            return VL_ERR_BAD_CODE;
        }
    }
    if (st->cur_func < 0) {
        set_err(errbuf, errcap, line, "label outside function");
        return VL_ERR_BAD_CODE;
    }
    name = dup_range(s, (size_t)(ne - s));
    if (name == NULL) {
        set_err(errbuf, errcap, line, "out of memory");
        return VL_ERR_OOM;
    }
    if (find_label(st, name, &dummy)) {
        vl_free(name);
        set_err(errbuf, errcap, line, "duplicate label");
        return VL_ERR_BAD_CODE;
    }
    rc = ensure_labels(st);
    if (rc != VL_OK) {
        vl_free(name);
        set_err(errbuf, errcap, line, "out of memory");
        return rc;
    }
    st->labels[st->nlabels].name = name;
    st->labels[st->nlabels].offset = st->cur_offset;
    st->nlabels++;
    return VL_OK;
}

/* Collect one instruction line into the pending buffer. Returns a vl_status. */
static vl_status do_instruction(asm_state *st, const char *s, const char *e,
                                int line, char *errbuf, size_t errcap) {
    const char *p = s;
    const char *tok;
    size_t len;
    int op = -1;
    int i;
    int size;
    int is_jump;
    ins_t *in;
    vl_status rc;

    if (st->cur_func < 0) {
        set_err(errbuf, errcap, line, "instruction outside function");
        return VL_ERR_BAD_CODE;
    }

    next_token(&p, e, &tok, &len);
    for (i = 0; i < OP__COUNT; i++) {
        if (tok_eq(tok, len, vl_op_name(i))) {
            op = i;
            break;
        }
    }
    if (op < 0) {
        set_err(errbuf, errcap, line, "unknown mnemonic");
        return VL_ERR_BAD_CODE;
    }

    size = vl_op_operand_size(op);
    is_jump = (op == OP_JMP || op == OP_JMPIF || op == OP_JMPIFNOT);

    rc = ensure_ins(st);
    if (rc != VL_OK) {
        set_err(errbuf, errcap, line, "out of memory");
        return rc;
    }
    in = &st->ins[st->nins];
    in->line = line;
    in->op = (uint8_t)op;
    in->size = size;
    in->is_jump = is_jump;
    in->imm = 0;
    in->label = NULL;
    in->offset = st->cur_offset;

    if (size == 0) {
        next_token(&p, e, &tok, &len);
        if (len != 0) {
            set_err(errbuf, errcap, line, "unexpected operand");
            return VL_ERR_BAD_CODE;
        }
    } else {
        next_token(&p, e, &tok, &len);
        if (len == 0) {
            set_err(errbuf, errcap, line, "missing operand");
            return VL_ERR_BAD_CODE;
        }
        if (is_jump) {
            char *name = dup_range(tok, len);
            if (name == NULL) {
                set_err(errbuf, errcap, line, "out of memory");
                return VL_ERR_OOM;
            }
            in->label = name;
        } else {
            int64_t v;
            if (!parse_int(tok, len, &v)) {
                set_err(errbuf, errcap, line, "bad integer operand");
                return VL_ERR_BAD_CODE;
            }
            if (size == 1 && (v < 0 || v > 0xFF)) {
                set_err(errbuf, errcap, line, "operand out of range");
                return VL_ERR_BAD_CODE;
            }
            if (size == 2 && (v < 0 || v > 0xFFFF)) {
                set_err(errbuf, errcap, line, "operand out of range");
                return VL_ERR_BAD_CODE;
            }
            if (size == 4 && (v < INT32_MIN || v > INT32_MAX)) {
                set_err(errbuf, errcap, line, "operand out of range");
                return VL_ERR_BAD_CODE;
            }
            in->imm = v;
        }
        /* Reject any trailing tokens after the operand. */
        next_token(&p, e, &tok, &len);
        if (len != 0) {
            vl_free(in->label);
            in->label = NULL;
            set_err(errbuf, errcap, line, "trailing tokens");
            return VL_ERR_BAD_CODE;
        }
    }

    st->nins++;
    st->cur_offset += 1u + (uint32_t)size;
    return VL_OK;
}

/* Handle a ".const ..." directive. Returns a vl_status. */
static vl_status do_const(asm_state *st, const char **p, const char *e, int line,
                          char *errbuf, size_t errcap) {
    const char *tok;
    size_t len;

    next_token(p, e, &tok, &len);
    if (tok_eq(tok, len, "int")) {
        int64_t v;
        next_token(p, e, &tok, &len);
        if (len == 0 || !parse_int(tok, len, &v)) {
            set_err(errbuf, errcap, line, "bad int constant");
            return VL_ERR_BAD_CODE;
        }
        next_token(p, e, &tok, &len);
        if (len != 0) {
            set_err(errbuf, errcap, line, "trailing tokens");
            return VL_ERR_BAD_CODE;
        }
        if (vl_builder_const_int(st->b, v) < 0) {
            set_err(errbuf, errcap, line, "out of memory");
            return VL_ERR_OOM;
        }
        return VL_OK;
    }
    if (tok_eq(tok, len, "real")) {
        double v;
        next_token(p, e, &tok, &len);
        if (len == 0 || !parse_real(tok, len, &v)) {
            set_err(errbuf, errcap, line, "bad real constant");
            return VL_ERR_BAD_CODE;
        }
        next_token(p, e, &tok, &len);
        if (len != 0) {
            set_err(errbuf, errcap, line, "trailing tokens");
            return VL_ERR_BAD_CODE;
        }
        if (vl_builder_const_real(st->b, v) < 0) {
            set_err(errbuf, errcap, line, "out of memory");
            return VL_ERR_OOM;
        }
        return VL_OK;
    }
    if (tok_eq(tok, len, "string")) {
        const char *q = *p;
        uint8_t *data = NULL;
        uint32_t dlen = 0;
        vl_status rc;
        while (q < e && is_ws(*q)) {
            q++;
        }
        if (q >= e || *q != '"') {
            set_err(errbuf, errcap, line, "expected string literal");
            return VL_ERR_BAD_CODE;
        }
        rc = parse_string_lit(&q, e, &data, &dlen);
        if (rc != VL_OK) {
            set_err(errbuf, errcap, line, "bad string literal");
            return rc;
        }
        *p = q;
        next_token(p, e, &tok, &len);
        if (len != 0) {
            vl_free(data);
            set_err(errbuf, errcap, line, "trailing tokens");
            return VL_ERR_BAD_CODE;
        }
        if (vl_builder_const_string(st->b, (const char *)data, dlen) < 0) {
            vl_free(data);
            set_err(errbuf, errcap, line, "out of memory");
            return VL_ERR_OOM;
        }
        vl_free(data);
        return VL_OK;
    }
    set_err(errbuf, errcap, line, "bad .const kind");
    return VL_ERR_BAD_CODE;
}

/* Handle a directive line (leading '.'). Returns a vl_status. */
static vl_status do_directive(asm_state *st, const char *s, const char *e,
                              int line, char *errbuf, size_t errcap) {
    const char *p = s;
    const char *tok;
    size_t len;

    next_token(&p, e, &tok, &len);
    if (tok_eq(tok, len, ".const")) {
        return do_const(st, &p, e, line, errbuf, errcap);
    }
    if (tok_eq(tok, len, ".func")) {
        int64_t arity, locals;
        int idx;
        vl_status rc;
        next_token(&p, e, &tok, &len);
        if (len == 0 || !parse_int(tok, len, &arity) || arity < 0 || arity > 0xFF) {
            set_err(errbuf, errcap, line, "bad .func arity");
            return VL_ERR_BAD_CODE;
        }
        next_token(&p, e, &tok, &len);
        if (len == 0 || !parse_int(tok, len, &locals) || locals < 0 ||
            locals > 0xFFFF) {
            set_err(errbuf, errcap, line, "bad .func locals");
            return VL_ERR_BAD_CODE;
        }
        next_token(&p, e, &tok, &len);
        if (len != 0) {
            set_err(errbuf, errcap, line, "trailing tokens");
            return VL_ERR_BAD_CODE;
        }
        /* Emit the previously collected function before starting a new one. */
        if (st->cur_func >= 0) {
            rc = st_flush(st, errbuf, errcap);
            if (rc != VL_OK) {
                return rc;
            }
        }
        idx = vl_builder_begin_func(st->b, (uint8_t)arity, (uint16_t)locals);
        if (idx < 0) {
            set_err(errbuf, errcap, line, "out of memory");
            return VL_ERR_OOM;
        }
        st->cur_func = idx;
        st->cur_offset = 0;
        return VL_OK;
    }
    if (tok_eq(tok, len, ".upval")) {
        int64_t idx;
        uint8_t is_local;
        vl_status rc;
        next_token(&p, e, &tok, &len);
        if (tok_eq(tok, len, "local")) {
            is_local = 1;
        } else if (tok_eq(tok, len, "upval")) {
            is_local = 0;
        } else {
            set_err(errbuf, errcap, line, "bad .upval kind");
            return VL_ERR_BAD_CODE;
        }
        next_token(&p, e, &tok, &len);
        if (len == 0 || !parse_int(tok, len, &idx) || idx < 0 || idx > 0xFF) {
            set_err(errbuf, errcap, line, "bad .upval index");
            return VL_ERR_BAD_CODE;
        }
        next_token(&p, e, &tok, &len);
        if (len != 0) {
            set_err(errbuf, errcap, line, "trailing tokens");
            return VL_ERR_BAD_CODE;
        }
        if (st->cur_func < 0) {
            set_err(errbuf, errcap, line, "upval outside function");
            return VL_ERR_BAD_CODE;
        }
        rc = vl_builder_add_upval(st->b, st->cur_func, is_local, (uint8_t)idx);
        if (rc != VL_OK) {
            set_err(errbuf, errcap, line, "add upval failed");
            return rc;
        }
        return VL_OK;
    }
    if (tok_eq(tok, len, ".entry")) {
        int64_t n;
        next_token(&p, e, &tok, &len);
        if (len == 0 || !parse_int(tok, len, &n) || n < 0 || n > INT32_MAX) {
            set_err(errbuf, errcap, line, "bad .entry index");
            return VL_ERR_BAD_CODE;
        }
        next_token(&p, e, &tok, &len);
        if (len != 0) {
            set_err(errbuf, errcap, line, "trailing tokens");
            return VL_ERR_BAD_CODE;
        }
        vl_builder_set_entry(st->b, (int)n);
        return VL_OK;
    }
    set_err(errbuf, errcap, line, "unknown directive");
    return VL_ERR_BAD_CODE;
}

/*
 * Trim a raw line [start, lineend) down to its significant content: strip a
 * '#' comment (respecting double-quoted strings) and surrounding whitespace.
 * Stores the trimmed range in s_out and e_out.
 */
static void trim_line(const char *start, const char *lineend,
                      const char **s_out, const char **e_out) {
    const char *p = start;
    const char *content_end = lineend;
    const char *s, *ee;
    int in_str = 0;

    while (p < lineend) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p + 1 < lineend) {
                p += 2;
                continue;
            }
            if (c == '"') {
                in_str = 0;
            }
            p++;
        } else {
            if (c == '#') {
                content_end = p;
                break;
            }
            if (c == '"') {
                in_str = 1;
            }
            p++;
        }
    }

    s = start;
    while (s < content_end && is_ws(*s)) {
        s++;
    }
    ee = content_end;
    while (ee > s && is_ws(*(ee - 1))) {
        ee--;
    }
    *s_out = s;
    *e_out = ee;
}

/* Dispatch a single trimmed, non-empty source line. Returns a vl_status. */
static vl_status do_line(asm_state *st, const char *s, const char *e, int line,
                         char *errbuf, size_t errcap) {
    if (*s == '.') {
        return do_directive(st, s, e, line, errbuf, errcap);
    }
    if (e > s && *(e - 1) == ':') {
        return do_label(st, s, e, line, errbuf, errcap);
    }
    return do_instruction(st, s, e, line, errbuf, errcap);
}

/*
 * Assemble text[0,len) into a freshly allocated .qbc buffer (*out, *out_len).
 * On error returns a non-OK status and writes "line N: <reason>" into errbuf.
 */
vl_status vl_assemble(const char *text, size_t len, uint8_t **out,
                      size_t *out_len, char *errbuf, size_t errcap) {
    asm_state st;
    const char *p = text;
    const char *end = text + len;
    int line_no = 0;
    vl_status rc = VL_OK;

    memset(&st, 0, sizeof(st));
    st.cur_func = -1;

    if (out == NULL || out_len == NULL || (text == NULL && len != 0)) {
        set_err(errbuf, errcap, 0, "invalid arguments");
        return VL_ERR_IO;
    }

    st.b = vl_builder_new();
    if (st.b == NULL) {
        set_err(errbuf, errcap, 0, "out of memory");
        return VL_ERR_OOM;
    }

    while (p < end) {
        const char *lineend = p;
        const char *s, *e;
        line_no++;
        while (lineend < end && *lineend != '\n') {
            lineend++;
        }
        trim_line(p, lineend, &s, &e);
        if (s < e) {
            rc = do_line(&st, s, e, line_no, errbuf, errcap);
            if (rc != VL_OK) {
                goto done;
            }
        }
        p = (lineend < end) ? lineend + 1 : end;
    }

    /* Emit the final function's collected instructions. */
    if (st.cur_func >= 0) {
        rc = st_flush(&st, errbuf, errcap);
        if (rc != VL_OK) {
            goto done;
        }
    }

    rc = vl_builder_finish(st.b, out, out_len);
    if (rc != VL_OK) {
        set_err(errbuf, errcap, line_no, "module finalization failed");
    }

done:
    reset_pending(&st);
    vl_builder_free(st.b);
    return rc;
}
