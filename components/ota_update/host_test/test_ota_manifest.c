/*
 * Host-runnable unit tests for ota_manifest (plain asserts, no framework).
 *
 * Build & run with any host C compiler, e.g.:
 *   gcc -Wall -Wextra -I../include ../ota_manifest.c test_ota_manifest.c -o test_ota_manifest && ./test_ota_manifest
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ota_manifest.h"

static void test_parse_basic(void)
{
    const char *json = "{\"version\": \"1.2.3\", \"url\": \"https://example.com/fw.bin\"}";
    ota_manifest_t m;
    bool ok = ota_manifest_parse(json, strlen(json), &m);
    assert(ok);
    assert(strcmp(m.version, "1.2.3") == 0);
    assert(strcmp(m.url, "https://example.com/fw.bin") == 0);
    printf("PASS parse_basic\n");
}

static void test_parse_key_order_and_whitespace(void)
{
    /* url first, no spaces around ':' */
    const char *json = "{\"url\":\"https://x/y.bin\",\"version\":\"abc123\"}";
    ota_manifest_t m;
    bool ok = ota_manifest_parse(json, strlen(json), &m);
    assert(ok);
    assert(strcmp(m.version, "abc123") == 0);
    assert(strcmp(m.url, "https://x/y.bin") == 0);
    printf("PASS parse_key_order_and_whitespace\n");
}

static void test_parse_ignores_extra_keys(void)
{
    const char *json = "{\"note\": \"ignore me\", \"version\": \"2.0.0\", "
                        "\"url\": \"https://x/fw.bin\", \"size\": 12345}";
    ota_manifest_t m;
    bool ok = ota_manifest_parse(json, strlen(json), &m);
    assert(ok);
    assert(strcmp(m.version, "2.0.0") == 0);
    assert(strcmp(m.url, "https://x/fw.bin") == 0);
    printf("PASS parse_ignores_extra_keys\n");
}

static void test_parse_missing_field_fails(void)
{
    ota_manifest_t m;

    const char *no_url = "{\"version\": \"1.0.0\"}";
    assert(!ota_manifest_parse(no_url, strlen(no_url), &m));

    const char *no_version = "{\"url\": \"https://x/fw.bin\"}";
    assert(!ota_manifest_parse(no_version, strlen(no_version), &m));

    const char *empty_value = "{\"version\": \"\", \"url\": \"https://x/fw.bin\"}";
    assert(!ota_manifest_parse(empty_value, strlen(empty_value), &m));

    assert(!ota_manifest_parse(NULL, 0, &m));
    assert(!ota_manifest_parse("", 0, &m));

    printf("PASS parse_missing_field_fails\n");
}

static void test_parse_oversized_field_fails(void)
{
    char json[512];
    char long_url[OTA_MANIFEST_MAX_URL_LEN + 32];
    memset(long_url, 'a', sizeof(long_url) - 1);
    long_url[sizeof(long_url) - 1] = '\0';
    snprintf(json, sizeof(json), "{\"version\": \"1.0.0\", \"url\": \"%s\"}", long_url);

    ota_manifest_t m;
    assert(!ota_manifest_parse(json, strlen(json), &m));
    printf("PASS parse_oversized_field_fails\n");
}

static void test_is_update_available(void)
{
    assert(ota_manifest_is_update_available("1.0.0", "1.0.1"));
    assert(!ota_manifest_is_update_available("1.0.0", "1.0.0"));
    assert(!ota_manifest_is_update_available(NULL, "1.0.0"));
    assert(!ota_manifest_is_update_available("1.0.0", NULL));
    printf("PASS is_update_available\n");
}

int main(void)
{
    test_parse_basic();
    test_parse_key_order_and_whitespace();
    test_parse_ignores_extra_keys();
    test_parse_missing_field_fails();
    test_parse_oversized_field_fails();
    test_is_update_available();
    printf("ALL TESTS PASSED\n");
    return 0;
}
