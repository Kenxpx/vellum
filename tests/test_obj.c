/*
 * test_obj.c - direct unit tests for the reference-counted heap objects in
 * obj.h: strings, arrays, and maps, plus the vl_retain/vl_release lifecycle.
 * Every program here is benign and every object is released exactly once.
 */
#include "obj.h"
#include "value.h"
#include "internal.h"

#include "harness.h"

/* Exercise vl_string_new: length, byte contents, and NUL terminator. */
static void test_string(void) {
    vl_string *s = vl_string_new("hi", 2);
    CHECK(s != NULL);
    CHECK_EQ(s->len, 2);
    CHECK(memcmp(s->data, "hi", 2) == 0);
    CHECK_EQ(s->data[2], '\0');
    vl_release((vl_obj *)s);
}

/* Push three ints, read each back, overwrite index 1, then release. */
static void test_array(void) {
    vl_array *a = vl_array_new();
    vl_value out;
    CHECK(a != NULL);

    CHECK_OK(vl_array_push(a, vl_int(10)));
    CHECK_OK(vl_array_push(a, vl_int(20)));
    CHECK_OK(vl_array_push(a, vl_int(30)));
    CHECK_EQ(a->len, 3);

    CHECK_OK(vl_array_get(a, 0, &out));
    CHECK_EQ(out.tag, VL_INT);
    CHECK_EQ(out.as.i, 10);
    CHECK_OK(vl_array_get(a, 1, &out));
    CHECK_EQ(out.as.i, 20);
    CHECK_OK(vl_array_get(a, 2, &out));
    CHECK_EQ(out.as.i, 30);

    /* Out-of-range access traps. */
    CHECK_EQ(vl_array_get(a, 3, &out), VL_ERR_TRAP);

    CHECK_OK(vl_array_set(a, 1, vl_int(99)));
    CHECK_OK(vl_array_get(a, 1, &out));
    CHECK_EQ(out.as.i, 99);

    vl_release((vl_obj *)a);
}

/* Set int keys, read them, overwrite one, add a string key, then release. */
static void test_map(void) {
    vl_map *m = vl_map_new();
    vl_string *key;
    vl_value out;
    CHECK(m != NULL);

    CHECK_OK(vl_map_set(m, vl_int(1), vl_int(100)));
    CHECK_OK(vl_map_set(m, vl_int(2), vl_int(200)));

    CHECK_EQ(vl_map_get(m, vl_int(1), &out), 1);
    CHECK_EQ(out.as.i, 100);
    CHECK_EQ(vl_map_get(m, vl_int(2), &out), 1);
    CHECK_EQ(out.as.i, 200);

    /* Missing key reports no hit. */
    CHECK_EQ(vl_map_get(m, vl_int(3), &out), 0);

    /* Overwrite key 1. */
    CHECK_OK(vl_map_set(m, vl_int(1), vl_int(111)));
    CHECK_EQ(vl_map_get(m, vl_int(1), &out), 1);
    CHECK_EQ(out.as.i, 111);

    /* A string key maps to an int value (kind stays homogeneous). */
    key = vl_string_new("k", 1);
    CHECK(key != NULL);
    CHECK_OK(vl_map_set(m, vl_obj_val((vl_obj *)key), vl_int(500)));
    CHECK_EQ(vl_map_get(m, vl_obj_val((vl_obj *)key), &out), 1);
    CHECK_EQ(out.as.i, 500);

    /* Drop our own reference to the key; the map still holds one. */
    vl_release((vl_obj *)key);
    vl_release((vl_obj *)m);
}

/* An extra retain keeps an object alive until the matching release. */
static void test_refcount(void) {
    vl_string *s = vl_string_new("live", 4);
    CHECK(s != NULL);
    CHECK_EQ(s->head.rc, 1);

    vl_retain((vl_obj *)s);
    CHECK_EQ(s->head.rc, 2);

    /* First release drops the extra ref; object stays alive and usable. */
    vl_release((vl_obj *)s);
    CHECK_EQ(s->head.rc, 1);
    CHECK(memcmp(s->data, "live", 4) == 0);

    /* Final release frees it. */
    vl_release((vl_obj *)s);
}

int main(void) {
    test_string();
    test_array();
    test_map();
    test_refcount();
    return TEST_SUMMARY();
}
