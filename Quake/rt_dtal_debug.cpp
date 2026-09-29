#include "rt_dtal_debug.h"
#include "qr_gui.h"

#include <imgui.h>

#include <cmath>

namespace
{

constexpr int kMaxArrows = 1024;

float g_arrows[kMaxArrows * RT_DTAL_DEBUG_FLOATS_PER_ARROW];

}

extern "C" void RT_DtalDebugDrawGui (int mode, unsigned int frame_id, float dt, int fb_x, int fb_y, int fb_w, int fb_h, int drawable_h)
{
	if (mode != 2 || !QR_GUI_Ready ())
		return;

	const bool own_frame = QR_GUI_BeginFrame (frame_id, dt, fb_x, fb_y, fb_w, fb_h, drawable_h) != 0;

	ImDrawList *dl = ImGui::GetBackgroundDrawList ();

	const int count = RT_DtalDebugBuildArrows (g_arrows, kMaxArrows, fb_w, fb_h);

	for (int i = 0; i < count; i++)
	{
		const float *a = &g_arrows[i * RT_DTAL_DEBUG_FLOATS_PER_ARROW];
		const float  x0 = a[0], y0 = a[1], x1 = a[2], y1 = a[3];
		const ImU32  col = ImGui::GetColorU32 (ImVec4 (a[4], a[5], a[6], 1.0f));

		const float dx = x1 - x0, dy = y1 - y0;
		const float len = std::sqrt (dx * dx + dy * dy);

		if (len < 3.0f)
			continue;

		const float ang = std::atan2 (dy, dx);
		const float head = len < 18.0f ? len * 0.5f : 12.0f;
		const float spread = 0.5f;

		dl->AddCircleFilled (ImVec2 (x0, y0), 2.5f, col);
		dl->AddLine (ImVec2 (x0, y0), ImVec2 (x1, y1), col, 2.0f);
		dl->AddLine (ImVec2 (x1, y1), ImVec2 (x1 - head * std::cos (ang - spread), y1 - head * std::sin (ang - spread)), col, 2.0f);
		dl->AddLine (ImVec2 (x1, y1), ImVec2 (x1 - head * std::cos (ang + spread), y1 - head * std::sin (ang + spread)), col, 2.0f);
	}

	if (own_frame)
		QR_GUI_EndFrame ();
}
