// Guided physical-gamepad validation probe for SaidaEngine P0.6.
//
// It exercises the EXACT GLFW calls the engine's desktop backend uses in
// src/core/Input.cpp:305-336 (glfwJoystickIsGamepad -> glfwGetGamepadState ->
// glfwGetGamepadName) and reports state using the same standard-mapping order
// as saida::GamepadButton / saida::GamepadAxis (a 1:1 mirror of GLFW_GAMEPAD_*).
// A PASS here therefore proves the engine's real code path against real hardware.
//
//   gamepad_probe 0    -> detection only (no user input needed)
//   gamepad_probe 40   -> detection + 40 s guided coverage capture
//
// Standalone diagnostic (not a CMake target, like tools/vulkan_preflight.cpp).
// Build from an MSYS2 UCRT64 shell:
//   g++ -O2 -std=c++17 tools/gamepad_probe.cpp -o gamepad_probe.exe \
//       $(pkg-config --cflags glfw3) /ucrt64/lib/libglfw3.a -lgdi32 \
//       -static-libgcc -static-libstdc++ -static

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

// Mirrors saida::GamepadButton (InputEnums.hpp) == GLFW standard button order.
static const char* kButtonNames[15] = {
    "A", "B", "X", "Y", "LeftBumper", "RightBumper", "Back", "Start", "Guide",
    "LeftThumb", "RightThumb", "DpadUp", "DpadRight", "DpadDown", "DpadLeft"};

// Mirrors saida::GamepadAxis == GLFW standard axis order.
static const char* kAxisNames[6] = {"LeftX",        "LeftY",       "RightX",
                                    "RightY",       "LeftTrigger", "RightTrigger"};

static void pump(int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        glfwPollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main(int argc, char** argv) {
    const double seconds = (argc > 1) ? std::atof(argv[1]) : 0.0;

    if (!glfwInit()) {
        std::printf("[PROBE] FATAL glfwInit failed\n");
        return 2;
    }
    std::printf("[PROBE] GLFW %s\n", glfwGetVersionString());
    std::fflush(stdout);

    // Warm up so already-plugged joysticks are enumerated on Windows.
    pump(600);

    int firstGamepad = -1;
    int presentCount = 0;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (!glfwJoystickPresent(jid)) continue;
        ++presentCount;
        const char* jname = glfwGetJoystickName(jid);
        const char* guid = glfwGetJoystickGUID(jid);
        const int isPad = glfwJoystickIsGamepad(jid);
        const char* pname = isPad ? glfwGetGamepadName(jid) : "(unmapped joystick)";
        std::printf(
            "[PROBE] slot %d present  name=\"%s\"  guid=%s  isGamepad=%s  gamepadName=\"%s\"\n",
            jid, jname ? jname : "?", guid ? guid : "?", isPad ? "YES" : "no",
            pname ? pname : "?");
        if (isPad && firstGamepad < 0) firstGamepad = jid;
    }
    if (presentCount == 0)
        std::printf("[PROBE] no joystick present in any slot\n");
    std::fflush(stdout);

    if (firstGamepad < 0) {
        std::printf(
            "[PROBE] RESULT NO-GAMEPAD : aucune manette reconnue comme gamepad "
            "standard par GLFW.\n");
        std::printf(
            "[PROBE] (present mais isGamepad=no => mapping/pilote manquant, ex. "
            "pad virtuel non standard)\n");
        glfwTerminate();
        return 1;
    }

    const int jid = firstGamepad;
    std::printf("[PROBE] Using gamepad slot %d : \"%s\"\n", jid,
                glfwGetGamepadName(jid));
    std::fflush(stdout);

    if (seconds <= 0.0) {
        std::printf("[PROBE] RESULT RECOGNIZED : detection only (pas de capture).\n");
        glfwTerminate();
        return 0;
    }

    std::printf("[PROBE] Capture %.0f s. GO.\n", seconds);
    std::fflush(stdout);

    bool everPressed[15] = {false};
    float axisMin[6] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
    float axisMax[6] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
    unsigned char prevButtons[15] = {0};

    const auto t0 = std::chrono::steady_clock::now();
    bool lostMidway = false;
    for (;;) {
        glfwPollEvents();
        if (!glfwJoystickIsGamepad(jid)) {
            if (!lostMidway) {
                std::printf("[PROBE] WARN gamepad slot %d unmapped/disconnected mid-capture\n",
                            jid);
                std::fflush(stdout);
                lostMidway = true;
            }
        }
        GLFWgamepadstate st;
        if (glfwGetGamepadState(jid, &st) == GLFW_TRUE) {
            for (int b = 0; b < 15; ++b) {
                if (st.buttons[b] == GLFW_PRESS && prevButtons[b] == GLFW_RELEASE) {
                    everPressed[b] = true;
                    std::printf("[PROBE] button DOWN  %-11s (idx %d)\n", kButtonNames[b], b);
                    std::fflush(stdout);
                }
                prevButtons[b] = st.buttons[b];
            }
            for (int a = 0; a < 6; ++a) {
                const float v = st.axes[a];
                if (v < axisMin[a]) axisMin[a] = v;
                if (v > axisMax[a]) axisMax[a] = v;
            }
        }
        const double el =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el >= seconds) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    std::printf("\n[PROBE] ==== COVERAGE ====\n");
    for (int b = 0; b < 15; ++b)
        std::printf("[PROBE] button %-11s : %s\n", kButtonNames[b],
                    everPressed[b] ? "OK" : "--");
    for (int a = 0; a < 6; ++a)
        std::printf("[PROBE] axis   %-12s : min=%+0.2f max=%+0.2f\n", kAxisNames[a],
                    axisMin[a], axisMax[a]);

    auto seen = [&](int b) { return everPressed[b]; };
    const bool faces = seen(0) && seen(1) && seen(2) && seen(3);
    const bool bumpers = seen(4) && seen(5);
    const bool thumbs = seen(9) && seen(10);
    const bool startback = seen(6) && seen(7);
    const bool dpad = seen(11) && seen(12) && seen(13) && seen(14);
    const bool ltFull = axisMax[4] > 0.5f;
    const bool rtFull = axisMax[5] > 0.5f;
    const bool lstick = axisMin[0] < -0.7f && axisMax[0] > 0.7f && axisMin[1] < -0.7f &&
                        axisMax[1] > 0.7f;
    const bool rstick = axisMin[2] < -0.7f && axisMax[2] > 0.7f && axisMin[3] < -0.7f &&
                        axisMax[3] > 0.7f;

    std::printf(
        "\n[PROBE] faces=%d bumpers=%d LT=%d RT=%d thumbs=%d start/back=%d dpad=%d "
        "Lstick=%d Rstick=%d guide=%s\n",
        faces, bumpers, ltFull, rtFull, thumbs, startback, dpad, lstick, rstick,
        seen(8) ? "OK" : "-- (optionnel)");

    const bool full = faces && bumpers && thumbs && startback && dpad && ltFull &&
                      rtFull && lstick && rstick;
    if (full)
        std::printf(
            "[PROBE] RESULT PASS : gamepad reconnu + mapping standard complet couvert.\n");
    else
        std::printf(
            "[PROBE] RESULT PARTIAL : reconnu, couverture incomplete (relancer et "
            "couvrir les '--').\n");

    glfwTerminate();
    return full ? 0 : 3;
}
