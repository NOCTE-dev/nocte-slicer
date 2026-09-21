// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The NØCTE palette, in one place.
//
// ADR-003 replaces OrcaSlicer's teal (#009688 and its derivatives) and green (#00AE42) with a
// monochrome scale: pure black, pure white and neutral greys. The palette is changed at its
// central points — StateColor::gDarkColors, ColorRGB(A)::ORCA(), the ImGui palette, the SVG
// replace map in BitmapCache, the toolbar tints in GLTexture, the 3D background in GLCanvas3D
// and the top bar — rather than at every call site, so those files take value-only edits and
// every number they use is defined here.
//
// The accent is white on a dark surface and near-black on a light one, so there is no single
// "accent colour": NOCTE_ACCENT is the dark-theme accent, NOCTE_ACCENT_ON_LIGHT the light-theme
// one. Where an accent is used as a *surface* that upstream paints white text on (the confirm
// button, the switch track), the surface must stay dark enough for that text to read, which is
// what NOCTE_ACCENT_SURFACE is for.
//
// Header only; no CMake entry is needed. Each constant is given both as a wxColour-ready hex
// string and, where a float colour is needed, as a 0..1 triple.

#ifndef slic3r_GUI_Nocte_NocteTheme_hpp_
#define slic3r_GUI_Nocte_NocteTheme_hpp_

namespace Slic3r {
namespace GUI {
namespace Nocte {

/*--- Surfaces ---------------------------------------------------------------------------*/

inline constexpr const char* NOCTE_BG_DARK       = "#0F0F10"; // deepest surface: 3D viewport, top bar
inline constexpr const char* NOCTE_BG_PANEL      = "#1A1A1C"; // window and panel background (dark)
inline constexpr const char* NOCTE_BG_RAISED     = "#222226"; // side bar title bars, gradients (dark)
inline constexpr const char* NOCTE_BG_LIGHT      = "#E8E8E9"; // 3D viewport and page background (light)

/*--- Lines and text ---------------------------------------------------------------------*/

inline constexpr const char* NOCTE_BORDER        = "#3A3A3E"; // input, combo and static-box borders
inline constexpr const char* NOCTE_TEXT          = "#F2F2F2"; // body text on a dark surface
inline constexpr const char* NOCTE_TEXT_MUTED    = "#9A9A9E"; // secondary text, captions
inline constexpr const char* NOCTE_NEUTRAL       = "#8C8C90"; // reads on both themes: gizmo and
                                                              // measure highlights, ImGui accents

/*--- Accent -----------------------------------------------------------------------------*/

inline constexpr const char* NOCTE_ACCENT          = "#FFFFFF"; // accent on a dark surface
inline constexpr const char* NOCTE_ACCENT_ON_LIGHT = "#1A1A1C"; // accent on a light surface
inline constexpr const char* NOCTE_ACCENT_HOVER    = "#D0D0D4";
inline constexpr const char* NOCTE_ACCENT_PRESSED  = "#A8A8AC";

// Accent used as a filled surface that carries white text (upstream's #009688 buttons).
inline constexpr const char* NOCTE_ACCENT_SURFACE       = "#55555D";
inline constexpr const char* NOCTE_ACCENT_SURFACE_HOVER = "#6A6A73";

/*--- Selection --------------------------------------------------------------------------*/

inline constexpr const char* NOCTE_SELECTION_BG      = "#2A2A2E"; // checked list item (dark)
inline constexpr const char* NOCTE_SELECTION_BG_SOFT = "#28282C"; // focused combo / dropdown (dark)

/*--- Float triples, for ColorRGB / ColorRGBA / ImVec4 -----------------------------------*/
// Same numbers as above, divided by 255.

inline constexpr float NOCTE_BG_DARK_RGB[3]        = {0.059f, 0.059f, 0.063f}; // #0F0F10
inline constexpr float NOCTE_BG_PANEL_RGB[3]       = {0.102f, 0.102f, 0.110f}; // #1A1A1C
inline constexpr float NOCTE_BG_LIGHT_RGB[3]       = {0.910f, 0.910f, 0.914f}; // #E8E8E9
inline constexpr float NOCTE_BORDER_RGB[3]         = {0.227f, 0.227f, 0.243f}; // #3A3A3E
inline constexpr float NOCTE_TEXT_RGB[3]           = {0.949f, 0.949f, 0.949f}; // #F2F2F2
inline constexpr float NOCTE_TEXT_MUTED_RGB[3]     = {0.604f, 0.604f, 0.620f}; // #9A9A9E
inline constexpr float NOCTE_NEUTRAL_RGB[3]        = {0.549f, 0.549f, 0.565f}; // #8C8C90
inline constexpr float NOCTE_ACCENT_RGB[3]         = {1.000f, 1.000f, 1.000f}; // #FFFFFF
inline constexpr float NOCTE_ACCENT_HOVER_RGB[3]   = {0.816f, 0.816f, 0.831f}; // #D0D0D4
inline constexpr float NOCTE_ACCENT_PRESSED_RGB[3] = {0.659f, 0.659f, 0.675f}; // #A8A8AC
inline constexpr float NOCTE_ACCENT_SURFACE_RGB[3] = {0.333f, 0.333f, 0.365f}; // #55555D
inline constexpr float NOCTE_SELECTION_BG_RGB[3]   = {0.165f, 0.165f, 0.180f}; // #2A2A2E

/*--- Byte triples, for the GLTexture toolbar tints ---------------------------------------*/

inline constexpr unsigned char NOCTE_BG_PANEL_U8[3]   = {26, 26, 28};    // #1A1A1C
inline constexpr unsigned char NOCTE_BORDER_U8[3]     = {58, 58, 62};    // #3A3A3E
inline constexpr unsigned char NOCTE_TEXT_U8[3]       = {242, 242, 242}; // #F2F2F2
inline constexpr unsigned char NOCTE_NEUTRAL_U8[3]    = {140, 140, 144}; // #8C8C90
inline constexpr unsigned char NOCTE_DISABLED_U8[3]   = {200, 200, 200}; // light-theme disabled
inline constexpr unsigned char NOCTE_DISABLED_D_U8[3] = {76, 76, 84};    // dark-theme disabled

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_NocteTheme_hpp_
