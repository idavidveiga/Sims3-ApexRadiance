#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "features/room_ambient_policy.h"
using BYTE=unsigned char;
std::mutex g_structureMx;
std::unordered_map<uintptr_t,uint64_t> g_roomStructures;
std::vector<uintptr_t> g_structureRooms;
std::atomic<bool> g_structurePending{false};
#include "extracted_structure.h"
int checks=0,failures=0;
void Check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",text);}}
int main(){
 alignas(16) BYTE room[0x700]={};uintptr_t walls[3]={11,22,33};
 *reinterpret_cast<int*>(room+0xC)=16;
 *reinterpret_cast<uintptr_t*>(room+0x30)=reinterpret_cast<uintptr_t>(walls);
 *reinterpret_cast<uintptr_t*>(room+0x34)=reinterpret_cast<uintptr_t>(walls+2);
 NoteRoomStructure(room);Check(g_structurePending&&g_structureRooms.size()==1,"new room schedules refresh");
 g_structurePending=false;g_structureRooms.clear();
 for(int i=0;i<10000;++i){room[0x100]=BYTE(i);NoteRoomStructure(room);}
 Check(!g_structurePending&&g_structureRooms.empty(),"LOD and repeated gathers do not schedule refresh");
 room[0x18]=1;NoteRoomStructure(room);Check(g_structurePending,"roof classification change schedules refresh");
 walls[0]=44;NoteRoomStructure(room);Check(g_structureRooms.size()==1,"wall change coalesces same room");
 g_structurePending=false;g_structureRooms.clear();
 *reinterpret_cast<uintptr_t*>(room+0x34)=reinterpret_cast<uintptr_t>(walls+3);NoteRoomStructure(room);Check(g_structurePending,"closing wall changes signature");
 g_structurePending=false;g_structureRooms.clear();
 *reinterpret_cast<uintptr_t*>(room+0x34)=1;NoteRoomStructure(room);Check(!g_structurePending,"invalid wall vector ignored");
 *reinterpret_cast<int*>(room+0xC)=0;NoteRoomStructure(room);Check(!g_structurePending,"exterior room ignored");
 Check(RoomAmbientPolicy::FloorEditReady(1250,1000),"floor editing quiet interval is 250 ms");
 Check(!RoomAmbientPolicy::FloorEditReady(1249,1000),"floor edits remain debounced");
 Check(!RoomAmbientPolicy::StructureRefreshDue(1249,1000),"structure refresh is bounded");
 Check(RoomAmbientPolicy::StructureRefreshDue(1250,1000),"structure refresh is due after 250 ms");
 std::printf("Room structure: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
