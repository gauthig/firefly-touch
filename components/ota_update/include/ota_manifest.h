/*
 * ota_manifest — parses the small, fixed-shape JSON manifest the OTA update
 * check fetches, and compares it against the running app version.
 *
 * Pure C, no ESP-IDF dependencies: every function here is host-testable
 * (see host_test/test_ota_manifest.c). Deliberately not a general JSON
 * parser and does not depend on cJSON -- the manifest shape is fixed to
 * exactly two string fields:
 *
 *   {"version": "1.2.3", "url": "https://example.com/fw.bin"}
 *
 * Key order does not matter, extra/unknown keys are ignored, whitespace
 * around punctuation is tolerated. Nested objects/arrays and escaped
 * characters inside string values are NOT supported -- if the manifest
 * ever needs more than two flat string fields, pull in cJSON instead of
 * growing this by hand.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_MANIFEST_MAX_VERSION_LEN  32
#define OTA_MANIFEST_MAX_URL_LEN      256

typedef struct {
    char version[OTA_MANIFEST_MAX_VERSION_LEN];
    char url[OTA_MANIFEST_MAX_URL_LEN];
} ota_manifest_t;

/*
 * Parses `json` (length `len`, need not be NUL-terminated) into `out`.
 * Returns false if either "version" or "url" is missing, empty, or would
 * overflow its fixed-size field -- callers should treat a false return as
 * "manifest unusable", not attempt a partial update.
 */
bool ota_manifest_parse(const char *json, size_t len, ota_manifest_t *out);

/*
 * True if `manifest_version` differs from `running_version`. Versions are
 * treated as opaque strings (plain case-sensitive compare) -- this project
 * derives versions from `git describe` via ESP-IDF, not hand-maintained
 * semver, so "different" is the only signal available: it does not imply
 * the manifest version is newer, only that it does not match what is
 * currently running.
 */
bool ota_manifest_is_update_available(const char *running_version, const char *manifest_version);

#ifdef __cplusplus
}
#endif
