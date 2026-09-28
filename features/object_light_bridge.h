#pragma once
// Street lamps light nearby outdoor objects (fences, bushes, props) like they light the ground (see object_light_bridge.cpp).
#include <string>
namespace ObjectLightBridge {
bool Install(std::string& error);
void Uninstall();
void SetStrength(float s);
void SetAllObjects(bool on); // also railings, stairs, columns (objects whose script never enabled lamp light); new rigs only
void OnPresent();   // render thread, every frame
std::string Status();
// Stock pink lamp colour -> warm white (0 = game colour, 1 = warm white). Applies to lights created afterwards.
bool InstallLampColour();
void UninstallLampColour();
void SetLampTint(float amount);
std::string LampColourStatus();
}
