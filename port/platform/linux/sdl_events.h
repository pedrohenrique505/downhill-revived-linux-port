/* SDL video is pumped on the window thread. Consumers take disjoint ranges so
   the controller worker cannot swallow a close/text event, and the window
   cannot swallow controller hotplug events. SDL_PeepEvents is thread-safe. */
#ifndef DHD_LINUX_SDL_EVENTS_H
#define DHD_LINUX_SDL_EVENTS_H
#include <SDL3/SDL.h>

static inline int linux_gamepad_event(SDL_Event *event)
{
    return SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION,
                          SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED) > 0;
}

static inline int linux_window_event(SDL_Event *event)
{
    if (SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                       SDL_EVENT_JOYSTICK_AXIS_MOTION - 1) > 0) return 1;
    return SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED + 1,
                          SDL_EVENT_LAST) > 0;
}
#endif
