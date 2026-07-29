/*
 * test_crc32.c - unit tests for crc32.h.
 *
 * Verifies the canonical CRC-32 check value, the empty-input result, and that
 * an incremental two-part computation matches the one-shot CRC of the whole.
 */
#include "crc32.h"
#include "harness.h"

/* Run the CRC-32 tests and report a summary. */
int main(void)
{
    const char *msg = "123456789";

    /* Canonical CRC-32 check value for the ASCII string "123456789". */
    CHECK_EQ(vl_crc32(msg, 9), 0xCBF43926u);

    /* The CRC-32 of empty input is 0. */
    CHECK_EQ(vl_crc32(NULL, 0), 0u);
    CHECK_EQ(vl_crc32(msg, 0), 0u);

    /* Incremental update in two halves equals the one-shot CRC. */
    {
        uint32_t one_shot = vl_crc32(msg, 9);
        uint32_t crc = vl_crc32_update(0, msg, 4);
        crc = vl_crc32_update(crc, msg + 4, 5);
        CHECK_EQ(crc, one_shot);
    }

    /* A single incremental update seeded with 0 equals the one-shot CRC. */
    CHECK_EQ(vl_crc32_update(0, msg, 9), vl_crc32(msg, 9));

    return TEST_SUMMARY();
}
