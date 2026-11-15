/* Internal declarations shared by the URL parser's files. */
#ifndef AURA_URL_INT_H
#define AURA_URL_INT_H

#include "url.h"

bool url_forbidden_host(uint32_t c);
bool url_host_parse(const char *s, size_t n, bool special, AuBuf *out, AuUrlHostType *type, uint32_t *ipv4,
                    uint16_t ipv6[8]);
void url_ipv6_serialize(const uint16_t a[8], AuBuf *out);

#endif
