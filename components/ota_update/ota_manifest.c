#include "ota_manifest.h"

#include <string.h>

/*
 * Finds the string value for `"key": "value"` inside `json[0..len)` and
 * copies it (NUL-terminated) into `out` (capacity `out_cap`). Returns
 * false if the key is not found, its value is not a quoted string, the
 * string is empty, or it would not fit in `out`.
 *
 * Deliberately simple: scans for `"key"`, then the next `:`, then the
 * next `"..."` pair. Does not handle escaped quotes inside the value --
 * fine for version strings and plain URLs, which never contain a `"`.
 */
static bool find_string_field(const char *json, size_t len, const char *key,
                               char *out, size_t out_cap)
{
    size_t key_len = strlen(key);
    /* quoted key needs room for the two quotes around it */
    if (key_len + 2 > len) {
        return false;
    }

    for (size_t i = 0; i + key_len + 2 <= len; i++) {
        if (json[i] != '"' || strncmp(&json[i + 1], key, key_len) != 0 ||
            json[i + 1 + key_len] != '"') {
            continue;
        }

        size_t pos = i + 1 + key_len + 1;

        /* skip whitespace then the ':' then whitespace */
        while (pos < len && (json[pos] == ' ' || json[pos] == '\t' ||
                              json[pos] == '\n' || json[pos] == '\r')) {
            pos++;
        }
        if (pos >= len || json[pos] != ':') {
            continue;
        }
        pos++;
        while (pos < len && (json[pos] == ' ' || json[pos] == '\t' ||
                              json[pos] == '\n' || json[pos] == '\r')) {
            pos++;
        }
        if (pos >= len || json[pos] != '"') {
            continue;
        }
        pos++;

        size_t value_start = pos;
        while (pos < len && json[pos] != '"') {
            pos++;
        }
        if (pos >= len) {
            continue;
        }

        size_t value_len = pos - value_start;
        if (value_len == 0 || value_len >= out_cap) {
            return false;
        }

        memcpy(out, &json[value_start], value_len);
        out[value_len] = '\0';
        return true;
    }

    return false;
}

bool ota_manifest_parse(const char *json, size_t len, ota_manifest_t *out)
{
    if (json == NULL || out == NULL || len == 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (!find_string_field(json, len, "version", out->version, sizeof(out->version))) {
        return false;
    }
    if (!find_string_field(json, len, "url", out->url, sizeof(out->url))) {
        return false;
    }

    return true;
}

bool ota_manifest_is_update_available(const char *running_version, const char *manifest_version)
{
    if (running_version == NULL || manifest_version == NULL) {
        return false;
    }
    return strcmp(running_version, manifest_version) != 0;
}
