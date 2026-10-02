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

constexpr int kImageWidth  = 1920;
constexpr int kImageHeight = 1080;
constexpr int kTileSize    = 32;
constexpr double kBenchmarkDurationSeconds = 10.0; // one benchmark run lasts this long

// ---------------------------------------------------------------------------
// Camera.
//
// All scene and ray-tracing logic lives in Scene (Intel Embree backend).
// Only the camera — mapping a pixel to a primary ray — stays here.
// ---------------------------------------------------------------------------

// Perspective camera at the origin, looking down -Z.
// Maps the pixel center (x + 0.5, y + 0.5) to a normalized direction,
// correcting for aspect so the scene renders without distortion.
Ray generateCameraRay(int x, int y, int width, int height) {
    const double ndcX = (static_cast<double>(x) + 0.5) / static_cast<double>(width) * 2.0 - 1.0;
    const double ndcY = (static_cast<double>(y) + 0.5) / static_cast<double>(height) * 2.0 - 1.0;
    const double aspect = static_cast<double>(width) / static_cast<double>(height);

    // Screen y grows downward, world y grows upward: flip ndcY.
    return {
        Vec3{0.0, 0.0, 0.0},
        Vec3{ndcX * aspect, -ndcY, -1.0}.normalize()
    };
}

[[nodiscard]] uint8_t toByte(double v) {
    double c = v * 255.0;
    if (c < 0.0) {
        c = 0.0;
    } else if (c > 255.0) {
        c = 255.0;
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
    {"AMD Ryzen 9 9950X3D",  88000},
    {"Intel Core i9-14900K", 75000},
    {"AMD Ryzen 7 7800X3D",  45000},
    {"Apple M3 Max",         35000},
    {"Intel Core i5-13600K", 30000},
};

// UI state machine.
enum class State {
    Idle,       // Waiting for the user to press "Run Benchmark"
    Rendering   // Benchmark running on the worker pool in the background
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

    GLFWwindow* window = glfwCreateWindow(kImageWidth, kImageHeight, "LinuxBench", nullptr, nullptr);
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
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Benchmark tool: do not scatter imgui.ini files
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // A larger font for the final score readout. The font atlas is built on
    // the first Render(), so every font must be registered before the loop.
    ImFontConfig scoreFontConfig;
    scoreFontConfig.SizePixels = 56.0f;
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

    std::printf("LinuxBench: %dx%d image, %dx%d tiles, %zu tiles/frame, %u workers, %.0f s run\n",
                kImageWidth, kImageHeight, kTileSize, kTileSize,
                static_cast<std::size_t>((kImageWidth + kTileSize - 1) / kTileSize) *
                static_cast<std::size_t>((kImageHeight + kTileSize - 1) / kTileSize),
                std::thread::hardware_concurrency(),
                kBenchmarkDurationSeconds);
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
    Framebuffer framebuffer(kImageWidth, kImageHeight);
    const std::size_t totalTiles =
        ((kImageWidth + kTileSize - 1) / kTileSize) *
        ((kImageHeight + kTileSize - 1) / kTileSize);

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
    std::atomic<std::size_t> tilesDone{0};          // progress of the current frame (live)
    std::atomic<int>        completedFrames{0};     // frames finished so far (live, read by the UI)
    std::chrono::steady_clock::time_point renderStart;
    std::chrono::steady_clock::time_point renderDeadline; // renderStart + kBenchmarkDurationSeconds
    std::chrono::steady_clock::time_point renderEnd; // written by the render thread, published via renderFinished
    int finalScore = 0;                              // written by the render thread, published via renderFinished
    bool hasResult = false;                          // main-thread-only: a completed run is on display
    double lastRunMs = -1.0;                         // main-thread-only, for the UI
    int lastTitleShown = -1;                         // main-thread-only, throttles title-bar progress updates

    // Starts the benchmark on a background thread so the UI loop never blocks.
    //
    // A run lasts kBenchmarkDurationSeconds: full frames are rendered back to
    // back until the deadline. The worker threads persist across frames —
    // the pool is created once (see above) and its workers park between
    // frames — so the OS scheduler never sees per-frame thread churn. Each
    // frame gets a fresh TileDispatcher; executeFrameAndWait() blocks until
    // every tile is rendered and all workers are back to sleep, so
    // completedFrames is only bumped for provably complete frames.
    auto startBenchmark = [&]() {
        framebuffer.clear();
        tilesDone.store(0, std::memory_order_relaxed);
        completedFrames.store(0, std::memory_order_relaxed);
        renderFinished.store(false, std::memory_order_relaxed);
        renderStart = std::chrono::steady_clock::now();
        renderDeadline = renderStart +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(kBenchmarkDurationSeconds));

        renderThread = std::thread([&]() {
            // Render frame after frame while the elapsed time is still under
            // the 10-second budget.
            while (std::chrono::steady_clock::now() < renderDeadline) {
                // A fresh work queue for this frame. The worker threads
                // themselves persist: they were spawned once with the pool
                // and are simply woken up for each frame.
                TileDispatcher dispatcher(kImageWidth, kImageHeight, kTileSize);
                tilesDone.store(0, std::memory_order_relaxed); // restart the frame progress bar

                // Wake every worker, let them pull tiles from the dispatcher
                // and render them, and block until the dispatcher is empty
                // and all workers are back to sleep: this frame is provably
                // complete before the counter below is bumped.
                pool.executeFrameAndWait(dispatcher, [&](std::size_t) {
                    while (const auto tileOpt = dispatcher.getNextTile()) {
                        const Tile& tile = *tileOpt;
                        // Render every pixel of this tile. Row-major order keeps
                        // the Framebuffer stores cache-friendly.
                        const int xEnd = tile.x + tile.width;
                        const int yEnd = tile.y + tile.height;
                        for (int y = tile.y; y < yEnd; ++y) {
                            for (int x = tile.x; x < xEnd; ++x) {
                                const Vec3 color = scene.traceRay(generateCameraRay(x, y, kImageWidth, kImageHeight));
                                framebuffer.setPixel(x, y,
                                                     toByte(color.x),
                                                     toByte(color.y),
                                                     toByte(color.z));
                            }
                        }
                        tilesDone.fetch_add(1, std::memory_order_relaxed);
                    }
                });

                // This frame is complete: count it, then loop for the next one.
                completedFrames.fetch_add(1, std::memory_order_relaxed);
            }

            // The time window is over: stamp the end and compute the score.
            // Time-precise: frames per second * 1000 — 880 frames in exactly
            // 10.0 s → 88,000 pts, and a CPU that finishes the same work a
            // fraction of a second faster is rewarded proportionally
            // instead of being rounded down to whole integer frames.
            renderEnd = std::chrono::steady_clock::now();
            const double exactElapsedSeconds = std::chrono::duration<double>(renderEnd - renderStart).count();
            finalScore = static_cast<int>(
                (static_cast<double>(completedFrames.load(std::memory_order_relaxed)) / exactElapsedSeconds) * 1000.0);
            renderFinished.store(true, std::memory_order_release); // publishes renderEnd, finalScore, completedFrames
        });

        state = State::Rendering;
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Headless autorun: start the benchmark once the delay has elapsed.
        if (autorun.enabled && !autorunTriggered && state == State::Idle) {
            const auto now = std::chrono::steady_clock::now();
            if (now - appStart >= std::chrono::duration<double>(autorun.delaySeconds)) {
                startBenchmark();
                autorunTriggered = true;
            }
        }

        // Collect a finished run. The acquire-load pairs with the release-store
        // in the render thread, so reading renderEnd, finalScore and
        // completedFrames here is safe.
        if (state == State::Rendering && renderFinished.load(std::memory_order_acquire)) {
            // renderFinished is only published after the 10-second loop has
            // exited and every worker has been joined (see startBenchmark), so
            // by the time we get here the last image is complete and
            // renderThread has nothing left to do — the join is a fast no-op,
            // not a wait.
            renderThread.join();
            lastRunMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                            renderEnd - renderStart).count();
            const int frames = completedFrames.load(std::memory_order_relaxed);
            hasResult = true;
            state = State::Idle;

            std::printf("LinuxBench: benchmark complete — %.1f s, %d frames, score %d pts\n",
                        lastRunMs / 1000.0, frames, finalScore);
            std::fflush(stdout);

            try {
                framebuffer.savePPM("benchmark.ppm");
                saveToCSV(finalScore, lastRunMs, getCPUName());
                std::printf("Wrote benchmark.ppm (%dx%d, P6)\n", kImageWidth, kImageHeight);
                std::printf("Appended result to linuxbench_results.csv\n");
                std::fflush(stdout); // do not hold the line in the buffer when the log is redirected
            } catch (const std::exception& e) {
                std::fprintf(stderr, "LinuxBench: %s\n", e.what());
            }

            if (autorun.enabled && autorunTriggered) {
                break; // headless mode: the first run is done, leave the UI loop
            }
        }

        // Live preview: while the benchmark is running we still upload and
        // render every frame. The framebuffer double-buffers its pixels —
        // updateTexture() copies the back buffer into a front buffer under a
        // short lock — so the GL upload never races the worker pool and the
        // old deadlock is gone. Progress is also shown in the title bar.
        if (state != State::Idle) {
            const int cur = static_cast<int>(tilesDone.load(std::memory_order_relaxed));
            const int frames = completedFrames.load(std::memory_order_relaxed);
            if (cur != lastTitleShown) {
                char title[128];
                std::snprintf(title, sizeof(title), "LinuxBench \u2014 Rendering\u2026 frame %d \u00B7 %d / %d tiles",
                              frames, cur, static_cast<int>(totalTiles));
                glfwSetWindowTitle(window, title);
                lastTitleShown = cur;
            }
        }

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
            // Live status: time left, frames finished so far, progress bars.
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - renderStart).count();
            const double timeLeft = std::max(0.0, kBenchmarkDurationSeconds - elapsed);
            const int frames = completedFrames.load(std::memory_order_relaxed);
            const std::size_t tiles = tilesDone.load(std::memory_order_relaxed);

            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "Benchmark running\u2026");
            ImGui::Text("Time left: %.1f s", timeLeft);
            ImGui::Text("Frames completed: %d", frames);
            ImGui::ProgressBar(static_cast<float>(std::min(1.0, elapsed / kBenchmarkDurationSeconds)),
                               ImVec2(280, 0));
            ImGui::Text("Current frame: %zu / %zu tiles", tiles, totalTiles);
            ImGui::ProgressBar(static_cast<float>(static_cast<double>(tiles) / static_cast<double>(totalTiles)),
                               ImVec2(280, 0));
            ImGui::Separator();
        } else {
            // Idle: the run button, and — once a run has completed — the score.
            if (ImGui::Button("Run Benchmark", ImVec2(220, 48))) {
                startBenchmark();
            }
            if (hasResult) {
                const int frames = completedFrames.load(std::memory_order_relaxed);
                ImGui::Separator();
                ImGui::PushFont(scoreFont);
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "%d pts", finalScore);
                ImGui::PopFont();
                ImGui::Text("Total time: %.3f ms", lastRunMs);
                if (frames > 0 && lastRunMs > 0.0) {
                    const double avgMs = lastRunMs / static_cast<double>(frames);
                    ImGui::Text("%d frames \u00B7 %.3f ms/frame \u00B7 %.1f fps",
                                frames, avgMs, 1000.0 / avgMs);
                }

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
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f, 6.0f));
                if (ImGui::BeginTable("leaderboard", 3,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 52.0f);
                    ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 110.0f);
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
        ImGui::Image(reinterpret_cast<ImTextureID>(framebuffer.getTextureID()),
                     ImVec2(static_cast<float>(kImageWidth), static_cast<float>(kImageHeight)));

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

        // A run just finished (or we're back to Idle): restore the title bar.
        if (lastTitleShown != -1) {
            glfwSetWindowTitle(window, "LinuxBench");
            lastTitleShown = -1;
        }
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
