// qr_gui.h -- Dear ImGui bridge for the qr light editor.
//
// The host stays C: this header exposes ImGui as a small function set and the
// panel is built from qr_editor.c. The C++ side (qr_gui.cpp) owns the ImGui
// context, the SDL2 input backend, the style and fonts, and the render
// backend: ImGui draw lists are uploaded through qrUploadRasterizedGeometry
// (SWAPCHAIN render type) with per-draw scissor, so no ImGui Vulkan pipelines
// or descriptor pools exist next to the renderer.

#ifndef QR_GUI_H
#define QR_GUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates the ImGui context, the SDL2 input backend and the font atlas.
// qr_instance is the QrInstance the draw lists are uploaded to; font_path may
// be NULL, the default ImGui font is used then.
void QR_GUI_Init (void *sdl_window, void *qr_instance, void *font_data, int font_size);
int  QR_GUI_Ready (void);
void QR_GUI_Shutdown (void);

// One ImGui frame. SCR_UpdateScreen can run more than once per host frame, so
// the frame id guards against a second NewFrame; returns 0 when the id was
// already started. The viewport is the drawable rect in GL convention
// (x, y, width, height) plus the height of the whole drawable (for the Vulkan
// Y flip of the viewport).
int  QR_GUI_BeginFrame (unsigned int frame_id, float dt, int x, int y, int width, int height, int drawable_height);

// Builds the draw data and uploads it to the rasterizer. Must be called in the
// same frame as QR_GUI_BeginFrame.
void QR_GUI_EndFrame (void);

// Feeds one SDL_Event (as const void *) into ImGui, returns 1 if the GUI wants
// the event and the engine must not handle it.
int  QR_GUI_ProcessEvent (const void *sdl_event);

// Returns 1 while the GUI owns the mouse (the panel is open).
int  QR_GUI_WantsMouse (void);
// Returns 1 while a GUI text field has keyboard focus.
int  QR_GUI_WantsKeyboard (void);
// Tells ImGui to draw the mouse cursor itself.
void QR_GUI_SetMouseCursor (int enable);

// ----- the panel -----

void QR_GUI_BeginPanel (const char *id, int x, int y, int width, int height);
void QR_GUI_EndPanel (void);

void QR_GUI_Backdrop (float alpha);
int  QR_GUI_BeginDialog (const char *title, float width);
void QR_GUI_EndDialog (void);

int  QR_GUI_Canvas (const char *id, float width, float height, float *out_x, float *out_y);
void QR_GUI_CanvasLine (float x0, float y0, float x1, float y1, uint32_t argb, float thickness);
void QR_GUI_CanvasRect (float x0, float y0, float x1, float y1, uint32_t argb);
void QR_GUI_CanvasCircle (float cx, float cy, float radius, uint32_t argb, float thickness, int filled);
void QR_GUI_CanvasText (float x, float y, uint32_t argb, const char *text);
void QR_GUI_BeginScroll (void);
void QR_GUI_EndScroll (void);

void QR_GUI_Label (const char *text);
void QR_GUI_LabelDim (const char *text);
void QR_GUI_LabelRight (const char *text);
void QR_GUI_Separator (void);
void QR_GUI_SectionHeader (const char *label);
void QR_GUI_SectionTitle (const char *label);
void QR_GUI_Spacing (void);
void QR_GUI_SameLine (void);
void QR_GUI_Tooltip (const char *text);
// Widens the padding of the dialog opened between these two calls.
void QR_GUI_PushWindowPadding (float x, float y);
void QR_GUI_PopWindowPadding (void);
// Width of a string in the current font.
float QR_GUI_TextWidth (const char *text);
// 1 while the left or right Ctrl key is held (the canvas' modifier clicks).
int  QR_GUI_CtrlDown (void);

// Every widget carries its parameter's tooltip (shown after the panel's hover
// delay); pass NULL for widgets that have none.
int  QR_GUI_Button (const char *label);
int  QR_GUI_Checkbox (const char *label, int *value, const char *tooltip);
int  QR_GUI_CheckboxMixed (const char *label, int *value, int mixed, const char *tooltip);
int  QR_GUI_SliderFloat (const char *label, float *value, float min, float max, const char *tooltip);
int  QR_GUI_SliderFloatFmt (const char *label, float *value, float min, float max, const char *format, const char *tooltip);
int  QR_GUI_SliderFloatMixed (const char *label, float *value, float min, float max, int mixed, const char *tooltip);
int  QR_GUI_SliderInt (const char *label, int *value, int min, int max, const char *tooltip);
// items are count NUL-terminated strings.
int  QR_GUI_Combo (const char *label, int *value, const char *const *items, int count, const char *tooltip);
// A row of equal-width tabs; a click on another one selects it. Returns 1 when
// the selection changed (the caller draws the content of *selected itself).
int  QR_GUI_Tabs (const char *id, const char *const *items, int count, int *selected, int *reset_pressed);
int  QR_GUI_InputText (const char *label, char *buf, size_t capacity, const char *tooltip);
// A row of three floats (X, Y, Z) on one line, clamped to min..max. Returns 1
// when any of them changed.
int  QR_GUI_Vec3Input (const char *label, float v[3], float min, float max, const char *tooltip);
int  QR_GUI_Vec3InputMixed (const char *label, float v[3], float min, float max, const unsigned char mixed[3], const char *tooltip);
// A path field with a "..." button: returns 1 when the text changed and 2 when
// the browse button was pressed (both can be set: 3). An empty path shows NONE.
int  QR_GUI_TexturePath (const char *label, char *buf, size_t capacity, const char *tooltip);
// An enabled checkbox and a color editor with a hex field. Returns 1 if either
// changed.
int  QR_GUI_ColorHex (const char *label, float rgb[3], int *enabled, const char *tooltip);
int  QR_GUI_ColorHexMixed (const char *label, float rgb[3], int *enabled, int mixed, const char *tooltip);
// A row of the color_emissive list: a swatch/hex editor with a remove button.
// Returns 1 when the color changed and 2 when remove was pressed (3 = both).
int  QR_GUI_ColorRow (const char *id, float rgb[3], const char *tooltip);
// A square button with a circular arrow, right-aligned in the current row.
// Returns 1 when pressed; drawn grayed out while enabled is 0.
int  QR_GUI_ResetButton (const char *label, int enabled);
// Grays out (and blocks) the widgets drawn between these two calls.
void QR_GUI_PushDisabled (int disabled);
void QR_GUI_PopDisabled (void);
// Returns nonzero while the section is open.
int  QR_GUI_Section (const char *label, int default_open);
int  QR_GUI_SectionSelected (const char *label, int selected);

// ID scope for the widgets of one material (animation frames share the same
// parameter names, so their widgets would collide without it).
void QR_GUI_PushID (const char *id);
void QR_GUI_PopID (void);

// A small centerd yes/no dialog drawn on top of the editor. Returns 1 for the
// first button, 2 for the second and 0 while it is up.
int  QR_GUI_Dialog (const char *title, const char *text, const char *yes, const char *no);
int  QR_GUI_DialogCentered (const char *title, const char *text, const char *yes, const char *no);
int  QR_GUI_DialogVertical (const char *title, const char *text, const char *first, const char *second, const char *third);

// A texture preview drawn at the current cursor position (texture is an
// QrMaterial handle). While the left mouse button is held over it, returns 1
// and fills out_u/out_v with the cursor's normalized position (0..1).
int  QR_GUI_ImagePick (const char *id, int64_t texture, int tex_w, int tex_h, float *out_u, float *out_v);
int  QR_GUI_PolygonEdit (const char *id, int64_t texture, int tex_w, int tex_h, float (*uv)[2], int *count, int max_count);
// 1 while any ImGui item is being dragged or edited.
int  QR_GUI_AnyItemActive (void);

// ----- the rt_stats readout -----

void QR_GUI_OverlayBegin (const char *id, float x, float y, float alpha, const char *title);
void QR_GUI_OverlayBeginBottom (const char *id, float x, float bottom_margin, float alpha, const char *title);
void QR_GUI_OverlaySection (const char *title);
void QR_GUI_OverlayEnd (void);
void QR_GUI_OverlayRow (const char *label, const char *value, const float *samples, int count, uint32_t color);
void QR_GUI_OverlayBudgetRow (const char *label, const char *value, const float *samples, int count, float warn_ms, float crit_ms);
void QR_GUI_OverlayNote (const char *text);

// A short message shown in the corner of the editor interface (Apply/Cancel
// confirmations, errors). Fades out on its own.
void QR_GUI_Notify (const char *text);
void QR_GUI_DrawCrosshair (void);

// ----- the flying-mode overlay -----

// Hint lines in the bottom-left corner, drawn with a soft shadow.
void QR_GUI_DrawHint (const char *const *lines, int count);
// One line in the bottom-right corner (the light editor's placement prompts).
void QR_GUI_LabelBottomRight (const char *text);
// The cursor position ImGui last saw (the editor's own hit tests).
void QR_GUI_GetMousePos (float *x, float *y);

void QR_GUI_DrawPolyline (const float *xy, int count, uint32_t argb, float thickness);
void QR_GUI_DrawCircle (float cx, float cy, float radius, uint32_t argb, float thickness);

#ifdef __cplusplus
}
#endif

#endif /* QR_GUI_H */
