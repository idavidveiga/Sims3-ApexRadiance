// The Violet theme (see violet_theme.h).
#include "violet_theme.h"
#include "apex_log.h"
#include "apex_util.h"
#include <windows.h>
#include <initializer_list>
#include <string>

namespace VioletTheme {
namespace {

constexpr float kFontSize = 15.0f; // Segoe UI at 1080p (the overlay's FontScaleMain doubles it at 4K)

ImFont* g_regular = nullptr;
ImFont* g_bold = nullptr;
float g_baseSize = 13.0f; // ImGui's default font

std::wstring FontsFolder() {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"C:\\Windows\\Fonts\\";
    return std::wstring(dir) + L"\\Fonts\\";
}

// First of the files that exists, as UTF-8 (ImGui opens it with _wfopen after converting back); empty when none does.
std::string FindFont(const std::wstring& folder, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* name : names) {
        const std::wstring path = folder + name;
        if (ApexUtil::FileExists(path)) return ApexUtil::ToUtf8(path);
    }
    return {};
}

} // namespace

ImVec4 Col(unsigned rgb, float alpha) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f, static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

void ApplyStyle(ImGuiStyle& style) {
    ImGui::StyleColorsDark(&style); // anything not set below keeps a sane dark value

    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(7.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 14.0f;
    style.ScrollbarSize = 8.0f; // thin
    style.GrabMinSize = 12.0f;  // the slider grab (ui/widgets.cpp draws a circle of this diameter)
    style.DisabledAlpha = 0.5f; // disabled rows dim as a whole
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Col(kText);
    c[ImGuiCol_TextDisabled] = Col(kTextMuted);
    c[ImGuiCol_WindowBg] = Col(kWindowBg);
    c[ImGuiCol_ChildBg] = Col(0, 0.0f); // the sidebar and content areas show the window; cards push their own colour
    c[ImGuiCol_PopupBg] = Col(kHoverBg, 0.98f);
    c[ImGuiCol_Border] = Col(kCardBorder);
    c[ImGuiCol_BorderShadow] = Col(0, 0.0f);
    c[ImGuiCol_FrameBg] = Col(0x26272D);
    c[ImGuiCol_FrameBgHovered] = Col(0x2E2F36);
    c[ImGuiCol_FrameBgActive] = Col(0x34353D);
    c[ImGuiCol_TitleBg] = Col(kWindowBg);
    c[ImGuiCol_TitleBgActive] = Col(kCardBg);
    c[ImGuiCol_TitleBgCollapsed] = Col(kWindowBg);
    c[ImGuiCol_MenuBarBg] = Col(kCardBg);
    c[ImGuiCol_ScrollbarBg] = Col(0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Col(kToggleOff);
    c[ImGuiCol_ScrollbarGrabHovered] = Col(0x4A4B54);
    c[ImGuiCol_ScrollbarGrabActive] = Col(kAccentDark);
    c[ImGuiCol_CheckMark] = Col(kAccent);
    c[ImGuiCol_CheckboxSelectedBg] = Col(0x26272D);
    c[ImGuiCol_SliderGrab] = Col(kAccent);
    c[ImGuiCol_SliderGrabActive] = Col(kAccentLight);
    c[ImGuiCol_Button] = Col(kCardBorder);
    c[ImGuiCol_ButtonHovered] = Col(kAccentDark);
    c[ImGuiCol_ButtonActive] = Col(kAccent);
    c[ImGuiCol_Header] = Col(kSelectedBg);
    c[ImGuiCol_HeaderHovered] = Col(kAccentDark, 0.45f);
    c[ImGuiCol_HeaderActive] = Col(kAccentDark, 0.70f);
    c[ImGuiCol_Separator] = Col(kCardBorder);
    c[ImGuiCol_SeparatorHovered] = Col(kAccentDark);
    c[ImGuiCol_SeparatorActive] = Col(kAccent);
    c[ImGuiCol_ResizeGrip] = Col(kAccent, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = Col(kAccent, 0.60f);
    c[ImGuiCol_ResizeGripActive] = Col(kAccent, 0.90f);
    c[ImGuiCol_InputTextCursor] = Col(kAccentLight);
    c[ImGuiCol_TabHovered] = Col(kAccentDark, 0.80f);
    c[ImGuiCol_Tab] = Col(kSelectedBg);
    c[ImGuiCol_TabSelected] = Col(kAccentDark);
    c[ImGuiCol_TabSelectedOverline] = Col(kAccent);
    c[ImGuiCol_TabDimmed] = Col(kSelectedBg);
    c[ImGuiCol_TabDimmedSelected] = Col(kAccentDark, 0.70f);
    c[ImGuiCol_TabDimmedSelectedOverline] = Col(kAccent, 0.50f);
    c[ImGuiCol_PlotLines] = Col(kAccent);
    c[ImGuiCol_PlotLinesHovered] = Col(kAccentLight);
    c[ImGuiCol_PlotHistogram] = Col(kAccent);
    c[ImGuiCol_PlotHistogramHovered] = Col(kAccentLight);
    c[ImGuiCol_TableHeaderBg] = Col(kSelectedBg);
    c[ImGuiCol_TableBorderStrong] = Col(kCardBorder);
    c[ImGuiCol_TableBorderLight] = Col(kSelectedBg);
    c[ImGuiCol_TableRowBg] = Col(0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Col(0xFFFFFF, 0.03f);
    c[ImGuiCol_TextLink] = Col(kAccent);
    c[ImGuiCol_TextSelectedBg] = Col(kAccent, 0.35f);
    c[ImGuiCol_TreeLines] = Col(kCardBorder);
    c[ImGuiCol_DragDropTarget] = Col(kAccent);
    c[ImGuiCol_NavCursor] = Col(kAccentLight); // the keyboard focus ring (2 px, rounded like the frame) on every nav-focusable widget
    c[ImGuiCol_NavWindowingHighlight] = Col(kAccentLight, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = Col(0, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = Col(0, 0.45f);
}

void LoadFonts(ImGuiIO& io) {
    ImFontAtlas* atlas = io.Fonts;
    const std::wstring folder = FontsFolder();
    const std::string regular = FindFont(folder, {L"segoeui.ttf"});
    const std::string bold = FindFont(folder, {L"segoeuib.ttf"});
    if (regular.empty()) {
        LOG_WARNING("[Overlay] Segoe UI not found: the menu uses ImGui's default font");
        return; // ImGui adds its default font at the first frame
    }
    g_regular = atlas->AddFontFromFileTTF(regular.c_str(), kFontSize);
    if (!g_regular) {
        LOG_WARNING("[Overlay] Segoe UI could not be loaded: the menu uses ImGui's default font");
        return;
    }
    g_baseSize = kFontSize;
    if (!bold.empty()) g_bold = atlas->AddFontFromFileTTF(bold.c_str(), kFontSize);
    if (!g_bold) g_bold = g_regular;
    io.FontDefault = g_regular;
    LOG_INFO(std::string("[Overlay] Fonts: Segoe UI") + (g_bold != g_regular ? " + Bold" : "") + " (icons: Lucide vector strokes)");
}

ImFont* RegularFont() { return g_regular; }
ImFont* BoldFont() { return g_bold ? g_bold : g_regular; }
float BaseFontSize() { return g_baseSize; }

} // namespace VioletTheme
