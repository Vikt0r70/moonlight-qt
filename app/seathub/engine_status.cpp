#include "engine_status.h"

#include <SDL.h>
#include <cstring>

bool parseConnectionStatusUpdate(int category, int priority, const char* message, int* status)
{
    if (!message || !status || category != SDL_LOG_CATEGORY_APPLICATION
        || priority != SDL_LOG_PRIORITY_INFO) return false;
    const char* prefix = "Connection status update: ";
    const size_t length = std::strlen(prefix);
    if (std::strncmp(message, prefix, length) != 0) return false;
    const char* value = message + length;
    if ((value[0] != '0' && value[0] != '1') || value[1] != '\0') return false;
    *status = value[0] - '0';
    return true;
}
