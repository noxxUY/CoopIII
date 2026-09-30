// The camera of a player riding in somebody else's car.
//
// **A frame behind.** TheCamera.Process is called from inside CGame::Process
// (0x0048C9B5), after CWorld::Process has run the frame's physics and put
// every ped sitting in a car on its seat. CoopIII corrects a car somebody
// else drives after CGame::Process returns, so for a rider the camera had
// already aimed at his car where this machine's physics left it, and the
// frame then drew the car where its driver's stream said. Every frame, the
// camera looked at where the car had been. The call is taken (a call site,
// not a detour) and Client::BeforeCamera corrects the car the local player
// rides in there, then sits its occupants back on it the way CWorld::Process
// does (CPed::SetPedPositionInCar, then the matrix to the RwFrame), and the
// pass after the frame leaves that car alone. Everything else is corrected
// where it was.
//
// **The unique jump's shot.** The driver's machine runs USJ about its own
// player (game/stunt.h), and in the jump it asks for a fixed camera on the
// car: SET_FIXED_CAMERA_POSITION, then POINT_CAMERA_AT_CAR, and
// RESTORE_CAMERA_JUMPCUT once it has landed or failed. The call to
// CCamera::TakeControl in POINT_CAMERA_AT_CAR's handler is taken, and when
// the script asking is USJ and the car is the one the local player drives,
// where the camera stands goes to the session (C_StuntCamera,
// docs/protocol.md §1.35); RESTORE_CAMERA_JUMPCUT's call says it is over.
// A rider's camera takes the same shot with the same two calls the handlers
// make, and gives it back with RestoreWithJumpCut. The slow motion needs
// nothing: the driver's game runs at a quarter speed and its car moves that
// way on everybody's screen. The rider's own world is not slowed; his time
// scale is his.
#pragma once

#include <cctype>
#include <cstdint>

namespace coopiii {
class Client;
struct WorldBridge;
} // namespace coopiii

namespace coopiii::game {

// Whether a script's name (CRunningScript's eight bytes, lowered by
// NAME_THREAD but compared either way) is the unique jumps' thread.
inline bool IsStuntShotThread(const char *name) {
	if (!name)
		return false;
	const char want[] = "usj";
	for (int i = 0; i < 3; ++i)
		if (std::tolower(static_cast<unsigned char>(name[i])) != want[i])
			return false;
	return name[3] == '\0';
}

// The CCamera::Process call and the two camera calls of the jump. Each one
// is checked to still call what the retail image calls there, and one that
// does not is left alone and logged. Not fatal.
bool InstallRideCamera(Client &client);
void RemoveRideCamera();
bool RideCameraOrderTaken();

void AddRideCameraToBridge(WorldBridge &bridge);

} // namespace coopiii::game
