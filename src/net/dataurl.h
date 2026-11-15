/* data: URLs (the Fetch standard's data: URL processor).
 *
 * To decode one, call au_data_url_decode with the URL (as typed or as
 * serialized by the URL parser). It returns false when the URL is not a
 * valid data: URL (no comma, or bad base64). Otherwise mime receives the
 * MIME type essence (lower case), charset its charset parameter (empty
 * when absent), and body the decoded bytes. A missing or invalid type is
 * text/plain with charset US-ASCII. The input is limited by the URL
 * parser's cap (AU_URL_MAX_INPUT), and the decoded body is at most three
 * times the input (the URL parser turns an invalid UTF-8 byte into
 * U+FFFD, three bytes). */
#ifndef AURA_NET_DATAURL_H
#define AURA_NET_DATAURL_H

#include "../core/core.h"

typedef struct AuDataUrl {
    AuBuf mime;
    AuBuf charset;
    AuBuf body;
} AuDataUrl;

void au_data_url_init(AuDataUrl *d);
void au_data_url_free(AuDataUrl *d);
bool au_data_url_decode(const char *url, AuDataUrl *d);

#endif
