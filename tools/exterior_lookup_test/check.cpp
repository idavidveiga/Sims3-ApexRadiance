#include <cstdint>
#include <cstdio>
#include <iterator>
#include <unordered_map>

// Native floor/manager access is mocked. Lookup and LevelForPoint are extracted
// from production, and compared with the pre-change exterior lookup. Geometry,
// GPU output and gameplay latency are outside this fixture's scope.
uint32_t g_pointGen=1;
bool g_wallCheck=false;
long g_checkHits=0, g_checkBad=0;
std::unordered_map<uintptr_t,uintptr_t> managers, floors, owners;
long lookups=0;
uintptr_t StoryManager(uintptr_t tracker,int story) { return managers[tracker+static_cast<uintptr_t>(story)]; }
uintptr_t LevelManager(uintptr_t level) { return owners[level]; }
uintptr_t LevelFor(uintptr_t manager) { ++lookups; return floors[manager]; }
#include "extracted_lookup.h"

int checks=0, failures=0;
void Check(bool ok,const char* what) {
    ++checks;
    if (!ok) { ++failures; if (failures<12) std::printf("FAIL: %s\n",what); }
}
void Bind(uintptr_t tracker,int story,uintptr_t manager,uintptr_t floor) {
    managers[tracker+static_cast<uintptr_t>(story)]=manager;
    floors[manager]=floor;
    if (floor) owners[floor]=manager;
}
void Compare(uintptr_t tracker,int floor) {
    const uintptr_t actual=Production::Lookup(tracker,floor);
    const uintptr_t expected=Reference::Lookup(tracker,floor);
    Check(actual==expected,"same floor supplied to the unchanged lower-wall filter");
}
int main() {
    constexpr uintptr_t lot=10000;
    for (int s=1;s<=8;++s) Bind(lot,s,100+s,1000+s);
    long optimized=0, original=0;
    for (int point=0;point<1000;++point) {
        ++g_pointGen;
        for (int lamp=0;lamp<12;++lamp)
            for (int story=0;story<7;++story) {
                const long a=lookups;
                const uintptr_t current=Production::Lookup(lot,story);
                optimized+=lookups-a;
                const long b=lookups;
                const uintptr_t previous=Reference::Lookup(lot,story);
                original+=lookups-b;
                Check(current==previous,"alternating stories/lights select identical floors");
            }
    }
    Check(optimized==7000,"each of seven floor links searched once per point");
    Check(original==84000,"reference searches links at every story transition");
    std::printf("Mock multi-story workload: %ld -> %ld full link searches (not a game-time benchmark)\n",original,optimized);
    // Single-story workloads retain the original last-manager shortcut.
    ++g_pointGen;
    Production::Lookup(lot,0); Reference::Lookup(lot,0);
    const long single=lookups;
    for(int p=0;p<1000;++p) { ++g_pointGen; Compare(lot,0); }
    Check(lookups==single,"single-story warm shortcut adds no full link searches");
    // All edits/loading transitions occur between samples, not during one
    // synchronous point evaluation, matching the existing per-point caches.
    for(int p=0;p<100;++p) {
        ++g_pointGen;
        const uintptr_t old=floors[101];
        owners[old]=0; // old floor unloaded; same manager address reused
        Bind(lot,1,101,2000+p);
        Compare(lot,0); Compare(lot,1); Compare(lot,0);
    }
    ++g_pointGen;
    owners[floors[102]]=0; floors[102]=0;
    Compare(lot,1); Compare(lot,0); Compare(lot,1);
    Check(Production::Lookup(lot,1)==0,"missing floor keeps the native fallback");
    ++g_pointGen;
    Bind(lot,2,102,8888); Compare(lot,1);
    ++g_pointGen;
    managers[lot+3]=0; Compare(lot,2);
    ++g_pointGen;
    Bind(lot,3,103,8889); Compare(lot,2);
    // A different loaded lot evicts bounded cache slots; no value crosses the
    // sample boundary, even with more than eight distinct managers.
    for(int p=0;p<50;++p) {
        ++g_pointGen;
        for(int l=0;l<3;++l) {
            const uintptr_t tracker=lot+1000*(l+1);
            for(int s=1;s<=7;++s) {
                Bind(tracker,s,500+l*10+s,9000+l*10+s);
                Compare(tracker,s-1);
            }
        }
    }
    ++g_pointGen;
    g_wallCheck=true;
    Compare(lot,0); Compare(lot,1); Compare(lot,0);
    Check(g_checkHits>0 && g_checkBad==0,"existing read-only lookup self-check matches reference");
    std::printf("Exterior lookup: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
