// qr_gui.cpp -- Dear ImGui bridge for the qr light editor (see qr_gui.h).
//
// The render backend lives here: ImGui draw lists are converted to QrVertex
// arrays and uploaded through qrUploadRasterizedGeometry with the SWAPCHAIN
// render type and a per-draw scissor rect, exactly like the engine's own 2D
// draws. No ImGui Vulkan backend, pipelines or descriptor pools are involved.

#include "qr_gui.h"
#include "cursor.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_internal.h>

#include <SDL.h>

#include <qray/qray.h>

#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>

namespace
{

QrInstance   g_instance      = 0;
QrMaterial   g_font_material = QR_NO_MATERIAL;
ImFont      *g_stats_font    = nullptr;
bool         g_ready         = false;
bool         g_frame_open    = false;
bool         g_custom_cursor = false;
unsigned int g_last_frame_id = 0xFFFFFFFFu;

int          g_fb_x = 0, g_fb_y = 0, g_fb_w = 0, g_fb_h = 0, g_drawable_h = 0;

char   g_notify[256] = "";
double g_notify_time = -1000.0;

std::vector<QrVertex> g_verts;
std::vector<uint32_t> g_indices;

// The label column of the panel: every widget is drawn next to its key name.
// (10% wider than the first cut: the material keys are long.)
constexpr float kLabelWidth      = 174.0f;
// The reset button every parameter row carries at its right edge, and the
// browse button of a texture path.
constexpr float kResetButtonSize = 24.0f;
constexpr float kBrowseButtonW   = 26.0f;
constexpr float kPi              = 3.14159265358979323846f;

float ClampF (float v, float mn, float mx)
{
	return v < mn ? mn : (v > mx ? mx : v);
}

ImU32 PackedColorToU32 (uint32_t argb)
{
	return IM_COL32 ((argb >> 0) & 0xFF, (argb >> 8) & 0xFF, (argb >> 16) & 0xFF, (argb >> 24) & 0xFF);
}

// A tooltip for the item just drawn (SetItemTooltip applies the panel's hover
// delay, see ApplyStyle).
void ItemTooltip (const char *text)
{
	if (text && *text)
		ImGui::SetItemTooltip ("%s", text);
}

// The width a row's main widget may take: the reset button and the spacing
// before it are reserved at the right edge of the panel.
float RowWidth (float extra)
{
	const ImGuiStyle &style = ImGui::GetStyle ();
	const float       width = ImGui::GetContentRegionAvail ().x - kResetButtonSize - style.ItemSpacing.x - extra;

	return width > 24.0f ? width : 24.0f;
}

void LabelColumn (const char *label, const char *tooltip)
{
	ImGui::AlignTextToFramePadding ();
	ImGui::TextUnformatted (label);
	ItemTooltip (tooltip);
	ImGui::SameLine (kLabelWidth);
}

void WidgetId (char *out, size_t outsize, const char *label)
{
	snprintf (out, outsize, "##%s", label);
}

// The X/Y/Z tags in front of a vec3 row's fields, each on its axis color.
void AxisLabel (int i)
{
	static const char *const axis[3] = { "X", "Y", "Z" };
	static const ImU32       bg[3] = {
	    IM_COL32 (150, 40, 40, 255),
	    IM_COL32 (40, 130, 40, 255),
	    IM_COL32 (45, 70, 170, 255),
	};

	ImGui::AlignTextToFramePadding ();

	const ImVec2 pos = ImGui::GetCursorScreenPos ();
	const ImVec2 size = ImGui::CalcTextSize (axis[i]);
	const float  frame_h = ImGui::GetFrameHeight ();

	ImGui::GetWindowDrawList ()->AddRectFilled (ImVec2 (pos.x - 4.0f, pos.y),
	                                            ImVec2 (pos.x + size.x + 4.0f, pos.y + frame_h), bg[i]);
	ImGui::TextUnformatted (axis[i]);
}

void ApplyStyle (void)
{
	ImGui::StyleColorsDark ();

	ImGuiStyle &s = ImGui::GetStyle ();

	s.WindowRounding    = 0.0f;
	s.ChildRounding     = 6.0f;
	s.FrameRounding     = 5.0f;
	s.GrabRounding      = 5.0f;
	s.PopupRounding     = 6.0f;
	s.ScrollbarRounding = 6.0f;
	s.TabRounding       = 5.0f;
	s.WindowBorderSize  = 1.0f;
	s.FramePadding      = ImVec2 (9.0f, 5.0f);
	s.ItemSpacing       = ImVec2 (9.0f, 7.0f);
	s.ItemInnerSpacing  = ImVec2 (7.0f, 5.0f);
	s.WindowPadding     = ImVec2 (14.0f, 12.0f);
	s.ScrollbarSize     = 14.0f;
	s.GrabMinSize       = 10.0f;

	// A parameter's hint appears after a second of hovering, like every other
	// tooltip of the panel (the disabled ones included: a locked roughness still
	// says why it is locked).
	s.HoverDelayNormal          = 1.0f;
	s.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayNormal |
	                              ImGuiHoveredFlags_AllowWhenDisabled;

	const ImVec4 accent = ImVec4 (0.26f, 0.59f, 0.98f, 1.00f);

	s.Colors[ImGuiCol_WindowBg]             = ImVec4 (0.075f, 0.082f, 0.100f, 0.97f);
	s.Colors[ImGuiCol_ChildBg]              = ImVec4 (0.055f, 0.060f, 0.075f, 0.85f);
	s.Colors[ImGuiCol_PopupBg]              = ImVec4 (0.090f, 0.095f, 0.115f, 0.98f);
	s.Colors[ImGuiCol_Border]               = ImVec4 (0.240f, 0.260f, 0.310f, 1.00f);
	s.Colors[ImGuiCol_FrameBg]              = ImVec4 (0.160f, 0.170f, 0.210f, 1.00f);
	s.Colors[ImGuiCol_FrameBgHovered]       = ImVec4 (0.220f, 0.240f, 0.290f, 1.00f);
	s.Colors[ImGuiCol_FrameBgActive]        = ImVec4 (0.260f, 0.280f, 0.340f, 1.00f);
	s.Colors[ImGuiCol_Button]               = ImVec4 (0.190f, 0.210f, 0.260f, 1.00f);
	s.Colors[ImGuiCol_ButtonHovered]        = ImVec4 (0.270f, 0.300f, 0.370f, 1.00f);
	s.Colors[ImGuiCol_ButtonActive]         = accent;
	s.Colors[ImGuiCol_Header]               = ImVec4 (0.200f, 0.240f, 0.310f, 1.00f);
	s.Colors[ImGuiCol_HeaderHovered]        = ImVec4 (0.260f, 0.310f, 0.400f, 1.00f);
	s.Colors[ImGuiCol_HeaderActive]         = ImVec4 (0.300f, 0.360f, 0.460f, 1.00f);
	s.Colors[ImGuiCol_SliderGrab]           = accent;
	s.Colors[ImGuiCol_SliderGrabActive]     = ImVec4 (0.420f, 0.700f, 1.000f, 1.00f);
	s.Colors[ImGuiCol_CheckMark]            = accent;
	s.Colors[ImGuiCol_Separator]            = ImVec4 (0.240f, 0.260f, 0.310f, 1.00f);
	s.Colors[ImGuiCol_Text]                 = ImVec4 (0.900f, 0.920f, 0.950f, 1.00f);
	s.Colors[ImGuiCol_TextDisabled]         = ImVec4 (0.500f, 0.520f, 0.570f, 1.00f);
	s.Colors[ImGuiCol_ScrollbarBg]          = ImVec4 (0.060f, 0.065f, 0.080f, 1.00f);
	s.Colors[ImGuiCol_ScrollbarGrab]        = ImVec4 (0.300f, 0.330f, 0.390f, 1.00f);
	s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4 (0.380f, 0.420f, 0.500f, 1.00f);
	s.Colors[ImGuiCol_ScrollbarGrabActive]  = accent;
	s.Colors[ImGuiCol_TitleBg]              = ImVec4 (0.100f, 0.110f, 0.140f, 1.00f);
}

void UploadDrawData (void)
{
	ImDrawData *dd = ImGui::GetDrawData ();

	if (!dd || dd->CmdListsCount == 0 || dd->DisplaySize.x <= 0.0f || dd->DisplaySize.y <= 0.0f)
		return;

	const float W = dd->DisplaySize.x;
	const float H = dd->DisplaySize.y;

	// The same ortho as the engine's 2D canvas: (0,0) top-left, Y down.
	float m[16] = {};
	m[0]  = 2.0f / W;
	m[5]  = 2.0f / H;
	m[10] = -1.0f;
	m[12] = -1.0f;
	m[13] = -1.0f;
	m[15] = 1.0f;

	// Vulkan viewport keeps the Y flip; the scissor is top-left in both.
	QrViewport vp = {};
	vp.x        = (float)g_fb_x;
	vp.y        = (float)(g_drawable_h - (g_fb_y + g_fb_h));
	vp.width    = (float)g_fb_w;
	vp.height   = (float)g_fb_h;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;

	const int area_x0 = g_fb_x;
	const int area_y0 = g_drawable_h - (g_fb_y + g_fb_h);
	const int area_x1 = area_x0 + g_fb_w;
	const int area_y1 = area_y0 + g_fb_h;

	const QrTransform identity = [] {
		QrTransform t = {};
		t.matrix[0][0] = t.matrix[1][1] = t.matrix[2][2] = 1.0f;
		return t;
	} ();

	for (int n = 0; n < dd->CmdListsCount; n++)
	{
		const ImDrawList *dl = dd->CmdLists[n];

		g_verts.resize ((size_t)dl->VtxBuffer.Size);
		for (int i = 0; i < dl->VtxBuffer.Size; i++)
		{
			const ImDrawVert &v = dl->VtxBuffer[i];
			QrVertex         &rv = g_verts[(size_t)i];

			rv = QrVertex ();
			rv.position[0] = v.pos.x;
			rv.position[1] = v.pos.y;
			rv.position[2] = 0.0f;
			rv.normal[2]   = 1.0f;
			rv.texCoord[0] = v.uv.x;
			rv.texCoord[1] = v.uv.y;
			rv.packedColor = v.col;
		}

		for (const ImDrawCmd &cmd : dl->CmdBuffer)
		{
			if (cmd.UserCallback != nullptr || cmd.ElemCount == 0)
				continue;

			// the clip rect in display coordinates -> a scissor in the drawable
			float cx0 = ClampF ((float)cmd.ClipRect.x, 0.0f, W);
			float cy0 = ClampF ((float)cmd.ClipRect.y, 0.0f, H);
			float cx1 = ClampF ((float)cmd.ClipRect.z, 0.0f, W);
			float cy1 = ClampF ((float)cmd.ClipRect.w, 0.0f, H);

			int x0 = area_x0 + (int)floorf (cx0);
			int y0 = area_y0 + (int)floorf (cy0);
			int x1 = area_x0 + (int)ceilf (cx1);
			int y1 = area_y0 + (int)ceilf (cy1);

			if (x0 < area_x0) x0 = area_x0;
			if (y0 < area_y0) y0 = area_y0;
			if (x1 > area_x1) x1 = area_x1;
			if (y1 > area_y1) y1 = area_y1;
			if (x1 <= x0 || y1 <= y0)
				continue;

			// rebase the indices to the command's own vertex span
			g_indices.resize (cmd.ElemCount);
			unsigned maxv = 0;
			const ImDrawIdx *src = dl->IdxBuffer.Data + cmd.IdxOffset;
			for (unsigned i = 0; i < cmd.ElemCount; i++)
			{
				unsigned idx = (unsigned)src[i] - (unsigned)cmd.VtxOffset;
				g_indices[i] = idx;
				if (idx > maxv)
					maxv = idx;
			}

			QrRasterizedGeometryUploadInfo info = {};
			info.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN;
			info.vertexCount = maxv + 1;
			info.pVertices = g_verts.data () + cmd.VtxOffset;
			info.indexCount = cmd.ElemCount;
			info.pIndices = g_indices.data ();
			info.transform = identity;
			info.color.data[0] = info.color.data[1] = info.color.data[2] = info.color.data[3] = 1.0f;
			info.material = (QrMaterial)(uintptr_t)cmd.GetTexID ();
			info.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
			info.blendFuncSrc = QR_BLEND_FACTOR_SRC_ALPHA;
			info.blendFuncDst = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			info.scissor = { x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };

			qrUploadRasterizedGeometry (g_instance, &info, m, &vp);
		}
	}
}

void DrawNotification (void)
{
	const double age = ImGui::GetTime () - g_notify_time;

	if (g_notify[0] == '\0' || age > 6.0)
		return;

	float alpha = 1.0f;
	if (age > 5.0)
		alpha = (float)(6.0 - age); // fade out over the last second

	ImDrawList  *dl = ImGui::GetForegroundDrawList ();
	const ImVec2 pos (14.0f, 14.0f);

	dl->AddText (ImVec2 (pos.x + 1.0f, pos.y + 1.0f), IM_COL32 (0, 0, 0, (int)(170.0f * alpha)), g_notify);
	dl->AddText (pos, IM_COL32 (255, 226, 138, (int)(235.0f * alpha)), g_notify);
}

void DrawMouseCursor (void)
{
	if (!g_custom_cursor)
		return;

	ImGuiIO &io = ImGui::GetIO ();
	int64_t  texture = QR_NO_MATERIAL;
	int      size = 0, hotX = 0, hotY = 0;

	if (!Cursor_GetGuiCursor (&texture, &size, &hotX, &hotY))
	{
		io.MouseDrawCursor = true;
		return;
	}

	io.MouseDrawCursor = false;

	const ImVec2 pos (io.MousePos.x - (float)hotX, io.MousePos.y - (float)hotY);
	ImGui::GetForegroundDrawList ()->AddImage ((ImTextureID)(uintptr_t)texture, pos, ImVec2 (pos.x + (float)size, pos.y + (float)size));
}

} // namespace

void QR_GUI_Init (void *sdl_window, void *qr_instance, const char *font_path)
{
	if (g_ready || sdl_window == NULL || qr_instance == NULL)
		return;

	IMGUI_CHECKVERSION ();
	ImGui::CreateContext ();

	ImGuiIO &io = ImGui::GetIO ();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

	ApplyStyle ();

	if (!ImGui_ImplSDL2_InitForOther ((SDL_Window *)sdl_window))
	{
		fprintf (stderr, "qr gui: SDL2 backend init failed\n");
		ImGui::DestroyContext (); // the context is created before the backend
		return;
	}

	ImFont *font = nullptr;
	if (font_path && font_path[0])
	{
		font         = io.Fonts->AddFontFromFileTTF (font_path, 19.0f);
		g_stats_font = io.Fonts->AddFontFromFileTTF (font_path, 14.0f);
	}
	if (!font)
	{
		ImFontConfig cfg;
		cfg.SizePixels = 19.0f;
		font = io.Fonts->AddFontDefault (&cfg);
		if (font_path && font_path[0])
			fprintf (stderr, "qr gui: cannot load '%s', using the default font\n", font_path);
	}

	g_instance = (QrInstance)qr_instance;

	// The legacy atlas path: one texture, one material, no ImTextureData flow.
	unsigned char *pixels = nullptr;
	int            w = 0, h = 0;
	io.Fonts->GetTexDataAsRGBA32 (&pixels, &w, &h, nullptr);

	if (pixels && w > 0 && h > 0)
	{
		QrMaterialCreateInfo info = {};
		info.flags = 0;
		info.size = { (uint32_t)w, (uint32_t)h };
		info.textures.pDataAlbedoAlpha = pixels;
		info.pRelativePath = nullptr;
		info.filter = QR_SAMPLER_FILTER_LINEAR;
		info.addressModeU = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

		QrResult r = qrCreateMaterial (g_instance, &info, &g_font_material);
		if (r != QR_SUCCESS)
		{
			fprintf (stderr, "qr gui: font atlas material creation failed (%d)\n", (int)r);
			g_font_material = QR_NO_MATERIAL;
		}
		io.Fonts->SetTexID ((ImTextureID)(uintptr_t)g_font_material);
	}

	g_ready = true;
}

int QR_GUI_Ready (void)
{
	return g_ready ? 1 : 0;
}

void QR_GUI_Shutdown (void)
{
	if (!g_ready)
		return;

	// the font atlas is the bridge's own material and outlives the context
	if (g_font_material != QR_NO_MATERIAL && g_instance)
	{
		qrDestroyMaterial (g_instance, g_font_material);
		g_font_material = QR_NO_MATERIAL;
	}

	ImGui_ImplSDL2_Shutdown ();
	ImGui::DestroyContext ();
	g_ready = false;
}

int QR_GUI_BeginFrame (unsigned int frame_id, float dt, int x, int y, int width, int height, int drawable_height)
{
	if (!g_ready || g_frame_open || frame_id == g_last_frame_id)
		return 0;

	g_last_frame_id = frame_id;
	g_fb_x = x;
	g_fb_y = y;
	g_fb_w = width;
	g_fb_h = height;
	g_drawable_h = drawable_height;

	ImGui_ImplSDL2_NewFrame ();

	ImGuiIO &io = ImGui::GetIO ();
	io.DisplaySize = ImVec2 ((float)width, (float)height);
	io.DisplayFramebufferScale = ImVec2 (1.0f, 1.0f);
	io.DeltaTime = dt > 0.0f ? dt : (1.0f / 60.0f);

	ImGui::NewFrame ();
	g_frame_open = true;
	return 1;
}

void QR_GUI_EndFrame (void)
{
	if (!g_frame_open)
		return;

	DrawNotification ();
	DrawMouseCursor ();

	ImGui::Render ();
	g_frame_open = false;

	UploadDrawData ();
}

int QR_GUI_ProcessEvent (const void *sdl_event)
{
	if (!g_ready || sdl_event == NULL)
		return 0;

	const SDL_Event *e = (const SDL_Event *)sdl_event;

	ImGui_ImplSDL2_ProcessEvent (e);

	switch (e->type)
	{
	case SDL_MOUSEMOTION:
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEWHEEL:
	case SDL_TEXTINPUT:
	case SDL_TEXTEDITING:
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		return 1;
	default:
		return 0;
	}
}

int QR_GUI_WantsMouse (void)
{
	if (!g_ready)
		return 0;
	return ImGui::GetIO ().WantCaptureMouse ? 1 : 0;
}

int QR_GUI_WantsKeyboard (void)
{
	if (!g_ready)
		return 0;
	return ImGui::GetIO ().WantTextInput ? 1 : 0;
}

void QR_GUI_SetMouseCursor (int enable)
{
	if (!g_ready)
		return;
	g_custom_cursor = enable != 0;
	ImGui::GetIO ().MouseDrawCursor = false;
}

void QR_GUI_BeginPanel (const char *id, int x, int y, int width, int height)
{
	ImGui::SetNextWindowPos (ImVec2 ((float)x, (float)y));
	ImGui::SetNextWindowSize (ImVec2 ((float)width, (float)height));

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
	                         ImGuiWindowFlags_NoSavedSettings;

	ImGui::Begin (id, nullptr, flags);
}

void QR_GUI_EndPanel (void)
{
	ImGui::End ();
}

void QR_GUI_Backdrop (float alpha)
{
	if (!g_ready)
		return;

	if (alpha < 0.0f)
		alpha = 0.0f;
	else if (alpha > 1.0f)
		alpha = 1.0f;

	ImGuiIO &io = ImGui::GetIO ();

	ImGui::GetBackgroundDrawList ()->AddRectFilled (ImVec2 (0.0f, 0.0f), io.DisplaySize, IM_COL32 (0, 0, 0, (int)(alpha * 255.0f)));
}

int QR_GUI_BeginDialog (const char *title, float width)
{
	const ImVec2 center (ImGui::GetIO ().DisplaySize.x * 0.5f, ImGui::GetIO ().DisplaySize.y * 0.5f);

	ImGui::SetNextWindowPos (center, ImGuiCond_Always, ImVec2 (0.5f, 0.5f));
	ImGui::SetNextWindowSize (ImVec2 (width, 0.0f), ImGuiCond_Always);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
	                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

	return ImGui::Begin (title, nullptr, flags) ? 1 : 0;
}

void QR_GUI_EndDialog (void)
{
	ImGui::End ();
}

static float g_canvas_x;
static float g_canvas_y;

int QR_GUI_Canvas (const char *id, float width, float height, float *out_x, float *out_y)
{
	const ImVec2 origin = ImGui::GetCursorScreenPos ();
	int          result = 0;

	ImGui::InvisibleButton (id, ImVec2 (width, height));
	if (ImGui::IsItemActive ())
		result |= 2;
	if (ImGui::IsItemHovered ())
		result |= 4;

	g_canvas_x = origin.x;
	g_canvas_y = origin.y;

	if (out_x)
		*out_x = origin.x;
	if (out_y)
		*out_y = origin.y;

	return result;
}

void QR_GUI_CanvasLine (float x0, float y0, float x1, float y1, uint32_t argb, float thickness)
{
	ImGui::GetWindowDrawList ()->AddLine (ImVec2 (g_canvas_x + x0, g_canvas_y + y0), ImVec2 (g_canvas_x + x1, g_canvas_y + y1), PackedColorToU32 (argb),
	                                      thickness > 0.0f ? thickness : 1.0f);
}

void QR_GUI_CanvasRect (float x0, float y0, float x1, float y1, uint32_t argb)
{
	ImGui::GetWindowDrawList ()->AddRectFilled (ImVec2 (g_canvas_x + x0, g_canvas_y + y0), ImVec2 (g_canvas_x + x1, g_canvas_y + y1), PackedColorToU32 (argb));
}

void QR_GUI_CanvasCircle (float cx, float cy, float radius, uint32_t argb, float thickness, int filled)
{
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	const ImVec2 c (g_canvas_x + cx, g_canvas_y + cy);

	if (filled)
		dl->AddCircleFilled (c, radius, PackedColorToU32 (argb));
	else
		dl->AddCircle (c, radius, PackedColorToU32 (argb), 0, thickness > 0.0f ? thickness : 1.0f);
}

void QR_GUI_CanvasText (float x, float y, uint32_t argb, const char *text)
{
	ImGui::GetWindowDrawList ()->AddText (ImVec2 (g_canvas_x + x, g_canvas_y + y), PackedColorToU32 (argb), text ? text : "");
}

void QR_GUI_BeginScroll (void)
{
	ImGui::BeginChild ("##qr_scroll", ImVec2 (0.0f, 0.0f), ImGuiChildFlags_None,
	                   ImGuiWindowFlags_NoSavedSettings);
}

void QR_GUI_EndScroll (void)
{
	ImGui::EndChild ();
}

void QR_GUI_Label (const char *text)
{
	ImGui::TextUnformatted (text);
}

void QR_GUI_LabelDim (const char *text)
{
	ImGui::TextDisabled ("%s", text);
}

void QR_GUI_LabelRight (const char *text)
{
	const char *label = text ? text : "";
	float       avail, width;

	ImGui::SameLine ();
	avail = ImGui::GetContentRegionAvail ().x;
	width = ImGui::CalcTextSize (label).x;
	if (width < avail)
		ImGui::SetCursorPosX (ImGui::GetCursorPosX () + avail - width);
	ImGui::TextDisabled ("%s", label);
}

void QR_GUI_Separator (void)
{
	ImGui::Separator ();
}

void QR_GUI_SectionHeader (const char *label)
{
	QR_GUI_Separator ();
	QR_GUI_SectionTitle (label);
}

void QR_GUI_SectionTitle (const char *label)
{
	const ImVec2 pos = ImGui::GetCursorScreenPos ();
	const float  avail = ImGui::GetContentRegionAvail ().x;
	const ImVec2 size = ImGui::CalcTextSize (label);
	const float  x = (avail - size.x) * 0.5f;
	const ImU32  col = ImGui::GetColorU32 (ImGuiCol_Text);
	ImDrawList  *dl = ImGui::GetWindowDrawList ();

	ImGui::Dummy (ImVec2 (avail, size.y));

	dl->AddText (ImVec2 (pos.x + x - 0.6f, pos.y), col, label);
	dl->AddText (ImVec2 (pos.x + x + 0.6f, pos.y), col, label);
	dl->AddText (ImVec2 (pos.x + x, pos.y - 0.6f), col, label);
	dl->AddText (ImVec2 (pos.x + x, pos.y + 0.6f), col, label);
	dl->AddText (ImVec2 (pos.x + x, pos.y), col, label);
}

void QR_GUI_Spacing (void)
{
	ImGui::Spacing ();
}

void QR_GUI_SameLine (void)
{
	ImGui::SameLine ();
}

void QR_GUI_PushWindowPadding (float x, float y)
{
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (x, y));
}

void QR_GUI_PopWindowPadding (void)
{
	ImGui::PopStyleVar ();
}

float QR_GUI_TextWidth (const char *text)
{
	return ImGui::CalcTextSize (text ? text : "").x;
}

int QR_GUI_CtrlDown (void)
{
	return ImGui::GetIO ().KeyCtrl ? 1 : 0;
}

void QR_GUI_Tooltip (const char *text)
{
	ImGui::SetItemTooltip ("%s", text);
}

int QR_GUI_Button (const char *label)
{
	return ImGui::Button (label) ? 1 : 0;
}

// A row of equal-width tabs: the selected one is drawn in the "active" color
// and a click on another one selects it (the caller draws the content itself,
// so the editor keeps whatever state each tab needs).
int QR_GUI_Tabs (const char *id, const char *const *items, int count, int *selected, int *reset_pressed)
{
	const float spacing = ImGui::GetStyle ().ItemSpacing.x;
	const float size    = ImGui::GetFrameHeight ();
	const float width   = (ImGui::GetContentRegionAvail ().x - size - spacing * (float)count) / (float)count;
	int         changed = 0;

	ImGui::PushID (id);
	for (int i = 0; i < count; i++)
	{
		const bool active = (i == *selected);

		if (active)
			ImGui::PushStyleColor (ImGuiCol_Button, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
		if (ImGui::Button (items[i], ImVec2 (width, 0.0f)) && !active)
		{
			*selected = i;
			changed = 1;
		}
		if (active)
			ImGui::PopStyleColor ();
		if (i + 1 < count)
			ImGui::SameLine ();
	}
	ImGui::SameLine ();
	*reset_pressed = ImGui::Button ("##reset_all", ImVec2 (size, size)) ? 1 : 0;
	const ImVec2 min = ImGui::GetItemRectMin ();
	const ImVec2 max = ImGui::GetItemRectMax ();
	const float x = (min.x + max.x) * 0.5f;
	const float y = (min.y + max.y) * 0.5f;
	const float r = size * 0.23f;
	const ImU32 color = ImGui::GetColorU32 (ImGuiCol_Text);
	ImDrawList *draw = ImGui::GetWindowDrawList ();
	draw->AddRect (ImVec2 (x - r * 0.7f, y - r * 0.55f), ImVec2 (x + r * 0.7f, y + r), color, 1.0f, 0, 1.5f);
	draw->AddLine (ImVec2 (x - r, y - r * 0.8f), ImVec2 (x + r, y - r * 0.8f), color, 1.5f);
	draw->AddRect (ImVec2 (x - r * 0.35f, y - r * 1.25f), ImVec2 (x + r * 0.35f, y - r * 0.8f), color, 0.0f, 0, 1.5f);
	draw->AddLine (ImVec2 (x - r * 0.25f, y - r * 0.25f), ImVec2 (x - r * 0.25f, y + r * 0.65f), color, 1.0f);
	draw->AddLine (ImVec2 (x + r * 0.25f, y - r * 0.25f), ImVec2 (x + r * 0.25f, y + r * 0.65f), color, 1.0f);
	ItemTooltip ("Reset all saved work and settings to defaults.");
	ImGui::PopID ();

	return changed;
}

int QR_GUI_Checkbox (const char *label, int *value, const char *tooltip)
{
	bool v = *value != 0;
	bool changed;

	changed = ImGui::Checkbox (label, &v);
	ItemTooltip (tooltip);
	if (changed)
		*value = v ? 1 : 0;
	return changed ? 1 : 0;
}

int QR_GUI_CheckboxMixed (const char *label, int *value, int mixed, const char *tooltip)
{
	bool v = *value != 0;
	bool changed;

	if (mixed)
		ImGui::PushItemFlag (ImGuiItemFlags_MixedValue, true);
	changed = ImGui::Checkbox (label, &v);
	if (mixed)
		ImGui::PopItemFlag ();
	ItemTooltip (tooltip);
	if (changed)
		*value = v ? 1 : 0;
	return changed ? 1 : 0;
}

int QR_GUI_SliderFloatFmt (const char *label, float *value, float min, float max, const char *format, const char *tooltip)
{
	char id[192];
	WidgetId (id, sizeof (id), label);

	LabelColumn (label, tooltip);
	ImGui::SetNextItemWidth (RowWidth (0.0f));
	bool changed = ImGui::SliderFloat (id, value, min, max, format, ImGuiSliderFlags_AlwaysClamp);
	if (tooltip && *tooltip)
		ImGui::SetItemTooltip ("%s\nCtrl+click to type a value", tooltip);
	else
		ImGui::SetItemTooltip ("Ctrl+click to type a value");
	return changed ? 1 : 0;
}

int QR_GUI_SliderFloat (const char *label, float *value, float min, float max, const char *tooltip)
{
	return QR_GUI_SliderFloatFmt (label, value, min, max, "%.2f", tooltip);
}

int QR_GUI_SliderInt (const char *label, int *value, int min, int max, const char *tooltip)
{
	char id[192];
	WidgetId (id, sizeof (id), label);

	LabelColumn (label, tooltip);
	ImGui::SetNextItemWidth (RowWidth (0.0f));
	bool changed = ImGui::SliderInt (id, value, min, max, "%d", ImGuiSliderFlags_AlwaysClamp);
	if (tooltip && *tooltip)
		ImGui::SetItemTooltip ("%s\nCtrl+click to type a value", tooltip);
	else
		ImGui::SetItemTooltip ("Ctrl+click to type a value");
	return changed ? 1 : 0;
}

int QR_GUI_SliderFloatMixed (const char *label, float *value, float min, float max, int mixed, const char *tooltip)
{
	char id[192];
	bool changed;

	if (!mixed)
		return QR_GUI_SliderFloat (label, value, min, max, tooltip);

	WidgetId (id, sizeof (id), label);

	LabelColumn (label, tooltip);
	ImGui::SetNextItemWidth (RowWidth (0.0f));
	changed = ImGui::SliderFloat (id, value, min, max, "?", ImGuiSliderFlags_AlwaysClamp);
	if (tooltip && *tooltip)
		ImGui::SetItemTooltip ("%s\nCtrl+click to type a value", tooltip);
	else
		ImGui::SetItemTooltip ("Ctrl+click to type a value");
	return changed ? 1 : 0;
}

int QR_GUI_Combo (const char *label, int *value, const char *const *items, int count, const char *tooltip)
{
	char id[192];
	WidgetId (id, sizeof (id), label);

	LabelColumn (label, tooltip);
	ImGui::SetNextItemWidth (RowWidth (0.0f));
	int changed = ImGui::Combo (id, value, items, count) ? 1 : 0;
	ItemTooltip (tooltip);
	return changed;
}

int QR_GUI_InputText (const char *label, char *buf, size_t capacity, const char *tooltip)
{
	char id[192];
	WidgetId (id, sizeof (id), label);

	LabelColumn (label, tooltip);
	ImGui::SetNextItemWidth (RowWidth (0.0f));
	int changed = ImGui::InputText (id, buf, capacity) ? 1 : 0;
	ItemTooltip (tooltip);
	return changed;
}

int QR_GUI_Vec3Input (const char *label, float v[3], float min, float max, const char *tooltip)
{
	static const char *const axis[3] = { "X", "Y", "Z" };
	char               id[192];
	int                i, changed = 0;
	const ImGuiStyle  &style = ImGui::GetStyle ();
	const float        letter = ImGui::CalcTextSize ("X").x;
	float              field;

	WidgetId (id, sizeof (id), label);

	LabelColumn (label, tooltip);

	// measured after the label column: the row starts where the fields start,
	// otherwise the three fields are laid out from the full panel width and
	// step past the right edge (the reset button's width is already reserved)
	field = (RowWidth (0.0f) - letter * 3.0f - style.ItemSpacing.x * 5.0f) / 3.0f;
	if (field < 32.0f)
		field = 32.0f;

	ImGui::PushID (label);
	for (i = 0; i < 3; i++)
	{
		char aid[16];

		if (i)
			ImGui::SameLine ();
		AxisLabel (i);
		ImGui::SameLine ();
		snprintf (aid, sizeof (aid), "##%s", axis[i]);
		ImGui::SetNextItemWidth (field);
		if (ImGui::InputFloat (aid, &v[i], 0.0f, 0.0f, "%.2f"))
			changed = 1;
	}
	ItemTooltip (tooltip);
	ImGui::PopID ();

	for (i = 0; i < 3; i++)
		v[i] = ClampF (v[i], min, max);

	return changed;
}

int QR_GUI_Vec3InputMixed (const char *label, float v[3], float min, float max, const unsigned char mixed[3], const char *tooltip)
{
	static const char *const axis[3] = { "X", "Y", "Z" };
	char               id[192];
	int                i, changed = 0;
	bool               any = false;
	const ImGuiStyle  &style = ImGui::GetStyle ();
	const float        letter = ImGui::CalcTextSize ("X").x;
	float              field;

	for (i = 0; i < 3; i++)
	{
		if (mixed[i])
			any = true;
	}
	if (!any)
	{
		float before[3];

		for (i = 0; i < 3; i++)
			before[i] = v[i];
		QR_GUI_Vec3Input (label, v, min, max, tooltip);
		for (i = 0; i < 3; i++)
			if (v[i] != before[i])
				changed |= 1 << i;
		return changed;
	}

	WidgetId (id, sizeof (id), label);

	LabelColumn (label, tooltip);

	field = (RowWidth (0.0f) - letter * 3.0f - style.ItemSpacing.x * 5.0f) / 3.0f;
	if (field < 32.0f)
		field = 32.0f;

	ImGui::PushID (label);
	for (i = 0; i < 3; i++)
	{
		char aid[16];

		if (i)
			ImGui::SameLine ();
		AxisLabel (i);
		ImGui::SameLine ();
		snprintf (aid, sizeof (aid), "##%s", axis[i]);
		ImGui::SetNextItemWidth (field);
		if (ImGui::InputFloat (aid, &v[i], 0.0f, 0.0f, mixed[i] ? "?" : "%.2f",
		                       mixed[i] ? ImGuiInputTextFlags_AutoSelectAll : (ImGuiInputTextFlags)0))
			changed |= 1 << i;
	}
	ItemTooltip (tooltip);
	ImGui::PopID ();

	for (i = 0; i < 3; i++)
		v[i] = ClampF (v[i], min, max);

	return changed;
}

int QR_GUI_TexturePath (const char *label, char *buf, size_t capacity, const char *tooltip)
{
	char id[192];
	WidgetId (id, sizeof (id), label);

	int result = 0;

	LabelColumn (label, tooltip);
	ImGui::SetNextItemWidth (RowWidth (kBrowseButtonW + ImGui::GetStyle ().ItemSpacing.x));
	// An unauthored path is shown as NONE; the buffer stays empty, and typing
	// NONE by hand commits as "no texture" as well.
	if (ImGui::InputTextWithHint (id, "NONE", buf, capacity))
		result |= 1;
	ItemTooltip (tooltip);

	ImGui::SameLine ();
	ImGui::PushID (label);
	if (ImGui::Button ("...", ImVec2 (kBrowseButtonW, 0.0f)))
		result |= 2;
	ImGui::SetItemTooltip ("Browse for a file");
	ImGui::PopID ();

	return result;
}

// A square button with a circular arrow, right-aligned in its row: a parameter
// reset the widget before it did not have. Grayed out while the parameter still
// holds its original value.
int QR_GUI_ResetButton (const char *label, int enabled)
{
	// an ID of its own: the row widget already owns "##<label>", and two items
	// under one ID hand the click to the first of them
	char id[192];
	snprintf (id, sizeof (id), "##reset_%s", label);

	// right-aligned, but never over the widget that was just drawn
	const ImVec2 prev_max = ImGui::GetItemRectMax ();
	const float  after    = (prev_max.x - ImGui::GetWindowPos ().x) + ImGui::GetStyle ().ItemSpacing.x;
	const float  right    = ImGui::GetWindowContentRegionMax ().x - kResetButtonSize;
	const float  x        = right > after ? right : after;

	ImGui::SameLine (x);
	// a square, centerd on the row whose frame is a little taller
	ImGui::SetCursorPosY (ImGui::GetCursorPosY () +
	                      (ImGui::GetFrameHeight () - kResetButtonSize) * 0.5f);

	if (!enabled)
		ImGui::BeginDisabled ();

	ImGui::PushStyleColor (ImGuiCol_Button, ImVec4 (0.16f, 0.17f, 0.21f, 0.85f));
	const bool pressed = ImGui::Button (id, ImVec2 (kResetButtonSize, kResetButtonSize));
	ImGui::PopStyleColor ();

	// The circular arrow is drawn, not a glyph: the panel font has no U+21BA.
	const ImVec2 mn  = ImGui::GetItemRectMin ();
	const ImVec2 mx  = ImGui::GetItemRectMax ();
	const ImU32  col = ImGui::GetColorU32 (ImGuiCol_Text, enabled ? 1.0f : 0.45f);
	ImDrawList  *dl  = ImGui::GetWindowDrawList ();

	const ImVec2 c ((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
	const float  r  = kResetButtonSize * 0.27f;
	const float  a0 = kPi * 0.35f;
	const float  a1 = kPi * 1.85f;

	dl->PathArcTo (c, r, a0, a1, 24);
	dl->PathStroke (col, 0, 1.5f);

	const ImVec2 p (c.x + r * cosf (a1), c.y + r * sinf (a1));
	const ImVec2 dir (cosf (a1), sinf (a1));
	const ImVec2 tang (-dir.y, dir.x);
	const float  head = 3.4f;

	dl->AddTriangleFilled (ImVec2 (p.x + tang.x * head * 1.4f, p.y + tang.y * head * 1.4f),
	                       ImVec2 (p.x + dir.x * head, p.y + dir.y * head),
	                       ImVec2 (p.x - dir.x * head, p.y - dir.y * head), col);

	ImGui::SetItemTooltip ("Reset to the original value");

	if (!enabled)
		ImGui::EndDisabled ();

	return pressed ? 1 : 0;
}

static int g_disabled_depth = 0;

void QR_GUI_PushDisabled (int disabled)
{
	if (!disabled)
		return;

	ImGui::BeginDisabled ();
	++g_disabled_depth;
}

void QR_GUI_PopDisabled (void)
{
	if (g_disabled_depth <= 0)
		return;

	ImGui::EndDisabled ();
	--g_disabled_depth;
}

int QR_GUI_ColorHex (const char *label, float rgb[3], int *enabled, const char *tooltip)
{
	int result = 0;

	LabelColumn (label, tooltip);

	ImGui::PushID (label);

	bool en = *enabled != 0;
	if (ImGui::Checkbox ("##enabled", &en))
	{
		*enabled = en ? 1 : 0;
		result = 1;
	}
	ImGui::SetItemTooltip ("Enable the color");

	ImGui::SameLine ();

	// The item needs an explicit width: with the default one its swatch lands
	// past the right edge of the panel and can never be clicked, which is what
	// made the picker unreachable.
	ImGui::SetNextItemWidth (RowWidth (0.0f));
	if (ImGui::ColorEdit3 ("##color", rgb, ImGuiColorEditFlags_DisplayHex))
	{
		// editing the color is what enables it; the checkbox above only shows
		// the state and can turn it off again
		*enabled = 1;
		result = 1;
	}
	if (tooltip && *tooltip)
		ImGui::SetItemTooltip ("%s\nClick the swatch to pick, type the hex value", tooltip);
	else
		ImGui::SetItemTooltip ("Click the swatch to pick, type the hex value");

	ImGui::PopID ();

	return result;
}

int QR_GUI_ColorHexMixed (const char *label, float rgb[3], int *enabled, int mixed, const char *tooltip)
{
	int result = 0;

	if (!mixed)
		return QR_GUI_ColorHex (label, rgb, enabled, tooltip);

	LabelColumn (label, tooltip);

	ImGui::PushID (label);

	bool en = *enabled != 0;
	ImGui::PushItemFlag (ImGuiItemFlags_MixedValue, true);
	if (ImGui::Checkbox ("##enabled", &en))
	{
		*enabled = en ? 1 : 0;
		result = 1;
	}
	ImGui::PopItemFlag ();
	ImGui::SetItemTooltip ("Enable the color");

	ImGui::SameLine ();

	{
		char buf[16] = "?";

		ImGui::SetNextItemWidth (RowWidth (ImGui::GetFrameHeight () + ImGui::GetStyle ().ItemInnerSpacing.x));
		if (ImGui::InputText ("##color", buf, sizeof (buf),
		                      ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsUppercase))
		{
			char        *p = buf;
			unsigned int r, g, b;

			while (*p == '#' || *p == ' ')
				p++;
			if (sscanf (p, "%02X%02X%02X", &r, &g, &b) == 3)
			{
				rgb[0] = (float)r / 255.0f;
				rgb[1] = (float)g / 255.0f;
				rgb[2] = (float)b / 255.0f;
				*enabled = 1;
				result = 1;
			}
		}
		if (tooltip && *tooltip)
			ImGui::SetItemTooltip ("%s\nType the hex value or pick from the swatch", tooltip);
		else
			ImGui::SetItemTooltip ("Type the hex value or pick from the swatch");
	}

	ImGui::SameLine (0.0f, ImGui::GetStyle ().ItemInnerSpacing.x);

	if (ImGui::ColorEdit3 ("##pick", rgb, ImGuiColorEditFlags_NoInputs))
	{
		*enabled = 1;
		result = 1;
	}
	if (tooltip && *tooltip)
		ImGui::SetItemTooltip ("%s\nClick the swatch to pick, type the hex value", tooltip);
	else
		ImGui::SetItemTooltip ("Click the swatch to pick, type the hex value");

	ImGui::PopID ();

	return result;
}

int QR_GUI_Section (const char *label, int default_open)
{
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed;
	if (default_open)
		flags |= ImGuiTreeNodeFlags_DefaultOpen;

	return ImGui::CollapsingHeader (label, flags) ? 1 : 0;
}

int QR_GUI_SectionSelected (const char *label, int selected)
{
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed;
	int                clicked;

	ImGui::SetNextItemOpen (selected != 0, ImGuiCond_Always);

	if (selected)
	{
		const ImVec4 active = ImGui::GetStyleColorVec4 (ImGuiCol_HeaderActive);

		ImGui::PushStyleColor (ImGuiCol_Header, active);
		ImGui::PushStyleColor (ImGuiCol_HeaderHovered, active);
		ImGui::PushStyleColor (ImGuiCol_HeaderActive, active);
	}

	(void)ImGui::CollapsingHeader (label, flags);
	clicked = ImGui::IsItemClicked () ? 1 : 0;

	if (selected)
		ImGui::PopStyleColor (3);

	return clicked;
}

void QR_GUI_PushID (const char *id)
{
	ImGui::PushID (id);
}

void QR_GUI_PopID (void)
{
	ImGui::PopID ();
}

int QR_GUI_AnyItemActive (void)
{
	return ImGui::IsAnyItemActive () ? 1 : 0;
}

int QR_GUI_ImagePick (const char *id, int64_t texture, int tex_w, int tex_h, float *out_u, float *out_v)
{
	ImGui::PushID (id);

	float w = ImGui::GetContentRegionAvail ().x;
	float h;

	if (w < 32.0f)
		w = 32.0f;
	h = (tex_w > 0 && tex_h > 0) ? w * (float)tex_h / (float)tex_w : w;

	const ImVec2 pos = ImGui::GetCursorScreenPos ();
	const ImVec2 size (w, h);

	ImGui::Image ((ImTextureID)(uint64_t)texture, size, ImVec2 (0.0f, 0.0f), ImVec2 (1.0f, 1.0f));

	const bool hovered = ImGui::IsItemHovered ();
	const ImVec2 mouse = ImGui::GetIO ().MousePos;
	int          result = 0;

	if (hovered)
	{
		float u = (mouse.x - pos.x) / (w > 1.0f ? w : 1.0f);
		float v = (mouse.y - pos.y) / (h > 1.0f ? h : 1.0f);

		u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
		v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
		if (out_u)
			*out_u = u;
		if (out_v)
			*out_v = v;

		// the press takes effect at once and the drag keeps following it
		if (ImGui::IsMouseDown (ImGuiMouseButton_Left))
			result = 1;

		ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
	}

	ImDrawList *dl = ImGui::GetWindowDrawList ();

	dl->AddRect (pos, ImVec2 (pos.x + w, pos.y + h), IM_COL32 (150, 160, 185, 255));

	if (hovered)
	{
		const ImU32 col = IM_COL32 (255, 230, 150, 230);

		dl->AddLine (ImVec2 (mouse.x - 8.0f, mouse.y), ImVec2 (mouse.x + 8.0f, mouse.y), col, 1.5f);
		dl->AddLine (ImVec2 (mouse.x, mouse.y - 8.0f), ImVec2 (mouse.x, mouse.y + 8.0f), col, 1.5f);
		dl->AddCircle (mouse, 4.0f, col, 16, 1.5f);
	}

	ImGui::PopID ();
	return result;
}

static bool PointInPoly (const float (*uv)[2], int count, float u, float v)
{
	bool inside = false;

	for (int p = 0, q = count - 1; p < count; q = p++)
	{
		const float pu = uv[p][0], pv = uv[p][1];
		const float qu = uv[q][0], qv = uv[q][1];

		if (((pv > v) != (qv > v)) && (u < (qu - pu) * (v - pv) / (qv - pv) + pu))
			inside = !inside;
	}
	return inside;
}

int QR_GUI_PolygonEdit (const char *id, int64_t texture, int tex_w, int tex_h,
                        float (*uv)[2], int *count, int max_count)
{
	ImGui::PushID (id);

	int points = count ? *count : 0;

	if (tex_w <= 0 || tex_h <= 0 || !uv || points < 3 || max_count < points)
	{
		ImGui::PopID ();
		return 0;
	}
	if (max_count > 32)
		max_count = 32;

	static int     drag_index = -1;
	static int     drag_edge = -1;
	static bool    drag_all = false;
	static ImGuiID drag_item = 0;
	static float   drag_anchor[2] = {0.0f, 0.0f};
	static float   drag_base[32][2];
	static float   drag_edge_base[2][2];
	static int     drag_base_count = 0;

	float w = ImGui::GetContentRegionAvail ().x;

	if (w < 32.0f)
		w = 32.0f;

	const float  grab = 18.0f;
	const float  h = w * (float)tex_h / (float)tex_w;
	const ImVec2 pos = ImGui::GetCursorScreenPos ();
	const ImVec2 mouse = ImGui::GetIO ().MousePos;

	ImGui::Image ((ImTextureID)(uint64_t)texture, ImVec2 (w, h), ImVec2 (0.0f, 0.0f), ImVec2 (1.0f, 1.0f));

	ImGui::SetCursorScreenPos (pos);
	ImGui::InvisibleButton ("##mask", ImVec2 (w, h),
	                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

	const ImGuiID item = ImGui::GetItemID ();
	int           changed = 0;
	int           hover = -1;

	if (ImGui::IsItemHovered ())
	{
		for (int p = 0; p < points; p++)
		{
			const float px = pos.x + uv[p][0] * w;
			const float py = pos.y + uv[p][1] * h;

			if (mouse.x >= px - grab && mouse.x <= px + grab && mouse.y >= py - grab && mouse.y <= py + grab)
			{
				hover = p;
				break;
			}
		}
		ImGui::SetMouseCursor (hover >= 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
	}

	int hover_edge = -1;

	if (ImGui::IsItemHovered () && hover < 0)
	{
		float best = 9.0f;

		for (int p = 0; p < points; p++)
		{
			const int   q = (p + 1) % points;
			const float ax = pos.x + uv[p][0] * w, ay = pos.y + uv[p][1] * h;
			const float bx = pos.x + uv[q][0] * w, by = pos.y + uv[q][1] * h;
			const float dx = bx - ax, dy = by - ay;
			const float len2 = dx * dx + dy * dy;
			float       t = 0.0f;
			float       ex, ey, d;

			if (len2 > 1e-8f)
				t = ClampF (((mouse.x - ax) * dx + (mouse.y - ay) * dy) / len2, 0.0f, 1.0f);
			ex = ax + t * dx;
			ey = ay + t * dy;
			d = sqrtf ((mouse.x - ex) * (mouse.x - ex) + (mouse.y - ey) * (mouse.y - ey));

			if (d < best)
			{
				best = d;
				hover_edge = p;
			}
		}

		if (hover_edge >= 0)
			ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
	}

	const float u = ClampF ((mouse.x - pos.x) / (w > 1.0f ? w : 1.0f), 0.0f, 1.0f);
	const float v = ClampF ((mouse.y - pos.y) / (h > 1.0f ? h : 1.0f), 0.0f, 1.0f);
	const bool  inside = PointInPoly (uv, points, u, v);

	if (ImGui::IsItemActivated ())
	{
		if (ImGui::GetIO ().MouseDown[ImGuiMouseButton_Right])
		{
			if (hover >= 0 && points > 3)
			{
				for (int p = hover; p + 1 < points; p++)
				{
					uv[p][0] = uv[p + 1][0];
					uv[p][1] = uv[p + 1][1];
				}
				points--;
				*count = points;
				changed = 1;
			}
		}
		else if (hover >= 0)
		{
			drag_index = hover;
			drag_edge = -1;
			drag_all = false;
			drag_item = item;
		}
		else if (ImGui::GetIO ().KeyCtrl && points < max_count)
		{
			int   best = 1;
			float best_d = FLT_MAX;

			for (int p = 0; p < points; p++)
			{
				const int   q = (p + 1) % points;
				const float ax = uv[p][0], ay = uv[p][1];
				const float bx = uv[q][0], by = uv[q][1];
				const float dx = bx - ax, dy = by - ay;
				const float len2 = dx * dx + dy * dy;
				float       t = 0.0f;
				float       ex, ey, d;

				if (len2 > 1e-8f)
					t = ClampF (((u - ax) * dx + (v - ay) * dy) / len2, 0.0f, 1.0f);
				ex = ax + t * dx;
				ey = ay + t * dy;
				d = (u - ex) * (u - ex) + (v - ey) * (v - ey);

				if (d < best_d)
				{
					best_d = d;
					best = q;
				}
			}

			for (int p = points; p > best; p--)
			{
				uv[p][0] = uv[p - 1][0];
				uv[p][1] = uv[p - 1][1];
			}
			uv[best][0] = u;
			uv[best][1] = v;
			points++;
			*count = points;
			drag_index = best;
			drag_edge = -1;
			drag_all = false;
			drag_item = item;
			changed = 1;
		}
		else if (hover_edge >= 0)
		{
			const int q = (hover_edge + 1) % points;

			drag_edge = hover_edge;
			drag_edge_base[0][0] = uv[hover_edge][0];
			drag_edge_base[0][1] = uv[hover_edge][1];
			drag_edge_base[1][0] = uv[q][0];
			drag_edge_base[1][1] = uv[q][1];
			drag_anchor[0] = u;
			drag_anchor[1] = v;
			drag_index = -1;
			drag_all = false;
			drag_item = item;
		}
		else if (inside)
		{
			drag_anchor[0] = u;
			drag_anchor[1] = v;
			drag_base_count = points;
			for (int p = 0; p < points; p++)
			{
				drag_base[p][0] = uv[p][0];
				drag_base[p][1] = uv[p][1];
			}
			drag_index = -1;
			drag_edge = -1;
			drag_all = true;
			drag_item = item;
		}
	}

	if (drag_item == item && drag_all)
	{
		if (ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left))
		{
			const float du = u - drag_anchor[0];
			const float dv = v - drag_anchor[1];

			for (int p = 0; p < drag_base_count && p < points; p++)
			{
				const float nu = ClampF (drag_base[p][0] + du, 0.0f, 1.0f);
				const float nv = ClampF (drag_base[p][1] + dv, 0.0f, 1.0f);

				if (uv[p][0] != nu || uv[p][1] != nv)
				{
					uv[p][0] = nu;
					uv[p][1] = nv;
					changed = 1;
				}
			}
		}
		else
		{
			drag_all = false;
			drag_item = 0;
		}
	}
	else if (drag_item == item && drag_edge >= 0 && drag_edge < points)
	{
		if (ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left))
		{
			const int   q = (drag_edge + 1) % points;
			const float du = u - drag_anchor[0];
			const float dv = v - drag_anchor[1];
			const float a0 = ClampF (drag_edge_base[0][0] + du, 0.0f, 1.0f);
			const float b0 = ClampF (drag_edge_base[0][1] + dv, 0.0f, 1.0f);
			const float a1 = ClampF (drag_edge_base[1][0] + du, 0.0f, 1.0f);
			const float b1 = ClampF (drag_edge_base[1][1] + dv, 0.0f, 1.0f);

			if (uv[drag_edge][0] != a0 || uv[drag_edge][1] != b0)
			{
				uv[drag_edge][0] = a0;
				uv[drag_edge][1] = b0;
				changed = 1;
			}
			if (uv[q][0] != a1 || uv[q][1] != b1)
			{
				uv[q][0] = a1;
				uv[q][1] = b1;
				changed = 1;
			}
		}
		else
		{
			drag_edge = -1;
			drag_item = 0;
		}
	}
	else if (drag_item == item && drag_index >= 0 && drag_index < points)
	{
		if (ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left))
		{
			if (uv[drag_index][0] != u || uv[drag_index][1] != v)
			{
				uv[drag_index][0] = u;
				uv[drag_index][1] = v;
				changed = 1;
			}
		}
		else
		{
			drag_index = -1;
			drag_item = 0;
		}
	}

	ImDrawList *dl = ImGui::GetWindowDrawList ();
	ImVec2      pts[32];

	for (int p = 0; p < points; p++)
		pts[p] = ImVec2 (pos.x + uv[p][0] * w, pos.y + uv[p][1] * h);

	dl->AddConcavePolyFilled (pts, points, IM_COL32 (255, 255, 255, 128));
	dl->AddPolyline (pts, points, IM_COL32 (255, 255, 255, 220), ImDrawFlags_Closed, 1.5f);

	{
		const int edge = (drag_item == item && drag_edge >= 0) ? drag_edge : hover_edge;

		if (edge >= 0 && edge < points)
		{
			const int q = (edge + 1) % points;

			dl->AddLine (pts[edge], pts[q], IM_COL32 (255, 210, 120, 255), 2.5f);
		}
	}

	for (int p = 0; p < points; p++)
	{
		const ImU32 fill = (p == hover || (drag_item == item && p == drag_index))
		                       ? IM_COL32 (255, 210, 120, 255)
		                       : IM_COL32 (255, 255, 255, 255);

		dl->AddCircleFilled (pts[p], 9.0f, fill);
		dl->AddCircle (pts[p], 9.0f, IM_COL32 (0, 0, 0, 180), 16, 1.5f);
	}

	dl->AddRect (pos, ImVec2 (pos.x + w, pos.y + h), IM_COL32 (150, 160, 185, 255));

	ImGui::PopID ();
	return changed;
}

int QR_GUI_ColorRow (const char *id, float rgb[3], const char *tooltip)
{
	int result = 0;

	ImGui::PushID (id);
	ImGui::SetNextItemWidth (ImGui::GetContentRegionAvail ().x - kBrowseButtonW - ImGui::GetStyle ().ItemSpacing.x);
	if (ImGui::ColorEdit3 ("##color", rgb, ImGuiColorEditFlags_DisplayHex))
		result |= 1;
	if (tooltip && *tooltip)
		ImGui::SetItemTooltip ("%s\nClick the swatch to pick, type the hex value", tooltip);
	else
		ImGui::SetItemTooltip ("Click the swatch to pick, type the hex value");
	ImGui::SameLine ();
	if (ImGui::Button ("x", ImVec2 (kBrowseButtonW, 0.0f)))
		result |= 2;
	ImGui::SetItemTooltip ("Remove this color");
	ImGui::PopID ();

	return result;
}

int QR_GUI_Dialog (const char *title, const char *text, const char *yes, const char *no)
{
	const ImVec2 center (ImGui::GetIO ().DisplaySize.x * 0.5f, ImGui::GetIO ().DisplaySize.y * 0.5f);
	int          result = 0;

	ImGui::SetNextWindowPos (center, ImGuiCond_Always, ImVec2 (0.5f, 0.5f));
	ImGui::SetNextWindowSize (ImVec2 (440.0f, 0.0f), ImGuiCond_Always);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
	                               ImGuiWindowFlags_AlwaysAutoResize;

	if (ImGui::Begin (title, nullptr, flags))
	{
		ImGui::TextWrapped ("%s", text);
		ImGui::Spacing ();
		if (ImGui::Button (yes, ImVec2 (150.0f, 0.0f)))
			result = 1;
		ImGui::SameLine ();
		if (ImGui::Button (no, ImVec2 (150.0f, 0.0f)))
			result = 2;
	}
	ImGui::End ();

	return result;
}

void QR_GUI_Notify (const char *text)
{
	snprintf (g_notify, sizeof (g_notify), "%s", text ? text : "");
	g_notify_time = ImGui::GetTime ();
}

int QR_GUI_DialogCentered (const char *title, const char *text, const char *yes, const char *no)
{
	const ImVec2 center (ImGui::GetIO ().DisplaySize.x * 0.5f, ImGui::GetIO ().DisplaySize.y * 0.5f);
	int          result = 0;

	ImGui::SetNextWindowPos (center, ImGuiCond_Always, ImVec2 (0.5f, 0.5f));
	ImGui::SetNextWindowSize (ImVec2 (760.0f, 0.0f), ImGuiCond_Always);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
	                               ImGuiWindowFlags_AlwaysAutoResize;

	if (ImGui::Begin (title, nullptr, flags))
	{
		ImGui::SetWindowFontScale (1.5f);

		const ImGuiStyle &style = ImGui::GetStyle ();
		const float       avail = ImGui::GetContentRegionAvail ().x;
		const float       line = ImGui::CalcTextSize (text).x;
		const float       bw = 220.0f;
		const float       buttons = bw * 2.0f + style.ItemSpacing.x;

		if (line < avail)
		{
			ImGui::SetCursorPosX ((avail - line) * 0.5f);
			ImGui::TextUnformatted (text);
		}
		else
		{
			ImGui::TextWrapped ("%s", text);
		}

		ImGui::Spacing ();
		ImGui::Spacing ();

		if (buttons < ImGui::GetContentRegionAvail ().x)
			ImGui::SetCursorPosX ((ImGui::GetContentRegionAvail ().x - buttons) * 0.5f);

		if (ImGui::Button (yes, ImVec2 (bw, 0.0f)))
			result = 1;
		ImGui::SameLine ();
		if (ImGui::Button (no, ImVec2 (bw, 0.0f)))
			result = 2;

		ImGui::SetWindowFontScale (1.0f);
	}
	ImGui::End ();

	return result;
}

int QR_GUI_DialogVertical (const char *title, const char *text, const char *first, const char *second, const char *third)
{
	const ImVec2 center (ImGui::GetIO ().DisplaySize.x * 0.5f, ImGui::GetIO ().DisplaySize.y * 0.5f);
	int          result = 0;

	ImGui::SetNextWindowPos (center, ImGuiCond_Always, ImVec2 (0.5f, 0.5f));
	ImGui::SetNextWindowSize (ImVec2 (760.0f, 0.0f), ImGuiCond_Always);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
	                               ImGuiWindowFlags_AlwaysAutoResize;

	if (ImGui::Begin (title, nullptr, flags))
	{
		ImGui::SetWindowFontScale (1.5f);

		const float avail = ImGui::GetContentRegionAvail ().x;
		const float line = ImGui::CalcTextSize (text).x;
		const float bw = 220.0f;

		if (line < avail)
		{
			ImGui::SetCursorPosX ((avail - line) * 0.5f);
			ImGui::TextUnformatted (text);
		}
		else
		{
			ImGui::TextWrapped ("%s", text);
		}

		ImGui::Spacing ();
		ImGui::Spacing ();

		if (bw < ImGui::GetContentRegionAvail ().x)
			ImGui::SetCursorPosX ((ImGui::GetContentRegionAvail ().x - bw) * 0.5f);
		if (ImGui::Button (first, ImVec2 (bw, 0.0f)))
			result = 1;

		if (bw < ImGui::GetContentRegionAvail ().x)
			ImGui::SetCursorPosX ((ImGui::GetContentRegionAvail ().x - bw) * 0.5f);
		if (ImGui::Button (second, ImVec2 (bw, 0.0f)))
			result = 2;

		ImGui::Spacing ();

		if (third)
		{
			if (bw < ImGui::GetContentRegionAvail ().x)
				ImGui::SetCursorPosX ((ImGui::GetContentRegionAvail ().x - bw) * 0.5f);
			if (ImGui::Button (third, ImVec2 (bw, 0.0f)))
				result = 3;
		}

		ImGui::SetWindowFontScale (1.0f);
	}
	ImGui::End ();

	return result;
}

void QR_GUI_DrawCrosshair (void)
{
	ImDrawList  *dl = ImGui::GetForegroundDrawList ();
	const ImVec2 c (ImGui::GetIO ().DisplaySize.x * 0.5f, ImGui::GetIO ().DisplaySize.y * 0.5f);

	const float  gap = 4.0f;
	const float  len = 7.0f;
	const float  th  = 1.6f;
	const ImU32  shadow = IM_COL32 (0, 0, 0, 130);
	const ImU32  col = IM_COL32 (255, 255, 255, 225);

	const ImVec2 a0 (c.x - gap - len, c.y), a1 (c.x - gap, c.y);
	const ImVec2 b0 (c.x + gap, c.y), b1 (c.x + gap + len, c.y);
	const ImVec2 c0 (c.x, c.y - gap - len), c1 (c.x, c.y - gap);
	const ImVec2 d0 (c.x, c.y + gap), d1 (c.x, c.y + gap + len);

	dl->AddLine (a0, a1, shadow, th + 2.0f);
	dl->AddLine (b0, b1, shadow, th + 2.0f);
	dl->AddLine (c0, c1, shadow, th + 2.0f);
	dl->AddLine (d0, d1, shadow, th + 2.0f);

	dl->AddLine (a0, a1, col, th);
	dl->AddLine (b0, b1, col, th);
	dl->AddLine (c0, c1, col, th);
	dl->AddLine (d0, d1, col, th);
}

void QR_GUI_DrawHint (const char *const *lines, int count)
{
	ImDrawList *dl = ImGui::GetForegroundDrawList ();

	const float pad = 14.0f;
	const float line_height = ImGui::GetTextLineHeight () + 3.0f;
	ImVec2      pos (pad, ImGui::GetIO ().DisplaySize.y - pad - line_height * (float)count);

	for (int i = 0; i < count; i++)
	{
		const ImU32 col = (i == 0) ? IM_COL32 (140, 205, 255, 235) : IM_COL32 (228, 234, 244, 220);

		dl->AddText (ImVec2 (pos.x + 1.0f, pos.y + 1.0f), IM_COL32 (0, 0, 0, 160), lines[i]);
		dl->AddText (pos, col, lines[i]);

		pos.y += line_height;
	}
}

// One line in the bottom-right corner: the placement prompts of the light
// editor, where the bottom-left block would sit far from the cursor mode's
// panel.
void QR_GUI_LabelBottomRight (const char *text)
{
	ImDrawList *dl = ImGui::GetForegroundDrawList ();

	const float  pad = 14.0f;
	const ImVec2 size = ImGui::CalcTextSize (text);
	const ImVec2 pos (ImGui::GetIO ().DisplaySize.x - pad - size.x,
	                  ImGui::GetIO ().DisplaySize.y - pad - size.y);

	dl->AddText (ImVec2 (pos.x + 1.0f, pos.y + 1.0f), IM_COL32 (0, 0, 0, 160), text);
	dl->AddText (pos, IM_COL32 (255, 230, 150, 235), text);
}

// The cursor position ImGui last saw, for the editor's own hit tests (the axis
// gizmo).
void QR_GUI_GetMousePos (float *x, float *y)
{
	const ImVec2 pos = ImGui::GetIO ().MousePos;

	if (x)
		*x = pos.x;
	if (y)
		*y = pos.y;
}

void QR_GUI_DrawPolyline (const float *xy, int count, uint32_t argb, float thickness)
{
	if (!xy || count < 2)
		return;

	ImDrawList *dl = ImGui::GetBackgroundDrawList ();
	const ImU32 col = PackedColorToU32 (argb);
	const float th = thickness > 0.0f ? thickness : 1.0f;

	for (int i = 0; i + 1 < count; i++)
	{
		const ImVec2 a (xy[i * 2], xy[i * 2 + 1]);
		const ImVec2 b (xy[(i + 1) * 2], xy[(i + 1) * 2 + 1]);

		dl->AddLine (a, b, col, th);
	}
}

void QR_GUI_DrawCircle (float cx, float cy, float radius, uint32_t argb, float thickness)
{
	if (radius <= 0.0f)
		return;

	ImGui::GetBackgroundDrawList ()->AddCircle (ImVec2 (cx, cy), radius, PackedColorToU32 (argb), 0,
	                                            thickness > 0.0f ? thickness : 1.0f);
}

// ----- the rt_stats readout -----

namespace
{

constexpr float kOverlayLabelW = 108.0f;
constexpr float kOverlayValueW = 72.0f;
constexpr float kOverlayGraphW = 118.0f;
constexpr float kOverlayGraphH = 13.0f;
constexpr float kOverlayGapX   = 6.0f;
constexpr float kOverlayRowW   = kOverlayLabelW + kOverlayValueW + kOverlayGapX + kOverlayGraphW;

bool g_overlay_group = false;
bool g_overlay_first = true;

void OverlaySparkline (ImDrawList *dl, const ImVec2 &p, float w, float h, const float *v, int n, ImU32 col)
{
	dl->AddRectFilled (p, ImVec2 (p.x + w, p.y + h), IM_COL32 (0, 0, 0, 110), 2.0f);

	float vmax = 0.0f;
	for (int i = 0; i < n; i++)
		if (v[i] > vmax)
			vmax = v[i];

	if (vmax < 1e-4f)
		vmax = 1e-4f;
	vmax *= 1.15f;

	const float x0 = p.x + 1.0f, x1 = p.x + w - 1.0f;
	const float y0 = p.y + 1.0f, y1 = p.y + h - 1.0f;
	const float dy = y1 - y0;

	ImVec2 pts[128];
	if (n > (int)(sizeof (pts) / sizeof (pts[0])))
		n = (int)(sizeof (pts) / sizeof (pts[0]));

	for (int i = 0; i < n; i++)
	{
		const float t = (float)i / (float)(n - 1);
		pts[i] = ImVec2 (x0 + t * (x1 - x0), y1 - (v[i] / vmax) * dy);
	}

	const ImU32 fill = (col & 0x00FFFFFFu) | 0x28000000u;
	for (int i = 1; i < n; i++)
		dl->AddQuadFilled (ImVec2 (pts[i - 1].x, y1), pts[i - 1], pts[i], ImVec2 (pts[i].x, y1), fill);

	dl->AddPolyline (pts, n, col, ImDrawFlags_None, 1.0f);
}

}

void QR_GUI_OverlayBegin (const char *id, float x, float y, float alpha, const char *title)
{
	if (!g_ready)
		return;

	ImGui::SetNextWindowPos (ImVec2 (x, y), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha (alpha);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
	                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
	                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;

	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (8.0f, 6.0f));
	ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (6.0f, 2.0f));
	if (g_stats_font)
		ImGui::PushFont (g_stats_font, 14.0f);
	else
		ImGui::PushFont (nullptr, 14.0f);

	ImGui::Begin (id, nullptr, flags);

	if (title && title[0])
		ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.25f, 1.0f), "%s", title);

	g_overlay_group = true;
	g_overlay_first = true;
	ImGui::BeginGroup ();
}

void QR_GUI_OverlaySection (const char *title)
{
	if (!g_ready)
		return;

	if (g_overlay_group)
	{
		if (!g_overlay_first)
		{
			ImGui::EndGroup ();
			ImGui::SameLine ();
			ImGui::BeginGroup ();
		}
		g_overlay_first = false;
	}

	if (title && title[0])
	{
		ImGui::Dummy (ImVec2 (0.0f, 2.0f));
		ImGui::TextColored (ImVec4 (0.62f, 0.78f, 0.92f, 1.0f), "%s", title);
		ImGui::Dummy (ImVec2 (0.0f, 1.0f));
	}
}

void QR_GUI_OverlayRow (const char *label, const char *value, const float *samples, int count, uint32_t color)
{
	if (!g_ready)
		return;

	ImDrawList  *dl = ImGui::GetWindowDrawList ();
	const float  line_h = ImGui::GetTextLineHeight ();
	const float  row_h = line_h > kOverlayGraphH + 4.0f ? line_h : kOverlayGraphH + 4.0f;
	const ImVec2 pos = ImGui::GetCursorScreenPos ();
	const ImU32  col = PackedColorToU32 (color);

	ImGui::Dummy (ImVec2 (kOverlayRowW, row_h));

	dl->AddText (pos, col, label ? label : "");

	if (value && value[0])
	{
		const ImVec2 ts = ImGui::CalcTextSize (value);
		dl->AddText (ImVec2 (pos.x + kOverlayLabelW + kOverlayValueW - ts.x, pos.y), col, value);
	}

	if (samples && count >= 2)
	{
		const ImVec2 graph (pos.x + kOverlayLabelW + kOverlayValueW + kOverlayGapX,
		                    pos.y + (row_h - kOverlayGraphH) * 0.5f);
		OverlaySparkline (dl, graph, kOverlayGraphW, kOverlayGraphH, samples, count, col);
	}
}

void QR_GUI_OverlayNote (const char *text)
{
	if (!g_ready)
		return;

	ImGui::TextDisabled ("%s", text ? text : "");
}

void QR_GUI_OverlayEnd (void)
{
	if (!g_ready)
		return;

	if (g_overlay_group)
	{
		ImGui::EndGroup ();
		g_overlay_group = false;
	}

	ImGui::End ();
	ImGui::PopFont ();
	ImGui::PopStyleVar (2);
}
