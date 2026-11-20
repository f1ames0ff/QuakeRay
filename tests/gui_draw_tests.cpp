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
#include <stdexcept>

namespace
{

const ImDrawList *expectedDrawList = nullptr;
bool allowUncheckedUploads = false;
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
    SDL_Event ev = {};
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = windowId;
    ev.motion.x = 123;
    ev.motion.y = 456;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "mouse motion must be consumed");
    float mx = 0.0f, my = 0.0f;
    QR_GUI_GetMousePos(&mx, &my);
    Require(mx == 123.0f && my == 456.0f, "QR_GUI_GetMousePos must reflect latest SDL_MOUSEMOTION immediately");

    ev = {};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = windowId;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = SDL_PRESSED;
    ev.button.x = 321;
    ev.button.y = 654;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "mouse button down must be consumed");
    QR_GUI_GetMousePos(&mx, &my);
    Require(mx == 321.0f && my == 654.0f, "QR_GUI_GetMousePos must reflect latest SDL_MOUSEBUTTONDOWN immediately");

    ev = {};
    ev.type = SDL_MOUSEBUTTONUP;
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
    ev = {};
    ev.type = SDL_KEYDOWN;
    ev.key.windowID = windowId;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = SDLK_LCTRL;
    ev.key.keysym.scancode = SDL_SCANCODE_LCTRL;
    ev.key.keysym.mod = KMOD_LCTRL;
    Require(QR_GUI_ProcessEvent(&ev) == 1, "key down must be consumed");

    ev = {};
    ev.type = SDL_MOUSEBUTTONDOWN;
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

    SDL_Event clickOut = {};
    clickOut.type = SDL_MOUSEMOTION;
    clickOut.motion.windowID = windowId;
    clickOut.motion.x = 2;
    clickOut.motion.y = 2;
    QR_GUI_ProcessEvent(&clickOut);
    clickOut = {};
    clickOut.type = SDL_MOUSEBUTTONDOWN;
    clickOut.button.windowID = windowId;
    clickOut.button.button = SDL_BUTTON_LEFT;
    clickOut.button.state = SDL_PRESSED;
    clickOut.button.x = 2;
    clickOut.button.y = 2;
    QR_GUI_ProcessEvent(&clickOut);
    runPanelFrame([&] {
        changed = QR_GUI_ColorHexMixed("hex_en", rgbEn, &enabledFlag, 1, "tip");
    });
    clickOut.type = SDL_MOUSEBUTTONUP;
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
        TestNullContextGuards(window);

        SDL_DestroyWindow(window);
        SDL_Quit();
        std::cout << "GUI bridge tests passed: 16-bit rollover, null-context guards, font fallback, main-thread SDL prep, M09-M12, and H20\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        allowUncheckedUploads = false;
        QR_GUI_Shutdown();
        if (window != nullptr)
            SDL_DestroyWindow(window);
        SDL_Quit();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
