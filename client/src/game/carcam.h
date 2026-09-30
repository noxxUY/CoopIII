// The car camera, when another mod has replaced it.
//
// San Andreas style car cameras for GTA III (SACarCam, and its LCS build)
// replace the car camera by pointing CCam::Process's call for
// MODE_CAM_ON_A_STRING at their own function, and MODE_BEHINDBOAT's too
// (addresses.h, "the car camera's call"). They do it from DllMain, so by the
// time CoopIII installs, on the first game frame (docs/compat.md 2.2), those
// calls already lead out of gta3.exe. Nothing CoopIII hooks or redirects is on
// any site such a mod patches, so the two load together as they are;
// docs/compat.md 2.7 has both lists.
//
// One thing is not right as it is. Such a camera turns the tank's turret and
// the fire truck's cannon toward where it looks, for whatever car it follows.
// In single player that is always the car the player drives. In a session a
// rider's camera follows the car he rides in, and that car's gun is its
// driver's (S_VehicleAim): CorrectCarExtras writes it back after the frame,
// so it is drawn right, but the turret's motor plays on the rider's machine
// every frame his camera and the driver's gun disagree. So the call is taken
// once more and chained to whatever it led to, and around it the gun of any
// car the local player is not driving is put back, and kept quiet: its
// m_audioEntityId is -1 for the length of the call, and
// cAudioManager::PlayOneShot plays nothing for an entity below zero.
//
// Against the retail camera nothing is taken. It never touches a gun.
#pragma once

#include "../carextrasync.h"

#include <cstdint>

namespace coopiii::game {

// Where the code a call leads to lives.
enum class CodeHome : uint8_t {
	Exe,           // inside gta3.exe
	OtherModule,   // inside some other loaded image: an .asi, a .dll
	Nowhere,       // in no image at all
};

// Whose camera a car camera call site leads to.
enum class CarCamOwner : uint8_t {
	Retail,    // the call the image shipped with
	Mod,       // a call into another module's code: a car camera mod
	Unknown,   // not a call, or a call somewhere nobody can vouch for
};

inline CarCamOwner WhoOwnsCarCamCall(uint8_t opcode, uintptr_t target, uintptr_t retail,
                                     CodeHome home) {
	if (opcode != 0xE8)
		return CarCamOwner::Unknown;
	if (target == retail)
		return CarCamOwner::Retail;
	return home == CodeHome::OtherModule ? CarCamOwner::Mod : CarCamOwner::Unknown;
}

// Whether a car camera must be kept off the gun of the car it follows: an
// automobile whose control code has one, that the local player is not the
// driver of.
inline bool CarCameraKeepsOffGun(bool automobile, uint16_t model, bool localDrives) {
	return automobile && CarHasGun(model) && !localDrives;
}

// Reads the two car camera calls and says who owns them. When a mod owns the
// car camera's, the call comes to us and goes on to the mod. True when that
// happened or nothing needed it; false when the call could not be taken.
bool InstallCarCamera();
void RemoveCarCamera();

// Whether a car camera mod owns the car camera call. For the log.
bool CarCameraModInstalled();

} // namespace coopiii::game
