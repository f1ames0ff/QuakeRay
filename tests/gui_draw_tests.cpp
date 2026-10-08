#include "qr_gui.h"
#include "cursor.h"

#include <imgui.h>
#include <SDL.h>
#include <qray/qray.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{

const ImDrawList *expectedDrawList = nullptr;
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
    unsigned maxIndex = 0;
    for (unsigned i = 0; i < command.ElemCount; ++i)
    {
        const unsigned index = unsigned(list.IdxBuffer[int(command.IdxOffset + i)]);
        Require(static_cast<const uint32_t *>(info->pIndices)[i] == index,
                "ImGui indices are already local to VtxOffset");
        Require(index < info->vertexCount, "every index must belong to the uploaded vertex span");
        const ImDrawVert &source = list.VtxBuffer[int(command.VtxOffset + index)];
        const QrVertex &vertex = info->pVertices[index];
        Require(vertex.position[0] == source.pos.x && vertex.position[1] == source.pos.y,
                "vertex offset must select the original vertex position");
        Require(vertex.texCoord[0] == source.uv.x && vertex.texCoord[1] == source.uv.y,
                "vertex UVs must be preserved across rollover");
        Require(vertex.packedColor == source.col, "vertex color must be preserved across rollover");
        maxIndex = std::max(maxIndex, index);
    }
    Require(info->vertexCount == maxIndex + 1, "uploaded vertex count must cover the command's local indices");
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
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::cout << "GUI bridge tests passed: 16-bit rollover, multiple offsets, command indices and attributes\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        QR_GUI_Shutdown();
        if (window != nullptr)
            SDL_DestroyWindow(window);
        SDL_Quit();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
