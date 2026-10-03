// Draw the actual extracted Report UI with real Violet widgets, fonts, icons and translations.
// Game-facing APIs are inert. This is a native layout check, not gameplay validation.
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <filesystem>
#include <format>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <thread>
#include "ui/widgets.h"
#include "ui/violet_theme.h"
#include "ui/i18n.h"
#include "features/captures.h"
#include "framework/apex_log.h"
#include "framework/apex_paths.h"
#include "framework/d3d9_hooks.h"
#include "framework/overlay.h"
#include "imgui_internal.h"
namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) {} }
std::wstring testDirectory;
namespace ApexPaths { const std::wstring& ApexDirectory() { return testDirectory; } }
bool panelVisible = true;
namespace Overlay { bool IsVisible() { return panelVisible; } void SetVisible(bool value) { panelVisible = value; } }
namespace D3D9Hooks { bool RegisterPresent(const std::string&, PresentHook, Priority) { return true; } }
const char* GetGameVersionName() { return "layout test"; }
#include "features/captures.cpp"
namespace ApexConfig {
    struct UiSettings { bool captureScreenshot = true; };
    UiSettings settings;
    UiSettings GetUi() { return settings; }
    void SetUi(UiSettings value) { settings = value; }
    std::string KeyChordText(int key) { return std::format("F{}", key); }
}
namespace Hotkeys {
    enum class Action { Recorder = 6, Probe = 7, Diagnostics = 8, Compare = 9 };
    int Key(Action action) { return static_cast<int>(action); }
}
int recordingSeconds = -1;
namespace Recorder { bool Active() { return recordingSeconds >= 0; } int SecondsRecorded() { return recordingSeconds; } void RequestToggle() { recordingSeconds = 0; } void RequestStop() { recordingSeconds = -1; Captures::SaveReport(); } void RequestCancel() { recordingSeconds = -1; } }
bool aimed = false;
namespace LightProbe { bool Busy() { return false; } void Aim() { aimed = true; } }
namespace LightDiag { void RequestDump() {} }
struct ApexPatch { bool IsEnabled() { return true; } } lighting;
ApexPatch* Find(const char*) { return &lighting; }
const char* kNightLighting = "NightTerrainRelight";
bool Loading() { return false; }
using ApexUi::IconId;
using ApexUi::ButtonKind;
using VioletTheme::Col;
bool g_comparing = false;
void ToggleCompare() { g_comparing = !g_comparing; }
struct ButtonRecord { std::string label; ImVec2 center; bool enabled; };
std::vector<ButtonRecord> buttons;
namespace ApexUi {
    bool RecordedIconTextButton(const char*, IconId, const char*, ButtonKind);
    bool RecordedBeginAdvanced(const char*, const char*);
    bool BeginAdvanced(const char* id, const char* label) {
        const bool open=RecordedBeginAdvanced(id,label);
        const auto& item=ImGui::GetCurrentContext()->LastItemData;
        buttons.push_back({"Expand "+std::string(id),item.Rect.GetCenter(),!(item.ItemFlags & ImGuiItemFlags_Disabled)});
        return open;
    }
    bool IconTextButton(const char* label, IconId icon, const char* tooltip, ButtonKind kind) {
        const bool clicked = RecordedIconTextButton(label, icon, tooltip, kind);
        const auto& item = ImGui::GetCurrentContext()->LastItemData;
        buttons.push_back({label, item.Rect.GetCenter(), !(item.ItemFlags & ImGuiItemFlags_Disabled)});
        return clicked;
    }
}
#include "report_ui_under_test.inc"
#include "notice_under_test.inc"

void Rasterize(const std::filesystem::path& file, int width, int height) {
    unsigned char* atlas = nullptr;
    int aw = 0, ah = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    std::vector<BYTE> bgr(static_cast<size_t>(width) * height * 3, 21);
    const auto* data = ImGui::GetDrawData();
    for (int list = 0; list < data->CmdListsCount; ++list) {
        const auto* dl = data->CmdLists[list];
        for (const auto& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            for (unsigned index = cmd.IdxOffset; index < cmd.IdxOffset + cmd.ElemCount; index += 3) {
                const auto& a = dl->VtxBuffer[dl->IdxBuffer[index] + cmd.VtxOffset];
                const auto& b = dl->VtxBuffer[dl->IdxBuffer[index+1] + cmd.VtxOffset];
                const auto& c = dl->VtxBuffer[dl->IdxBuffer[index+2] + cmd.VtxOffset];
                const auto edge = [](ImVec2 p, ImVec2 q, float x, float y) { return (q.x-p.x)*(y-p.y)-(q.y-p.y)*(x-p.x); };
                const float area = edge(a.pos,b.pos,c.pos.x,c.pos.y);
                if (std::abs(area) < 0.0001f) continue;
                const int x0 = std::max({0, static_cast<int>(cmd.ClipRect.x), static_cast<int>(std::floor(std::min({a.pos.x,b.pos.x,c.pos.x})))});
                const int y0 = std::max({0, static_cast<int>(cmd.ClipRect.y), static_cast<int>(std::floor(std::min({a.pos.y,b.pos.y,c.pos.y})))});
                const int x1 = std::min({width, static_cast<int>(cmd.ClipRect.z), static_cast<int>(std::ceil(std::max({a.pos.x,b.pos.x,c.pos.x})))});
                const int y1 = std::min({height, static_cast<int>(cmd.ClipRect.w), static_cast<int>(std::ceil(std::max({a.pos.y,b.pos.y,c.pos.y})))});
                for (int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
                    const float wa=edge(b.pos,c.pos,x+0.5f,y+0.5f)/area, wb=edge(c.pos,a.pos,x+0.5f,y+0.5f)/area, wc=1-wa-wb;
                    if(wa<0 || wb<0 || wc<0) continue;
                    const int tx=std::clamp(static_cast<int>((a.uv.x*wa+b.uv.x*wb+c.uv.x*wc)*aw),0,aw-1);
                    const int ty=std::clamp(static_cast<int>((a.uv.y*wa+b.uv.y*wb+c.uv.y*wc)*ah),0,ah-1);
                    const float alpha=((a.col>>24)*wa+(b.col>>24)*wb+(c.col>>24)*wc)*atlas[(ty*aw+tx)*4+3]/65025.0f;
                    for(int channel=0;channel<3;++channel) {
                        const int shift=(2-channel)*8;
                        const float color=((a.col>>shift)&255)*wa+((b.col>>shift)&255)*wb+((c.col>>shift)&255)*wc;
                        auto& dest=bgr[(static_cast<size_t>(y)*width+x)*3+channel];
                        dest=static_cast<BYTE>(color*alpha+dest*(1-alpha));
                    }
                }
            }
        }
    }
    { std::lock_guard<std::mutex> lk(Captures::g_lock); Captures::g_shotJobs[file.parent_path()] = true; }
    Captures::WritePng(file, std::move(bgr), width, height);
    for(int n=0;Captures::Saving() && n<300;++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(Captures::Saving() || !std::filesystem::is_regular_file(file)) std::exit(3);
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    namespace fs=std::filesystem;
    const auto out=fs::absolute(argv[1]);
    testDirectory=(out / std::format("layout-storage-{}-{}",GetCurrentProcessId(),GetTickCount64())).wstring();
    fs::create_directories(testDirectory);
    Captures::SetScreenshots(false);
    Captures::SaveReport();
    const auto namedFolder=Captures::LastSave().folder;
    Captures::SaveFolderDescription(namedFolder, "Lamp changes slowly", "After switching the lamps off, the ground stays bright.");
    Captures::BeginSession();
    Captures::WriteText(Captures::NewFolder("Recording") / L"Recording.txt", "sample diagnostic");
    Captures::EndSession();
    ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr;
    VioletTheme::ApplyStyle(ImGui::GetStyle());
    VioletTheme::LoadFonts(io);
    unsigned char* pixels=nullptr;int aw=0,ah=0;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&aw,&ah);
    io.Fonts->SetTexID(1);
    int frames=0;
    const auto savedReceipt = Captures::LastSave();
    for(int lang=0;lang<4;++lang) for(int narrow=0;narrow<2;++narrow) for(int state=0;state<13;++state) {
        I18n::SetChoice(lang); I18n::ClearMissing();
        g_report.tab=state==1?1:0;recordingSeconds=(state==2 || state==6)?7:-1;
        Captures::g_result = savedReceipt;
        Captures::g_result.saving = state == 7;
        Captures::g_result.failed = state == 8;
        if(state==9) Captures::g_result.serial = 0;
        g_report.descriptionReceipt = Captures::g_result.serial;
        g_report.describe = false;
        g_report.descriptionEditing = false;
        g_report.shareShowing = state==3;
        g_report.contentsShowing = state==4;
        g_report.captureStarted = state==2 || state==6;
        g_report.receiptShowing = state==7 || state==8 || state==11 || state==12;
        g_report.described = state==11;
        const int width=narrow?430:760;
        io.DisplaySize=ImVec2(static_cast<float>(width),1100);io.DeltaTime=1.0f/60;
        ImGui::GetStyle().FontScaleMain=narrow?1.3f:1.0f;
        for(int warm=0;warm<3;++warm) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Report test",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
            if(state>=3 && state<=5 && warm==1) g_report.popupRequest=state==3?1:state==4?2:3;
            if(state==10 && warm==1) ReportOpenDescription(Captures::LastSave().folder);
            if(state==4) { g_report.detailsFolder=Captures::LastSave().folder;g_report.files=Captures::Files(g_report.detailsFolder); }
            for(const auto* window:ImGui::GetCurrentContext()->Windows) {
                if(std::strstr(window->Name,"ReportRecording")) {
                    const auto seed=ImHashStr("ReportOptions",0,window->ID);
                    const_cast<ImGuiWindow*>(window)->StateStorage.SetBool(ImHashStr("##AdvancedOpen",0,seed),state==6);
                }
            }
            buttons.clear();
            ReportPage();
            if((state>=3 && state<=5) && warm==2) ImGui::ClosePopupsOverWindow(ImGui::GetCurrentWindow(),false);
            ImGui::End();ImGui::Render();++frames;
        }
        const auto cardActive = [](const char* name) {
            for (const auto* window : ImGui::GetCurrentContext()->Windows)
                if (std::strstr(window->Name,name) && window->LastFrameActive == ImGui::GetFrameCount()) return true;
            return false;
        };
        const auto hasButton = [](const char* label) { return std::any_of(buttons.begin(),buttons.end(),[&](const auto& b) { return b.label==label; }); };
        if ((state==0 || state==9) && hasButton("Choose a point")) { std::puts("Capture options appeared before recording"); return 5; }
        if ((state==2 || state==6) && (!hasButton("Choose a point") || !hasButton("Save snapshot") || !hasButton("Save report") || !hasButton("Stop and continue"))) { std::puts("Capture options missing during recording"); return 6; }
        if (state==10 && (!cardActive("ReportDescription") || hasButton("Choose a point") || cardActive("ReportRecording") || cardActive("ReportSaveResult"))) { std::puts("Description did not replace the recording workspace"); return 7; }
        if (ImGui::IsPopupOpen("ReportDescription",ImGuiPopupFlags_AnyPopupId)) { std::puts("Description is still modal"); return 8; }
        if (state==1) {
            const auto named=std::find_if(g_report.list.begin(),g_report.list.end(),[&](const auto& e){return e.folder==namedFolder;});
            if(named==g_report.list.end() || named->title!="Lamp changes slowly" || named->description.empty()) return 12;
            if(hasButton("Remove") || hasButton("Contents") || hasButton("Remove all")) return 13;
            if(!hasButton("Open its folder") || !hasButton("Add or edit description")) return 14;
        }
        if(I18n::MissingCount()) { std::printf("Missing in lang %d state %d: %s\n",lang,state,I18n::MissingList(4000).c_str());return 4; }
        if(lang==1 && !narrow && (state<3 || state==6 || state==8 || state==10 || state==11 || state==12)) Rasterize(out / std::format("native-report-{}.png",state),width,state==11?520:1100);
        if(lang==3 && narrow && state==1) Rasterize(out / "native-library-fr-large.png",width,1100);
        if(lang==3 && narrow && state==0) Rasterize(out / "native-report-fr-large.png",width,1100);
        if(lang==3 && narrow && state==10) Rasterize(out / "native-report-details-fr-large.png",width,1100);
        if(lang==3 && narrow && state==2) Rasterize(out / "native-report-record-fr-large.png",width,1100);
    }
    // Click the real native widgets. Only the game-facing recording/probe APIs are stubbed.
    I18n::SetChoice(0);
    ImGui::GetStyle().FontScaleMain=1.0f;
    io.DisplaySize=ImVec2(760,1100);
    g_report={}; g_report.descriptionReceipt=Captures::LastSave().serial;
    g_report.describe=false; recordingSeconds=-1; panelVisible=true;
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Report test",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
        buttons.clear(); ReportPage(); ImGui::End(); ImGui::Render();
    };
    const auto expandNote = [&] {
        for(auto* window:ImGui::GetCurrentContext()->Windows) if(std::strstr(window->Name,"ReportSaveResult")) {
            const auto seed=ImHashStr("ReportSavedNote",0,window->ID);
            window->StateStorage.SetBool(ImHashStr("##AdvancedOpen",0,seed),true);
        }
        frame(); frame();
    };
    const auto button = [&](const char* label) -> ButtonRecord {
        for(const auto& b:buttons) if(b.label==label) return b;
        std::printf("Missing interaction button: %s (editing=%d recording=%d receipt=%d)\n",label,g_report.descriptionEditing,Recorder::Active(),g_report.receiptShowing);
        for(const auto& b:buttons) std::printf("  visible: %s\n",b.label.c_str());
        std::exit(10);
    };
    const auto click = [&](const char* label) {
        const auto b=button(label); io.AddMousePosEvent(b.center.x,b.center.y);
        frame(); // update the hovered child after the page switches its card
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame(); frame();
    };
    const auto require = [](bool okay,const char* message) { if(!okay) { std::puts(message); std::exit(11); } };
    frame(); frame();
    click("Start recording");
    require(Recorder::Active() && panelVisible,"Starting hid the panel or failed to start");
    std::this_thread::sleep_for(std::chrono::milliseconds(450)); frame();
    require(button("Choose a point").enabled,"Capture controls not available while recording");
    // Open the real advanced section through its retained state; comparison must keep controls visible.
    for(auto* window:ImGui::GetCurrentContext()->Windows) if(std::strstr(window->Name,"ReportRecording")) {
        const auto seed=ImHashStr("ReportOptions",0,window->ID);
        window->StateStorage.SetBool(ImHashStr("##AdvancedOpen",0,seed),true);
    }
    frame(); frame(); click("View without Apex");
    require(g_comparing && panelVisible,"Comparison hid the controls");
    click("Restore Apex effects"); require(!g_comparing,"Comparison did not restore effects");
    click("Return to game"); require(!panelVisible && Recorder::Active(),"Optional return stopped recording");
    panelVisible=true; frame(); click("Choose a point");
    require(aimed && !panelVisible,"Point selection did not temporarily hide the panel");
    panelVisible=true; std::this_thread::sleep_for(std::chrono::milliseconds(450)); frame();
    click("Cancel recording"); require(!Recorder::Active() && !g_report.receiptShowing,"Cancel created a completed report");
    std::this_thread::sleep_for(std::chrono::milliseconds(450)); frame();
    click("Start recording"); std::this_thread::sleep_for(std::chrono::milliseconds(450)); frame();
    click("Stop and continue"); std::this_thread::sleep_for(std::chrono::milliseconds(450)); frame(); frame();
    require(g_report.descriptionEditing && panelVisible,"Stop did not lead to inline details");
    g_comparing=true; frame(); click("Restore Apex effects");
    require(!g_comparing && g_report.descriptionEditing,"Comparison cannot be restored after recording");
    require(!button("Save capture").enabled,"Blank description can be saved");
    std::snprintf(g_report.description,sizeof(g_report.description)," \t\n"); frame();
    require(!button("Save capture").enabled,"Whitespace description can be saved");
    const auto folder=g_report.descriptionFolder;
    click("Cancel"); require(!g_report.descriptionEditing && !g_report.described && std::filesystem::exists(std::filesystem::path(testDirectory)/L"Captures"/folder),"Cancel discarded the capture or completed it");
    click("Add or edit description");
    std::snprintf(g_report.description,sizeof(g_report.description),"Lights stayed on after switching them off."); frame();
    require(button("Save capture").enabled,"A required description cannot be saved");
    click("Save capture");
    require(!g_report.descriptionEditing && g_report.described && Captures::ReadDescription(folder).Complete(),"Description did not complete the capture");
    const auto savedDescription=Captures::ReadDescription(folder).text;
    click("Expand ReportSavedFiles");
    require(g_report.contentsFor==folder && !g_report.files.empty(),"Expandable contents did not read the actual files");
    click("Expand ReportSendHelp");
    require(!ImGui::IsPopupOpen("ReportShare",ImGuiPopupFlags_AnyPopupId) && !ImGui::IsPopupOpen("ReportContents",ImGuiPopupFlags_AnyPopupId),"Receipt details still open popups");
    expandNote();
    click("Add or edit description");
    std::snprintf(g_report.description,sizeof(g_report.description),"unsaved edit"); frame(); click("Cancel");
    require(Captures::ReadDescription(folder).text==savedDescription,"Cancel changed an existing note");
    click("New recording"); require(!g_report.receiptShowing,"New recording did not return to preparation");
    std::puts("18 native interaction checks passed, including expandable files/sharing without informational popups");
    int noticeCases=0;
    for(int lang=0;lang<4;++lang) for(int width:{430,1920,3840}) for(float scale:{1.0f,1.4f}) for(int kind=0;kind<5;++kind) {
        I18n::SetChoice(lang); ImGui::GetStyle().FontScaleMain=scale;
        io.DisplaySize=ImVec2(static_cast<float>(width),width==3840?2160.0f:1080.0f);
        const std::string text=I18n::Tr(kind==0?"Capture removed. Undo is available in Saved files":kind==1?"Some files could not be saved. Open Report a problem to retry":kind==2?"Recording cancelled. No capture was saved":kind==3?"Description saved with the capture":"The ZIP may include your screenshot, notes, mod log and settings. Review it first");
        const IconId icon=kind==1?IconId::TriangleAlert:kind==2?IconId::Activity:kind==3?IconId::CircleCheck:IconId::Info;
        // Reproduce a persisted tiny rectangle, then require recovery on the very first frame.
        ImGui::NewFrame();ImGui::SetNextWindowSize(ImVec2(39,900));
        ImGui::Begin("##NoticeRegression",nullptr,ImGuiWindowFlags_NoDecoration);ImGui::TextWrapped("%s",text.c_str());ImGui::End();ImGui::Render();
        float stableWidth=0;
        for(int n=0;n<6;++n) {
            ImGui::NewFrame();
            if(BeginNoticePill("##NoticeRegression",NoticeContentWidth(text),1.0f,kind==2))NoticeText(text,icon,kind==2);
            const auto* w=ImGui::GetCurrentWindow();
            const float maxWidth=io.DisplaySize.x-40*ApexUi::Unit();
            require(w->Size.x>100 && w->Size.x<=maxWidth+1,"Notice width collapsed or escaped viewport margins");
            require(std::abs(w->Pos.x+w->Size.x*0.5f-io.DisplaySize.x*0.5f)<1.1f,"Notice is not horizontally centered");
            if(n>=2) {
                require(w->Size.y<ImGui::GetTextLineHeight()*12+30*ApexUi::Unit(),"Notice became a vertical text strip");
                if(stableWidth)require(std::abs(w->Size.x-stableWidth)<1,"Notice width kept drifting");
                stableWidth=w->Size.x;
            }
            EndNoticePill();ImGui::Render();
            if(lang==1 && width==430 && scale==1.0f && kind==0 && n==5) Rasterize(out/"native-notice-pt-narrow.png",width,160);
        }
        ++noticeCases;
    }
    std::printf("%d native notice regression cases passed in four languages, three viewport widths and two font sizes\n",noticeCases);
    ImGui::DestroyContext();
    std::printf("%d native UI frames rendered in all four languages; no missing texts or ImGui assertions\n",frames);
}
