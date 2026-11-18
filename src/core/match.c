#include "match.h"

#include <stddef.h>

/* p points at '['. Consumes the class or returns false for an unterminated one. */
static bool class_match(const char *p, size_t *after, char c) {
    size_t i = 1;
    bool negate = false;
    bool hit = false;

    if (p[i] == '!' || p[i] == '^') { negate = true; i++; }
    while (p[i] && p[i] != ']') {
        if (p[i + 1] == '-' && p[i + 2] && p[i + 2] != ']') {
            if (c >= p[i] && c <= p[i + 2]) hit = true;
            i += 3;
        } else {
            if (p[i] == c) hit = true;
            i++;
        }
    }
    if (p[i] != ']') return false;
    *after = i + 1;
    return negate ? !hit : hit;
}

bool katoshe_match(const char *pattern, const char *name) {
    const char *star = NULL;   /* last '*' seen */
    const char *resume = NULL; /* where the text retries from after that '*' */

    if (!pattern || !name) return false;

    while (*name) {
        if (*pattern == '*') {
            star = pattern++;
            resume = name;
            continue;
        }
        if (*pattern == '?') {
            pattern++;
            name++;
            continue;
        }
        if (*pattern == '[') {
            size_t after = 0;
            if (class_match(pattern, &after, *name)) {
                pattern += after;
                name++;
                continue;
            }
        } else if (*pattern == *name) {
            pattern++;
            name++;
            continue;
        }
        if (!star) return false;
        pattern = star + 1;
        name = ++resume;
    }
    while (*pattern == '*') pattern++;
    return *pattern == '\0';
}
