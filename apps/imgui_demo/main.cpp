/*
 * main.cpp — Spiritty ImGui demo entry point.
 *
 * Opens a GLFW window with an OpenGL 3.3 core context, initializes Dear
 * ImGui on top, and hosts a TerminalWidget that renders a Spiritty
 * terminal directly into the default framebuffer. ImGui draws a status
 * bar + connect dialog overlay (see PLAN.md Phase 5 — "v1 layout":
 * terminal owns the framebuffer, ImGui composites on top).
 *
 * Input flow:
 *   GLFW char/key/mouse callbacks → TerminalWidget::send_input()
 *                                  → net_send → Telnetty IAC-escape
 *                                  → socket / io_uring
 *
 * Output flow:
 *   socket → Telnetty parse → on_bytes_thunk (worker context)
 *          → SPSC byte queue → tick() drains → sp_terminal_write
 *          → sp_terminal_render → GL pixels in default FBO
 *
 * Backend selection (suspenders vs thread) happens at compile time in
 * apps/imgui_demo/CMakeLists.txt; both implementations satisfy net.h.
 */

#include "terminal_widget.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "net.h"

namespace {

constexpr int   kInitCols   = 100;
constexpr int   kInitRows   = 30;
constexpr float kCellWidth  = 8.0f;
constexpr float kCellHeight = 16.0f;

/* Fallback URI when the user just hits "Connect" without typing one. */
constexpr const char* kDefaultUri = "tcp://towel.blinkenlights.nl:23";

/* The widget is owned by the GLFW main loop but accessed by GLFW's C
 * callbacks. Stash a pointer into the window user-pointer slot to avoid
 * a global. */
struct AppCtx {
    TerminalWidget* widget = nullptr;
    /* Track whether ImGui swallowed the last key/mouse event so we don't
     * also forward it to the terminal (e.g. when typing in the connect
     * dialog). */
    bool imgui_wants_keyboard = false;
    bool imgui_wants_mouse    = false;
};

AppCtx* ctx_from(GLFWwindow* w) {
    return static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
}

/* ---- GLFW input callbacks -------------------------------------------- */

void on_char(GLFWwindow* w, unsigned int codepoint) {
    auto* ctx = ctx_from(w);
    ImGui_ImplGlfw_CharCallback(w, codepoint);
    if (!ctx || !ctx->widget || ctx->imgui_wants_keyboard) return;

    /* Encode codepoint as UTF-8. */
    char buf[4];
    int len = 0;
    if (codepoint < 0x80) {
        buf[len++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        buf[len++] = static_cast<char>(0xC0 | (codepoint >> 6));
        buf[len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        buf[len++] = static_cast<char>(0xE0 | (codepoint >> 12));
        buf[len++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        buf[len++] = static_cast<char>(0xF0 | (codepoint >> 18));
        buf[len++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    ctx->widget->send_input(buf, static_cast<size_t>(len));
}

/* Map non-character keys to the byte sequences a VT100/xterm peer expects.
 * GLFW's char callback already covers printable input — this only fires
 * for keys that produce no codepoint (Enter, arrows, F-keys, etc.). */
void on_key(GLFWwindow* w, int key, int scancode, int action, int mods) {
    auto* ctx = ctx_from(w);
    ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
    if (!ctx || !ctx->widget || ctx->imgui_wants_keyboard) return;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    /* Ctrl+letter → control byte (0x01..0x1A). Char callback won't fire
     * for these, so we emit the control code here. */
    if (ctrl && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        char b = static_cast<char>(key - GLFW_KEY_A + 1);
        ctx->widget->send_input(&b, 1);
        return;
    }

    const char* seq = nullptr;
    switch (key) {
        case GLFW_KEY_ENTER:        seq = "\r";        break;
        case GLFW_KEY_KP_ENTER:     seq = "\r";        break;
        case GLFW_KEY_TAB:          seq = "\t";        break;
        case GLFW_KEY_BACKSPACE:    seq = "\x7f";      break;
        case GLFW_KEY_ESCAPE:       seq = "\x1b";      break;
        case GLFW_KEY_UP:           seq = "\x1b[A";    break;
        case GLFW_KEY_DOWN:         seq = "\x1b[B";    break;
        case GLFW_KEY_RIGHT:        seq = "\x1b[C";    break;
        case GLFW_KEY_LEFT:         seq = "\x1b[D";    break;
        case GLFW_KEY_HOME:         seq = "\x1b[H";    break;
        case GLFW_KEY_END:          seq = "\x1b[F";    break;
        case GLFW_KEY_PAGE_UP:      seq = "\x1b[5~";   break;
        case GLFW_KEY_PAGE_DOWN:    seq = "\x1b[6~";   break;
        case GLFW_KEY_INSERT:       seq = "\x1b[2~";   break;
        case GLFW_KEY_DELETE:       seq = "\x1b[3~";   break;
        case GLFW_KEY_F1:           seq = "\x1bOP";    break;
        case GLFW_KEY_F2:           seq = "\x1bOQ";    break;
        case GLFW_KEY_F3:           seq = "\x1bOR";    break;
        case GLFW_KEY_F4:           seq = "\x1bOS";    break;
        case GLFW_KEY_F5:           seq = "\x1b[15~";  break;
        case GLFW_KEY_F6:           seq = "\x1b[17~";  break;
        case GLFW_KEY_F7:           seq = "\x1b[18~";  break;
        case GLFW_KEY_F8:           seq = "\x1b[19~";  break;
        case GLFW_KEY_F9:           seq = "\x1b[20~";  break;
        case GLFW_KEY_F10:          seq = "\x1b[21~";  break;
        case GLFW_KEY_F11:          seq = "\x1b[23~";  break;
        case GLFW_KEY_F12:          seq = "\x1b[24~";  break;
        default: break;
    }
    if (seq) ctx->widget->send_input(seq, std::strlen(seq));
}

void on_framebuffer_size(GLFWwindow* w, int fb_w, int fb_h) {
    auto* ctx = ctx_from(w);
    if (!ctx || !ctx->widget) return;
    int cols = fb_w / static_cast<int>(kCellWidth);
    int rows = fb_h / static_cast<int>(kCellHeight);
    if (cols < 8)  cols = 8;
    if (rows < 4)  rows = 4;
    ctx->widget->resize(cols, rows, fb_w, fb_h);
}

void on_glfw_error(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc ? desc : "(null)");
}

/* ---- CLI parsing ----------------------------------------------------- */

struct CliArgs {
    std::string connect_uri;     /* "" → no auto-connect */
};

CliArgs parse_args(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            a.connect_uri = argv[++i];
        } else if (std::strncmp(argv[i], "--connect=", 10) == 0) {
            a.connect_uri = argv[i] + 10;
        } else if (std::strcmp(argv[i], "--help") == 0
                || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: %s [--connect tcp://host:port]\n",
                        argc > 0 ? argv[0] : "spiritty_demo");
            std::exit(0);
        }
    }
    return a;
}

} /* namespace */

int main(int argc, char** argv) {
    CliArgs cli = parse_args(argc, argv);

    glfwSetErrorCallback(on_glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    /* OpenGL 3.3 core — same context the GL renderer expects. */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    const int win_w = static_cast<int>(kInitCols * kCellWidth);
    const int win_h = static_cast<int>(kInitRows * kCellHeight);

    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "Spiritty Demo",
                                          nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); /* vsync */

    /* Build the widget after the GL context is current — its constructor
     * initializes the renderer, which compiles GLSL programs. */
    TerminalWidget widget(kInitCols, kInitRows, kCellWidth, kCellHeight,
                          reinterpret_cast<void*(*)(const char*)>(
                              glfwGetProcAddress));

    AppCtx ctx;
    ctx.widget = &widget;
    glfwSetWindowUserPointer(window, &ctx);

    /* ImGui setup. */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; /* don't litter the cwd */
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, /*install_callbacks=*/false);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    /* Install our wrapper callbacks (which forward to the ImGui ones). */
    glfwSetCharCallback(window, on_char);
    glfwSetKeyCallback(window, on_key);
    glfwSetMouseButtonCallback(window, ImGui_ImplGlfw_MouseButtonCallback);
    glfwSetScrollCallback(window, ImGui_ImplGlfw_ScrollCallback);
    glfwSetFramebufferSizeCallback(window, on_framebuffer_size);

    /* Optional auto-connect. */
    char uri_buf[256];
    std::snprintf(uri_buf, sizeof uri_buf, "%s",
                  cli.connect_uri.empty() ? kDefaultUri : cli.connect_uri.c_str());
    if (!cli.connect_uri.empty()) {
        if (!widget.connect(cli.connect_uri)) {
            std::fprintf(stderr, "[demo] auto-connect to %s failed\n",
                         cli.connect_uri.c_str());
        }
    }

    bool show_connect_dialog = cli.connect_uri.empty();

    /* Main loop. */
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        /* Tick the network runtime (suspenders backend; no-op for thread). */
        net_pump();

        ctx.imgui_wants_keyboard = io.WantCaptureKeyboard;
        ctx.imgui_wants_mouse    = io.WantCaptureMouse;

        /* Reset GL state to a clean slate before the terminal renders;
         * ImGui's last frame may have left blending/scissor enabled. */
        int fb_w, fb_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);

        const float t = static_cast<float>(glfwGetTime());
        widget.tick(t);

        /* ImGui overlay pass. */
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        /* Status bar across the top. */
        if (ImGui::BeginMainMenuBar()) {
            ImGui::Text("spiritty_demo");
            ImGui::Separator();
            ImGui::Text("backend: %s", widget.backend_name());
            ImGui::Separator();
            ImGui::Text("%dx%d cells (%dx%d px)",
                        widget.cols(), widget.rows(), fb_w, fb_h);
            ImGui::Separator();
            ImGui::Text("rx: %llu B",
                        static_cast<unsigned long long>(widget.bytes_received()));
            ImGui::Separator();
            ImGui::Text("%s", widget.connected() ? "[connected]" : "[idle]");

            /* Right-aligned controls. */
            const float right_btns_w = 220.0f;
            ImGui::SameLine(ImGui::GetWindowWidth() - right_btns_w);
            if (widget.connected()) {
                if (ImGui::SmallButton("Disconnect")) {
                    widget.disconnect();
                    show_connect_dialog = true;
                }
            } else {
                if (ImGui::SmallButton("Connect...")) {
                    show_connect_dialog = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Quit")) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            ImGui::EndMainMenuBar();
        }

        if (show_connect_dialog) {
            ImGui::SetNextWindowSize(ImVec2(420, 120), ImGuiCond_Once);
            if (ImGui::Begin("Connect", &show_connect_dialog,
                             ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextWrapped("Enter a tcp:// URI. Default points at "
                                   "towel.blinkenlights.nl:23 (Star Wars ASCII).");
                ImGui::Spacing();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##uri", uri_buf, sizeof uri_buf);
                ImGui::Spacing();
                if (ImGui::Button("Connect", ImVec2(120, 0))) {
                    if (widget.connect(uri_buf)) {
                        show_connect_dialog = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                    show_connect_dialog = false;
                }
            }
            ImGui::End();
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    /* Teardown. */
    widget.disconnect();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
