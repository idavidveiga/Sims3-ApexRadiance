#include <cassert>
#include <cstdio>
#include <toml++/toml.hpp>
#include "apex_config.h"
#include "build_flavor.h"
#include "performance.h"
#include "profile_under_test.h"
int main() {
 using namespace ApexConfig;
 assert(kPublicBuild); assert(!UiSettings{}.developerMode);
 static_assert(kPartDeveloper == 256); static_assert(kPartAmbientOcclusion == 128);
 toml::table profile;
 profile.insert("developer", toml::table{{"enabled",true},{"controls",toml::table{{"false_color",true}}}});
 profile.insert("patches",toml::table{{"EdgeSmoothing",toml::table{{"metodo",0}}}});
 profile.insert("shortcuts",toml::table{{"menu_key","Ctrl+Shift+F11"}});
 profile.insert("qol",toml::table{{"picture",toml::table{{"enabled",true}}}});
 const auto parts=ProfilePartsOf(profile);
 assert(parts==(kPartDeveloper|kPartEdgeSmoothing|kPartShortcuts|kPartColor));
 auto ordinary=profile;KeepProfileParts(ordinary,kPartEdgeSmoothing|kPartColor);
 assert(!ordinary.contains("developer"));assert(!ordinary.contains("shortcuts"));
 assert(ProfilePartsOf(ordinary)==(kPartEdgeSmoothing|kPartColor));
 auto advanced=profile;KeepProfileParts(advanced,kPartDeveloper);
 assert(advanced.contains("developer"));assert(!advanced.contains("qol"));
 assert(ProfilePartsOf(advanced)==kPartDeveloper);
 auto none=profile;KeepProfileParts(none,0);assert(ProfilePartsOf(none)==0);
 auto old=profile;old.erase("developer");assert(!(ProfilePartsOf(old)&kPartDeveloper));
 for(int i=0;i<kProfilePartCount;i++)assert((ProfilePartName(i)[0]!=0)==(i!=4));
 static_assert((kProfilePartsAll & 16)==0);
 profile.insert("display",toml::table{{"mode","fullscreen"},{"target_fps",160}});
 assert(ProfilePartsOf(profile)==parts);
 auto migrated=profile;KeepProfileParts(migrated,kProfilePartsAll);
 assert(!migrated.contains("display"));assert(ProfilePartsOf(migrated)==parts);
 toml::table displayOnly{{"display",toml::table{{"mode","window"}}}};
 assert(ProfilePartsOf(displayOnly)==0);
 kPublicBuild.store(false);assert(!kPublicBuild);kPublicBuild.store(true);assert(kPublicBuild);
 puts("PASS: default-off mode, legacy profile bits, developer-only/normal/empty profile filtering and atomic gate");
}
