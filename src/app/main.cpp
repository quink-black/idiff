#include "app/app.h"
#include "app/platform/platform.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>

#include <SDL.h>
#include <nfd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

// Load the app icon from resource/idiff.png and set it on the SDL window.
// On macOS the .icns in the app bundle provides the Dock icon, so this is
// only needed on Linux and Windows.  OpenCV (already linked via idiff_core)
// handles PNG decoding; the result is converted to an SDL_Surface.
void set_window_icon(SDL_Window* window) {
#ifdef __APPLE__
    (void)window;
#else
    // Search order: resource path (macOS bundle) -> executable dir -> cwd
    std::vector<std::string> candidates;
    std::string rp = idiff::platform::get_resource_path();
    if (!rp.empty()) candidates.push_back(rp + "/idiff.png");

    // Locate the directory that holds the running executable.
    std::filesystem::path exe_dir;
#if defined(__linux__)
    exe_dir = std::filesystem::read_symlink("/proc/self/exe").parent_path();
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) exe_dir = std::filesystem::path(buf).parent_path();
#endif
    if (!exe_dir.empty()) candidates.push_back((exe_dir / "idiff.png").string());

    cv::Mat mat;
    for (const auto& p : candidates) {
        mat = cv::imread(p, cv::IMREAD_UNCHANGED);
        if (!mat.empty()) break;
    }
    if (mat.empty()) return;

    // SDL expects RGBA byte order regardless of platform.
    cv::Mat rgba;
    switch (mat.channels()) {
        case 3: cv::cvtColor(mat, rgba, cv::COLOR_BGR2RGBA); break;
        case 4: cv::cvtColor(mat, rgba, cv::COLOR_BGRA2RGBA); break;
        case 1: cv::cvtColor(mat, rgba, cv::COLOR_GRAY2RGBA); break;
        default: return;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
        rgba.data, rgba.cols, rgba.rows, 32,
        static_cast<int>(rgba.step),
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!surface) return;

    SDL_SetWindowIcon(window, surface);
    SDL_FreeSurface(surface);
#endif
}

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

    set_window_icon(window);

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
