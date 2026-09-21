// A synthetic second player.
//
//   ghost [host] [port] [nick] [-car] [-inout] [-shoot] [-throw]
//
// -car   also claims a vehicle and parks it in front of the player. Puts the
//        real client through the vehicle pool, CAutomobile's constructor and
//        CREATE_CAR's registration, and since the ghost drives it, through
//        seating a remote ped too.
// -inout on top of -car, gets out and back in on a cycle. No unit test can
//        reach the exit path - it's a dozen writes into a live CPed and a
//        live CVehicle - so this is what exercises it.
// -shoot holds an uzi and fires at the player, four rounds a second, with
//        PF_FIRING set in between. Drives the whole inbound combat path on
//        the real client: streaming a weapon model, CPed::GiveWeapon and
//        SetCurrentWeapon, then CWeapon::Fire on a CCivilianPed. That last
//        call is the one nothing headless can reach and the one most likely
//        to be wrong. Watch for a muzzle flash and a gunshot, and check the
//        *local* player isn't losing health - that's the bulletproof guard
//        around the replay doing its job.
// -throw holds a molotov and lobs one every three seconds, sends the
//        explosion a beat later. The two halves are kept separate: the throw
//        makes a bottle appear and fly, the explosion proves the observer's
//        copy ends silently instead of detonating wherever it feels like.
//        A second fire in the wrong street means CProjectileInfo::RemoveProjectile
//        isn't being suppressed.
//
// Connects like a real client, finds whoever else is in the session, and
// walks a slow circle a few metres from them. The ghost itself isn't the
// point - a real client now has to request a model, spawn a CPed, and write
// an interpolated pose onto it every frame.
//
// That spawn path is the one thing no unit test can reach, since it needs a
// live ped pool inside gta3.exe. This is how you exercise it without a
// second machine and a second person.

#include "clock.h"

#include <coopiii/net.h>
#include <coopiii/protocol.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace coopiii;

namespace {

constexpr float ORBIT_RADIUS_M = 4.0f;
constexpr float ORBIT_PERIOD_S = 12.0f;

} // namespace

int main(int argc, char **argv) {
	const char    *host = argc > 1 ? argv[1] : "127.0.0.1";
	const uint16_t port = argc > 2 ? uint16_t(std::atoi(argv[2])) : DEFAULT_PORT;
	const char    *nick = argc > 3 ? argv[3] : "ghost";

	// -car and -inout can go anywhere in the arguments, not positional - they
	// are modes, and the three positional args are already spoken for by the
	// connection info.
	bool carFlag   = false;
	bool inOutFlag = false;
	bool shootFlag = false;
	bool throwFlag = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-car") == 0)
			carFlag = true;
		if (std::strcmp(argv[i], "-inout") == 0)
			inOutFlag = true;
		if (std::strcmp(argv[i], "-shoot") == 0)
			shootFlag = true;
		if (std::strcmp(argv[i], "-throw") == 0)
			throwFlag = true;
	}

	WallClock::Start();

	if (!NetInit()) {
		std::printf("ENet failed to initialise\n");
		return 1;
	}

	NetClient client;
	if (!client.Connect(host, port)) {
		std::printf("could not start connecting to %s:%u\n", host, port);
		return 1;
	}
	std::printf("connecting to %s:%u as \"%s\"\n", host, port, nick);

	bool     helloSent = false;
	uint8_t  myId      = 0xFF;
	// With -car, the ghost also claims a vehicle and drives it in a wider
	// circle around the same player. Same point as the ped: it puts the real
	// client through a path no unit test can reach, allocating from the
	// vehicle pool and running CAutomobile's constructor and CREATE_CAR's
	// whole registration inside a live gta3.exe.
	const bool wantCar      = carFlag;
	uint16_t carModel       = 91;   // Idaho: an ordinary car, always streamable
	uint16_t myVehicleNetId = INVALID_NETID;
	bool     carClaimSent   = false;

	// -shoot / -throw. The weapon the ghost claims to hold has to be in the
	// snapshot as well as in the shot: the receiver puts the model in the
	// ped's hand from the snapshot and refuses to fire a gun the ped isn't
	// holding. Correct order, and worth exercising.
	constexpr uint8_t  WEAPON_UZI     = 3;
	constexpr uint8_t  WEAPON_MOLOTOV = 10;
	constexpr uint8_t  EXPLOSION_MOLOTOV_TYPE = 1;
	constexpr uint32_t SHOT_PERIOD_MS  = 250;    // four rounds a second
	constexpr uint32_t THROW_PERIOD_MS = 3000;
	// Gap between a throw and its explosion. Roughly a molotov's own
	// two-second fuse, giving the receiver time to actually fly the bottle
	// before it's told where it landed. Only way to see whether the bottle
	// gets removed silently.
	constexpr uint32_t FUSE_MS = 2000;

	const uint8_t heldWeapon = throwFlag ? WEAPON_MOLOTOV
	                           : shootFlag ? WEAPON_UZI
	                                       : 0;   // WEAPONTYPE_UNARMED
	uint32_t nextShotMs      = 0;
	uint32_t nextThrowMs     = 0;
	uint32_t pendingBlastMs  = 0;   // 0 = nothing in the air
	Vec3     pendingBlastPos{};

	// -inout: get out of the car and back in, on a cycle. Only way to exercise
	// the exit path in a real game - a remote ped leaving a seat has to be put
	// back on its feet, given its collision back, and stripped of the
	// objective that would otherwise send it walking back to the car.
	//
	// Without this flag the ghost gets in once and stays in. That's the right
	// default for looking at a driver, but only tests half the pair.
	const bool wantInOut   = inOutFlag;
	const uint32_t IN_MS   = 10000;   // seated for ten seconds
	const uint32_t OUT_MS  = 4000;    // on foot for four
	bool     ridingNow     = true;
	uint32_t nextSwitchMs  = 0;       // set when the car is first claimed

	uint8_t  targetId  = 0xFF;
	Vec3     target{0.0f, 0.0f, 0.0f};
	float    targetHeading = 0.0f;
	bool     haveTarget = false;
	uint32_t sentCount  = 0;

	RateLimiter          rate(SNAPSHOT_HZ);
	std::vector<Message> messages;

	const uint32_t startMs = WallClock::NowMs();

	while (true) {
		messages.clear();
		client.Service(messages);

		if (client.IsConnected() && !helloSent) {
			C_Hello hello;
			InitHeader(hello, WallClock::NowMs());
			hello.protocolVersion = PROTOCOL_VERSION;
			hello.modelId         = 0;
			std::memset(hello.nick, 0, sizeof(hello.nick));
			std::strncpy(hello.nick, nick, sizeof(hello.nick) - 1);
			client.Send(hello, CH_EVENT);
			helloSent = true;
			std::printf("sent hello\n");
		}

		for (const Message &msg : messages) {
			if (const S_Welcome *w = msg.as<S_Welcome>()) {
				if (w->reject != 0) {
					std::printf("rejected by the server (reason %u)\n", w->reject);
					return 1;
				}
				myId = w->playerId;
				std::printf("welcomed as player %u\n", myId);
			} else if (const S_PlayerJoin *j = msg.as<S_PlayerJoin>()) {
				if (j->playerId != myId) {
					char n[NICK_LEN];
					std::memcpy(n, j->nick, sizeof(n));
					n[NICK_LEN - 1] = '\0';
					targetId        = j->playerId;
					target          = j->pos;
					targetHeading   = j->heading;
					haveTarget      = true;
					std::printf("following player %u (\"%s\") at %.1f %.1f %.1f\n",
					            j->playerId, n, j->pos.x, j->pos.y, j->pos.z);
				}
			} else if (const S_PlayerState *s = msg.as<S_PlayerState>()) {
				if (s->playerId == targetId) {
					target        = s->body.pos;
					targetHeading = s->body.heading;
					haveTarget = true;
				}
			} else if (const S_PlayerLeave *l = msg.as<S_PlayerLeave>()) {
				if (l->playerId == targetId) {
					std::printf("player %u left\n", l->playerId);
					targetId   = 0xFF;
					haveTarget = false;
				}
			} else if (const S_EnterVehicle *e = msg.as<S_EnterVehicle>()) {
				if (e->playerId == myId && myVehicleNetId == INVALID_NETID) {
					myVehicleNetId = e->body.netId;
					nextSwitchMs   = WallClock::NowMs() + IN_MS;
					std::printf("our car is net %u\n", myVehicleNetId);
				}
			}
		}

		// No target, no packet. Circling the world origin instead used to look
		// harmless, but the origin is water - the receiving client faithfully
		// created a ped there, the engine drowned it within eight frames, and
		// every position after that landed on a corpse the renderer accepted
		// and drew as nothing. A client that doesn't know where it is
		// shouldn't be broadcasting a position at all.
		if (client.IsConnected() && helloSent && haveTarget &&
		    rate.Ready(WallClock::NowMs())) {
			const float t     = (WallClock::NowMs() - startMs) / 1000.0f;
			const float angle = 6.2831853f * (t / ORBIT_PERIOD_S);

			C_PlayerState pkt{};
			InitHeader(pkt, WallClock::NowMs());

			// Orbit the player we found. There's no "no target" case left to
			// handle here - the send is already gated on haveTarget above.
			const Vec3 centre = target;
			pkt.body.pos.x    = centre.x + ORBIT_RADIUS_M * std::cos(angle);
			pkt.body.pos.y    = centre.y + ORBIT_RADIUS_M * std::sin(angle);
			pkt.body.pos.z    = centre.z;

			// Face along the direction of travel, which is the tangent.
			pkt.body.heading = angle + 1.5707963f;

			const float speed      = 6.2831853f * ORBIT_RADIUS_M / ORBIT_PERIOD_S;
			pkt.body.moveSpeed.x   = -speed * std::sin(angle);
			pkt.body.moveSpeed.y   = speed * std::cos(angle);
			pkt.body.moveSpeed.z   = 0.0f;

			pkt.body.health    = 100.0f;
			pkt.body.armour    = 0.0f;
			pkt.body.moveState = 2;   // PEDMOVE_WALK
			pkt.body.pedState  = 0;

			// A walking ghost, holding nothing, aiming nowhere. animId2 has to
			// be ANIM_NONE, not 0 - 0 is ANIM_STD_WALK, and playing a walk as
			// a *partial* overlay isn't a thing. Same with animGroup: use
			// ASSOCGRP_PLAYER, not the zero a value-initialised body would
			// leave, since 0 is ASSOCGRP_STD and a ghost out of the std group
			// tests a path no real player ever takes.
			pkt.body.animGroup = 1;
			pkt.body.animId    = 0;   // ANIM_STD_WALK
			pkt.body.animTime  = std::fmod(t, 1.0f);
			pkt.body.animSpeed = 1.0f;
			pkt.body.animId2   = ANIM_NONE;
			pkt.body.animTime2 = 0.0f;
			pkt.body.weapon    = heldWeapon;
			pkt.body.aimPitch  = 0.0f;

			// Aim at the player rather than along the orbit when armed. A
			// remote player firing off into the distance looks the same
			// either way, aim sync working or not.
			if (heldWeapon != 0) {
				pkt.body.aimYaw =
				    std::atan2(-(centre.x - pkt.body.pos.x), centre.y - pkt.body.pos.y);
				pkt.body.flags = PF_AIMING | (shootFlag ? PF_FIRING : 0);
			} else {
				pkt.body.aimYaw = pkt.body.heading;
				pkt.body.flags  = 0;
			}

			client.Send(pkt, CH_SNAPSHOT);

			// ---- combat ----------------------------------------------------
			//
			// Sent on the reliable channel, outside the snapshot, because
			// that's how a real client sends them. Also because a shot is an
			// event: four a second, each one its own packet, and the
			// receiving client has to replay four muzzle flashes rather than
			// just see one flag set for a second.
			const uint32_t nowMs = WallClock::NowMs();

			if (shootFlag && nowMs >= nextShotMs) {
				nextShotMs = nowMs + SHOT_PERIOD_MS;

				C_Shot shot{};
				InitHeader(shot, nowMs);
				shot.body.weapon = WEAPON_UZI;
				// Roughly where a held gun sits: chest height, at the ped.
				shot.body.origin = Vec3{pkt.body.pos.x, pkt.body.pos.y,
				                        pkt.body.pos.z + 0.6f};
				shot.body.dir    = Vec3{-std::sin(pkt.body.aimYaw),
				                        std::cos(pkt.body.aimYaw), 0.0f};
				shot.body.speed  = 0.0f;   // instant hit: no projectile
				client.Send(shot, CH_EVENT);
			}

			if (throwFlag && nowMs >= nextThrowMs && pendingBlastMs == 0) {
				nextThrowMs = nowMs + THROW_PERIOD_MS;

				C_Shot shot{};
				InitHeader(shot, nowMs);
				shot.body.weapon = WEAPON_MOLOTOV;
				shot.body.origin = Vec3{pkt.body.pos.x, pkt.body.pos.y,
				                        pkt.body.pos.z + 0.6f};
				// A lobbed arc toward the player, at roughly the speed
				// CProjectileInfo::AddProjectile gives a half-charged throw.
				const float dx = -std::sin(pkt.body.aimYaw);
				const float dy = std::cos(pkt.body.aimYaw);
				shot.body.dir   = Vec3{dx * 0.9f, dy * 0.9f, 0.436f};
				shot.body.speed = 0.26f;
				client.Send(shot, CH_EVENT);

				// Where the real thing would land, so the explosion the ghost
				// sends later shows up somewhere plausible instead of right
				// on top of it.
				pendingBlastPos = Vec3{centre.x, centre.y, centre.z};
				pendingBlastMs  = nowMs + FUSE_MS;
			}

			if (pendingBlastMs != 0 && nowMs >= pendingBlastMs) {
				pendingBlastMs = 0;

				C_Explosion blast{};
				InitHeader(blast, nowMs);
				blast.body.type = EXPLOSION_MOLOTOV_TYPE;
				blast.body.pos  = pendingBlastPos;
				client.Send(blast, CH_EVENT);
				std::printf("molotov went off at (%.1f %.1f %.1f)\n",
				            pendingBlastPos.x, pendingBlastPos.y, pendingBlastPos.z);
			}

			// ---- the car ---------------------------------------------------
			if (wantCar) {
				if (myVehicleNetId == INVALID_NETID) {
					// Claim once. It's on the reliable channel so it will
					// arrive - sending it every tick would register 25 new
					// cars a second.
					if (!carClaimSent) {
						C_EnterVehicle claim{};
						InitHeader(claim, WallClock::NowMs());
						claim.body.netId   = INVALID_NETID;
						claim.body.seat    = 0;
						claim.body.modelId = carModel;
						claim.body.colour1 = 6;
						claim.body.colour2 = 6;
						claim.body.pos     = target;
						claim.body.rot     = Quat{0.0f, 0.0f, 0.0f, 1.0f};
						client.Send(claim, CH_EVENT);
						carClaimSent = true;
						std::printf("claiming a car (model %u)\n", carModel);
					}
				} else {
					// Kept in front of the player rather than orbiting them.
					//
					// An orbit reads well on paper but is useless as a test
					// fixture. The save point this always gets tested from is
					// a narrow alley, so a circle of any radius spends most of
					// its time inside a shutter or a wall - a car working
					// perfectly ends up looking like one that never spawned.
					// The camera sits behind the player looking along their
					// heading, so putting the car seven metres up that heading
					// keeps it in the middle of the screen.
					//
					// re3's forward vector for a heading is (-sin, cos); see
					// CPed::m_fRotationCur, Ped.h:444.
					constexpr float AHEAD_M = 7.0f;
					const float     fx = -std::sin(targetHeading);
					const float     fy = std::cos(targetHeading);

					// A slow weave across the player's line of sight, just so
					// interpolation and rotation have something to do.
					const float sway = 2.0f * std::sin(angle * 0.5f);

					C_VehicleState v{};
					InitHeader(v, WallClock::NowMs());
					v.body.netId = myVehicleNetId;
					v.body.pos.x = target.x + fx * AHEAD_M - fy * sway;
					v.body.pos.y = target.y + fy * AHEAD_M + fx * sway;
					v.body.pos.z = target.z;

					// Facing back at the player, as a quaternion about Z.
					// Carrying a rotation instead of a yaw is what lets this
					// generalise to a car on its roof.
					const float yaw = targetHeading + 3.1415927f;
					v.body.rot = Quat{0.0f, 0.0f, std::sin(yaw * 0.5f),
					                  std::cos(yaw * 0.5f)};

					v.body.moveSpeed.x = -fy * std::cos(angle * 0.5f);
					v.body.moveSpeed.y = fx * std::cos(angle * 0.5f);

					v.body.health = 1000.0f;
					v.body.gear   = 2;
					v.body.gas    = 0.8f;
					v.body.flags  = VEH_ENGINE_ON | VEH_LIGHTS;
					client.Send(v, CH_SNAPSHOT);

					// Get out, and later get back in. The car keeps weaving
					// either way - that's the point, the two halves need to
					// come apart cleanly. A driver still glued to a car that
					// left without them, or a ped still riding a seat it was
					// taken out of, both show up immediately.
					if (wantInOut && WallClock::NowMs() >= nextSwitchMs) {
						if (ridingNow) {
							C_ExitVehicle out;
							InitHeader(out, WallClock::NowMs());
							out.netId = myVehicleNetId;
							client.Send(out, CH_EVENT);
							ridingNow    = false;
							nextSwitchMs = WallClock::NowMs() + OUT_MS;
							std::printf("stepping out of car %u\n", myVehicleNetId);
						} else {
							// Back in by netId, not by identity - the session
							// already knows this car, and re-sending identity
							// with INVALID_NETID would just register a second
							// one.
							C_EnterVehicle back{};
							InitHeader(back, WallClock::NowMs());
							back.body.netId = myVehicleNetId;
							back.body.seat  = 0;
							client.Send(back, CH_EVENT);
							ridingNow    = true;
							nextSwitchMs = WallClock::NowMs() + IN_MS;
							std::printf("getting back into car %u\n", myVehicleNetId);
						}
					}
				}
			}

			if (++sentCount % 25 == 0)
				std::printf("t=%5.1fs  pos %.1f %.1f %.1f  %s\n", t, pkt.body.pos.x,
				            pkt.body.pos.y, pkt.body.pos.z,
				            haveTarget ? "orbiting a real player" : "no one else here yet");
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}
