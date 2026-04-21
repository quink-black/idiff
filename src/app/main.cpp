#include "app/app.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>

#include <SDL.h>
#include <nfd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

// On Windows the standard `argv` is encoded in the active code page
// (typically GBK/CP936 on Chinese systems), not UTF-8.  A user launching
// idiff with a CJK path such as `H:\=下载中=\a.jpg` would have those
// bytes mis-interpreted once they reach platform::utf8_to_wide(),
// producing a garbled wide path and a file-not-found error at best --
// or at worst, depending on the code page, a conversion that smuggles
// invalid UTF-8 (e.g. unpaired high bytes) deep into the loader.  Fetch
// the real UTF-16 command line and re-encode it as UTF-8 so the rest
// of the app can stick to its single-encoding contract.
std::vector<std::string> collect_startup_paths(int argc, char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv; // the ACP-encoded pair is unsafe for non-ASCII
    std::vector<std::string> paths;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) return paths;
    for (int i = 1; i < wargc; ++i) {
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                           nullptr, 0, nullptr, nullptr);
        if (utf8_len <= 1) continue; // 1 == just the null terminator
        std::string s(static_cast<size_t>(utf8_len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                            s.data(), utf8_len, nullptr, nullptr);
        paths.emplace_back(std::move(s));
    }
    LocalFree(wargv);
    return paths;
#else
    std::vector<std::string> paths;
    paths.reserve(argc > 1 ? argc - 1 : 0);
    for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
    return paths;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "idiff",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1600, 900,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    idiff::App app;
    if (!app.init(window, renderer)) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto startup_paths = collect_startup_paths(argc, argv);
    if (!startup_paths.empty()) {
        // Route through load_paths() so launching with a JSON
        // argument (e.g. `idiff config.json`) is equivalent to the
        // menu's "Open Comparison Config..." entry.
        app.load_paths(startup_paths);
    }

    // Make sure drag-and-drop file events are delivered.
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    bool running = true;
    while (running) {
        // Paths collected from SDL_DROPFILE events during this frame.
        // Batched so that sort / label / diff recompute only runs once.
        std::vector<std::string> dropped_paths;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                app.request_quit();
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                app.request_quit();
            }
            if (event.type == SDL_DROPFILE && event.drop.file) {
                dropped_paths.emplace_back(event.drop.file);
                SDL_free(event.drop.file);
            }
        }

        if (!dropped_paths.empty()) {
            // Drag-and-drop accepts both images and comparison-config
            // JSON files; the dispatcher figures out which is which.
            app.load_paths(dropped_paths);
        }

        app.frame();

        if (app.wants_quit()) {
            running = false;
        }
    }

    app.shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
