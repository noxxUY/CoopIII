// A synthetic second player.
//
//   ghost [host] [port] [nick] [-car] [-inout] [-shoot] [-throw] [-hurt]
//         [-flame] [-rocket] [-burn]
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
// -rocket the same two halves with a rocket launcher, and it exists because
//        the rocket is the one projectile the engine places somewhere nobody
//        asked for. CProjectileInfo::AddProjectile's rocket arm for a ped
//        that is neither the player nor chasing anybody copies the ped's own
//        matrix and never reads the fire source, so without the correction in
//        combat.cpp the missile is born inside the remote player and is gone
//        again before it is drawn - while the explosion still arrives and
//        still lands in the right street. That is the exact thing that was
//        reported, and one game plus this flag reproduces it.
//
//        The direction carries a deliberate upward tilt, because a rocket
//        flying flat out of a ped's chest is what a lost pitch looks like and
//        it should be obvious rather than arguable. The explosion follows at
//        1200 ms, inside the missile's own 1400 ms deadline, so the thing
//        that ends the flight is the owner saying so and not the clock
//        running out.
// -flame holds a flamethrower and keeps the trigger down. The shot goes
//        through CWeapon::FireAreaEffect into CShotInfo, which keeps lighting
//        fires for a second after the call returns, so this is the cheapest
//        way to put a real fire in the world without a second game running.
//        Watch for flame coming out of the remote ped, then go and stand in
//        it. Your health is the test and the answer depends on the server:
//
//          started with -friendlyfire   you burn. The ghost's fire is the
//                                       ghost's, and the session allows it.
//          started without              you do not. The flame still comes
//                                       out; only the damage is declined,
//                                       and CoopIII.log says so in as many
//                                       words rather than going quiet.
//
//        Either way the decision is made here, on this machine, about this
//        player, from a fire this engine put in this street - docs/protocol.md
//        §1.10.6. Nothing about it goes on the wire.
// -burn  claims to be on fire for four seconds out of every eight, by setting
//        PF_ON_FIRE in the snapshot the way a real client sets it from its own
//        CPed::m_pFire. This is the only way to see roadmap §5.7 phase three
//        without two real games and a rocket launcher: the ghost's ped should
//        catch fire, burn, and go out again on the cycle, without ever
//        fleeing, sprinting or breaking stride - the pose stream still owns
//        where it goes. Two things to watch beyond the flames. Stand next to
//        it: the fire should spread to you the way it does in single player,
//        and your own machine decides what that costs you. And check the
//        ghost's health does not move, because an observer's fire may not
//        take health off the player it is drawn on.
// -hurt  makes the shots real, in both directions. Needs the server started
//        with -friendlyfire, or it does nothing at all and that is the gate
//        working.
//
//        Outbound: a C_Damage per round, so the player watches their health
//        drop, dies, fades to the hospital and comes back. That covers
//        S_Damage reaching CPed::InflictDamage on the local player, the
//        CPed::SetDie detour catching the death, and the C_Respawn that
//        follows.
//
//        Inbound: the ghost takes the damage it is sent, announces its own
//        death when it runs out and respawns four seconds later. That is the
//        only way to see the observer half in a real game: a remote ped
//        playing a death animation, staying a corpse, and then being rebuilt
//        somewhere else. Shoot the ghost and watch it fall over.
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
	bool hurtFlag   = false;
	bool flameFlag  = false;
	bool rocketFlag = false;
	bool burnFlag   = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-car") == 0)
			carFlag = true;
		if (std::strcmp(argv[i], "-inout") == 0)
			inOutFlag = true;
		if (std::strcmp(argv[i], "-shoot") == 0)
			shootFlag = true;
		if (std::strcmp(argv[i], "-throw") == 0)
			throwFlag = true;
		if (std::strcmp(argv[i], "-hurt") == 0)
			hurtFlag = true;
		if (std::strcmp(argv[i], "-flame") == 0)
			flameFlag = true;
		if (std::strcmp(argv[i], "-rocket") == 0)
			rocketFlag = true;
		if (std::strcmp(argv[i], "-burn") == 0)
			burnFlag = true;
	}

	// -rocket and -throw are the same two halves with a different weapon, so
	// they share one in-flight slot. Asking for both is asking for one.
	if (rocketFlag && throwFlag) {
		throwFlag = false;
		std::printf("-rocket and -throw share one projectile; using the rocket\n");
	}

	// The flamethrower is a held trigger, not a burst, so it rides the same
	// shot cadence as everything else and just never lets go.
	if (flameFlag)
		shootFlag = true;

	// -hurt on its own would be a player losing health with nothing on
	// screen to explain it, which is a worse test than no test.
	if (hurtFlag && !shootFlag) {
		shootFlag = true;
		std::printf("-hurt implies -shoot, so the damage has a muzzle flash\n");
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
	constexpr uint8_t  WEAPON_UZI         = 3;
	constexpr uint8_t  WEAPON_ROCKETLAUNCHER = 8;
	constexpr uint8_t  WEAPON_FLAMETHROWER = 9;
	constexpr uint8_t  WEAPON_MOLOTOV     = 10;
	constexpr uint8_t  EXPLOSION_MOLOTOV_TYPE = 1;
	constexpr uint8_t  EXPLOSION_ROCKET_TYPE  = 2;
	constexpr uint32_t SHOT_PERIOD_MS  = 250;    // four rounds a second
	constexpr uint32_t THROW_PERIOD_MS = 3000;
	// Gap between a throw and its explosion. Roughly a molotov's own
	// two-second fuse, giving the receiver time to actually fly the bottle
	// before it's told where it landed. Only way to see whether the bottle
	// gets removed silently.
	constexpr uint32_t FUSE_MS = 2000;
	// A missile's own deadline is CTimer + 1400 (CProjectileInfo::AddProjectile,
	// 0x0055B310). Staying inside it means the flight is ended by its owner
	// saying where it went off, which is the half worth watching, rather than
	// by the observer's clock running out on its own.
	constexpr uint32_t ROCKET_FUSE_MS   = 1200;
	constexpr uint32_t ROCKET_PERIOD_MS = 4000;

	const bool    wantProjectile = throwFlag || rocketFlag;
	const uint8_t heldWeapon = rocketFlag ? WEAPON_ROCKETLAUNCHER
	                           : throwFlag  ? WEAPON_MOLOTOV
	                           : flameFlag ? WEAPON_FLAMETHROWER
	                           : shootFlag ? WEAPON_UZI
	                                       : 0;   // WEAPONTYPE_UNARMED
	const uint8_t  blastType =
	    rocketFlag ? EXPLOSION_ROCKET_TYPE : EXPLOSION_MOLOTOV_TYPE;
	const uint32_t projPeriodMs = rocketFlag ? ROCKET_PERIOD_MS : THROW_PERIOD_MS;
	const uint32_t projFuseMs   = rocketFlag ? ROCKET_FUSE_MS : FUSE_MS;
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
	uint16_t targetNetId = INVALID_NETID;
	Vec3     target{0.0f, 0.0f, 0.0f};
	float    targetHeading = 0.0f;
	bool     haveTarget = false;
	uint32_t sentCount  = 0;

	// -hurt. The ghost keeps its own health the way a real client keeps the
	// engine's: it goes down when somebody hits it, and when it runs out the
	// ghost is the one that says so. Nobody else gets a vote, which is the
	// rule the whole damage design is built on.
	uint16_t myNetId      = INVALID_NETID;
	float    ghostHealth  = 100.0f;
	bool     ghostDead    = false;
	uint32_t reviveAtMs   = 0;
	uint8_t  hitDirection = 0;
	// Per round. An uzi does about 8, and the player's own 0.33 multiplier is
	// applied on their machine, not here - the wire carries the argument, not
	// the outcome.
	constexpr float    HURT_PER_ROUND  = 8.0f;
	constexpr uint8_t  PEDPIECE_TORSO_ = 0;
	constexpr uint16_t ANIM_KO_FRONT   = 13;   // ANIM_STD_KO_FRONT
	// What GTA III itself waits before the hospital (CGameLogic::Update's
	// 0x1000 ms), so the ghost's corpse lies there about as long as a real
	// player's does.
	constexpr uint32_t GHOST_DEATH_MS  = 4096;

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
				myId    = w->playerId;
				myNetId = w->netId;
				std::printf("welcomed as player %u (net %u), friendly fire %s\n", myId,
				            myNetId,
				            (w->flags & SESSION_FRIENDLY_FIRE) ? "on" : "off");
				if (hurtFlag && !(w->flags & SESSION_FRIENDLY_FIRE))
					std::printf("  -hurt will do nothing: restart the server with "
					            "-friendlyfire\n");
			} else if (const S_PlayerJoin *j = msg.as<S_PlayerJoin>()) {
				if (j->playerId != myId) {
					char n[NICK_LEN];
					std::memcpy(n, j->nick, sizeof(n));
					n[NICK_LEN - 1] = '\0';
					targetId        = j->playerId;
					targetNetId     = j->netId;
					target          = j->pos;
					targetHeading   = j->heading;
					haveTarget      = true;
					std::printf("following player %u (\"%s\", net %u) at %.1f %.1f %.1f\n",
					            j->playerId, n, j->netId, j->pos.x, j->pos.y, j->pos.z);
				}
			} else if (const S_Damage *d = msg.as<S_Damage>()) {
				// Somebody shot the ghost. Health is ours to keep, so this is
				// the only place it goes down, and the death that follows is
				// ours to announce.
				if (!ghostDead && d->body.victimNetId == myNetId) {
					ghostHealth -= d->body.amount;
					std::printf("hit for %.0f by player %u, %.0f left\n", d->body.amount,
					            d->attackerId, ghostHealth > 0.0f ? ghostHealth : 0.0f);
					if (ghostHealth <= 0.0f) {
						ghostHealth = 0.0f;
						ghostDead   = true;
						reviveAtMs  = WallClock::NowMs() + GHOST_DEATH_MS;

						C_Death death;
						InitHeader(death, WallClock::NowMs());
						death.killerNetId =
						    d->attackerId == targetId ? targetNetId : INVALID_NETID;
						death.animId = ANIM_KO_FRONT;
						client.Send(death, CH_EVENT);
						std::printf("the ghost died\n");
					}
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

			// Health rides the snapshot like a real client's does. A ghost
			// lying dead keeps sending it, which is what makes the corpse
			// stay where it fell instead of drifting.
			pkt.body.health    = ghostHealth;
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

			// -burn: catch fire for four seconds out of every eight.
			//
			// A real client sets this from its own CPed::m_pFire, so the only
			// way to see it from here is to claim it. The cycle is the point:
			// lighting a remote ped is one thing and putting it out again is
			// another, and the second one is where an observer's fire would
			// otherwise sit on somebody who stopped burning ten seconds ago.
			if (burnFlag && std::fmod(t, 8.0f) < 4.0f)
				pkt.body.flags |= PF_ON_FIRE;

			client.Send(pkt, CH_SNAPSHOT);

			// ---- combat ----------------------------------------------------
			//
			// Sent on the reliable channel, outside the snapshot, because
			// that's how a real client sends them. Also because a shot is an
			// event: four a second, each one its own packet, and the
			// receiving client has to replay four muzzle flashes rather than
			// just see one flag set for a second.
			const uint32_t nowMs = WallClock::NowMs();

			// Back on our feet. The position is wherever the orbit has
			// reached, which stands in for the hospital a real client would
			// report: what matters to the receiver is that it is somewhere
			// else and that the ped has to be rebuilt to get there.
			if (ghostDead && nowMs >= reviveAtMs) {
				ghostDead   = false;
				ghostHealth = 100.0f;

				C_Respawn back;
				InitHeader(back, nowMs);
				back.body.pos     = pkt.body.pos;
				back.body.heading = pkt.body.heading;
				client.Send(back, CH_EVENT);
				std::printf("the ghost respawned\n");
			}

			if (shootFlag && !ghostDead && nowMs >= nextShotMs) {
				nextShotMs = nowMs + SHOT_PERIOD_MS;

				C_Shot shot{};
				InitHeader(shot, nowMs);
				shot.body.weapon = heldWeapon;
				// Roughly where a held gun sits: chest height, at the ped.
				shot.body.origin = Vec3{pkt.body.pos.x, pkt.body.pos.y,
				                        pkt.body.pos.z + 0.6f};
				shot.body.dir    = Vec3{-std::sin(pkt.body.aimYaw),
				                        std::cos(pkt.body.aimYaw), 0.0f};
				shot.body.speed  = 0.0f;   // instant hit: no projectile
				client.Send(shot, CH_EVENT);

				// And the hit that goes with it. A real client works this out
				// from its own bullet trace inside CPed::InflictDamage; the
				// ghost has no trace, so it just declares one. The packet is
				// identical either way, which is the point.
				if (hurtFlag && targetNetId != INVALID_NETID) {
					C_Damage hit{};
					InitHeader(hit, nowMs);
					hit.body.victimNetId = targetNetId;
					hit.body.weapon      = WEAPON_UZI;
					hit.body.amount      = HURT_PER_ROUND;
					hit.body.piece       = PEDPIECE_TORSO_;
					// Cycled so all four ANIM_STD_HIGHIMPACT_* reactions get
					// a turn. A player who always falls the same way is a
					// direction byte that never made it across.
					hit.body.direction   = hitDirection;
					hitDirection         = static_cast<uint8_t>((hitDirection + 1) % 4);
					client.Send(hit, CH_EVENT);
				}
			}

			if (wantProjectile && !ghostDead && nowMs >= nextThrowMs &&
			    pendingBlastMs == 0) {
				nextThrowMs = nowMs + projPeriodMs;

				C_Shot shot{};
				InitHeader(shot, nowMs);
				shot.body.weapon = heldWeapon;
				shot.body.origin = Vec3{pkt.body.pos.x, pkt.body.pos.y,
				                        pkt.body.pos.z + 0.6f};
				const float dx = -std::sin(pkt.body.aimYaw);
				const float dy = std::cos(pkt.body.aimYaw);
				if (rocketFlag) {
					// Toward the player and visibly upward, at the 1.25 the
					// engine gives a rocket. The tilt is the point: if it ever
					// stops arriving, the missile flies flat out of the ped's
					// chest and that is unmistakable on screen rather than
					// something to argue about afterwards.
					shot.body.dir   = Vec3{dx * 0.94f, dy * 0.94f, 0.34f};
					shot.body.speed = 1.25f;
				} else {
					// A lobbed arc toward the player, at roughly the speed
					// CProjectileInfo::AddProjectile gives a half-charged throw.
					shot.body.dir   = Vec3{dx * 0.9f, dy * 0.9f, 0.436f};
					shot.body.speed = 0.26f;
				}
				client.Send(shot, CH_EVENT);

				// Where the real thing would land, so the explosion the ghost
				// sends later shows up somewhere plausible instead of right
				// on top of it.
				pendingBlastPos = Vec3{centre.x, centre.y, centre.z};
				pendingBlastMs  = nowMs + projFuseMs;
			}

			if (pendingBlastMs != 0 && nowMs >= pendingBlastMs) {
				pendingBlastMs = 0;

				C_Explosion blast{};
				InitHeader(blast, nowMs);
				blast.body.type = blastType;
				blast.body.pos  = pendingBlastPos;
				client.Send(blast, CH_EVENT);
				std::printf("%s went off at (%.1f %.1f %.1f)\n",
				            rocketFlag ? "rocket" : "molotov", pendingBlastPos.x,
				            pendingBlastPos.y, pendingBlastPos.z);
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
