#include "gw_wayback.h"
#include "gw_util.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Building the archive request                                        */
/* ------------------------------------------------------------------ */

size_t gw_wayback_path(const char *date, const GWUrl *origin,
                       char *out, size_t cap)
{
    int n;

    if (date == NULL || date[0] == '\0' || origin == NULL) return 0;

    /*
     * The port is only spelled out when it is not the default for the scheme.
     * The archive keys its snapshots on the URL as it was originally seen, and
     * "http://example.com:80/" is a different string from "http://example.com/".
     */
    if ((origin->tls && origin->port != 443) ||
        (!origin->tls && origin->port != 80)) {
        n = snprintf(out, cap, "/web/%sid_/%s%s:%u%s",
                     date, origin->tls ? "https://" : "http://",
                     origin->host, (unsigned)origin->port,
                     origin->path[0] ? origin->path : "/");
    } else {
        n = snprintf(out, cap, "/web/%sid_/%s%s%s",
                     date, origin->tls ? "https://" : "http://",
                     origin->host, origin->path[0] ? origin->path : "/");
    }

    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

/* ------------------------------------------------------------------ */
/* Taking an archive URL apart                                         */
/* ------------------------------------------------------------------ */

int gw_wayback_parse(const char *url, size_t len,
                     char *stamp, size_t stamp_cap,
                     char *original, size_t original_cap)
{
    size_t i = 0;
    size_t ts_start, ts_end;

    if (stamp_cap) stamp[0] = '\0';
    if (original_cap) original[0] = '\0';

    /* Skip an absolute prefix, with or without a scheme. */
    if (gw_starts_ci(url + i, len - i, "https://")) i += 8;
    else if (gw_starts_ci(url + i, len - i, "http://")) i += 7;
    else if (len - i >= 2 && url[i] == '/' && url[i + 1] == '/') i += 2;

    if (i > 0) {
        if (!gw_starts_ci(url + i, len - i, GW_WB_HOST)) return 0;
        i += strlen(GW_WB_HOST);
    }

    if (!gw_starts_ci(url + i, len - i, "/web/")) return 0;
    i += 5;

    /*
     * The timestamp runs to the next '/', and may carry a modifier suffix
     * such as "id_" or "if_" that is not part of the digits.
     */
    ts_start = i;
    while (i < len && url[i] != '/') i++;
    if (i >= len) return 0;
    ts_end = i;
    i++;                                /* step over the '/' */

    while (ts_end > ts_start && (url[ts_end - 1] < '0' || url[ts_end - 1] > '9'))
        ts_end--;
    if (ts_end == ts_start) return 0;

    gw_copy_n(stamp, stamp_cap, url + ts_start, ts_end - ts_start);
    gw_copy_n(original, original_cap, url + i, len - i);
    return original[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Dates                                                               */
/* ------------------------------------------------------------------ */

static int digits_to_int(const char *s, size_t n)
{
    int v = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/*
 * Days since 1970-01-01 for a proleptic Gregorian date. The shifted-year
 * trick puts the leap day at the end of the cycle so the arithmetic has no
 * special cases.
 */
static long days_from_civil(long y, int m, int d)
{
    long era, doe;
    int  yoe, doy;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (int)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = (long)yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

long gw_wayback_daynum(const char *stamp)
{
    size_t len;
    int    year, month = 1, day = 1;

    if (stamp == NULL) return -1;
    len = strlen(stamp);
    if (len < 4) return -1;

    year = digits_to_int(stamp, 4);
    if (year < 1) return -1;

    if (len >= 6) {
        month = digits_to_int(stamp + 4, 2);
        if (month < 1 || month > 12) return -1;
    }
    if (len >= 8) {
        day = digits_to_int(stamp + 6, 2);
        if (day < 1 || day > 31) return -1;
    }
    return days_from_civil(year, month, day);
}

int gw_wayback_in_tolerance(const char *target, const char *snapshot,
                            long tolerance)
{
    long want, got;

    if (tolerance <= 0) return 1;

    want = gw_wayback_daynum(target);
    got = gw_wayback_daynum(snapshot);
    if (want < 0 || got < 0) return 1;   /* unparseable: do not reject */

    /*
     * Only newer snapshots are rejected. Receiving something older than asked
     * for is the archive saying that is the best it has, which is exactly what
     * a date-limited proxy wants; receiving something newer means seeing a
     * page from after the era being browsed.
     */
    if (got <= want) return 1;
    return (got - want) <= tolerance;
}

/* ------------------------------------------------------------------ */
/* GeoCities                                                           */
/* ------------------------------------------------------------------ */

int gw_wayback_geocities_host(const char *host, char *out, size_t cap)
{
    static const char kFrom[] = ".geocities.com";
    static const char kTo[]   = ".oocities.org";
    size_t hlen, flen;

    if (cap) out[0] = '\0';
    if (host == NULL) return 0;

    hlen = strlen(host);
    flen = sizeof(kFrom) - 1;

    if (gw_stricmp(host, "geocities.com") == 0 ||
        gw_stricmp(host, "www.geocities.com") == 0) {
        if (cap <= sizeof("www.oocities.org") - 1) return 0;
        strcpy(out, "www.oocities.org");
        return 1;
    }

    if (hlen > flen && gw_stricmp(host + hlen - flen, kFrom) == 0) {
        size_t keep = hlen - flen;
        if (keep + sizeof(kTo) > cap) return 0;
        memcpy(out, host, keep);
        strcpy(out + keep, kTo);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Settings page                                                       */
/* ------------------------------------------------------------------ */

static int append_str(char *out, size_t cap, size_t *used, const char *s)
{
    size_t n = strlen(s);

    if (*used + n >= cap) return 0;
    memcpy(out + *used, s, n);
    *used += n;
    out[*used] = '\0';
    return 1;
}

size_t gw_wayback_settings_page(const GWWaybackSettings *s,
                                char *out, size_t cap)
{
    char  line[256];
    size_t used = 0;

    if (cap == 0) return 0;
    out[0] = '\0';

    if (!append_str(out, cap, &used,
            "<html><head><title>Gateway - Wayback settings</title></head>\r\n"
            "<body bgcolor=\"#FFFFFF\">\r\n"
            "<p><b>Gateway</b> is serving the web as it was.</p>\r\n"
            "<form method=\"get\" action=\"/\">\r\n"
            "<p><b>Date</b> (YYYYMMDD, YYYYMM or YYYY)<br>\r\n"))
        return 0;

    snprintf(line, sizeof(line),
             "<input type=\"text\" name=\"date\" size=\"12\" value=\"%s\"></p>\r\n",
             s->date);
    if (!append_str(out, cap, &used, line)) return 0;

    snprintf(line, sizeof(line),
             "<p><b>Tolerance</b><br>\r\n"
             "<input type=\"text\" name=\"dateTolerance\" size=\"6\" value=\"%ld\">"
             " days newer than the date above</p>\r\n", s->tolerance);
    if (!append_str(out, cap, &used, line)) return 0;

    if (!append_str(out, cap, &used,
            "<p><b>Go to</b> (optional)<br>\r\n"
            "<input type=\"text\" name=\"targetUrl\" size=\"30\" value=\"\">"
            "<br>saves these settings and goes straight there</p>\r\n<p>"))
        return 0;

    snprintf(line, sizeof(line),
             "<input type=\"checkbox\" name=\"gcFix\"%s> GeoCities fix<br>\r\n",
             s->geocities ? " checked" : "");
    if (!append_str(out, cap, &used, line)) return 0;

    snprintf(line, sizeof(line),
             "<input type=\"checkbox\" name=\"quickImages\"%s> Quick images<br>\r\n",
             s->quick_images ? " checked" : "");
    if (!append_str(out, cap, &used, line)) return 0;

    snprintf(line, sizeof(line),
             "<input type=\"checkbox\" name=\"ctEncoding\"%s>"
             " Encoding in Content-Type</p>\r\n",
             s->ct_encoding ? " checked" : "");
    if (!append_str(out, cap, &used, line)) return 0;

    if (!append_str(out, cap, &used,
            "<p><input type=\"submit\" value=\"Save Settings\"></p>\r\n"
            "</form></body></html>\r\n"))
        return 0;

    return used;
}

int gw_wayback_apply_query(const char *query, size_t len,
                           GWWaybackSettings *s,
                           char *target, size_t target_cap)
{
    char value[GW_MAX_PATH];

    if (target_cap) target[0] = '\0';
    if (query == NULL || len == 0) return 0;

    if (gw_url_query_get(query, len, "date", value, sizeof(value)) &&
        value[0] != '\0')
        gw_copy_n(s->date, sizeof(s->date), value, strlen(value));

    if (gw_url_query_get(query, len, "dateTolerance", value, sizeof(value))) {
        long v = gw_parse_dec(value, strlen(value));
        if (v >= 0) s->tolerance = v;
    }

    /*
     * A checkbox that is off is not sent at all, so these are decided by
     * presence rather than by value. Reading them as "false unless present"
     * only works because the form always posts every other field, which a GET
     * form does.
     */
    s->geocities    = gw_url_query_has(query, len, "gcFix");
    s->quick_images = gw_url_query_has(query, len, "quickImages");
    s->ct_encoding  = gw_url_query_has(query, len, "ctEncoding");

    if (gw_url_query_get(query, len, "targetUrl", value, sizeof(value)) &&
        value[0] != '\0') {
        /* Bare hostnames are what people actually type. */
        if (!gw_starts_ci(value, strlen(value), "http://") &&
            !gw_starts_ci(value, strlen(value), "https://")) {
            if (target_cap > 7 + strlen(value)) {
                strcpy(target, "http://");
                strcat(target, value);
                return 1;
            }
            return 0;
        }
        gw_copy_n(target, target_cap, value, strlen(value));
        return 1;
    }
    return 0;
}
