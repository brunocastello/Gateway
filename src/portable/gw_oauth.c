#include "gw_oauth.h"
#include "gw_util.h"

#include <string.h>

static int unreserved(int ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

size_t gw_form_escape(const char *src, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (; *src; src++) {
        int ch = (unsigned char)*src;
        if (unreserved(ch)) {
            if (o + 1 >= cap) return 0;
            out[o++] = (char)ch;
        } else {
            if (o + 3 >= cap) return 0;
            out[o++] = '%';
            out[o++] = hex[(ch >> 4) & 0x0F];
            out[o++] = hex[ch & 0x0F];
        }
    }
    if (o >= cap) return 0;
    out[o] = '\0';
    return o;
}

static int add_field(char *out, size_t cap, size_t *used,
                     const char *name, const char *value, int first)
{
    size_t nlen = strlen(name);
    size_t n;

    if (value == NULL || value[0] == '\0') return 1;   /* optional field */

    if (!first) {
        if (*used + 1 >= cap) return 0;
        out[(*used)++] = '&';
    }
    if (*used + nlen + 1 >= cap) return 0;
    memcpy(out + *used, name, nlen);
    *used += nlen;
    out[(*used)++] = '=';

    n = gw_form_escape(value, out + *used, cap - *used);
    if (n == 0 && value[0] != '\0') return 0;
    *used += n;
    return 1;
}

size_t gw_oauth_refresh_body(const char *client_id, const char *client_secret,
                             const char *refresh_token, const char *scope,
                             char *out, size_t cap)
{
    size_t used = 0;

    if (client_id == NULL || refresh_token == NULL) return 0;
    if (client_id[0] == '\0' || refresh_token[0] == '\0') return 0;

    if (!add_field(out, cap, &used, "client_id", client_id, 1)) return 0;
    if (!add_field(out, cap, &used, "refresh_token", refresh_token, 0)) return 0;
    if (!add_field(out, cap, &used, "grant_type", "refresh_token", 0)) return 0;
    if (!add_field(out, cap, &used, "scope", scope, 0)) return 0;
    if (!add_field(out, cap, &used, "client_secret", client_secret, 0)) return 0;

    if (used >= cap) return 0;
    out[used] = '\0';
    return used;
}

/* Find "key" as a JSON object member and return the offset of its value. */
static long find_member(const char *json, size_t len, const char *key)
{
    size_t klen = strlen(key);
    size_t i;

    for (i = 0; i + klen + 2 < len; i++) {
        if (json[i] != '"') continue;
        if (memcmp(json + i + 1, key, klen) != 0) continue;
        if (json[i + 1 + klen] != '"') continue;
        {
            size_t j = i + klen + 2;
            while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                               json[j] == '\r' || json[j] == '\n')) j++;
            if (j >= len || json[j] != ':') continue;
            j++;
            while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                               json[j] == '\r' || json[j] == '\n')) j++;
            return (long)j;
        }
    }
    return -1;
}

int gw_json_string(const char *json, size_t len, const char *key,
                   char *out, size_t cap)
{
    long start = find_member(json, len, key);
    size_t i, o = 0;

    if (cap) out[0] = '\0';
    if (start < 0) return 0;

    i = (size_t)start;
    if (i >= len || json[i] != '"') return 0;
    i++;

    while (i < len && json[i] != '"') {
        char ch = json[i];
        if (ch == '\\' && i + 1 < len) {
            i++;
            switch (json[i]) {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'u':
                /* Gateway never needs non-ASCII out of a token response. */
                if (i + 4 < len) i += 4;
                ch = '?';
                break;
            default:  ch = json[i]; break;
            }
        }
        if (o + 1 < cap) out[o++] = ch;
        i++;
    }
    if (cap) out[o] = '\0';
    return o > 0;
}

long gw_json_number(const char *json, size_t len, const char *key)
{
    long start = find_member(json, len, key);
    if (start < 0) return -1;
    return gw_parse_dec(json + start, len - (size_t)start);
}
