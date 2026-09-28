#include "gw_prefs.h"
#include "gw_util.h"

#include <string.h>

/*
 * Find the end of the current line and the start of the next one.
 *
 * Three line endings have to work here. A prefs file typed on the Mac itself
 * ends lines with CR, one moved over from a modern machine ends them with LF,
 * and one that has been through a Windows editor uses CRLF. Splitting on LF
 * alone made a Mac-authored file look like a single line, which silently
 * turned every setting into its default.
 */
static void gw_prefs_line(const char *text, size_t len, size_t off,
                          size_t *line_end, size_t *next)
{
    size_t i = off;

    while (i < len && text[i] != '\n' && text[i] != '\r') i++;
    *line_end = i;

    if (i < len && text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')
        *next = i + 2;                      /* CRLF counts as one ending */
    else if (i < len)
        *next = i + 1;
    else
        *next = len;
}

int gw_prefs_get_nth(const char *text, size_t len, const char *key, int n,
                     char *out, size_t cap)
{
    size_t off = 0;
    size_t klen = strlen(key);
    int    seen = 0;

    if (cap) out[0] = '\0';

    while (off < len) {
        size_t line_end, next, i, vs;

        gw_prefs_line(text, len, off, &line_end, &next);

        /* trim leading blanks */
        i = off;
        while (i < line_end && (text[i] == ' ' || text[i] == '\t')) i++;

        if (i < line_end && text[i] != '#' && text[i] != ';') {
            size_t ke = i;
            while (ke < line_end && text[ke] != '=' && text[ke] != ':') ke++;
            if (ke < line_end) {
                size_t kend = ke;
                while (kend > i && (text[kend - 1] == ' ' ||
                                    text[kend - 1] == '\t')) kend--;
                if (kend - i == klen && gw_strnicmp(text + i, key, klen) == 0) {
                    if (seen++ == n) {
                        size_t end = line_end;
                        vs = ke + 1;
                        while (vs < end && (text[vs] == ' ' ||
                                            text[vs] == '\t')) vs++;
                        while (end > vs && (text[end - 1] == ' ' ||
                                            text[end - 1] == '\t')) end--;
                        gw_copy_n(out, cap, text + vs, end - vs);
                        /*
                         * 1 because the key is present at this index, even
                         * when its value is empty. Reporting an empty value
                         * as "not found" made a caller walking the indices
                         * stop at the first blank entry and never see the
                         * ones after it, which is a silent way to lose half
                         * a list. gw_prefs_get() keeps the old meaning by
                         * testing the value itself.
                         */
                        return 1;
                    }
                }
            }
        }
        off = next;
    }
    return 0;
}

int gw_prefs_get(const char *text, size_t len, const char *key,
                 char *out, size_t cap)
{
    /* "Set to something" rather than "present": a key with an empty value
     * should fall through to the caller's default, as it always has. */
    if (!gw_prefs_get_nth(text, len, key, 0, out, cap)) return 0;
    return out[0] != '\0';
}

long gw_prefs_get_num(const char *text, size_t len, const char *key, long def)
{
    char buf[32];
    long v;

    if (!gw_prefs_get(text, len, key, buf, sizeof(buf))) return def;
    v = gw_parse_dec(buf, strlen(buf));
    return v < 0 ? def : v;
}

/*
 * The nth entry across all occurrences of a key, splitting each value on ';'.
 *
 * Walks every occurrence of the key (like gw_prefs_get_nth), but within each
 * one, splits the value on ';', trims spaces around entries, and skips empty
 * ones. Returns 1 when the nth entry exists (whatever its value), 0 only
 * when there is no nth entry.
 *
 * This handles both storage forms — repeated keys and one ;-separated value
 * — so a file mixing the two forms is read correctly.
 */
int gw_prefs_get_nth_split(const char *text, size_t len, const char *key,
                           int n, char *out, size_t cap)
{
    size_t off = 0;
    size_t klen = strlen(key);
    int    seen = 0;   /* index across all entries */

    if (cap) out[0] = '\0';

    while (off < len) {
        size_t line_end, next, i;

        gw_prefs_line(text, len, off, &line_end, &next);

        /* trim leading blanks */
        i = off;
        while (i < line_end && (text[i] == ' ' || text[i] == '\t')) i++;

        if (i < line_end && text[i] != '#' && text[i] != ';') {
            size_t ke = i;
            while (ke < line_end && text[ke] != '=' && text[ke] != ':') ke++;
            if (ke < line_end) {
                size_t kend = ke;
                while (kend > i && (text[kend - 1] == ' ' ||
                                    text[kend - 1] == '\t')) kend--;
                if (kend - i == klen && gw_strnicmp(text + i, key, klen) == 0) {
                    /* Found an occurrence: split its value on ';'. */
                    size_t vs = ke + 1;
                    while (vs < line_end &&
                           (text[vs] == ' ' || text[vs] == '\t')) vs++;

                    /* Walk through each ';' separated entry. */
                    size_t entry_start = vs;
                    while (1) {
                        /* Scan forward until we hit ';' or the end of line. */
                        size_t scan = entry_start;
                        while (scan < line_end && text[scan] != ';')
                            scan++;

                        /* Trim trailing spaces from this entry. */
                        size_t es = entry_start;
                        while (es < line_end &&
                               (text[es] == ' ' || text[es] == '\t'))
                            es++;
                        size_t ee = scan;
                        while (ee > es &&
                               (text[ee - 1] == ' ' ||
                                text[ee - 1] == '\t'))
                            ee--;

                        if (ee > es) {
                            /* Non-empty entry: check if this is our index. */
                            if (seen++ == n) {
                                size_t copy_len = ee - es;
                                if (cap > 0 && copy_len >= cap)
                                    copy_len = cap - 1;
                                gw_copy_n(out, cap,
                                          text + es, copy_len);
                                return 1;
                            }
                        }

                        /* If we hit the end, done. Otherwise skip ';' and start next. */
                        if (scan == line_end)
                            break;
                        vs = scan + 1;
                        entry_start = vs;
                    }
                }
            }
        }
        off = next;
    }
    return 0;
}

/* The line ending the file already uses, so an edit does not mix conventions. */
static const char *gw_prefs_eol(const char *text, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (text[i] == '\r')
            return (i + 1 < len && text[i + 1] == '\n') ? "\r\n" : "\r";
        if (text[i] == '\n')
            return "\n";
    }
    return "\r";           /* a new file on this machine: Mac convention */
}

/*
 * Whether the text from i up to line_end reads "key = ...": the key, then
 * optional blanks, then a separator. Sets *sep to the separator's offset.
 */
static int gw_prefs_key_at(const char *text, size_t i, size_t line_end,
                           const char *key, size_t klen, size_t *sep)
{
    size_t ke = i, kend;

    while (ke < line_end && text[ke] != '=' && text[ke] != ':') ke++;
    if (ke == line_end) return 0;
    kend = ke;
    while (kend > i && (text[kend - 1] == ' ' || text[kend - 1] == '\t'))
        kend--;
    if (kend - i != klen || gw_strnicmp(text + i, key, klen) != 0) return 0;
    *sep = ke;
    return 1;
}

/*
 * Classify one line against key. Returns 1 for an active setting, 2 for a
 * commented one ("# key = value" or ";key = value"), 0 otherwise. *start is
 * where the key itself begins and *sep where its separator is.
 */
static int gw_prefs_match(const char *text, size_t off, size_t line_end,
                          const char *key, size_t klen,
                          size_t *start, size_t *sep)
{
    size_t i = off;
    int commented = 0;

    while (i < line_end && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i < line_end && (text[i] == '#' || text[i] == ';')) {
        commented = 1;
        i++;
        while (i < line_end && (text[i] == ' ' || text[i] == '\t')) i++;
    }
    if (!gw_prefs_key_at(text, i, line_end, key, klen, sep)) return 0;
    *start = i;
    return commented ? 2 : 1;
}

size_t gw_prefs_set(const char *text, size_t len, const char *key,
                    const char *value, char *out, size_t cap)
{
    size_t off = 0;
    size_t used = 0;
    size_t klen = strlen(key);
    size_t vlen = strlen(value);
    const char *eol = gw_prefs_eol(text, len);
    size_t eol_len = strlen(eol);
    size_t target = (size_t)-1;     /* offset of the line to rewrite */
    int want = 1;                   /* the kind of line target is */

    /*
     * An active line is rewritten where it stands. Failing that, the first
     * commented copy is brought back in its place; failing that, append.
     */
    for (; want <= 2 && target == (size_t)-1; want++) {
        for (off = 0; off < len; ) {
            size_t line_end, next, start, sep;
            gw_prefs_line(text, len, off, &line_end, &next);
            if (gw_prefs_match(text, off, line_end, key, klen,
                               &start, &sep) == want) {
                target = off;
                break;
            }
            off = next;
        }
    }
    want--;

    for (off = 0; off < len; ) {
        size_t line_end, next, start, sep;
        int kind;

        gw_prefs_line(text, len, off, &line_end, &next);
        kind = gw_prefs_match(text, off, line_end, key, klen, &start, &sep);

        if (off == target) {
            /* Rewrite in place, keeping the key exactly as the user typed it
             * and the indentation before any comment marker. */
            size_t lead = off;
            while (lead < line_end && (text[lead] == ' ' || text[lead] == '\t'))
                lead++;
            if (want == 1) lead = start;
            if (used + (lead - off) + (sep - start) + 2 + vlen + eol_len > cap)
                return 0;
            memcpy(out + used, text + off, lead - off);
            used += lead - off;
            memcpy(out + used, text + start, sep - start);
            used += sep - start;
            out[used++] = text[sep];        /* the separator they used */
            out[used++] = ' ';
            memcpy(out + used, value, vlen);
            used += vlen;
            memcpy(out + used, eol, eol_len);
            used += eol_len;
        } else if (kind == 1 && want == 1) {
            /* A later active duplicate of the key just rewritten: drop it,
             * or it would reappear on the next read. Comments are kept. */
        } else {
            size_t n = next - off;
            if (used + n > cap) return 0;
            memcpy(out + used, text + off, n);
            used += n;
        }
        off = next;
    }

    if (target == (size_t)-1) {
        if (used > 0 && out[used - 1] != '\n' && out[used - 1] != '\r') {
            if (used + eol_len > cap) return 0;
            memcpy(out + used, eol, eol_len);
            used += eol_len;
        }
        if (used + klen + 3 + vlen + eol_len > cap) return 0;
        memcpy(out + used, key, klen);
        used += klen;
        out[used++] = ' ';
        out[used++] = '=';
        out[used++] = ' ';
        memcpy(out + used, value, vlen);
        used += vlen;
        memcpy(out + used, eol, eol_len);
        used += eol_len;
    }
    return used;
}

size_t gw_prefs_comment(const char *text, size_t len, const char *key,
                        char *out, size_t cap)
{
    size_t off = 0;
    size_t used = 0;
    size_t klen = strlen(key);
    size_t vs = 0, ve = 0;              /* the first active line's value */
    size_t slot = (size_t)-1;           /* the first commented copy's line */
    int active = 0;

    /* Where the value is, and whether a commented copy is waiting for it. */
    while (off < len) {
        size_t line_end, next, start, sep;
        int kind;

        gw_prefs_line(text, len, off, &line_end, &next);
        kind = gw_prefs_match(text, off, line_end, key, klen, &start, &sep);
        if (kind == 1 && !active) {
            active = 1;
            vs = sep + 1;
            ve = line_end;
            while (vs < ve && (text[vs] == ' ' || text[vs] == '\t')) vs++;
            while (ve > vs && (text[ve - 1] == ' ' || text[ve - 1] == '\t'))
                ve--;
        } else if (kind == 2 && slot == (size_t)-1) {
            slot = off;
        }
        off = next;
    }

    for (off = 0; off < len; ) {
        size_t line_end, next, start, sep, n;
        int kind;

        gw_prefs_line(text, len, off, &line_end, &next);
        kind = active ? gw_prefs_match(text, off, line_end, key, klen,
                                       &start, &sep) : 0;

        if (kind == 1 && slot != (size_t)-1) {
            /* The value moves to the commented copy below or above. */
        } else if (off == slot && active) {
            /*
             * The first commented copy takes the value, where it stands and
             * as it is spaced. Otherwise an older commented copy -- the
             * example's placeholder, say -- would sit first and be what
             * comes back, over the value the user had actually set.
             */
            n = (sep + 1 - off) + 1 + (ve - vs) + (next - line_end);
            if (used + n > cap) return 0;
            memcpy(out + used, text + off, sep + 1 - off);
            used += sep + 1 - off;
            out[used++] = ' ';
            memcpy(out + used, text + vs, ve - vs);
            used += ve - vs;
            memcpy(out + used, text + line_end, next - line_end);
            used += next - line_end;
        } else {
            if (kind == 1) {
                /* No commented copy to take it: comment the line itself. */
                if (used + 2 > cap) return 0;
                out[used++] = '#';
                out[used++] = ' ';
            }
            n = next - off;
            if (used + n > cap) return 0;
            memcpy(out + used, text + off, n);
            used += n;
        }
        off = next;
    }
    return used;
}

int gw_prefs_get_commented(const char *text, size_t len, const char *key,
                           char *out, size_t cap)
{
    size_t off = 0;
    size_t klen = strlen(key);

    if (cap) out[0] = '\0';
    while (off < len) {
        size_t line_end, next, start, sep;

        gw_prefs_line(text, len, off, &line_end, &next);
        if (gw_prefs_match(text, off, line_end, key, klen,
                           &start, &sep) == 2) {
            size_t vs = sep + 1, end = line_end;
            while (vs < end && (text[vs] == ' ' || text[vs] == '\t')) vs++;
            while (end > vs && (text[end - 1] == ' ' ||
                                text[end - 1] == '\t')) end--;
            gw_copy_n(out, cap, text + vs, end - vs);
            return 1;
        }
        off = next;
    }
    return 0;
}

/* UI input accepts pasted lines as well as semicolons. Compact in place:
 * output never grows, and empty entries cannot create a leading separator. */
void gw_prefs_normalize_list(char *value)
{
    char *read = value, *write = value;
    int first = 1;
    while (*read) {
        char *begin = read, *end;
        while (*read && *read != ';' && *read != '\r' && *read != '\n') ++read;
        end = read;
        if (*read) ++read;
        while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
        while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
        if (begin == end) continue;
        if (!first) *write++ = ';';
        while (begin < end) *write++ = *begin++;
        first = 0;
    }
    *write = '\0';
}
