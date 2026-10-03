#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include "ui/widgets.h"
#include "ui/violet_theme.h"
#include "ui/i18n.h"
#include "framework/apex_log.h"
#include "imgui_internal.h"
namespace ApexLog { void Write(Level,const std::string&,const std::source_location&) {} }
using ApexUi::IconId;
using VioletTheme::Col;
#include "profile_picker_under_test.inc"
int main(){
 ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(1000,800);
 VioletTheme::ApplyStyle(ImGui::GetStyle());VioletTheme::LoadFonts(io);
 unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);io.Fonts->SetTexID(1);
 int cases=0;
 for(int lang=0;lang<4;lang++)for(float scale:{1.f,1.4f})for(int frame=0;frame<5;frame++){
 I18n::SetChoice(lang);ImGui::GetStyle().FontScaleMain=scale;ImGui::NewFrame();
 ImGui::SetNextWindowSize(ImVec2(900,600));ImGui::Begin("Controls");
 {ApexUi::ControlSizeScope primary(ApexUi::ControlSize::Primary);
 const float expected=VioletTheme::kControlPrimary*scale;const int stack=ImGui::GetCurrentContext()->StyleVarStack.Size;
 if(frame==0)ImGui::OpenPopup("##ProfileIcons");
 IconId chosen=IconId::Bookmark;ProfileIconPicker(chosen);
 if(ImGui::GetCurrentContext()->StyleVarStack.Size!=stack||fabs(ImGui::GetFrameHeight()-expected)>.1f)return 11;
 ImGui::SameLine();char name[64]="Moonlight";ImGui::InputText("##Name",name,sizeof(name));
 if(fabs(ImGui::GetItemRectSize().y-expected)>.1f)return 12;
 ImGui::SameLine();ApexUi::TextButton("Save");if(fabs(ImGui::GetItemRectSize().y-expected)>.1f)return 13;
 }
 for(const char* label:{"Apply","Delete","Save"}){
 ApexUi::ControlSizeScope compact(ApexUi::ControlSize::Compact);auto* dl=ImGui::GetWindowDrawList();int begin=dl->VtxBuffer.Size;
 ApexUi::IconTextButton(label,IconId::Save);ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();ImVec2 center((a.x+b.x)*.5f,(a.y+b.y)*.5f);
 const ImVec2 white=ImGui::GetFontTexUvWhitePixel();float y0=100000,y1=-100000;
 for(int j=begin;j<dl->VtxBuffer.Size;j++){const auto& v=dl->VtxBuffer[j];if(fabs(v.uv.x-white.x)>1e-6f||fabs(v.uv.y-white.y)>1e-6f){y0=std::min(y0,v.pos.y);y1=std::max(y1,v.pos.y);}}
 if(y1>y0&&fabs((y0+y1)*.5f-center.y)>1.1f){printf("text %s error %.2f\n",label,(y0+y1)*.5f-center.y);return 14;}
 }
 {
 const ApexUi::ControlSizeScope compact(ApexUi::ControlSize::Compact);
 auto* dl=ImGui::GetWindowDrawList();const int first=dl->VtxBuffer.Size;
 ImGui::SetCursorPosX(20);ImGui::PushItemWidth(600);
 for(int row=0;row<3;row++){
 if(ApexUi::BeginControlRow(row==0?"Save a report":row==1?"Record a few seconds":"Lighting snapshot",row==1?"Writes down what the lighting does while you make the problem happen.":"The log and your settings.",ApexUi::ButtonWidth("Save",true))){ApexUi::IconTextButton("Save",IconId::Save);ApexUi::EndControlRow();}
 }
 ImGui::PopItemWidth();
 float dividerX=-1;int dividers=0;
 for(int j=first;j+3<dl->VtxBuffer.Size;j++){
 const auto& a=dl->VtxBuffer[j];const auto& b=dl->VtxBuffer[j+1];const auto& c=dl->VtxBuffer[j+2];
 if(fabs(a.pos.y-b.pos.y)<.01f&&fabs(c.pos.y-a.pos.y-1)<.01f&&b.pos.x-a.pos.x>200){
 if(dividerX>=0&&fabs(dividerX-a.pos.x)>.01f)return 16;dividerX=a.pos.x;dividers++;}
 }
 if(dividers<2)return 17;
 }
 ImGui::End();ImGui::Render();if(ImGui::GetCurrentContext()->ErrorCountCurrentFrame)return 15;cases++;
 }
 printf("PASS: %d multi-frame cases, EN/PT/ES/FR at two scales; popup stack, adjacent heights, text ink centres.\n",cases);
 ImGui::DestroyContext();return 0;
}
