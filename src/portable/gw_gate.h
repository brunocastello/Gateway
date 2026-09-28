/*
 * gw_gate.h - which settings apply, given the settings that decide it.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 *
 * Some settings only mean something under another one's value: the custom
 * mail servers under provider = custom, the far leg's TLS options under
 * tunnel_tls = 1, the proxy's host and port under any tunnel_proxy, its
 * login and Host line under tunnel_proxy = http. Both settings windows dim a
 * row that does not apply and comment its line out on Save, so the prefs
 * file reads the way the window looks. The rule lives here once.
 */
#ifndef GW_GATE_H
#define GW_GATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hands back the current value of a deciding setting, or NULL for unset. */
typedef const char *(*GWGateLookup)(const char *key, void *ctx);

/* 1 when key is one of the deciding settings: changing it can dim or
 * restore other rows. */
int gw_gate_decides(const char *key);

/* 1 when key applies only under some value of a deciding setting. */
int gw_gate_gated(const char *key);

/*
 * 1 when key applies under the deciding values get reports. Settings that
 * are not gated always apply. An unset deciding setting takes Gateway's own
 * default: provider outlook, tunnel_tls 1, tunnel_proxy none.
 */
int gw_gate_applies(const char *key, GWGateLookup get, void *ctx);

/* 1 when key, while it applies, may not be left empty. */
int gw_gate_required(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* GW_GATE_H */
