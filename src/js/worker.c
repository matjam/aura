/* Workers (stub while the fetch engine lands). */
#include "js_int.h"

bool js_worker_defer_fetch(AuJs *js, int kind, uint32_t id, int status, const char *text, const char *const *headers,
                           size_t nheaders, const char *url, const void *data, size_t len)
{
    (void)js; (void)kind; (void)id; (void)status; (void)text; (void)headers; (void)nheaders; (void)url; (void)data; (void)len;
    return false;
}
bool js_worker_can_wait(AuJs *js) { (void)js; return false; }
bool js_worker_wait(AuJs *js) { (void)js; return false; }
