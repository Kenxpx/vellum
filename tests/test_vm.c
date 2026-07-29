/*
 * test_vm.c - end-to-end execution tests for the Vellum VM (vm.h).
 *
 * Each case assembles a small, well-formed module with the programmatic builder
 * (builder.h), serializes it, loads and verifies it, then runs the entry
 * function with vl_run_module and checks the integer/bool result the program
 * returns via RET. Every program is benign: no heterogeneous maps, no functions
 * capturing multiple object upvalues, and no huge-locals tail calls.
 */
#include "builder.h"
#include "module.h"
#include "obj.h"
#include "opcodes.h"
#include "value.h"
#include "verify.h"
#include "vm.h"
#include "harness.h"

/*
 * Serialize builder b, load and verify the resulting module, run its entry
 * function, and store the returned value in *out. Returns the first failing
 * status, or VL_OK if the whole pipeline succeeded.
 */
static vl_status build_run(vl_builder *b, vl_value *out)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    vl_module *m = NULL;
    vl_status st;

    st = vl_builder_finish(b, &buf, &len);
    if (st != VL_OK) {
        return st;
    }
    st = vl_module_load(buf, len, &m);
    vl_free(buf);
    if (st != VL_OK) {
        return st;
    }
    st = vl_verify_module(m);
    if (st == VL_OK) {
        st = vl_run_module(m, NULL, out);
    }
    vl_module_free(m);
    return st;
}

/* (a) 3 * 4 + 2 evaluates to the integer 14. */
static void test_arith(void)
{
    vl_builder *b = vl_builder_new();
    vl_value r = vl_nil();
    int fn;

    CHECK(b != NULL);
    if (!b) {
        return;
    }
    fn = vl_builder_begin_func(b, 0, 0);
    vl_builder_set_entry(b, fn);
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 3));
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 4));
    CHECK_OK(vl_builder_emit(b, fn, OP_MUL));
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 2));
    CHECK_OK(vl_builder_emit(b, fn, OP_ADD));
    CHECK_OK(vl_builder_emit(b, fn, OP_RET));

    CHECK_OK(build_run(b, &r));
    CHECK_EQ(r.tag, VL_INT);
    CHECK_EQ(r.as.i, 14);
    vl_val_release(r);
    vl_builder_free(b);
}

/* (b) build the array [10, 20]; LEN leaves 2 on the stack, which RET returns. */
static void test_array_len(void)
{
    vl_builder *b = vl_builder_new();
    vl_value r = vl_nil();
    int fn;

    CHECK(b != NULL);
    if (!b) {
        return;
    }
    fn = vl_builder_begin_func(b, 0, 0);
    vl_builder_set_entry(b, fn);
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 10));
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 20));
    CHECK_OK(vl_builder_emit_u16(b, fn, OP_NEWARRAY, 2));
    CHECK_OK(vl_builder_emit(b, fn, OP_LEN));
    CHECK_OK(vl_builder_emit(b, fn, OP_RET));

    CHECK_OK(build_run(b, &r));
    CHECK_EQ(r.tag, VL_INT);
    CHECK_EQ(r.as.i, 2);
    vl_val_release(r);
    vl_builder_free(b);
}

/* (c) a homogeneous int map {1:100, 2:200}; MAPGET of key 2 returns 200. */
static void test_map_get(void)
{
    vl_builder *b = vl_builder_new();
    vl_value r = vl_nil();
    int fn;

    CHECK(b != NULL);
    if (!b) {
        return;
    }
    fn = vl_builder_begin_func(b, 0, 0);
    vl_builder_set_entry(b, fn);
    /* Two (key, value) pairs, both int-valued, folded into one map. */
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 1));
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 100));
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 2));
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 200));
    CHECK_OK(vl_builder_emit_u16(b, fn, OP_NEWMAP, 2));
    /* Stack: [map]; push the lookup key, then MAPGET pops key then map. */
    CHECK_OK(vl_builder_emit_i32(b, fn, OP_PUSHINT, 2));
    CHECK_OK(vl_builder_emit(b, fn, OP_MAPGET));
    CHECK_OK(vl_builder_emit(b, fn, OP_RET));

    CHECK_OK(build_run(b, &r));
    CHECK_EQ(r.tag, VL_INT);
    CHECK_EQ(r.as.i, 200);
    vl_val_release(r);
    vl_builder_free(b);
}

/* (d) a closure capturing one int local (42); calling it returns 42. */
static void test_closure_capture(void)
{
    vl_builder *b = vl_builder_new();
    vl_value r = vl_nil();
    int entry, inner;

    CHECK(b != NULL);
    if (!b) {
        return;
    }
    /* Entry: one local holds 42; make a closure over it and call it. */
    entry = vl_builder_begin_func(b, 0, 1);
    /* Inner: no locals, one upvalue (the parent local at slot 0). */
    inner = vl_builder_begin_func(b, 0, 0);
    CHECK_OK(vl_builder_add_upval(b, inner, 1, 0));
    vl_builder_set_entry(b, entry);

    CHECK_OK(vl_builder_emit_i32(b, entry, OP_PUSHINT, 42));
    CHECK_OK(vl_builder_emit_u16(b, entry, OP_STORELOCAL, 0));
    CHECK_OK(vl_builder_emit_u16(b, entry, OP_CLOSURE, (uint16_t)inner));
    CHECK_OK(vl_builder_emit_u8(b, entry, OP_CALL, 0));
    CHECK_OK(vl_builder_emit(b, entry, OP_RET));

    CHECK_OK(vl_builder_emit_u8(b, inner, OP_GETUPVAL, 0));
    CHECK_OK(vl_builder_emit(b, inner, OP_RET));

    CHECK_OK(build_run(b, &r));
    CHECK_EQ(r.tag, VL_INT);
    CHECK_EQ(r.as.i, 42);
    vl_val_release(r);
    vl_builder_free(b);
}

/* (e) a doubling function dbl(x) = x + x; dbl(21) returns 42. */
static void test_call_double(void)
{
    vl_builder *b = vl_builder_new();
    vl_value r = vl_nil();
    int entry, dbl;

    CHECK(b != NULL);
    if (!b) {
        return;
    }
    entry = vl_builder_begin_func(b, 0, 0);
    dbl = vl_builder_begin_func(b, 1, 1); /* arity 1, one local (the argument) */
    vl_builder_set_entry(b, entry);

    /* Entry: closure over dbl, push 21, call with one argument, return result. */
    CHECK_OK(vl_builder_emit_u16(b, entry, OP_CLOSURE, (uint16_t)dbl));
    CHECK_OK(vl_builder_emit_i32(b, entry, OP_PUSHINT, 21));
    CHECK_OK(vl_builder_emit_u8(b, entry, OP_CALL, 1));
    CHECK_OK(vl_builder_emit(b, entry, OP_RET));

    /* dbl: load the argument twice, add, return. */
    CHECK_OK(vl_builder_emit_u16(b, dbl, OP_LOADLOCAL, 0));
    CHECK_OK(vl_builder_emit_u16(b, dbl, OP_LOADLOCAL, 0));
    CHECK_OK(vl_builder_emit(b, dbl, OP_ADD));
    CHECK_OK(vl_builder_emit(b, dbl, OP_RET));

    CHECK_OK(build_run(b, &r));
    CHECK_EQ(r.tag, VL_INT);
    CHECK_EQ(r.as.i, 42);
    vl_val_release(r);
    vl_builder_free(b);
}

int main(void)
{
    test_arith();
    test_array_len();
    test_map_get();
    test_closure_capture();
    test_call_double();
    return TEST_SUMMARY();
}
