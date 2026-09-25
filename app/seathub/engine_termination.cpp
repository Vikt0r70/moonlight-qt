#include "engine_termination.h"

#include <SDL.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace {
// `Session::clConnectionTerminated`'s own format string, up to `%d` (`session.cpp:144-146`).
const char* const kPrefix = "Connection terminated: ";
} // namespace

bool parseConnectionTerminated(int category, int priority, const char* message, int* code)
{
    if (message == nullptr || code == nullptr) {
        return false;
    }
    if (category != SDL_LOG_CATEGORY_APPLICATION || priority != SDL_LOG_PRIORITY_ERROR) {
        return false;
    }

    const size_t prefixLen = std::strlen(kPrefix);
    if (std::strncmp(message, kPrefix, prefixLen) != 0) {
        // Covers a different prefix entirely and a same-text-different-case one: `strncmp` is
        // case-sensitive, and upstream's own line is always exactly this case.
        return false;
    }

    const char* numberStart = message + prefixLen;
    if (*numberStart == '\0') {
        // "Connection terminated: " with nothing after it.
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(numberStart, &end, 10);
    if (end == numberStart || *end != '\0') {
        // No digits at all (including a bare "-" with nothing after it, which `strtol` also
        // refuses), or trailing characters after the number (e.g. "12x").
        return false;
    }
    if (errno == ERANGE || parsed > INT_MAX || parsed < INT_MIN) {
        return false;
    }

    *code = static_cast<int>(parsed);
    return true;
}
