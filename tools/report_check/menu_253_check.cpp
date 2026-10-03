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
namespace ApexPaths { const std::wstring& ApexDirectory() { return testDirectory; } const std::wstring& GameDocumentsDirectory() { return testDirectory; } }
bool panelVisible = true;
namespace Overlay { bool IsVisible() { return panelVisible; } void SetVisible(bool value) { panelVisible = value; } void SetCaptureSuppressed(bool) {} bool PostGameKeyPress(WPARAM vk) { return vk == VK_F10; } }
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
void CardNote(const char* text) { ApexUi::MutedText(text); }
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
    Captures::WritePng({file, file.parent_path(), true, 0}, std::move(bgr), width, height);
    for(int n=0;Captures::Saving() && n<300;++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(Captures::Saving() || !std::filesystem::is_regular_file(file)) std::exit(3);
}

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const auto out=std::filesystem::absolute(argv[1]);
    testDirectory=(out / std::format("storage-{}",GetCurrentProcessId())).wstring();
    std::filesystem::create_directories(testDirectory);
    Captures::SetScreenshots(false); Captures::SaveReport();
    const auto folder=Captures::LastSave().folder;
    Captures::SaveFolderDescription(folder,"Keep these notes","Existing notes must survive the UI restoration.");
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    VioletTheme::ApplyStyle(ImGui::GetStyle());VioletTheme::LoadFonts(io);
    unsigned char* pixels=nullptr;int aw=0,ah=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&aw,&ah);io.Fonts->SetTexID(1);
    int noticeCases=0;
    for(int lang=0;lang<4;++lang)for(int width:{430,900,1920})for(float scale:{1.0f,1.4f}) {
        I18n::SetChoice(lang);I18n::ClearMissing();
        io.DisplaySize=ImVec2(static_cast<float>(width),1080);io.DeltaTime=1.f/60;
        ImGui::GetStyle().FontScaleMain=scale;
        const std::string text=I18n::Tr("Some files could not be saved. Open Report a problem to retry");
        ImGui::NewFrame();
        if(!BeginNoticePill("##NoticeRegression",NoticeContentWidth(text)))return 12;
        const float lineStart=ImGui::GetCursorScreenPos().y;
        NoticeText(text,IconId::TriangleAlert);
        if(ImGui::GetCursorScreenPos().y-lineStart>ImGui::GetTextLineHeightWithSpacing()+1)return 13;
        const auto* notice=ImGui::GetCurrentWindow();
        const auto* viewport=ImGui::GetMainViewport();
        if(notice->Size.x + 0.1f < NoticeContentWidth(text) + 18.0f * ApexUi::Unit() ||
           std::abs(notice->Pos.x+notice->Size.x*0.5f-(viewport->Pos.x+viewport->Size.x*0.5f))>1.1f)return 14;
        EndNoticePill();ImGui::Render();++noticeCases;
    }
    int checked=0;
    for(int lang=0;lang<4;++lang)for(int narrow=0;narrow<2;++narrow)for(int state=0;state<5;++state){
        I18n::SetChoice(lang);I18n::ClearMissing();g_report={};buttons.clear();
        ImGui::GetCurrentContext()->OpenPopupStack.clear();
        recordingSeconds=state==1?7:-1;
        Captures::g_result.saving=state==2;Captures::g_result.failed=state==3;
        Captures::g_shotJobs.clear();
        if(state==2)Captures::g_shotJobs[Captures::Root()/folder]=true;
        io.DisplaySize=ImVec2(narrow?430:900,1500);io.DeltaTime=1.f/60;
        ImGui::GetStyle().FontScaleMain=narrow?1.3f:1.f;
        for(int warm=0;warm<3;++warm){
            if(state==4 && warm==1)++Captures::g_result.serial;
            buttons.clear();ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("restored report",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize);ReportPage();ImGui::End();ImGui::Render();
        }
        const auto button=[&](const char* name){return std::find_if(buttons.begin(),buttons.end(),[&](const auto& b){return b.label==name;});};
        if(button("Save##Report")==buttons.end()||button(state==1?"Stop##Rec":"Start##Rec")==buttons.end()||button("Start a session##Sess")==buttons.end())return 3;
        if(state==1&&!button("Stop##Rec")->enabled)return 4;
        if((state==1||state==2)&&button("Save##Report")->enabled)return 5;
        if(state==3&&button("Retry saving")==buttons.end())return 6;
        if(state==4&&button("Save capture")==buttons.end())return 9;
        if(I18n::MissingCount()){printf("Missing: %s\n",I18n::MissingList(5000).c_str());return 7;}
        if(lang==1&&!narrow&&state==0)Rasterize(out/"report-restored-pt.png",900,1500);
        if(lang==1&&!narrow&&state==4)Rasterize(out/"report-optional-notes-pt.png",900,1500);
        ++checked;
    }
    Captures::g_shotJobs.clear();Captures::g_result.saving=false;Captures::g_result.failed=false;
    if(Captures::ReadDescription(folder).title!="Keep these notes")return 8;
    Captures::SetDescription("");Captures::SaveReport();
    const auto generated=Captures::ReadDescription(Captures::LastSave().folder);
    if(generated.title.empty()||!generated.Complete()||generated.text.find("Game: layout test")==std::string::npos)return 10;
    Captures::BeginSession();Captures::EndSession();
    if(!Captures::ReadDescription(Captures::LastSave().folder).Complete())return 11;
    ImGui::DestroyContext();printf("PASS: %d one-line notice cases; %d native page cases; EN/PT/ES/FR, two sizes, idle/recording/saving/failure, controls and existing notes preserved.\n",noticeCases,checked);
    return 0;
}
