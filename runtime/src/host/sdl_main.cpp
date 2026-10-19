#ifndef _WIN32
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <atomic>
#include <cstdio>
#include <exception>
#include <thread>
#include "gpu/display.h"
#include "gpu/overlay.h"
#include "host/platform.h"
#include "host/media.h"
#include "hid/host_input.h"

namespace display {
void on_main_thread(std::function<void()> action) {
    if (SDL_IsMainThread()) { action(); return; }
    if (!SDL_RunOnMainThread([](void* opaque) { (*static_cast<std::function<void()>*>(opaque))(); }, &action, true))
        hw_fatal("SDL main-thread dispatch failed");
}
int run_main(int argc, char** argv, int (*runtime)(int, char**)) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL video: %s\n", SDL_GetError()); return 1; }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) std::fprintf(stderr, "SDL audio unavailable: %s\n", SDL_GetError());
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR))
        std::fprintf(stderr, "SDL controller initialization: %s\n", SDL_GetError());
    std::atomic<bool> finished{false};
    int result = 1;
    std::thread worker([&] {
        try { result = runtime(argc, argv); }
        catch (const std::exception& error) { hw_log("runtime exception: %s", error.what()); result = 3; }
        finished = true;
    });
    auto event = [](const SDL_Event& event) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            validation_report(); hw_log("window closed - exiting"); host::process_exit(0);
        }
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
            hid::host_keyboard_event(static_cast<int>(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN);
        if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
            hid::host_gamepad_event(event.type, event.gdevice.which);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) host::set_media_focus(true);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) { host::set_media_focus(false); hid::host_keyboard_clear(); }
        handle_window_event(event);
        if (!overlay::process_event(event) && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.key == SDLK_F1 || event.key.key == SDLK_ESCAPE) overlay::toggle();
            if (event.key.key == SDLK_F11) toggle_borderless();
        }
    };
    while (!finished) {
        SDL_Event current;
        if (SDL_WaitEventTimeout(&current, 8)) event(current);
        while (SDL_PollEvent(&current)) event(current);
    }
    worker.join();
    // Runtime workers may remain after a startup failure. End the process before
    // global destructors or SDL teardown can race those workers.
    host::process_exit(result);
}
}  // namespace display
#endif
