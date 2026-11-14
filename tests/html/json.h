/* A small JSON reader for the html5lib tokenizer tests. Strings are
 * decoded to UTF-8; a lone surrogate escape becomes its 3-byte
 * (WTF-8) form so the caller can see it. */
#ifndef AURA_TEST_JSON_H
#define AURA_TEST_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType type;
    double num;
    char *str;  /* J_STR: value; NUL-terminated, len bytes */
    size_t len;
    struct JVal **items; /* J_ARR, J_OBJ (values) */
    char **keys;         /* J_OBJ */
    size_t *keylens;
    size_t n;
} JVal;

/* Returns NULL on a syntax error. */
JVal *json_parse(const char *s, size_t len);
void json_free(JVal *v);
JVal *json_get(const JVal *obj, const char *key);
/* Decodes \uHHHH escapes in s (the html5lib doubleEscaped option). */
char *json_unescape_u(const char *s, size_t len, size_t *outlen);

#endif
