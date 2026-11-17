/* Copies the user's preferences (src/browser/prefs.h, read only) into
 * the network stack's configuration and policy stores. */
#include "cookies.h"
#include "net.h"
#include "net_prefs.h"

void au_net_apply_prefs(AuNet *net, const AuPrefs *p)
{
    AuNetConfig *c = au_net_config(net);
    /* the default too, so that clearing a custom string goes back to it */
    au_net_set_config_str(&c->user_agent, au_prefs_user_agent(p));
    c->send_dnt = p->send_dnt;
    c->send_gpc = p->send_gpc;
    c->https_first = p->https_first;
    c->legacy_tls = p->legacy_tls;
    c->cookie_policy = p->cookies == AU_COOKIES_NONE        ? AU_COOKIE_POLICY_NONE
                       : p->cookies == AU_COOKIES_SAME_SITE ? AU_COOKIE_POLICY_SAME_SITE
                                                            : AU_COOKIE_POLICY_ALL;
    c->block_third_party_cookies = p->block_third_party_cookies;
    au_net_set_config_str(&c->http_proxy_host, p->http_proxy_host ? p->http_proxy_host : "");
    c->http_proxy_port = p->http_proxy_port;
    au_net_set_config_str(&c->https_proxy_host, p->https_proxy_host ? p->https_proxy_host : "");
    c->https_proxy_port = p->https_proxy_port;
    AuCookieJar *jar = au_net_cookie_jar(net);
    if (jar) {
        au_cookie_jar_set_policy(jar, c->cookie_policy, c->block_third_party_cookies);
        au_cookie_rules_parse(jar, p->cookie_sites);
    }
}

size_t au_net_cache_bytes_from_prefs(const AuPrefs *p)
{
    return p->memory_cache_kb > 0 ? (size_t)p->memory_cache_kb * 1024 : 0;
}
