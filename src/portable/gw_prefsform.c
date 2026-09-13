/*
 * gw_prefsform.c - see gw_prefsform.h.
 *
 * Defaults here have to agree with the ones the readers pass to
 * GWConfig_Num() and GWConfig_Str(). Where they disagree the window shows one
 * number and Gateway uses another, which is the worst of both: the setting
 * looks wrong and behaves right, or the reverse, and nothing on screen says
 * which. docs/prefs.md is the third copy and is checked by eye.
 */

#include "gw_prefsform.h"

#include <stdio.h>
#include <string.h>

#include "gw_util.h"

static const char *kGroupNames[kGWGroupCount] = {
    "Modules",
    "Web proxy",
    "Wayback",
    "Mail",
    "Log"
};

const char *gw_prefsform_group_name(int group)
{
    if (group < 0 || group >= kGWGroupCount) return "";
    return kGroupNames[group];
}

/*
 * The fields, in the order they are drawn.
 *
 * Ports and module switches are marked as needing a restart because they are
 * read once, when the listeners are opened. Everything else is read as it is
 * used and takes effect on the next request -- which is worth knowing, since
 * it means rewrite_https or the archive date can be changed while a page is
 * loading and the next page obeys.
 */
static const GWPrefField kFields[] = {
/*  key                 label                    kind             group            def      min   max  choices                needs  hint */
{ "http_enabled",     "Web proxy",              kGWFieldFlag,   kGWGroupModules, "1",       0,    0, NULL,                   1, NULL },
{ "mail_enabled",     "Mail",                   kGWFieldFlag,   kGWGroupModules, "1",       0,    0, NULL,                   1, NULL },
{ "wayback_enabled",  "Wayback",                kGWFieldFlag,   kGWGroupModules, "1",       0,    0, NULL,                   1, NULL },
{ "max_sessions",     "Concurrent connections", kGWFieldNumber, kGWGroupModules, "12",      2,   16, NULL,                   1,
  "Each costs about 110 KB." },

{ "http_port",        "Port",                   kGWFieldNumber, kGWGroupWeb,     "8765",    1, 65535, NULL,                  1, NULL },
{ "rewrite_https",    "Rewrite https:// links to http://", kGWFieldFlag, kGWGroupWeb, "1", 0, 0, NULL,                       0,
  "For a browser with no modern TLS of its own." },
{ "connect_mitm",     "Terminate TLS for a typed https:// URL", kGWFieldFlag, kGWGroupWeb, "0", 0, 0, NULL,                  0,
  "Needs the Gateway CA installed in the browser." },
{ "follow_redirects", "Follow redirects",       kGWFieldChoice, kGWGroupWeb,     "auto",    0,    0, "auto|always|never",    0,
  "auto follows the hops the browser could not." },
{ "max_body_mb",      "Largest response (MB)",  kGWFieldNumber, kGWGroupWeb,     "0",       0, 1024, NULL,                   0,
  "0 for no limit." },
{ "max_connects",     "Connections opening at once", kGWFieldNumber, kGWGroupWeb, "8",      1,    8, NULL,                   0, NULL },

{ "wayback_port",     "Port",                   kGWFieldNumber, kGWGroupArchive, "8888",    1, 65535, NULL,                  1, NULL },
{ "wayback_date",     "Era (YYYYMMDD)",         kGWFieldText,   kGWGroupArchive, "19991128",0,    0, NULL,                   0,
  "Also settable from the browser." },
{ "wayback_tolerance","Days newer allowed",     kGWFieldNumber, kGWGroupArchive, "730",     0, 36500, NULL,                  0, NULL },
{ "wayback_connects", "Connections opening at once", kGWFieldNumber, kGWGroupArchive, "1",  1,    8, NULL,                   0,
  "The archive refuses bursts." },
{ "wayback_geocities","GeoCities fix",          kGWFieldFlag,   kGWGroupArchive, "1",       0,    0, NULL,                   0, NULL },
{ "wayback_cache",    "Let the browser keep snapshots", kGWFieldFlag, kGWGroupArchive, "1", 0,    0, NULL,                   0, NULL },
{ "wayback_settings", "Serve the settings page", kGWFieldFlag,  kGWGroupArchive, "1",       0,    0, NULL,                   0, NULL },

{ "provider",         "Provider",               kGWFieldChoice, kGWGroupMail,    "outlook", 0,    0, "outlook|gmail",        1, NULL },
{ "oauth_user",       "Address",                kGWFieldText,   kGWGroupMail,    "",        0,    0, NULL,                   1, NULL },
{ "local_password",   "Password for the mail client", kGWFieldSecret, kGWGroupMail, "",     0,    0, NULL,                   0,
  "Checked here; never leaves the machine." },
{ "imap_port",        "IMAP port",              kGWFieldNumber, kGWGroupMail,    "1993",    1, 65535, NULL,                  1, NULL },
{ "pop_port",         "POP port",               kGWFieldNumber, kGWGroupMail,    "1995",    1, 65535, NULL,                  1, NULL },
{ "smtp_port",        "SMTP port",              kGWFieldNumber, kGWGroupMail,    "1587",    1, 65535, NULL,                  1, NULL },

{ "show_window",      "Show the log window at launch", kGWFieldFlag, kGWGroupLog, "1",      0,    0, NULL,                   0, NULL },
{ "log_file",         "Also write the log to a file", kGWFieldFlag, kGWGroupLog, "0",       0,    0, NULL,                   1,
  "The window keeps only the last 200 lines." }
};

const GWPrefField *gw_prefsform_fields(int *count)
{
    if (count != NULL)
        *count = (int)(sizeof(kFields) / sizeof(kFields[0]));
    return kFields;
}

const GWPrefField *gw_prefsform_find(const char *key)
{
    int i, n = (int)(sizeof(kFields) / sizeof(kFields[0]));

    if (key == NULL) return NULL;
    for (i = 0; i < n; i++)
        if (gw_stricmp(kFields[i].key, key) == 0) return &kFields[i];
    return NULL;
}

/* The spellings a person might reasonably type for "on" and "off". */
static int flag_value(const char *v, int *out)
{
    if (gw_stricmp(v, "1") == 0 || gw_stricmp(v, "yes") == 0 ||
        gw_stricmp(v, "on") == 0 || gw_stricmp(v, "true") == 0) {
        *out = 1;
        return 1;
    }
    if (gw_stricmp(v, "0") == 0 || gw_stricmp(v, "no") == 0 ||
        gw_stricmp(v, "off") == 0 || gw_stricmp(v, "false") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

int gw_prefsform_validate(const GWPrefField *f, const char *value,
                          char *out, size_t cap,
                          const char **why)
{
    static char msg[128];

    if (why != NULL) *why = NULL;
    if (f == NULL || value == NULL || out == NULL || cap == 0) return 0;
    out[0] = '\0';

    switch (f->kind) {
    case kGWFieldFlag: {
        int on;

        if (!flag_value(value, &on)) {
            if (why != NULL) *why = "That has to be on or off.";
            return 0;
        }
        gw_copy_n(out, cap, on ? "1" : "0", 1);
        return 1;
    }

    case kGWFieldNumber: {
        long v = gw_parse_dec(value, strlen(value));

        if (v < 0) {
            if (why != NULL) {
                snprintf(msg, sizeof(msg), "%s has to be a number.", f->label);
                *why = msg;
            }
            return 0;
        }
        /*
         * Clamped rather than refused. Someone typing 40 into a field that
         * stops at 16 has said "as many as you can", and answering that with
         * a sheet they have to dismiss before they can save teaches nothing
         * the clamped number does not.
         */
        if (v < f->min) v = f->min;
        if (v > f->max) v = f->max;
        snprintf(out, cap, "%ld", v);
        return 1;
    }

    case kGWFieldChoice: {
        const char *p = f->choices;

        while (p != NULL && *p != '\0') {
            const char *end = strchr(p, '|');
            size_t n = (end != NULL) ? (size_t)(end - p) : strlen(p);

            if (strlen(value) == n && gw_strnicmp(p, value, n) == 0) {
                gw_copy_n(out, cap, p, n);
                return 1;
            }
            p = (end != NULL) ? end + 1 : NULL;
        }
        if (why != NULL) {
            snprintf(msg, sizeof(msg), "%s has to be one of: %s",
                     f->label, f->choices ? f->choices : "");
            *why = msg;
        }
        return 0;
    }

    case kGWFieldText:
    case kGWFieldSecret:
    default:
        /*
         * A value with a line break in it would become two lines in the file
         * and the second would be read as a setting of its own. Everything
         * else a person can type is theirs to type.
         */
        if (strchr(value, '\n') != NULL || strchr(value, '\r') != NULL) {
            if (why != NULL) *why = "That cannot contain a line break.";
            return 0;
        }
        gw_copy_n(out, cap, value, strlen(value));
        return 1;
    }
}

int gw_prefsform_changed(const char *current, const char *value)
{
    if (current == NULL) current = "";
    if (value == NULL) value = "";
    return strcmp(current, value) != 0;
}
