#include "gw_gate.h"
#include "gw_fwd.h"
#include "gw_provider.h"
#include "gw_util.h"

#include <stddef.h>
#include <string.h>

enum { kCustomMail = 1, kTunnelTls, kAnyProxy, kHttpProxy };

typedef struct {
    const char *key;
    int         when;
    int         required;
} GWGate;

/* The mail servers come from gw_provider.c; these are the rest. */
static const GWGate kGates[] = {
    { "tunnel_tls12",       kTunnelTls, 0 },
    { "tunnel_insecure",    kTunnelTls, 0 },
    { "tunnel_sni",         kTunnelTls, 0 },
    { "tunnel_proxy_host",  kAnyProxy,  1 },
    { "tunnel_proxy_port",  kAnyProxy,  0 },
    { "tunnel_settle_ms",   kAnyProxy,  0 },
    { "tunnel_proxy_user",  kHttpProxy, 0 },
    { "tunnel_proxy_pass",  kHttpProxy, 0 },
    { "tunnel_host_header", kHttpProxy, 0 },
    { NULL, 0, 0 }
};

static const GWGate *gw_gate_find(const char *key)
{
    int i;

    for (i = 0; kGates[i].key != NULL; i++)
        if (gw_stricmp(kGates[i].key, key) == 0) return &kGates[i];
    return NULL;
}

/* Which condition gates key, or 0 for none. */
static int gw_gate_when(const char *key)
{
    const GWGate *g;

    if (gw_provider_custom_only(key)) return kCustomMail;
    g = gw_gate_find(key);
    return g != NULL ? g->when : 0;
}

int gw_gate_decides(const char *key)
{
    return gw_stricmp(key, "provider") == 0 ||
           gw_stricmp(key, "tunnel_tls") == 0 ||
           gw_stricmp(key, "tunnel_proxy") == 0;
}

int gw_gate_gated(const char *key)
{
    return gw_gate_when(key) != 0;
}

static const char *gw_gate_get(GWGateLookup get, void *ctx, const char *key,
                               const char *def)
{
    const char *v = get(key, ctx);
    return v != NULL && v[0] != '\0' ? v : def;
}

int gw_gate_applies(const char *key, GWGateLookup get, void *ctx)
{
    const char *v;
    long n;

    switch (gw_gate_when(key)) {
    case kCustomMail:
        return gw_provider_is_custom(gw_gate_get(get, ctx, "provider",
                                                 "outlook"));
    case kTunnelTls:
        /* Read as GWConfig_Num reads it: not a number means the default. */
        v = gw_gate_get(get, ctx, "tunnel_tls", "1");
        n = gw_parse_dec(v, strlen(v));
        return n < 0 || n != 0;
    case kAnyProxy:
        v = gw_gate_get(get, ctx, "tunnel_proxy", "none");
        return gw_fwd_kind(v) != GW_FWD_NONE;
    case kHttpProxy:
        v = gw_gate_get(get, ctx, "tunnel_proxy", "none");
        return gw_fwd_kind(v) == GW_FWD_HTTP;
    default:
        return 1;
    }
}

int gw_gate_required(const char *key, GWGateLookup get, void *ctx)
{
    const GWGate *g;
    const char *v;
    long n;

    /* Not a gated setting, but useless empty: a 0.3.6 file has no tunnel
     * section at all, so this binds only once the tunnel is switched on. */
    if (gw_stricmp(key, "tunnel_remote_host") == 0) {
        v = gw_gate_get(get, ctx, "tunnel_enabled", "0");
        n = gw_parse_dec(v, strlen(v));
        return n > 0;
    }
    if (!gw_gate_applies(key, get, ctx)) return 0;

    /* Custom has no provider to fall back on: every server and the token
     * endpoint must be named. Its ports and scope may be left as they are. */
    if (gw_stricmp(key, "imap_host") == 0 || gw_stricmp(key, "pop_host") == 0 ||
        gw_stricmp(key, "smtp_host") == 0 || gw_stricmp(key, "oauth_host") == 0 ||
        gw_stricmp(key, "oauth_path") == 0)
        return 1;
    g = gw_gate_find(key);
    return g != NULL && g->required;
}

const char *gw_gate_canonical(const char *key, const char *value)
{
    if (value == NULL) value = "";
    if (gw_stricmp(key, "provider") == 0) {
        /* As gw_provider_default reads it: anything unknown is Outlook. */
        if (gw_provider_is_custom(value)) return "custom";
        if (gw_stricmp(value, "gmail") == 0 || gw_stricmp(value, "google") == 0)
            return "gmail";
        return "outlook";
    }
    if (gw_stricmp(key, "tunnel_proxy") == 0) {
        switch (gw_fwd_kind(value)) {
        case GW_FWD_NONE:   return "none";
        case GW_FWD_HTTP:   return "http";
        case GW_FWD_SOCKS5: return "socks5";
        default:            return NULL;
        }
    }
    return NULL;
}
