#pragma once
// Experimental CAS catalogue preset metadata cache.
// The ICASUtils native calls are intercepted through the shared Mono ICall resolver used by Faster Create-a-Style.
// No UIImage/thumbnail handles or managed list references are retained.
#include <string>

namespace FastCasCatalog {
bool Start(std::string* error);
void Stop();
bool Running();
void* MaybeWrap(const char* nameSpace, const char* className, const char* methodName, void* native);
std::string StatusText();
}
