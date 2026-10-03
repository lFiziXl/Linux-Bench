#include "Framebuffer.h"
#include "Scene.h"
#include "TileDispatcher.h"
#include "ThreadPool.h"
#include "Vec3.h"

// GLFW must be included before the ImGui backends (they build on GLFW types).
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kRenderWidth  = 3840;
constexpr int kRenderHeight = 2160;
constexpr int kWindowWidth  = 1920;
constexpr int kWindowHeight = 1080;
constexpr int kTileSize    = 64;
constexpr int kSamplesPerPixel = 1024; // path-traced samples per pixel (MSAA + BSDF integration)

// ---------------------------------------------------------------------------
// Camera.
//
// All scene and ray-tracing logic lives in Scene (Intel Embree backend).
// Only the camera — mapping a pixel to a primary ray — stays here.
// ---------------------------------------------------------------------------

// Perspective camera positioned in the world, looking at the sphere field.
// Maps the sample position (x, y) — integer pixel plus a fractional jitter
// offset — to a normalized direction, correcting for aspect so the scene
// renders without distortion.
const Vec3 kCameraPos{0.0, 4.0, 6.0};
const Vec3 kCameraTarget{0.0, 6.0, -11.0};

Ray generateCameraRay(double x, double y, int width, int height) {
    const double ndcX = (x + 0.5) / static_cast<double>(width) * 2.0 - 1.0;
    const double ndcY = (y + 0.5) / static_cast<double>(height) * 2.0 - 1.0;
    const double aspect = static_cast<double>(width) / static_cast<double>(height);

    const Vec3 viewDir = (kCameraTarget - kCameraPos).normalize();
    const Vec3 right   = viewDir.cross(Vec3(0.0, 1.0, 0.0)).normalize();
    const Vec3 up      = right.cross(viewDir).normalize();

    // Screen y grows downward, world y grows upward: the minus sign flips ndcY.
    return {kCameraPos, (viewDir + right * (ndcX * aspect) - up * ndcY).normalize()};
}

[[nodiscard]] uint8_t toByte(double v) {
    // Gamma 2.2 encode: perceptual brightening before the 8-bit quantize.
    const double c = std::pow(std::max(0.0, v), 1.0 / 2.2) * 255.0;
    if (c > 255.0) {
        return 255;
    }
    return static_cast<uint8_t>(c + 0.5);
}

// ---------------------------------------------------------------------------
// CPU identification & result export.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string trimWhitespace(const std::string& s) {
    const std::string whitespace = " \t\r\n";
    const auto begin = s.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(whitespace);
    return s.substr(begin, end - begin + 1);
}

// Reads /proc/cpuinfo and returns the first "model name" value;
// "Unknown CPU" when the file or field is unavailable.
[[nodiscard]] std::string getCPUName() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) != 0) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = trimWhitespace(line.substr(colon + 1));
        if (!name.empty()) {
            return name;
        }
    }
    return "Unknown CPU";
}

// Appends one line to linuxbench_results.csv:
//   YYYY-MM-DD HH:MM:SS, CPU Name, Score, TimeMs
void saveToCSV(int score, double timeMs, const std::string& cpuName) {
    const bool create = !std::ifstream("linuxbench_results.csv", std::ios::binary).good();
    std::ofstream csv("linuxbench_results.csv", std::ios::app);
    if (!csv) {
        return; // logging must never crash a finished benchmark
    }
    if (create) {
        csv << "Timestamp, CPU Name, Score, TimeMs\n"; // header on first run only
    }
    const std::time_t nowT = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&nowT, &tm);
    const std::string timestamp = std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                              tm.tm_hour, tm.tm_min, tm.tm_sec);
    csv << timestamp << ", " << cpuName << ", " << score << ", " << timeMs << "\n";
}

// Reference CPU profiles — the leaderboard is ranked against these.
struct CpuProfile {
    const char* name;
    int         score;
};

constexpr CpuProfile kReferenceCpus[] = {
    {"AMD Ryzen 9 9950X3D",  33200},
    {"Intel Core i9-14900K", 28300},
    {"AMD Ryzen 7 7800X3D",  17000},
    {"Apple M3 Max",         13200},
    {"Intel Core i5-13600K", 11300},
};

// UI state machine.
enum class State {
    Idle,       // Waiting for the user to press a run button
    Rendering   // A run in progress on the worker pool in the background
};

// Run mode: a fixed 3-pass benchmark (scored, logged to CSV) or an
// unbounded stress test (live per-pass score, stopped by the user).
enum class RunMode {
    Benchmark,
    Stress
};

// ---------------------------------------------------------------------------
// Headless mode.
//
// LINUXBENCH_AUTORUN=<seconds> starts the benchmark <seconds> after startup
// and exits as soon as the first run completes — for CI / servers where
// nobody is present to press the button.
// ---------------------------------------------------------------------------
struct Autorun {
    bool   enabled = false;
    double delaySeconds = 0.0;
};

[[nodiscard]] Autorun parseAutorun() {
    const char* env = std::getenv("LINUXBENCH_AUTORUN");
    if (env == nullptr) {
        return {};
    }
    const double delay = std::atof(env);
    if (delay < 0.0) {
        return {};
    }
    return {true, delay};
}

} // namespace

int main() {
    const Autorun autorun = parseAutorun();
    if (autorun.enabled) {
        std::printf("LinuxBench: autorun enabled (start in %.1f s, exit after the first run)\n",
                    autorun.delaySeconds);
        std::fflush(stdout);
    }
    const std::chrono::steady_clock::time_point appStart = std::chrono::steady_clock::now();
    bool autorunTriggered = false;

    // ------------------------------------------------------------------ GLFW
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "LinuxBench: failed to initialize GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // required on macOS, harmless elsewhere

    GLFWwindow* window = glfwCreateWindow(kWindowWidth, kWindowHeight, "Linux Benchmark", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "LinuxBench: failed to create the window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync: no point redrawing faster than the display

    // ----------------------------------------------------------------- ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // HiDPI: ask GLFW for the monitor's content scale (1.0 on a standard
    // display, 2.0 on a 200% HiDPI one) and scale the whole UI by it.
    // ImGui does not auto-scale, so without this the text overflows the
    // fixed-size widgets on HiDPI monitors.
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float scale = xscale;
    ImGui::GetStyle().ScaleAllSizes(scale);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Benchmark tool: do not scatter imgui.ini files
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // HiDPI: scale the default UI font to the monitor's DPI (ImGui's
    // built-in default is 13px at 100%).
    ImFontConfig defaultFontCfg;
    defaultFontCfg.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&defaultFontCfg);

    // A larger font for the final score readout. The font atlas is built on
    // the first Render(), so every font must be registered before the loop.
    ImFontConfig scoreFontConfig;
    scoreFontConfig.SizePixels = 56.0f * scale;
    ImFont* scoreFont = io.Fonts->AddFontDefault(&scoreFontConfig);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        std::fprintf(stderr, "LinuxBench: failed to initialize the ImGui OpenGL3 backend\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    std::printf("LinuxBench: Render: %dx%d, Window: %dx%d, %dx%d tiles, %zu tiles, %d spp, %u workers — benchmark (3 passes) + stress test\n",
                kRenderWidth, kRenderHeight, kWindowWidth, kWindowHeight, kTileSize, kTileSize,
                static_cast<std::size_t>((kRenderWidth + kTileSize - 1) / kTileSize) *
                static_cast<std::size_t>((kRenderHeight + kTileSize - 1) / kTileSize),
                kSamplesPerPixel,
                std::thread::hardware_concurrency());
    std::fflush(stdout);

    // ----------------------------------------------------------------- Scene
    // Built once, before the UI loop. After construction the scene is
    // read-only, and Embree's intersection queries are thread-safe on a
    // committed scene — which is what lets the worker pool call traceRay()
    // in parallel.
    Scene scene; // throws std::runtime_error if the Embree BVH build fails
    std::printf("LinuxBench: scene ready — %zu spheres, %zu triangles (Embree %d.%d.%d)\n",
                scene.sphereCount(), scene.triangleCount(),
                RTC_VERSION_MAJOR, RTC_VERSION_MINOR, RTC_VERSION_PATCH);
    std::fflush(stdout);

    // -------------------------------------------------------------- Rendering
    Framebuffer framebuffer(kRenderWidth, kRenderHeight);
    const std::size_t totalTiles =
        ((kRenderWidth + kTileSize - 1) / kTileSize) *
        ((kRenderHeight + kTileSize - 1) / kTileSize);

    // Persistent worker pool: threads are spawned here ONCE and joined ONCE
    // in the destructor. Between frames they park on a condition variable
    // (zero CPU), so a run reuses the same worker threads across every frame
    // instead of recreating them — no per-frame OS scheduler churn.
    // Declared before renderThread on purpose: at teardown the render thread
    // is joined first, so no worker can outlive the pool.
    ThreadPool pool; // default: hardware_concurrency() workers

    State state = State::Idle;

    std::thread             renderThread;
    std::atomic<bool>       renderFinished{false};
    std::atomic<bool>       cancelRequested{false};  // Stop button (main thread) → render thread
    std::atomic<int>        currentPass{0};          // 1-based pass currently in flight (live)
    std::atomic<int>        lastPassScore{0};        // score of the newest completed pass (live)
    std::atomic<int>        passesCompleted{0};      // published via renderFinished
    std::atomic<std::size_t> tilesDone{0};           // tile progress of the current pass (live)
    std::chrono::steady_clock::time_point renderStart;
    std::chrono::steady_clock::time_point renderEnd; // written by the render thread, published via renderFinished
    int finalScore = 0;                              // written by the render thread, published via renderFinished
    double lastPassSeconds = 0.0;                    // written by the render thread, published via renderFinished
    RunMode activeMode = RunMode::Benchmark;         // main-thread-only: which run is in flight
    RunMode lastResultMode = RunMode::Benchmark;     // main-thread-only: which completed run is on display
    bool hasResult = false;                          // main-thread-only: a completed run is on display
    double lastRunMs = -1.0;                         // main-thread-only, for the UI

    // Starts a run on a background thread so the UI loop never blocks.
    //
    // Every pass renders one full path-traced 4K frame (kSamplesPerPixel
    // samples per pixel). The render thread loops over the passes, handing a
    // fresh TileDispatcher to the persistent pool each time;
    // executeFrameAndWait() blocks until every tile is rendered and all
    // workers are back to sleep, so each pass is provably complete before
    // the next one starts. Benchmark mode does exactly 3 passes and scores
    // the summed time; stress mode loops until the user hits Stop.
    auto startBenchmark = [&](RunMode mode) {
        cancelRequested.store(false, std::memory_order_relaxed);
        currentPass.store(0, std::memory_order_relaxed);
        lastPassScore.store(0, std::memory_order_relaxed);
        passesCompleted.store(0, std::memory_order_relaxed);
        tilesDone.store(0, std::memory_order_relaxed);
        renderFinished.store(false, std::memory_order_relaxed);
        renderStart = std::chrono::steady_clock::now();

        // `mode` is a parameter of this lambda and dies when it returns, so
        // the render-thread lambda must capture it BY VALUE — a plain [&]
        // capture would dangle the moment startBenchmark() returns.
        renderThread = std::thread([&, mode]() {
            const int maxPasses = (mode == RunMode::Benchmark) ? 3 : INT_MAX;
            double totalElapsedSeconds = 0.0; // sum of the completed passes

            for (int p = 0; p < maxPasses; ++p) {
                if (cancelRequested.load(std::memory_order_relaxed)) {
                    break; // Stop pressed: don't start the next pass
                }
                currentPass.store(p + 1, std::memory_order_relaxed);
                framebuffer.clear(); // fresh back buffer for this pass
                tilesDone.store(0, std::memory_order_relaxed);

                const auto passStart = std::chrono::steady_clock::now();

                // A fresh work queue for this pass. The worker threads
                // themselves persist: spawned once with the pool, they are
                // simply woken up again.
                TileDispatcher dispatcher(kRenderWidth, kRenderHeight, kTileSize);

                // Wake every worker, let them pull tiles from the dispatcher
                // and render them, and block until the dispatcher is empty
                // and all workers are back to sleep: this pass is provably
                // complete before the next one starts.
                pool.executeFrameAndWait(dispatcher, [&](std::size_t worker_id) {
                    // Per-worker, per-pass PRNG seed (worker_id + 1 + p*100):
                    // no shared RNG state, no locks — and the noise pattern
                    // varies on every pass, so no pass is a warm-cache repeat
                    // of the last one.
                    XorShift32 rng(static_cast<std::uint32_t>(worker_id) + 1u
                                   + static_cast<std::uint32_t>(p) * 100u);

                    while (const auto tileOpt = dispatcher.getNextTile()) {
                        const Tile& tile = *tileOpt;
                        // Render every pixel of this tile. Row-major order keeps
                        // the Framebuffer stores cache-friendly.
                        const int xEnd = tile.x + tile.width;
                        const int yEnd = tile.y + tile.height;
                        for (int y = tile.y; y < yEnd; ++y) {
                            for (int x = tile.x; x < xEnd; ++x) {
                                // kSamplesPerPixel jittered samples (MSAA + soft-shadow
                                // + area-light integration), averaged — this is the
                                // path-traced frame.
                                Vec3 accumulated{};
                                for (int s = 0; s < kSamplesPerPixel; ++s) {
                                    const double jx = x + rng.nextFloat();
                                    const double jy = y + rng.nextFloat();
                                    accumulated = accumulated +
                                        scene.traceRay(generateCameraRay(jx, jy, kRenderWidth, kRenderHeight), rng);
                                }
                                framebuffer.setPixel(x, y,
                                                     toByte(accumulated.x / static_cast<double>(kSamplesPerPixel)),
                                                     toByte(accumulated.y / static_cast<double>(kSamplesPerPixel)),
                                                     toByte(accumulated.z / static_cast<double>(kSamplesPerPixel)));
                            }
                        }
                        tilesDone.fetch_add(1, std::memory_order_relaxed);
                    }
                });

                const double passSeconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - passStart).count();
                totalElapsedSeconds += passSeconds;
                lastPassSeconds = passSeconds;
                lastPassScore.store(static_cast<int>(1600000.0 / passSeconds), std::memory_order_relaxed);
                passesCompleted.fetch_add(1, std::memory_order_relaxed);

                std::printf("LinuxBench: pass %d complete — %.3f s, %d pts/pass\n",
                            p + 1, passSeconds, lastPassScore.load(std::memory_order_relaxed));
                std::fflush(stdout);
            }

            // All scheduled passes are done (or the run was stopped): stamp
            // the end and compute the final score.
            //
            // Benchmark: three frames of work divided by the summed pass
            // time — (1,600,000 * 3) / total. Stress: the last completed
            // pass's score is the reported one.
            renderEnd = std::chrono::steady_clock::now();
            if (mode == RunMode::Benchmark) {
                finalScore = static_cast<int>((1600000.0 * 3.0) / totalElapsedSeconds);
            } else {
                finalScore = lastPassScore.load(std::memory_order_relaxed);
            }
            renderFinished.store(true, std::memory_order_release); // publishes renderEnd, finalScore, lastPassSeconds
        });

        activeMode = mode; // main-thread-only: which run is in flight
        state = State::Rendering;
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Headless autorun: start the benchmark once the delay has elapsed.
        if (autorun.enabled && !autorunTriggered && state == State::Idle) {
            const auto now = std::chrono::steady_clock::now();
            if (now - appStart >= std::chrono::duration<double>(autorun.delaySeconds)) {
                startBenchmark(RunMode::Benchmark);
                autorunTriggered = true;
            }
        }

        // Collect a finished run. The acquire-load pairs with the release-store
        // in the render thread, so reading renderEnd, finalScore and
        // lastPassSeconds here is safe.
        if (state == State::Rendering && renderFinished.load(std::memory_order_acquire)) {
            // renderFinished is only published after the final pass's
            // executeFrameAndWait() returned — every tile rendered, all
            // workers back to sleep (see startBenchmark) — so the join below
            // is a fast no-op, not a wait.
            renderThread.join();

            const bool cancelled = cancelRequested.load(std::memory_order_acquire);
            const int completedPasses = passesCompleted.load(std::memory_order_relaxed);
            state = State::Idle;

            if (activeMode == RunMode::Benchmark && !cancelled) {
                // Official benchmark: all 3 passes done — score, PPM and CSV.
                lastRunMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                                renderEnd - renderStart).count();
                hasResult = true;
                lastResultMode = RunMode::Benchmark;

                std::printf("LinuxBench: benchmark complete — 3 passes, %.3f s total, score %d pts\n",
                            lastRunMs / 1000.0, finalScore);
                std::fflush(stdout);

                try {
                    framebuffer.savePPM("benchmark.ppm");
                    saveToCSV(finalScore, lastRunMs, getCPUName());
                    std::printf("Wrote benchmark.ppm (%dx%d, P6)\n", kRenderWidth, kRenderHeight);
                    std::printf("Appended result to linuxbench_results.csv\n");
                    std::fflush(stdout); // do not hold the line in the buffer when the log is redirected
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "LinuxBench: %s\n", e.what());
                }
            } else if (activeMode == RunMode::Stress && cancelled && completedPasses >= 1) {
                // Stress test stopped by the user: report the last completed
                // pass's score. Stress runs are diagnostic — no CSV, no PPM.
                lastRunMs = lastPassSeconds * 1000.0;
                hasResult = true;
                lastResultMode = RunMode::Stress;

                std::printf("LinuxBench: stress test stopped — %d pass(es), last pass %.3f s, %d pts/pass\n",
                            completedPasses, lastPassSeconds, finalScore);
                std::fflush(stdout);
            } else {
                // Stopped before a full pass landed (or a benchmark aborted):
                // nothing to record — any previous result on display is dropped.
                hasResult = false;
                std::printf("LinuxBench: run stopped after %d completed pass(es) — no result recorded\n",
                            completedPasses);
                std::fflush(stdout);
            }

            if (autorun.enabled && autorunTriggered) {
                break; // headless mode: the first run is done, leave the UI loop
            }
        }

        // Live preview: while a run is in flight we still upload and render
        // every frame. The framebuffer double-buffers its pixels —
        // updateTexture() copies the back buffer into a front buffer under a
        // short lock — so the GL upload never races the worker pool and the
        // old deadlock is gone.

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // OpenGL calls are main-thread only. updateTexture() first copies the
        // back buffer into the front buffer under its lock, so the upload is
        // safe even while workers are still writing — this is the live preview.
        framebuffer.updateTexture();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

        ImGui::Begin("LinuxBench", nullptr, windowFlags);

        if (state == State::Rendering) {
            // Live status: mode, pass, last-pass score, elapsed time and tile
            // progress for the current frame.
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - renderStart).count();
            const std::size_t tiles = tilesDone.load(std::memory_order_relaxed);
            const int pass = currentPass.load(std::memory_order_relaxed);

            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "Path tracing\u2026 %d samples/pixel", kSamplesPerPixel);
            if (activeMode == RunMode::Benchmark) {
                ImGui::Text("Pass %d / 3", pass);
            } else {
                ImGui::Text("Stress Test: Pass %d", pass);
            }
            const int lastScore = lastPassScore.load(std::memory_order_relaxed);
            if (lastScore > 0) {
                ImGui::Text("Last pass: %d pts", lastScore);
            }
            if (cancelRequested.load(std::memory_order_relaxed)) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "Stopping\u2026 finishing the current pass");
            }
            ImGui::Text("Elapsed: %.1f s", elapsed);
            ImGui::Text("%zu / %zu tiles", tiles, totalTiles);
            ImGui::ProgressBar(static_cast<float>(static_cast<double>(tiles) / static_cast<double>(totalTiles)),
                               ImVec2(280 * scale, 0));
            ImGui::Separator();

            // Stop: asks the render thread to finish the current pass and
            // exit the pass loop. The pool drains cleanly, then the run
            // collection above returns the UI to Idle.
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.13f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.08f, 0.08f, 1.0f));
            if (ImGui::Button("Stop", ImVec2(140 * scale, 36 * scale))) {
                cancelRequested.store(true, std::memory_order_relaxed);
            }
            ImGui::PopStyleColor(3);
        } else {
            // Idle: the two run modes side by side, and — once a run has
            // completed — the score.
            if (ImGui::Button("Run Benchmark (3 Passes)", ImVec2(260 * scale, 48 * scale))) {
                startBenchmark(RunMode::Benchmark);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stress Test (Infinite)", ImVec2(260 * scale, 48 * scale))) {
                startBenchmark(RunMode::Stress);
            }
            if (hasResult) {
                ImGui::Separator();
                ImGui::PushFont(scoreFont);
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "%d pts", finalScore);
                ImGui::PopFont();
                if (lastResultMode == RunMode::Benchmark) {
                    ImGui::Text("Benchmark \u2014 3 passes, %.3f ms total", lastRunMs);
                } else {
                    ImGui::Text("Stress test \u2014 last pass, %.3f ms", lastRunMs);
                }
                ImGui::Text("%dx%d \u00B7 %d samples/pixel", kRenderWidth, kRenderHeight, kSamplesPerPixel);

                // Leaderboard: reference profiles + the user's run, ranked by score.
                struct LeaderboardEntry {
                    std::string name;
                    int         score;
                    bool        isUser = false;
                };
                std::vector<LeaderboardEntry> board;
                board.reserve(sizeof(kReferenceCpus) / sizeof(kReferenceCpus[0]) + 1);
                for (const auto& cpu : kReferenceCpus) {
                    board.push_back({cpu.name, cpu.score, false});
                }
                board.push_back({getCPUName() + " (Your CPU)", finalScore, true});
                std::stable_sort(board.begin(), board.end(),
                                 [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
                                     return a.score > b.score;
                                 });

                ImGui::Separator();
                ImGui::Text("Leaderboard");
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f * scale, 6.0f * scale));
                if (ImGui::BeginTable("leaderboard", 3,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 52.0f * scale);
                    ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 110.0f * scale);
                    ImGui::TableHeadersRow();

                    int rank = 1;
                    for (const auto& entry : board) {
                        if (entry.isUser) {
                            // Highlight the user's run in bright orange so it
                            // stands out against the reference profiles.
                            ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(1.0f, 0.72f, 0.20f, 0.22f));
                            ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 0.72f, 0.20f, 0.22f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.25f, 1.0f));
                        }
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("#%d", rank++);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%s", entry.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%d pts", entry.score);
                        if (entry.isUser) {
                            ImGui::PopStyleColor(3);
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
            }
        }

        // The texture was just uploaded above; ImGui only references its ID.
        // Scale the 4K render down to fill the remaining content region,
        // preserving the aspect ratio, so it fits the window without scrolling.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspect = static_cast<float>(kRenderWidth) / static_cast<float>(kRenderHeight);
        float imgWidth = avail.x;
        float imgHeight = imgWidth / aspect;
        ImGui::Image(reinterpret_cast<ImTextureID>(framebuffer.getTextureID()), ImVec2(imgWidth, imgHeight));

        ImGui::End();
        ImGui::EndFrame();
        ImGui::Render();

        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);
        glClearColor(0.11f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Never tear down while workers might still write into the framebuffer.
    if (renderThread.joinable()) {
        renderThread.join();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
