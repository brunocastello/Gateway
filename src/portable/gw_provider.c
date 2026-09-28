#include "gw_provider.h"
#include "gw_util.h"

#include <stddef.h>

typedef struct {
    const char *key;
    const char *outlook;
    const char *gmail;
} GWProviderDefault;

/* Every custom-only setting, in the order the prefs file lists them. */
static const GWProviderDefault kDefaults[] = {
    { "imap_host",          "outlook.office365.com",  "imap.gmail.com" },
    { "imap_upstream_port", "993",                    "993" },
    { "pop_host",           "outlook.office365.com",  "pop.gmail.com" },
    { "pop_upstream_port",  "995",                    "995" },
    { "smtp_host",          "smtp-mail.outlook.com",  "smtp.gmail.com" },
    { "smtp_upstream_port", "587",                    "587" },
    { "smtp_starttls",      "1",                      "1" },
    { "oauth_host",         "login.microsoftonline.com", "oauth2.googleapis.com" },
    { "oauth_path",         "/common/oauth2/v2.0/token", "/token" },
    { "oauth_scope",
      "offline_access https://outlook.office.com/IMAP.AccessAsUser.All "
      "https://outlook.office.com/POP.AccessAsUser.All "
      "https://outlook.office.com/SMTP.Send",
      "https://mail.google.com/" },
    { NULL, NULL, NULL }
};

int gw_provider_is_custom(const char *provider)
{
    return provider != NULL && gw_stricmp(provider, "custom") == 0;
}

static const GWProviderDefault *gw_provider_find(const char *key)
{
    int i;

    for (i = 0; kDefaults[i].key != NULL; i++)
        if (gw_stricmp(kDefaults[i].key, key) == 0) return &kDefaults[i];
    return NULL;
}

int gw_provider_custom_only(const char *key)
{
    return gw_provider_find(key) != NULL;
}

const char *gw_provider_default(const char *provider, const char *key)
{
    const GWProviderDefault *d = gw_provider_find(key);
    int gmail;

    if (d == NULL || gw_provider_is_custom(provider)) return NULL;
    gmail = provider != NULL && (gw_stricmp(provider, "gmail") == 0 ||
                                 gw_stricmp(provider, "google") == 0);
    return gmail ? d->gmail : d->outlook;
}
