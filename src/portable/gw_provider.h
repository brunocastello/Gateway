/*
 * gw_provider.h - what each mail provider supplies on its own.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 *
 * Outlook and Gmail differ by six hostnames and a scope; the upstream ports
 * and STARTTLS are the same for both. Those settings are read from the prefs
 * file only when provider = custom -- otherwise the file keeps them commented
 * out and these values stand, so the file means what the settings window
 * shows, and a stale host left behind from a custom setup cannot quietly
 * redirect Outlook mail.
 */
#ifndef GW_PROVIDER_H
#define GW_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when provider (as written in the prefs) is "custom". */
int gw_provider_is_custom(const char *provider);

/* 1 when key is one of the settings read only under provider = custom. */
int gw_provider_custom_only(const char *key);

/*
 * The value provider uses for key, as text ("993" for a port), or NULL when
 * the provider is custom or key is not a custom-only setting. "gmail" and
 * "google" mean Gmail; anything else, unset included, means Outlook.
 */
const char *gw_provider_default(const char *provider, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* GW_PROVIDER_H */
