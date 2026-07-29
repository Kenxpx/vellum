/*
 * builder.h - a programmatic builder that assembles a Vellum module in memory
 * and serializes it to the .qbc byte layout the loader reads. Used by the text
 * assembler, the tests, and the qasm tool.
 */
#ifndef VELLUM_BUILDER_H
#define VELLUM_BUILDER_H

#include "internal.h"

typedef struct vl_builder vl_builder;

vl_builder *vl_builder_new(void);
void vl_builder_free(vl_builder *b);

/* Add a constant, returning its pool index (or -1 on allocation failure). */
int vl_builder_const_int(vl_builder *b, int64_t v);
int vl_builder_const_real(vl_builder *b, double v);
int vl_builder_const_string(vl_builder *b, const char *s, uint32_t len);

/* Begin a function (arity, local count); returns its function index. */
int vl_builder_begin_func(vl_builder *b, uint8_t arity, uint16_t num_locals);
/* Add an upvalue capture descriptor to a function. */
vl_status vl_builder_add_upval(vl_builder *b, int func, uint8_t is_local,
                               uint8_t index);
/* Set the module entry function. */
void vl_builder_set_entry(vl_builder *b, int func);

/* Emit instructions into a function (operand form must match the opcode). */
vl_status vl_builder_emit(vl_builder *b, int func, uint8_t op);
vl_status vl_builder_emit_u8(vl_builder *b, int func, uint8_t op, uint8_t a);
vl_status vl_builder_emit_u16(vl_builder *b, int func, uint8_t op, uint16_t a);
vl_status vl_builder_emit_i16(vl_builder *b, int func, uint8_t op, int16_t a);
vl_status vl_builder_emit_i32(vl_builder *b, int func, uint8_t op, int32_t a);

/* Serialize to a freshly allocated .qbc buffer (*out, *out_len); caller frees. */
vl_status vl_builder_finish(vl_builder *b, uint8_t **out, size_t *out_len);

#endif /* VELLUM_BUILDER_H */
