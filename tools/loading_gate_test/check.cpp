// Read-only mock checks of the actual loaded-world reader and extracted notice
// and blur guards. Does not start the game or create a graphics device.
#include "features/world_session.h"
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
uintptr_t fixtureGlobal = 0;
unsigned char getterCode[6]={0xA1,0,0,0,0,0xC3};
bool loaderPresent=false;
uintptr_t rootVtable[62]={},serviceVtable[2]={};
struct RootMock {uintptr_t* vtable=rootVtable; void* Find(uint32_t id,int recursive){return id==0x95947678u&&!recursive&&loaderPresent?this:nullptr;}} rootMock;
struct ServiceMock {uintptr_t* vtable=serviceVtable; void* Root(){return &rootMock;}} serviceMock;
uintptr_t servicePointer=reinterpret_cast<uintptr_t>(&serviceMock);
template<class T> uintptr_t Address(T p){static_assert(sizeof p==sizeof(uintptr_t));uintptr_t v;std::memcpy(&v,&p,sizeof v);return v;}
namespace GameAddr { uintptr_t Get(Id id) { return id==Id::UiServiceGetter?reinterpret_cast<uintptr_t>(getterCode):fixtureGlobal; } }
enum class Startup { Loading, Running };
std::atomic<Startup> g_startup{Startup::Loading};
std::atomic<bool> g_menuAvailable{false};
bool g_hintStarted=false;
int g_hintLeftMs=0;
unsigned long long g_hintLastDraw=0;
constexpr int kHintMs=8000;
namespace ApexConfig { struct Ui {bool startNote=true;} ui; Ui& GetUi(){return ui;} }
namespace Overlay { bool visible=false; bool IsVisible(){return visible;} }
struct BlurMock {
    WorldSession::Settled world;
    bool blurOn=true,ready=true,inBlur=false,internalPass=false;
    bool focusSnap=false,mapOpen=true;
    float mapFade=1;
    LARGE_INTEGER lastFadeTick{123};
} g;
int passes=0;
#include "extracted_loading_guards.h"
int checks=0, failures=0;
void Check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",text);}}
int main(){
    const uint32_t serviceGlobal=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&servicePointer));
    std::memcpy(getterCode+1,&serviceGlobal,4);
    serviceVtable[1]=Address(&ServiceMock::Root);rootVtable[0xF4/4]=Address(&RootMock::Find);
    Check(WorldSession::LoaderDismissed(),"removed loading window releases gate");
    loaderPresent=true;Check(!WorldSession::LoaderDismissed(),"attached loading window blocks ready world");
    loaderPresent=false;servicePointer=0;Check(!WorldSession::LoaderDismissed(),"UI not initialised fails closed");
    servicePointer=reinterpret_cast<uintptr_t>(&serviceMock);getterCode[0]=0;Check(!WorldSession::LoaderDismissed(),"unknown UI getter fails closed");getterCode[0]=0xA1;
    WorldSession::Settled settled;
    settled.Update(true,0);Check(!settled.ready,"zero clock first frame is not ready");
    settled.Update(true,2999);Check(!settled.ready,"blur waits through final settling interval");
    settled.Update(true,3000);Check(settled.ready,"continuous loaded world becomes ready");
    settled.Update(false,3001);Check(!settled.ready&&!settled.tracking,"new load invalidates readiness immediately");
    settled.Update(true,9000);Check(!settled.ready,"new world starts fresh delay");
    settled.Update(true,12000);Check(settled.ready,"new world settles independently");
    alignas(uintptr_t) std::array<unsigned char,512> world{};
    uintptr_t worldPtr=reinterpret_cast<uintptr_t>(world.data());
    fixtureGlobal=reinterpret_cast<uintptr_t>(&worldPtr);
    for(int active=0;active<=1;++active)for(int mode=-3;mode<=8;++mode){
        world[0x41]=static_cast<unsigned char>(active);
        std::memcpy(world.data()+0x1B4,&mode,sizeof(mode));
        const bool expected=active&&mode>=1&&mode<=3;
        Check(WorldSession::IsActive()==expected,"actual world fields classify loading/tool/game modes");
        passes=0;g.world.ready=true;g.focusSnap=false;g.lastFadeTick.QuadPart=123;g.mapOpen=true;g.mapFade=1;
        ExtractedBlurGuard(nullptr);
        Check(passes==int(expected),"loading bypasses blur GPU work including debug path");
        if(!expected)Check(g.focusSnap&&g.lastFadeTick.QuadPart==0&&!g.mapOpen&&g.mapFade==0,"loading discards stale focus timing and map fade");
    }
    loaderPresent=true;world[0x41]=1;int liveMode=1;std::memcpy(world.data()+0x1B4,&liveMode,4);
    Check(!WorldSession::IsActive(),"world already loaded behind loading window remains unavailable");
    g.world.ready=true;passes=0;ExtractedBlurGuard(nullptr);Check(passes==0,"attached loading window blocks blur even after previous ready frame");
    loaderPresent=false;
    fixtureGlobal=0;Check(!WorldSession::IsActive(),"unresolved address fails closed");
    fixtureGlobal=1;Check(!WorldSession::IsActive(),"unreadable global fails closed");
    fixtureGlobal=reinterpret_cast<uintptr_t>(&worldPtr);worldPtr=0;
    Check(!WorldSession::IsActive(),"null world fails closed");
    worldPtr=1;Check(!WorldSession::IsActive(),"unreadable world fails closed");
    g_startup=Startup::Running;
    for(int frame=0;frame<10000;++frame)UpdateHint();
    Check(!g_hintStarted&&g_hintLeftMs==0,"long startup loading does not start or consume hint");
    g_menuAvailable=true;UpdateHint();
    Check(g_hintStarted&&g_hintLeftMs==8000&&g_hintLastDraw==0,"first ready frame starts full hint");
    g_hintLeftMs=3200;g_hintLastDraw=123;g_menuAvailable=false;UpdateHint();
    Check(g_hintLeftMs==3200&&g_hintLastDraw==0,"subsequent loading pauses existing hint");
    g_menuAvailable=true;UpdateHint();Check(g_hintLeftMs==3200,"returning world does not restart hint");
    g_hintStarted=false;g_hintLeftMs=0;ApexConfig::ui.startNote=false;UpdateHint();
    Check(g_hintStarted&&g_hintLeftMs==0,"saved disabled notice remains disabled");
    ApexConfig::ui.startNote=true;g_hintStarted=false;Overlay::visible=true;UpdateHint();
    Check(g_hintLeftMs==0,"open menu suppresses redundant start hint");
    std::printf("Loading guards: %d checks, %d failures; extracted code/mocks, not gameplay\n",checks,failures);
    return failures?1:0;
}
