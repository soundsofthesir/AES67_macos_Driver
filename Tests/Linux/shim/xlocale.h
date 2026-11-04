// Linux stand-in for <xlocale.h>, for the portable test build only. glibc
// has locale_t and uselocale() but no snprintf_l, so switch the thread's
// locale around vsnprintf.
#pragma once
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>

static inline int snprintf_l(char* buffer, size_t size, locale_t locale, const char* format, ...) {
    locale_t previous = uselocale(locale);
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    uselocale(previous);
    return result;
}
