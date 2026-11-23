#include "qr_gui.h"
#include "cursor.h"

#include <imgui.h>
#include <SDL.h>
#include <qray/qray.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{

struct CapturedUpload
{
    QrMaterial material;
    QrViewport viewport;
    QrRect2D   scissor;
    uint32_t   indexCount;
};

const ImDrawList *expectedDrawList = nullptr;
bool allowUncheckedUploads = false;
bool captureUploads = false;
std::vector<CapturedUpload> capturedUploads;
size_t nextCommand = 0;
unsigned uploadedIndices = 0;
unsigned offsetCommands = 0;
unsigned materialCreates = 0;
unsigned materialDestroys = 0;

void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void TestDrawList(unsigned frame, unsigned rectangles)
{
    Require(QR_GUI_BeginFrame(frame, 1.0f / 60.0f, 0, 0, 3840, 2160, 2160) != 0,
            "GUI frame must begin");
    ImDrawList *list = ImGui::GetForegroundDrawList();
    Require((list->Flags & ImDrawListFlags_AllowVtxOffset) != 0,
            "GUI draw lists must allow 16-bit vertex offset rollover");

    for (unsigned i = 0; i < rectangles; ++i)
    {
        if ((i % 4096) == 0)
        {
            if (i != 0)
            {
                list->PopTextureID();
                list->PopClipRect();
            }
            const float inset = (i / 4096) % 2 ? 4.0f : 0.0f;
            list->PushClipRect(ImVec2(inset, inset), ImVec2(3840.0f - inset, 2160.0f - inset));
            list->PushTextureID(static_cast<ImTextureID>(2 + (i / 4096) % 3));
        }
        const float x = 16.0f + float(i % 200) * 16.0f;
        const float y = 16.0f + float((i / 200) % 100) * 16.0f;
        const ImU32 color = IM_COL32(i % 256, (i / 256) % 256, 127, 255);
        list->AddRectFilled(ImVec2(x, y), ImVec2(x + 8.0f, y + 8.0f), color);
    }
    list->PopTextureID();
    list->PopClipRect();
    Require(list->VtxBuffer.Size == int(rectangles * 4), "all requested vertices must exist");

    expectedDrawList = list;
    nextCommand = 0;
    uploadedIndices = 0;
    offsetCommands = 0;
    QR_GUI_EndFrame();
    Require(uploadedIndices == rectangles * 6, "all triangles must reach the renderer");
    Require((offsetCommands != 0) == (rectangles * 4 >= 65536),
            "large draw lists must use nonzero vertex offsets");
    if (rectangles * 4 > 131072)
        Require(offsetCommands >= 2, "multiple vertex rollovers must be supported");
    Require(ImGui::GetDrawData()->TotalVtxCount == int(rectangles * 4),
            "rendered draw data must preserve all vertices");
    expectedDrawList = nullptr;
}

void TestNullContextGuards(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;

    Require(QR_GUI_Ready() == 0, "GUI bridge must report not ready before init / after shutdown");
    QR_GUI_Notify("guard test");
    QR_GUI_Notify(nullptr);

    float mx = 42.0f;
    float my = 99.0f;
    QR_GUI_GetMousePos(&mx, &my);
    Require(mx == 0.0f && my == 0.0f, "QR_GUI_GetMousePos must zero outputs when not ready");
    QR_GUI_GetMousePos(nullptr, nullptr);

    Require(QR_GUI_AnyItemActive() == 0, "QR_GUI_AnyItemActive must return 0 when not ready");
    Require(QR_GUI_CtrlDown() == 0, "QR_GUI_CtrlDown must return 0 when not ready");
    QR_GUI_ClearInputState();
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    Require(QR_GUI_BeginFrame(1, 1.0f / 60.0f, 0, 0, 640, 480, 480) == 0,
            "QR_GUI_BeginFrame must fail cleanly when not ready");

    // Early-return init failures must take ownership of font_data without leaking or creating materials.
    void *nullWinFont = std::malloc(128);
    Require(nullWinFont != nullptr, "test font allocation must succeed");
    std::memset(nullWinFont, 0x5A, 128);
    QR_GUI_Init(nullptr, fakeInstance, nullWinFont, 128);
    Require(QR_GUI_Ready() == 0, "QR_GUI_Init must reject NULL SDL window");

    if (window != nullptr)
    {
        void *nullInstFont = std::malloc(128);
        Require(nullInstFont != nullptr, "test font allocation must succeed");
        std::memset(nullInstFont, 0x5A, 128);
        QR_GUI_Init(window, nullptr, nullInstFont, 128);
        Require(QR_GUI_Ready() == 0, "QR_GUI_Init must reject NULL QrInstance");
    }

    Require(materialCreates == beforeCreates && materialDestroys == beforeDestroys,
            "failed QR_GUI_Init calls must not allocate or destroy materials");
}

void TestFontFallback(SDL_Window *window, size_t size, unsigned char fill, const char *label)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;

    void *buf = std::malloc(size);
    Require(buf != nullptr, "font fallback test allocation must succeed");
    std::memset(buf, fill, size);

    QR_GUI_Init(window, fakeInstance, buf, static_cast<int>(size));
    Require(QR_GUI_Ready() != 0, label);

    const ImGuiIO &io = ImGui::GetIO();
    Require(io.ConfigErrorRecoveryEnableAssert,
            "QR_GUI_Init must restore ImGui error-recovery assert setting");
    Require(io.FontDefault != nullptr, "fallback font must be assigned as io.FontDefault");
    Require(io.Fonts->Fonts.Size > 0 && io.Fonts->Fonts[0] == io.FontDefault,
            "primary 19px fallback font must be Fonts[0] ahead of overlay mono fonts");
    Require(io.FontDefault->LegacySize == 19.0f,
            "fallback default font must preserve 19px UI sizing");

    TestDrawList(1, 8);
    QR_GUI_Shutdown();
    Require(QR_GUI_Ready() == 0, "QR_GUI_Shutdown must clear ready flag");
    Require(materialCreates == beforeCreates + 1 && materialDestroys == beforeDestroys + 1,
            "font fallback cycle must keep font atlas lifetime balanced");
}

void TestMainThreadPrepareAndDeferredPlatform(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;

    QR_GUI_Init(window, fakeInstance, nullptr, 0);
    Require(QR_GUI_Ready() != 0, "GUI bridge must initialize for main-thread prepare test");

    // Duplicate init while ready must free font_data and leave state untouched.
    void *dupFont = std::malloc(128);
    Require(dupFont != nullptr, "duplicate init font allocation must succeed");
    std::memset(dupFont, 0x33, 128);
    QR_GUI_Init(window, fakeInstance, dupFont, 128);
    Require(materialCreates == beforeCreates + 1, "duplicate QR_GUI_Init must be a no-op");

    const ImGuiIO &io = ImGui::GetIO();
    Require((io.BackendFlags & ImGuiBackendFlags_HasGamepad) == 0,
            "SDL2 gamepad polling must be disabled");
    Require(ImGui::GetPlatformIO().Platform_OpenInShellFn == nullptr,
            "Platform_OpenInShellFn must be disabled for worker-thread safety");

    // Repeated idle main-thread prepares without BeginFrame must be safe.
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    TestDrawList(1, 8);

    // Deferred clipboard writes must update cache immediately and flush to SDL on the main thread.
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    Require(QR_GUI_BeginFrame(2, 1.0f / 60.0f, 0, 0, 3840, 2160, 2160) != 0,
            "clipboard regression frame must begin");
    ImGui::SetClipboardText("quakeray-deferred-clipboard");
    const char *cached = ImGui::GetClipboardText();
    Require(cached != nullptr && std::strcmp(cached, "quakeray-deferred-clipboard") == 0,
            "deferred clipboard write must be visible immediately through the cache");
    QR_GUI_EndFrame();

    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    char *sdlClip = SDL_GetClipboardText();
    Require(sdlClip != nullptr && std::strcmp(sdlClip, "quakeray-deferred-clipboard") == 0,
            "QR_GUI_PrepareMainThreadFrame must flush deferred clipboard writes to SDL");
    SDL_free(sdlClip);

    QR_GUI_Shutdown();
    Require(materialCreates == beforeCreates + 1 && materialDestroys == beforeDestroys + 1,
            "main-thread prepare test must keep font atlas lifetime balanced");
}

void TestDuplicateFrameId(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;

    QR_GUI_Init(window, fakeInstance, nullptr, 0);
    Require(QR_GUI_Ready() != 0, "GUI bridge must initialize for duplicate frame_id test");

    // Two SCR_UpdateScreen passes within the same host_framecount must both render.
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    TestDrawList(42, 8);
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    TestDrawList(42, 8);

    // A nested BeginFrame while a frame is already open must still return 0.
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    Require(QR_GUI_BeginFrame(42, 1.0f / 60.0f, 0, 0, 3840, 2160, 2160) != 0,
            "outer frame must begin");
    Require(QR_GUI_BeginFrame(42, 1.0f / 60.0f, 0, 0, 3840, 2160, 2160) == 0,
            "nested QR_GUI_BeginFrame must be rejected while frame is open");
    QR_GUI_EndFrame();

    QR_GUI_Shutdown();
    Require(materialCreates == beforeCreates + 1 && materialDestroys == beforeDestroys + 1,
            "duplicate frame_id test must keep font atlas lifetime balanced");
}

void TestMousePosCacheAndClearInputState(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;
    const Uint32 windowId = SDL_GetWindowID(window);

    QR_GUI_Init(window, fakeInstance, nullptr, 0);
    Require(QR_GUI_Ready() != 0, "GUI bridge must initialize for mouse cache / clear input test");

    // M09: ProcessEvent must update QR_GUI_GetMousePos immediately before NewFrame runs.
    SDL_Event ev;
    ev.motion = {};
    ev.motion.type = SDL_MOUSEMOTION;
    ev.motion.windowID = windowId;
    ev.motion.x = 123;
    ev.motion.y = 456;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "mouse motion must be consumed");
    float mx = 0.0f, my = 0.0f;
    QR_GUI_GetMousePos(&mx, &my);
    Require(mx == 123.0f && my == 456.0f, "QR_GUI_GetMousePos must reflect latest SDL_MOUSEMOTION immediately");

    ev.button = {};
    ev.button.type = SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_PRESSED;
    ev.button.x = 321;
    ev.button.y = 654;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "mouse button down must be consumed");
    QR_GUI_GetMousePos(&mx, &my);
    Require(mx == 321.0f && my == 654.0f, "QR_GUI_GetMousePos must reflect latest SDL_MOUSEBUTTONDOWN immediately");

    ev.button = {};
    ev.button.type = SDL_MOUSEBUTTONUP;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_RELEASED;
    ev.button.x = 111;
    ev.button.y = 222;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "mouse button up must be consumed");
    QR_GUI_GetMousePos(&mx, &my);
    Require(mx == 111.0f && my == 222.0f, "QR_GUI_GetMousePos must reflect latest SDL_MOUSEBUTTONUP immediately");

    // Clear the unpumped M09 down->up mouse events so ImGui's trickle queue
    // does not defer the M11 key/mouse presses below to a later frame.
    QR_GUI_ClearInputState();

    // M11: Press Ctrl + LMB, pump one frame, then verify QR_GUI_ClearInputState clears them.
    ev.key = {};
    ev.key.type = SDL_KEYDOWN;
    ev.key.windowID = windowId;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = SDLK_LCTRL;
    ev.key.keysym.scancode = SDL_SCANCODE_LCTRL;
    ev.key.keysym.mod = KMOD_LCTRL;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "key down must be consumed");

    ev.button = {};
    ev.button.type = SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_PRESSED;
    ev.button.x = 77;
    ev.button.y = 88;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "mouse down must be consumed");

    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    Require(QR_GUI_BeginFrame(1, 1.0f / 60.0f, 0, 0, 1280, 720, 720) != 0, "input frame must begin");
    Require(QR_GUI_CtrlDown() == 1, "Ctrl modifier must be active after SDL_KEYDOWN");
    Require(ImGui::GetIO().MouseDown[0], "LMB must be active after SDL_MOUSEBUTTONDOWN");
    QR_GUI_EndFrame();

    QR_GUI_ClearInputState();
    Require(QR_GUI_CtrlDown() == 0, "QR_GUI_ClearInputState must clear Ctrl modifier");
    Require(ImGui::GetIO().KeyMods == ImGuiMod_None, "QR_GUI_ClearInputState must clear KeyMods");
    Require(!ImGui::GetIO().MouseDown[0], "QR_GUI_ClearInputState must clear held mouse buttons");
    QR_GUI_GetMousePos(&mx, &my);
    Require(mx != 77.0f || my != 88.0f, "QR_GUI_ClearInputState must clear cached mouse position");

    // Verify cleared state persists across the next frame.
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    Require(QR_GUI_BeginFrame(2, 1.0f / 60.0f, 0, 0, 1280, 720, 720) != 0, "post-clear frame must begin");
    Require(QR_GUI_CtrlDown() == 0, "Ctrl must stay cleared on next frame");
    Require(!ImGui::GetIO().MouseDown[0], "LMB must stay cleared on next frame");
    QR_GUI_EndFrame();

    QR_GUI_Shutdown();
    Require(materialCreates == beforeCreates + 1 && materialDestroys == beforeDestroys + 1,
            "mouse cache / clear input test must keep font atlas lifetime balanced");
}

void TestColorHexMixedAndVec3Input(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;
    const Uint32 windowId = SDL_GetWindowID(window);

    QR_GUI_Init(window, fakeInstance, nullptr, 0);
    Require(QR_GUI_Ready() != 0, "GUI bridge must initialize for widget regression tests");
    allowUncheckedUploads = true;

    unsigned frame = 1;
    auto runPanelFrame = [&](auto &&drawFn) {
        QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
        Require(QR_GUI_BeginFrame(frame++, 1.0f / 60.0f, 0, 0, 1280, 720, 720) != 0, "widget frame must begin");
        QR_GUI_BeginPanel("widget_test", 100, 100, 520, 420);
        drawFn();
        QR_GUI_EndPanel();
        QR_GUI_EndFrame();
    };

    // --- H20: QR_GUI_ColorHexMixed NULL enabled + partial 5-char hex must not commit ---
    float rgbNull[3] = { 0.25f, 0.50f, 0.75f };
    int changed = 0;

    // Frame 1 & 2: submit focus request for ##color (offset 0 when enabled == nullptr), then activate it
    runPanelFrame([&] {
        ImGui::SetKeyboardFocusHere(0);
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    Require(changed == 0, "focusing ColorHexMixed with NULL enabled must not report change");
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    Require(changed == 0 && QR_GUI_AnyItemActive() != 0,
            "ColorHexMixed ##color input must become active after SetKeyboardFocusHere");

    // Frame 3: type 5 hex digits without Enter -> must not commit
    ImGui::GetIO().AddInputCharactersUTF8("FF880");
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    Require(changed == 0, "5-char partial hex input must not commit while typing");
    Require(rgbNull[0] == 0.25f && rgbNull[1] == 0.50f && rgbNull[2] == 0.75f,
            "partial hex input must leave rgb untouched");

    // Frame 4: press Enter with only 5 hex digits -> must still reject
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    Require(changed == 0, "pressing Enter on 5-char hex must not commit");
    Require(rgbNull[0] == 0.25f && rgbNull[1] == 0.50f && rgbNull[2] == 0.75f,
            "invalid 5-char hex on Enter must leave rgb untouched");

    // Refocus, activate, type valid 6-digit hex with '#', then press Enter -> must commit with NULL enabled
    runPanelFrame([&] {
        ImGui::SetKeyboardFocusHere(0);
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    Require(QR_GUI_AnyItemActive() != 0, "ColorHexMixed ##color must reactivate for valid hex test");
    ImGui::GetIO().AddInputCharactersUTF8("#112233");
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    Require(changed == 0, "valid 6-digit hex must wait for Enter or deactivation before committing");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_null", rgbNull, nullptr, 1, "tip");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    Require(changed == 1, "valid 6-digit hex with Enter must commit even with NULL enabled");
    Require(std::fabs(rgbNull[0] - float(0x11) / 255.0f) < 1e-5f &&
            std::fabs(rgbNull[1] - float(0x22) / 255.0f) < 1e-5f &&
            std::fabs(rgbNull[2] - float(0x33) / 255.0f) < 1e-5f,
            "committed hex color channels must match #112233");

    // Non-NULL enabled: focus ##color (offset 1 past ##enabled checkbox), type 6-digit hex, commit on click-outside blur
    float rgbEn[3] = { 0.1f, 0.2f, 0.3f };
    int enabledFlag = 0;
    runPanelFrame([&] {
        ImGui::SetKeyboardFocusHere(1); // 0 = checkbox, 1 = ##color input
        changed = QR_GUI_ColorHexMixed("hex_en", rgbEn, &enabledFlag, 1, "tip");
    });
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_en", rgbEn, &enabledFlag, 1, "tip");
    });
    Require(changed == 0 && QR_GUI_AnyItemActive() != 0, "ColorHexMixed ##color with enabled must activate");
    ImGui::GetIO().AddInputCharactersUTF8("445566");
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_en", rgbEn, &enabledFlag, 1, "tip");
    });
    Require(changed == 0 && enabledFlag == 0, "6-digit hex must not commit before Enter or blur");

    SDL_Event clickOut;
    clickOut.motion = {};
    clickOut.motion.type = SDL_MOUSEMOTION;
    clickOut.motion.windowID = windowId;
    clickOut.motion.x = 2;
    clickOut.motion.y = 2;
    QR_GUI_ProcessEvent(&clickOut);
    clickOut.button = {};
    clickOut.button.type = SDL_MOUSEBUTTONDOWN;
    clickOut.button.windowID = windowId;
    clickOut.button.button = SDL_BUTTON_LEFT;
    clickOut.button.state = SDL_PRESSED;
    clickOut.button.x = 2;
    clickOut.button.y = 2;
    QR_GUI_ProcessEvent(&clickOut);
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_en", rgbEn, &enabledFlag, 1, "tip");
    });
    clickOut.button.type = SDL_MOUSEBUTTONUP;
    clickOut.button.state = SDL_RELEASED;
    QR_GUI_ProcessEvent(&clickOut);
    Require(changed == 1 && enabledFlag == 1, "valid 6-digit hex must commit on deactivation and set *enabled = 1");
    Require(std::fabs(rgbEn[0] - float(0x44) / 255.0f) < 1e-5f &&
            std::fabs(rgbEn[1] - float(0x55) / 255.0f) < 1e-5f &&
            std::fabs(rgbEn[2] - float(0x66) / 255.0f) < 1e-5f,
            "deactivation-committed hex color channels must match 445566");

    QR_GUI_ClearInputState();

    // --- M10: QR_GUI_Vec3Input & QR_GUI_Vec3InputMixed untouched out-of-range axes & bitmask ---
    float vOut[3] = { 500.0f, -999.0f, 42.0f };
    const unsigned char noMixed[3] = { 0, 0, 0 };
    const unsigned char yMixed[3] = { 0, 1, 0 };
    int maskPlain = -1, maskUnmixed = -1, maskMixed = -1;
    runPanelFrame([&] {
        maskPlain = QR_GUI_Vec3Input("v_plain", vOut, -128.0f, 128.0f, "vec tip");
        maskUnmixed = QR_GUI_Vec3InputMixed("v_unmixed", vOut, -128.0f, 128.0f, noMixed, "vec tip");
        maskMixed = QR_GUI_Vec3InputMixed("v_mixed", vOut, -128.0f, 128.0f, yMixed, "vec tip");
    });
    Require(maskPlain == 0 && maskUnmixed == 0 && maskMixed == 0,
            "unedited Vec3Input / Vec3InputMixed must return 0 even with out-of-range values");
    Require(vOut[0] == 500.0f && vOut[1] == -999.0f && vOut[2] == 42.0f,
            "unedited out-of-range axes must not be clamped");

    // Edit ONLY Y axis in Vec3InputMixed -> returns bit 2 (1 << 1), clamps Y, preserves out-of-range X and Z
    float vEditY[3] = { 500.0f, 10.0f, -999.0f };
    runPanelFrame([&] {
        ImGui::SetKeyboardFocusHere(1); // 0 = X, 1 = Y, 2 = Z
        maskMixed = QR_GUI_Vec3InputMixed("v_edit_y", vEditY, -128.0f, 128.0f, yMixed, "vec tip");
    });
    runPanelFrame([&] {
        maskMixed = QR_GUI_Vec3InputMixed("v_edit_y", vEditY, -128.0f, 128.0f, yMixed, "vec tip");
    });
    Require(maskMixed == 0 && QR_GUI_AnyItemActive() != 0, "Vec3InputMixed Y axis must activate");
    ImGui::GetIO().AddInputCharactersUTF8("250");
    runPanelFrame([&] {
        maskMixed |= QR_GUI_Vec3InputMixed("v_edit_y", vEditY, -128.0f, 128.0f, yMixed, "vec tip");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    runPanelFrame([&] {
        maskMixed |= QR_GUI_Vec3InputMixed("v_edit_y", vEditY, -128.0f, 128.0f, yMixed, "vec tip");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    Require(maskMixed == (1 << 1), "editing only Y in Vec3InputMixed must return bitmask 1 << 1");
    Require(vEditY[0] == 500.0f && vEditY[1] == 128.0f && vEditY[2] == -999.0f,
            "only edited Y axis must be clamped; out-of-range X and Z must stay untouched");

    // Edit ONLY Z axis in QR_GUI_Vec3Input -> returns bit 4 (1 << 2), clamps Z, preserves out-of-range X
    float vEditZ[3] = { 500.0f, 10.0f, 0.0f };
    runPanelFrame([&] {
        ImGui::SetKeyboardFocusHere(2); // 0 = X, 1 = Y, 2 = Z
        maskPlain = QR_GUI_Vec3Input("v_edit_z", vEditZ, -128.0f, 128.0f, "vec tip");
    });
    runPanelFrame([&] {
        maskPlain = QR_GUI_Vec3Input("v_edit_z", vEditZ, -128.0f, 128.0f, "vec tip");
    });
    Require(maskPlain == 0 && QR_GUI_AnyItemActive() != 0, "Vec3Input Z axis must activate");
    ImGui::GetIO().AddInputCharactersUTF8("-300");
    runPanelFrame([&] {
        maskPlain |= QR_GUI_Vec3Input("v_edit_z", vEditZ, -128.0f, 128.0f, "vec tip");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    runPanelFrame([&] {
        maskPlain |= QR_GUI_Vec3Input("v_edit_z", vEditZ, -128.0f, 128.0f, "vec tip");
    });
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    Require(maskPlain == (1 << 2), "editing only Z in Vec3Input must return bitmask 1 << 2");
    Require(vEditZ[0] == 500.0f && vEditZ[1] == 10.0f && vEditZ[2] == -128.0f,
            "only edited Z axis must be clamped; out-of-range X must stay untouched");

    allowUncheckedUploads = false;
    QR_GUI_Shutdown();
    Require(materialCreates == beforeCreates + 1 && materialDestroys == beforeDestroys + 1,
            "widget regression test must keep font atlas lifetime balanced");
}

void TestScissorAndDrawCircleGuards(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;

    QR_GUI_Init(window, fakeInstance, nullptr, 0);
    Require(QR_GUI_Ready() != 0, "GUI bridge must initialize for L12 scissor / DrawCircle test");

    allowUncheckedUploads = true;
    captureUploads = true;
    capturedUploads.clear();

    // Sub-rect viewport exercising Vulkan Y-flip: area_x0 = 100, area_y0 = 1000 - (50 + 600) = 350.
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    Require(QR_GUI_BeginFrame(1, 1.0f / 60.0f, 100, 50, 800, 600, 1000) != 0,
            "scissor test frame 1 must begin");

    ImDrawList *fg = ImGui::GetForegroundDrawList();
    // 1. Valid fractional clip rect -> scissor {110, 370, 101, 101}
    fg->PushClipRect(ImVec2(10.25f, 20.75f), ImVec2(110.25f, 120.75f));
    fg->PushTextureID(static_cast<ImTextureID>(2));
    fg->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(200.0f, 200.0f), IM_COL32(255, 255, 255, 255));
    fg->PopTextureID();
    fg->PopClipRect();

    // 2. Fractional empty width (30.5..30.5) -> must be skipped, not leaked as a 1px strip
    fg->PushClipRect(ImVec2(30.5f, 40.5f), ImVec2(30.5f, 90.0f));
    fg->PushTextureID(static_cast<ImTextureID>(3));
    fg->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(200.0f, 200.0f), IM_COL32(255, 0, 0, 255));
    fg->PopTextureID();
    fg->PopClipRect();

    // 3. Fractional empty height (60.5..60.5) -> must be skipped
    fg->PushClipRect(ImVec2(10.0f, 60.5f), ImVec2(80.0f, 60.5f));
    fg->PushTextureID(static_cast<ImTextureID>(4));
    fg->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(200.0f, 200.0f), IM_COL32(0, 255, 0, 255));
    fg->PopTextureID();
    fg->PopClipRect();

    // 4. Explicitly inverted fractional clip rect (50.6..50.4) -> must be skipped
    fg->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f));
    fg->PushTextureID(static_cast<ImTextureID>(5));
    fg->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), IM_COL32(0, 0, 255, 255));
    fg->CmdBuffer.back().ClipRect = ImVec4(50.6f, 40.5f, 50.4f, 90.0f);
    fg->PopTextureID();
    fg->PopClipRect();

    // Non-finite and non-positive QR_GUI_DrawCircle inputs must emit zero geometry.
    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    const float infVal = std::numeric_limits<float>::infinity();
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    QR_GUI_DrawCircle(nanVal, 100.0f, 10.0f, 0xFFFFFFFFu, 2.0f);
    QR_GUI_DrawCircle(100.0f, nanVal, 10.0f, 0xFFFFFFFFu, 2.0f);
    QR_GUI_DrawCircle(100.0f, 100.0f, nanVal, 0xFFFFFFFFu, 2.0f);
    QR_GUI_DrawCircle(100.0f, 100.0f, infVal, 0xFFFFFFFFu, 2.0f);
    QR_GUI_DrawCircle(100.0f, 100.0f, 0.0f, 0xFFFFFFFFu, 2.0f);
    QR_GUI_DrawCircle(100.0f, 100.0f, -5.0f, 0xFFFFFFFFu, 2.0f);
    Require(bg->VtxBuffer.Size == 0, "QR_GUI_DrawCircle must ignore NaN, Inf, and non-positive radii");

    QR_GUI_DrawCircle(200.0f, 150.0f, 20.0f, 0xFFFFFFFFu, 2.0f);
    Require(bg->VtxBuffer.Size > 0, "valid QR_GUI_DrawCircle must emit geometry");

    QR_GUI_EndFrame();

    bool foundValidFg = false;
    for (const CapturedUpload &up : capturedUploads)
    {
        Require(up.material != 3 && up.material != 4 && up.material != 5,
                "empty and inverted fractional clip rects must not upload any scissor command");
        if (up.material == 2)
        {
            foundValidFg = true;
            Require(up.viewport.x == 100.0f && up.viewport.y == 350.0f &&
                    up.viewport.width == 800.0f && up.viewport.height == 600.0f,
                    "Vulkan viewport Y-flip coordinates must match drawable height");
            Require(up.scissor.x == 110 && up.scissor.y == 370 &&
                    up.scissor.width == 101 && up.scissor.height == 101,
                    "fractional scissor coordinates must match floor/ceil bounds in flipped drawable space");
        }
    }
    Require(foundValidFg, "valid fractional clip rect must upload its draw command");

    // Negative drawable origin (area_x0 = -20, area_y0 = -200) must clamp scissor x/y >= 0.
    capturedUploads.clear();
    QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
    Require(QR_GUI_BeginFrame(2, 1.0f / 60.0f, -20, 900, 400, 300, 1000) != 0,
            "negative origin scissor test frame must begin");
    fg = ImGui::GetForegroundDrawList();
    fg->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(400.0f, 300.0f));
    fg->PushTextureID(static_cast<ImTextureID>(2));
    fg->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(400.0f, 300.0f), IM_COL32(255, 255, 255, 255));
    fg->PopTextureID();
    fg->PopClipRect();
    QR_GUI_EndFrame();

    Require(capturedUploads.size() == 1, "clamped negative-origin frame must emit one command");
    Require(capturedUploads[0].scissor.x == 0 && capturedUploads[0].scissor.y == 0 &&
            capturedUploads[0].scissor.width == 380 && capturedUploads[0].scissor.height == 100,
            "scissor offset must clamp to >= 0 when viewport area starts negative");

    captureUploads = false;
    capturedUploads.clear();
    allowUncheckedUploads = false;
    QR_GUI_Shutdown();
    Require(materialCreates == beforeCreates + 1 && materialDestroys == beforeDestroys + 1,
            "scissor / DrawCircle test must keep font atlas lifetime balanced");
}

void TestOverlayReinitLifecycle(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;
    const float samples[4] = { 1.0f, 2.5f, 4.0f, 3.0f };

    allowUncheckedUploads = true;
    captureUploads = true;

    for (int cycle = 0; cycle < 2; ++cycle)
    {
        QR_GUI_Init(window, fakeInstance, nullptr, 0);
        Require(QR_GUI_Ready() != 0, "GUI bridge must initialize on overlay lifecycle cycle");

        // QR_GUI_OverlayBegin uses ImGuiWindowFlags_AlwaysAutoResize, so ImGui
        // measures auto-fit bounds on frame 1 and emits draw commands on frame 2.
        for (unsigned frame = 1; frame <= 2; ++frame)
        {
            capturedUploads.clear();
            QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
            Require(QR_GUI_BeginFrame(frame, 1.0f / 60.0f, 0, 0, 1920, 1080, 1080) != 0,
                    "overlay frame must begin");
            QR_GUI_OverlayBegin("##rt_stats_test", 12.0f, 12.0f, 0.7f, "Stats", 2);
            QR_GUI_OverlaySection("CPU");
            QR_GUI_OverlayRow("Frame", "4.0 ms", samples, 4, 0xFF80FF80u);
            QR_GUI_OverlayBudgetRow("Draw", "2.5 ms", samples, 4, 8.0f, 16.0f);
            QR_GUI_OverlayNote("reinit ok");
            QR_GUI_OverlayEnd();
            QR_GUI_EndFrame();
        }

        Require(!capturedUploads.empty(), "overlay draw must upload geometry on every init cycle");
        QR_GUI_Shutdown();
        Require(QR_GUI_Ready() == 0, "QR_GUI_Shutdown must clear ready flag between overlay cycles");
    }

    captureUploads = false;
    capturedUploads.clear();
    allowUncheckedUploads = false;
    Require(materialCreates == beforeCreates + 2 && materialDestroys == beforeDestroys + 2,
            "Shutdown -> Init -> Overlay lifecycle must balance font atlas materials across cycles");
}

void TestImagePickAndPolygonEdit(SDL_Window *window)
{
    void *const fakeInstance = reinterpret_cast<void *>(uintptr_t(1));
    const unsigned beforeCreates = materialCreates;
    const unsigned beforeDestroys = materialDestroys;
    const Uint32 windowId = SDL_GetWindowID(window);

    QR_GUI_Init(window, fakeInstance, nullptr, 0);
    Require(QR_GUI_Ready() != 0, "GUI bridge must initialize for ImagePick / PolygonEdit tests");
    allowUncheckedUploads = true;

    auto sendMove = [&](int x, int y) {
        SDL_Event ev;
        ev.motion = {};
        ev.motion.type = SDL_MOUSEMOTION;
        ev.motion.windowID = windowId;
        ev.motion.x = x;
        ev.motion.y = y;
        QR_GUI_ProcessEvent(&ev);
    };
    auto sendButton = [&](bool down, int x, int y) {
        SDL_Event ev;
        ev.button = {};
        ev.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        ev.button.windowID = windowId;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        ev.button.x = x;
        ev.button.y = y;
        QR_GUI_ProcessEvent(&ev);
    };
    auto sendModKey = [&](SDL_Keycode sym, SDL_Scancode scancode, Uint16 mod, bool down) {
        SDL_Event ev;
        ev.key = {};
        ev.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.windowID = windowId;
        ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        ev.key.keysym.sym = sym;
        ev.key.keysym.scancode = scancode;
        ev.key.keysym.mod = down ? mod : 0;
        QR_GUI_ProcessEvent(&ev);
    };
    auto sendWheel = [&](int y) {
        SDL_Event ev;
        ev.wheel = {};
        ev.wheel.type = SDL_MOUSEWHEEL;
        ev.wheel.windowID = windowId;
        ev.wheel.y = y;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        ev.wheel.preciseY = static_cast<float>(y);
#endif
        QR_GUI_ProcessEvent(&ev);
    };

    unsigned frame = 1;
    auto runPanel = [&](auto &&drawFn, bool drawBlocker, ImVec2 blockerPos, ImVec2 blockerSize) {
        QR_GUI_PrepareMainThreadFrame(1.0f / 60.0f);
        Require(QR_GUI_BeginFrame(frame++, 1.0f / 60.0f, 0, 0, 1280, 720, 720) != 0,
                "ImagePick/PolygonEdit frame must begin");
        QR_GUI_BeginPanel("preview_test", 100, 100, 500, 560);
        drawFn();
        QR_GUI_EndPanel();
        if (drawBlocker)
        {
            ImGui::SetNextWindowPos(blockerPos);
            ImGui::SetNextWindowSize(blockerSize);
            ImGui::Begin("popup_blocker", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::End();
        }
        QR_GUI_EndFrame();
    };

    // --- L11a: QR_GUI_ImagePick ActiveID, external drag rejection, and popup blocking zoom ---
    float u = -1.0f, v = -1.0f;
    int picked = 0;
    ImVec2 imgMin(0.0f, 0.0f), imgMax(0.0f, 0.0f);
    runPanel([&] {
        picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v);
        imgMin = ImGui::GetItemRectMin();
        imgMax = ImGui::GetItemRectMax();
    }, false, ImVec2(0, 0), ImVec2(0, 0));

    const float imgW = imgMax.x - imgMin.x;
    const float imgH = imgMax.y - imgMin.y;
    Require(imgW > 100.0f && imgH > 100.0f, "ImagePick must occupy expected panel width");
    const int cx = static_cast<int>(std::lround(imgMin.x + imgW * 0.5f));
    const int cy = static_cast<int>(std::lround(imgMin.y + imgH * 0.5f));
    const int q25x = static_cast<int>(std::lround(imgMin.x + imgW * 0.25f));
    const int q25y = static_cast<int>(std::lround(imgMin.y + imgH * 0.25f));
    const int q75x = static_cast<int>(std::lround(imgMin.x + imgW * 0.75f));

    // Drag originating outside preview into preview must NOT pick.
    sendMove(10, 10);
    sendButton(true, 10, 10);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    Require(picked == 0, "LMB pressed outside ImagePick must not pick");
    sendMove(cx, cy);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    Require(picked == 0, "dragging into ImagePick from outside must not overwrite color");
    QR_GUI_ClearInputState();

    // Clicking and dragging inside ImagePick must acquire ActiveID and update UV.
    sendMove(cx, cy);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    Require(picked == 0 && QR_GUI_AnyItemActive() == 0, "hovering ImagePick must not activate it");
    sendButton(true, cx, cy);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    Require(picked == 1 && QR_GUI_AnyItemActive() == 1,
            "clicking inside ImagePick must return 1 and acquire ImGui ActiveID");
    Require(std::fabs(u - 0.5f) < 0.02f && std::fabs(v - 0.5f) < 0.02f,
            "center click must sample UV near (0.5, 0.5)");
    sendMove(q75x, q25y);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    Require(picked == 1 && QR_GUI_AnyItemActive() == 1,
            "dragging inside ImagePick must maintain ActiveID");
    Require(std::fabs(u - 0.75f) < 0.02f && std::fabs(v - 0.25f) < 0.02f,
            "drag must track updated UV coordinates");
    QR_GUI_ClearInputState();

    // Overlying window/popup over ImagePick must block Ctrl+wheel zoom.
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             true, imgMin, ImVec2(imgW, imgH));
    sendMove(cx, cy);
    sendModKey(SDLK_LCTRL, SDL_SCANCODE_LCTRL, KMOD_LCTRL, true);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             true, imgMin, ImVec2(imgW, imgH));
    sendWheel(3);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             true, imgMin, ImVec2(imgW, imgH));
    QR_GUI_ClearInputState();
    // Let ImGui retire the closed popup_blocker window before sampling unblocked clicks.
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));

    // Verify zoom stayed 1.0f after blocked Ctrl+wheel by sampling the 25% screen position.
    sendMove(q25x, q25y);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    sendButton(true, q25x, q25y);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    Require(picked == 1 && std::fabs(u - 0.25f) < 0.02f && std::fabs(v - 0.25f) < 0.02f,
            "Ctrl+wheel over an overlying popup must not zoom ImagePick");
    QR_GUI_ClearInputState();

    // Verify unblocked Ctrl+wheel DOES zoom toward center.
    sendMove(cx, cy);
    sendModKey(SDLK_LCTRL, SDL_SCANCODE_LCTRL, KMOD_LCTRL, true);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    sendWheel(3);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    QR_GUI_ClearInputState();

    sendMove(q25x, q25y);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    sendButton(true, q25x, q25y);
    runPanel([&] { picked = QR_GUI_ImagePick("##img_test", 2, 256, 256, &u, &v); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    Require(picked == 1 && u > 0.35f && u < 0.49f,
            "unblocked Ctrl+wheel over ImagePick must zoom around cursor");
    QR_GUI_ClearInputState();

    // --- L11b: QR_GUI_PolygonEdit aspect-corrected Shift+drag edge normal on 4:1 texture ---
    // Edge 0: (0.30, 0.20) -> (0.45, 0.80); on 256x64 (w = 4*h), pixel edge is (0.6*h, 0.6*h) (exact 45 deg).
    float polyUv[3][2] = { { 0.30f, 0.20f }, { 0.45f, 0.80f }, { 0.85f, 0.50f } };
    int polyCount = 3;
    int polyChanged = 0;
    ImVec2 pMin(0.0f, 0.0f), pMax(0.0f, 0.0f);
    runPanel([&] {
        polyChanged = QR_GUI_PolygonEdit("##poly_test", 2, 256, 64, polyUv, &polyCount, 16);
        pMin = ImGui::GetItemRectMin();
        pMax = ImGui::GetItemRectMax();
    }, false, ImVec2(0, 0), ImVec2(0, 0));

    const float pw = pMax.x - pMin.x;
    const float ph = pMax.y - pMin.y;
    Require(pw > 200.0f && std::fabs(pw / ph - 4.0f) < 0.05f,
            "PolygonEdit preview must preserve 4:1 texture aspect ratio");

    const int midX = static_cast<int>(std::lround(pMin.x + 0.375f * pw));
    const int midY = static_cast<int>(std::lround(pMin.y + 0.50f * ph));

    // Hover edge 0 midpoint with Shift held, press LMB to begin normal-constrained drag, then move (-10, +10) px.
    sendMove(midX, midY);
    sendModKey(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, KMOD_LSHIFT, true);
    runPanel([&] { polyChanged = QR_GUI_PolygonEdit("##poly_test", 2, 256, 64, polyUv, &polyCount, 16); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    sendButton(true, midX, midY);
    runPanel([&] { polyChanged = QR_GUI_PolygonEdit("##poly_test", 2, 256, 64, polyUv, &polyCount, 16); },
             false, ImVec2(0, 0), ImVec2(0, 0));
    sendMove(midX - 10, midY + 10);
    runPanel([&] { polyChanged = QR_GUI_PolygonEdit("##poly_test", 2, 256, 64, polyUv, &polyCount, 16); },
             false, ImVec2(0, 0), ImVec2(0, 0));

    Require(polyChanged == 1, "Shift+drag along edge normal must update polygon edge");
    const float dx0 = (polyUv[0][0] - 0.30f) * pw;
    const float dy0 = (polyUv[0][1] - 0.20f) * ph;
    const float dx1 = (polyUv[1][0] - 0.45f) * pw;
    const float dy1 = (polyUv[1][1] - 0.80f) * ph;
    Require(std::fabs(dx0 - (-10.0f)) < 1.0f && std::fabs(dy0 - 10.0f) < 1.0f,
            "Shift+drag vertex 0 must move strictly along pixel-space normal (-10, +10)");
    Require(std::fabs(dx1 - (-10.0f)) < 1.0f && std::fabs(dy1 - 10.0f) < 1.0f,
            "Shift+drag vertex 1 must move strictly along pixel-space normal (-10, +10)");
    const float edgePxX = 0.15f * pw;
    const float edgePxY = 0.60f * ph;
    Require(std::fabs(dx0 * edgePxX + dy0 * edgePxY) < 1.0f,
            "Shift+drag displacement must be orthogonal to edge in pixel space");

    QR_GUI_ClearInputState();
    allowUncheckedUploads = false;
    QR_GUI_Shutdown();
    Require(materialCreates == beforeCreates + 1 && materialDestroys == beforeDestroys + 1,
            "ImagePick / PolygonEdit test must keep font atlas lifetime balanced");
}

}

QrResult QRCONV qrCreateMaterial(QrInstance, const QrMaterialCreateInfo *info, QrMaterial *material)
{
    Require(info != nullptr && material != nullptr, "font material arguments must be valid");
    Require(info->size.width > 0 && info->size.height > 0, "font atlas must be built");
    *material = 1;
    ++materialCreates;
    return QR_SUCCESS;
}

QrResult QRCONV qrDestroyMaterial(QrInstance, QrMaterial material)
{
    Require(material == 1, "only the font atlas material is owned by the GUI");
    ++materialDestroys;
    return QR_SUCCESS;
}

int Cursor_GetGuiCursor(int64_t *, int *, int *, int *)
{
    return 0;
}

QrResult QRCONV qrUploadRasterizedGeometry(QrInstance, const QrRasterizedGeometryUploadInfo *info,
                                         const float *viewProjection, const QrViewport *viewport)
{
    if (allowUncheckedUploads && expectedDrawList == nullptr)
    {
        Require(info != nullptr && viewProjection != nullptr && viewport != nullptr, "upload arguments must be valid");
        if (captureUploads)
            capturedUploads.push_back({ info->material, *viewport, info->scissor, info->indexCount });
        return QR_SUCCESS;
    }
    Require(expectedDrawList != nullptr, "unexpected geometry upload");
    const ImDrawList &list = *expectedDrawList;
    while (nextCommand < size_t(list.CmdBuffer.Size) && list.CmdBuffer[int(nextCommand)].ElemCount == 0)
        ++nextCommand;
    Require(nextCommand < size_t(list.CmdBuffer.Size), "unexpected extra draw command");
    const ImDrawCmd &command = list.CmdBuffer[int(nextCommand++)];
    Require(info != nullptr && viewProjection != nullptr && viewport != nullptr, "upload arguments must be valid");
    Require(info->renderType == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN, "GUI uses the swapchain draw list");
    Require(info->material == static_cast<QrMaterial>(command.GetTexID()), "command material must be preserved");
    Require(info->indexCount == command.ElemCount, "command index count must be preserved");
    Require(command.VtxOffset < unsigned(list.VtxBuffer.Size), "vertex offset must belong to the list");
    Require(info->vertexCount > 0 && info->vertexCount <= unsigned(list.VtxBuffer.Size) - command.VtxOffset,
            "uploaded vertex span must stay inside the draw list");
    Require(info->scissor.width > 0 && info->scissor.height > 0, "visible draw command must have a scissor");
    unsigned minIndex = UINT_MAX, maxIndex = 0;
    for (unsigned i = 0; i < command.ElemCount; ++i)
    {
        const unsigned index = unsigned(list.IdxBuffer[int(command.IdxOffset + i)]);
        minIndex = std::min(minIndex, index);
        maxIndex = std::max(maxIndex, index);
    }
    Require(minIndex != UINT_MAX, "a visible command must reference at least one vertex");
    Require(info->vertexCount == maxIndex - minIndex + 1,
            "uploaded vertex count must cover the command's compact local span");
    for (unsigned i = 0; i < command.ElemCount; ++i)
    {
        const unsigned index = unsigned(list.IdxBuffer[int(command.IdxOffset + i)]);
        const unsigned local = index - minIndex;
        Require(static_cast<const uint32_t *>(info->pIndices)[i] == local,
                "uploaded indices must be rebased to the compact vertex span");
        Require(local < info->vertexCount, "every index must belong to the uploaded vertex span");
        const ImDrawVert &source = list.VtxBuffer[int(command.VtxOffset + index)];
        const QrVertex &vertex = info->pVertices[local];
        Require(vertex.position[0] == source.pos.x && vertex.position[1] == source.pos.y,
                "vertex offset must select the original vertex position");
        Require(vertex.texCoord[0] == source.uv.x && vertex.texCoord[1] == source.uv.y,
                "vertex UVs must be preserved across rollover");
        Require(vertex.packedColor == source.col, "vertex color must be preserved across rollover");
    }
    uploadedIndices += command.ElemCount;
    if (command.VtxOffset != 0)
        ++offsetCommands;
    return QR_SUCCESS;
}

int main()
{
    SDL_Window *window = nullptr;
    try
    {
        Require(SDL_setenv("SDL_VIDEODRIVER", "dummy", 1) == 0, "headless SDL driver must be configured");
        SDL_SetMainReady();
        Require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0, SDL_GetError());
        window = SDL_CreateWindow("GUI draw regression", 0, 0, 3840, 2160, SDL_WINDOW_HIDDEN);
        Require(window != nullptr, SDL_GetError());

        TestNullContextGuards(window);

        QR_GUI_Init(window, reinterpret_cast<void *>(uintptr_t(1)), nullptr, 0);
        Require(QR_GUI_Ready() != 0, "real GUI bridge must initialize");
        Require(sizeof(ImDrawIdx) == 2, "regression must exercise 16-bit ImGui indices");
        Require((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0,
                "real GUI initialization must advertise vertex offset support");
        TestDrawList(1, 8);
        TestDrawList(2, 16383);
        TestDrawList(3, 16384);
        TestDrawList(4, 18000);
        TestDrawList(5, 40000);
        QR_GUI_Shutdown();
        Require(materialCreates == 1 && materialDestroys == 1, "font atlas lifetime must remain balanced");

        TestNullContextGuards(window);
        TestFontFallback(window, 16, 0xAB, "short TTF (<= 100 bytes) must fall back to default 19px font");
        TestFontFallback(window, 256, 0xDE, "corrupt TTF (> 100 bytes) must fall back without assert or UAF");
        TestMainThreadPrepareAndDeferredPlatform(window);
        TestDuplicateFrameId(window);
        TestMousePosCacheAndClearInputState(window);
        TestColorHexMixedAndVec3Input(window);
        TestScissorAndDrawCircleGuards(window);
        TestOverlayReinitLifecycle(window);
        TestImagePickAndPolygonEdit(window);
        TestNullContextGuards(window);

        SDL_DestroyWindow(window);
        SDL_Quit();
        std::cout << "GUI bridge tests passed: 16-bit rollover, null-context guards, font fallback, main-thread SDL prep, M09-M12, H20, and L02/L11/L12\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        captureUploads = false;
        allowUncheckedUploads = false;
        QR_GUI_Shutdown();
        if (window != nullptr)
            SDL_DestroyWindow(window);
        SDL_Quit();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
