#pragma once
#include <cstdint>
namespace WallUpdatePolicy {
// Exterior surfaces on lower stories remain visible from outside the building.
// Above-camera stories enter the barrier only when all-floor detail is enabled.
inline bool VisibleForBarrier(int level,int camera,bool exterior,bool atrium,bool allFloors) {
    if(level>camera)return allFloors&&exterior;
    return level==camera||exterior||atrium;
}
// A gather strictly after the last value-only request supersedes that request.
// Same-tick ordering, structural requests and unknown phases stay conservative.
inline bool DeferredCovered(bool valueOnly,int state,std::uint32_t started,std::uint32_t requested) {
    return valueOnly&&(state==2||state==5)
        &&static_cast<std::int32_t>(started-requested)>0;
}
}
