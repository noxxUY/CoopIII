// A synthetic second player.
//
//   ghost [host] [port] [nick] [-car] [-inout] [-shoot] [-throw] [-hurt]
//         [-flame] [-rocket] [-burn] [-far]
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
// -pitch rakes those shots up and down instead of firing them level. The one
//        thing an observer's engine cannot invent for itself is the slope:
//        CWeapon::FireInstantHit copies target.z from source.z for every ped
//        that is not the local player, so a replayed shot is flat unless the
//        wire's direction is really being applied. Trails that stay level
//        under -pitch mean it is not.
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
// -wanted N
//        claims N stars, the way a real client reports its own CWanted.
//        docs/wanted.md §7 is the list this exists to work through, and it is
//        the only way to reach any of it with one real game in front of you:
//        a real player's level comes out of crimes their own engine watched
//        them commit, so nothing short of a second machine can produce one.
//
//        What to watch is the receiving end, and none of it is the ghost.
//        With the default per-player rule, walk past it and your own stars
//        must stay empty - proximity spreads nothing. Get into its car
//        (-car -wanted 4) and yours should climb to four within a tick, and
//        the police that turn up are then your own engine's, generated from
//        your own CWanted, chasing you. Get out again and they must stay:
//        a level that evaporated at the door is the bug §4.4 argues against.
//        With the server set to `shared`, the stars should arrive wherever
//        you are standing and go again when the ghost drops to zero.
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
// -far   widens the orbit from four metres to a sweep between 20 and 200,
//        out and back every minute. That is the radar's business: the radar
//        reaches 120 m on foot, so the ghost's blip walks out to the rim,
//        pins to it the way every GTA III blip does, and comes back in. It
//        is also the one thing worth looking at twice, because it is the
//        handover docs/roadmap.md §5.3 will need - a player that far away
//        eventually stops having a ped, and the blip has to survive that.
//        On foot only; with -car the blip follows the car and this would be
//        testing the car.
// -limbs claims a pedestrian of its own three metres in front of the player,
//        as if the ghost's engine had generated one, and then takes it apart:
//        the head, both arms and both legs, one every three seconds, in
//        C_PedBodyPart packets (protocol 17). Then it despawns the ped and
//        claims another. That is the observer half of dismemberment with no
//        second game: the player's machine builds a replica and runs
//        CPed::RemoveBodyPart on it for each packet. Watch for each limb to
//        vanish with a spray of blood, the arms and legs to fly off, and the
//        game to still be running after a few rounds. The head does not fly;
//        the engine never spawns one (re3 RemoveBodyPart, `nodeId != PED_HEAD`).
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

// How far -pitch tilts the shot either side of level, and how long one sweep
// takes. 40 degrees because a trail that steep is unmistakable against a
// street; 6 seconds because the whole point is to watch it move.
constexpr float    SHOT_PITCH_DEG    = 40.0f;
constexpr uint32_t SHOT_PITCH_PERIOD = 6000;

// -far. The radar reaches 120 m on foot (addresses.h, RADAR_RANGE_ON_FOOT_M),
// so 20 to 200 m crosses the rim well inside it and well outside it. The
// periods are long because the speed is the tangent of the orbit and a
// 200 m circle in twelve seconds is 105 m/s, which is not a person.
constexpr float FAR_NEAR_M    = 20.0f;
constexpr float FAR_OUT_M     = 200.0f;
constexpr float FAR_PERIOD_S  = 180.0f;   // once round: ~7 m/s at 200 m
constexpr float FAR_BREATHE_S = 60.0f;    // out and back

} // namespace

int main(int argc, char **argv) {
	// Unbuffered, for the same reason as server/cli/console.cpp: the ghost is
	// always stopped by being killed, and a redirected, buffered stdout dies
	// with it.
	std::setvbuf(stdout, nullptr, _IONBF, 0);

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
	bool farFlag    = false;
	// -blowup: blow the ghost's car up on a cycle, so the whole destruction
	// path can be watched in a live game. There is no other way to reach it -
	// the observer half runs the engine's own CAutomobile::BlowUpCar on a
	// CAutomobile CoopIII built by hand, and nothing headless can touch that.
	bool blowUpFlag = false;
	bool limbsFlag  = false;
	bool pitchFlag  = false;
	// -extras A,B: the extra components the ghost claims its car has, which
	// is how the extras sync gets looked at. Default is "none on either
	// slot", because 0/0 would quietly fit component 0 twice and look like a
	// working sync whether or not anything crossed the wire.
	int8_t extra1 = -1, extra2 = -1;
	// -wanted N: the ghost claims N stars. docs/wanted.md §7 is what this is
	// for; 0 is off and is the default, because a ghost that was always
	// wanted would put police on every test that has nothing to do with them.
	int wantedLevel = 0;
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
		if (std::strcmp(argv[i], "-far") == 0)
			farFlag = true;
		if (std::strcmp(argv[i], "-blowup") == 0)
			blowUpFlag = true;
		if (std::strcmp(argv[i], "-limbs") == 0)
			limbsFlag = true;
		if (std::strcmp(argv[i], "-pitch") == 0)
			pitchFlag = true;
		if (std::strcmp(argv[i], "-wanted") == 0 && i + 1 < argc) {
			wantedLevel = std::atoi(argv[i + 1]);
			if (wantedLevel < 0)
				wantedLevel = 0;
			if (wantedLevel > WANTED_LEVEL_CEILING)
				wantedLevel = WANTED_LEVEL_CEILING;
			++i;
		}
		if (std::strcmp(argv[i], "-extras") == 0 && i + 1 < argc) {
			int a = -1, b = -1;
			if (std::sscanf(argv[i + 1], "%d,%d", &a, &b) >= 1) {
				extra1 = static_cast<int8_t>(a);
				extra2 = static_cast<int8_t>(b);
			}
			++i;
		}
	}

	// Both car modes need a car.
	if (blowUpFlag && !carFlag) {
		carFlag = true;
		std::printf("-blowup implies -car, since it is the car it blows up\n");
	}

	// -far and -car cannot both be what is being watched. A seated player's
	// blip is drawn at their car's position, by the engine, so a ghost 200 m
	// away in a car is testing the car's own sync and not the radar's rim.
	if (farFlag && carFlag) {
		farFlag = false;
		std::printf("-far is the on-foot rim test; -car wins and the orbit stays "
		            "close\n");
	}
	if (farFlag)
		std::printf("-far: sweeping %.0f to %.0f m, so the blip leaves the radar's "
		            "%.0f m rim and comes back\n",
		            FAR_NEAR_M, FAR_OUT_M, 120.0f);

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

	// -pitch is the trail test. A flat shot is the one thing an observer's own
	// engine would have produced anyway - CWeapon::FireInstantHit copies
	// target.z from source.z for every ped that is not the local player - so a
	// flat ghost cannot tell a working direction from a missing one. Sweeping
	// the pitch makes the answer visible from across the street: the trails
	// rake up and down, or they lie flat and the wire is not being used.
	if (pitchFlag && !shootFlag) {
		shootFlag = true;
		std::printf("-pitch implies -shoot, since it is the shots it tilts\n");
	}
	if (pitchFlag)
		std::printf("-pitch: sweeping the shot direction %.0f degrees up and down, so "
		            "a flat trail on screen means the direction is not arriving\n",
		            SHOT_PITCH_DEG);
	if (wantedLevel > 0)
		std::printf("-wanted %d: the ghost claims %d star%s. With the default "
		            "rule, get into its car and yours should climb to %d; with "
		            "the session set to shared, they should climb wherever you "
		            "are standing\n",
		            wantedLevel, wantedLevel, wantedLevel == 1 ? "" : "s",
		            wantedLevel);

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

	// -blowup. Eight seconds of ordinary driving, then the car explodes, and
	// then the ghost claims a fresh one - which is the only way to see the
	// second half of the rule, that a wreck is never respawned as a new car
	// but a genuinely new claim still is.
	//
	// A wreck also has to be left alone for long enough to watch the engine
	// clear it away by itself about 60 seconds later
	// (VEH_WRECK_REMOVAL_MS), so the pause afterwards is deliberately long.
	const bool     wantBlowUp    = blowUpFlag;
	const uint32_t DRIVE_MS      = 8000;
	const uint32_t WRECK_MS      = 75000;
	uint32_t       nextBlowUpMs  = 0;
	uint32_t       reclaimAtMs   = 0;   // 0 = not waiting to reclaim
	bool           carIsWrecked  = false;

	// -limbs. One pedestrian at a time: claimed, named by the server, taken
	// apart one limb per step, then despawned and claimed again. The order is
	// the five nodes InflictDamage passes, head first.
	constexpr uint8_t  LIMB_ORDER[] = {2, 3, 4, 7, 8};
	constexpr uint32_t LIMB_STEP_MS = 3000;
	constexpr uint16_t LIMB_PED_MODEL = 7;   // MI_MALE01
	uint32_t limbTempId   = 0;               // the claim in flight, 0 = none
	uint16_t limbPedNetId = INVALID_NETID;
	size_t   limbNext     = 0;
	uint32_t limbNextMs   = 0;

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
			} else if (const S_PedSpawn *ps = msg.as<S_PedSpawn>()) {
				if (limbTempId != 0 && ps->ownerPlayerId == myId &&
				    ps->tempId == limbTempId) {
					limbPedNetId = ps->netId;
					limbTempId   = 0;
					limbNext     = 0;
					// A moment for the player's machine to stream the model and
					// build the replica before the first limb arrives for it.
					limbNextMs   = WallClock::NowMs() + LIMB_STEP_MS;
					std::printf("our pedestrian is net %u\n", limbPedNetId);
				}
			} else if (const S_EnterVehicle *e = msg.as<S_EnterVehicle>()) {
				if (e->playerId == myId && myVehicleNetId == INVALID_NETID) {
					myVehicleNetId = e->body.netId;
					nextSwitchMs   = WallClock::NowMs() + IN_MS;
					nextBlowUpMs   = WallClock::NowMs() + DRIVE_MS;
					std::printf("our car is net %u\n", myVehicleNetId);
				}
			}
		}

		// -limbs: claim a pedestrian in front of the player, then take it apart.
		if (limbsFlag && client.IsConnected() && myId != 0xFF && haveTarget) {
			const uint32_t now = WallClock::NowMs();
			if (limbPedNetId == INVALID_NETID && limbTempId == 0) {
				static uint32_t nextTemp = 1;
				C_PedSpawn claim{};
				InitHeader(claim, now);
				claim.tempId       = nextTemp++;
				claim.body.modelId = LIMB_PED_MODEL;
				claim.body.pedType = 4;   // PEDTYPE_CIVMALE
				claim.body.pos.x   = target.x + 3.0f * -std::sin(targetHeading);
				claim.body.pos.y   = target.y + 3.0f * std::cos(targetHeading);
				claim.body.pos.z   = target.z;
				// Facing the player, so the limbs that fly go past them.
				claim.body.heading = targetHeading + 3.1415927f;
				client.Send(claim, CH_EVENT);
				limbTempId = claim.tempId;
				std::printf("claimed a pedestrian in front of the player\n");
			} else if (limbPedNetId != INVALID_NETID && now >= limbNextMs) {
				if (limbNext < sizeof(LIMB_ORDER)) {
					C_PedBodyPart off{};
					InitHeader(off, now);
					off.body.netId     = limbPedNetId;
					off.body.node      = LIMB_ORDER[limbNext];
					off.body.direction = static_cast<int8_t>(limbNext % 4);
					client.Send(off, CH_EVENT);
					std::printf("took node %u off ped %u\n", off.body.node, limbPedNetId);
					++limbNext;
					limbNextMs = now + LIMB_STEP_MS;
				} else {
					C_PedDespawn gone{};
					InitHeader(gone, now);
					gone.netId = limbPedNetId;
					client.Send(gone, CH_EVENT);
					std::printf("despawned ped %u; claiming another\n", limbPedNetId);
					limbPedNetId = INVALID_NETID;
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
			const float t      = (WallClock::NowMs() - startMs) / 1000.0f;
			const float period = farFlag ? FAR_PERIOD_S : ORBIT_PERIOD_S;
			const float angle  = 6.2831853f * (t / period);

			// -far breathes the orbit in and out across the radar's own rim
			// (120 m on foot, RADAR_RANGE_ON_FOOT_M) instead of holding one
			// radius. A fixed radius past the rim would pin the blip to the
			// edge and leave it there, which tests the clamp and nothing
			// else; what actually needs watching is the crossing in both
			// directions, and it is the same handover docs/roadmap.md 5.3
			// will need when a distant player stops having a ped at all.
			float radius = ORBIT_RADIUS_M;
			if (farFlag) {
				const float phase = 6.2831853f * (t / FAR_BREATHE_S);
				radius = FAR_NEAR_M +
				         (FAR_OUT_M - FAR_NEAR_M) * 0.5f * (1.0f - std::cos(phase));
			}

			C_PlayerState pkt{};
			InitHeader(pkt, WallClock::NowMs());

			// Orbit the player we found. There's no "no target" case left to
			// handle here - the send is already gated on haveTarget above.
			const Vec3 centre = target;
			pkt.body.pos.x    = centre.x + radius * std::cos(angle);
			pkt.body.pos.y    = centre.y + radius * std::sin(angle);
			pkt.body.pos.z    = centre.z;

			// Face along the direction of travel, which is the tangent.
			pkt.body.heading = angle + 1.5707963f;

			// Tangential only. The radial part of -far's sweep is slow enough
			// next to this that leaving it out costs nothing, and moveSpeed
			// is what the receiver picks a walk or a run from rather than
			// anything it positions with.
			const float speed      = 6.2831853f * radius / period;
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

			// -wanted N: claim N stars, earned rather than borrowed.
			//
			// The only way to look at docs/wanted.md from a live game with
			// one real player in it. A real client's level comes out of its
			// own CWanted, so nothing short of a second machine committing a
			// crime can produce one - and the thing worth watching is the
			// receiving end: whether getting into the ghost's car gives you
			// its stars, and whether the police that then turn up are your
			// own engine's.
			//
			// Never borrowed. A ghost claiming a borrowed level would be
			// claiming there is a third player it took the level from, and in
			// the shared rule that is exactly the value the receiver is meant
			// to ignore - so the one flag that would make this mode do
			// nothing is the one it must not set.
			if (wantedLevel > 0)
				pkt.body.flags = FlagsWithWanted(pkt.body.flags,
				                                 static_cast<uint8_t>(wantedLevel),
				                                 /*borrowed=*/false);

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
				// Level unless -pitch, in which case it rakes up and down. The
				// tilt is the only thing in a ghost's shot that an observer
				// could not have invented for itself, so it is the only thing
				// that proves the direction crossed the wire.
				float pitch = 0.0f;
				if (pitchFlag) {
					const float phase =
					    static_cast<float>(nowMs % SHOT_PITCH_PERIOD) /
					    static_cast<float>(SHOT_PITCH_PERIOD);
					pitch = SHOT_PITCH_DEG * 0.0174532925f *
					        std::sin(phase * 6.2831853f);
				}
				const float flat = std::cos(pitch);
				shot.body.dir    = Vec3{-std::sin(pkt.body.aimYaw) * flat,
				                        std::cos(pkt.body.aimYaw) * flat,
				                        std::sin(pitch)};
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
						// Explicit, not left at the struct's zero: a 0 here
						// would fit component 0 on both slots, which is a
						// plausible-looking car whether or not the field
						// crossed the wire at all. -1 is "nothing fitted",
						// which is what the engine itself passes.
						claim.body.extra1  = extra1;
						claim.body.extra2  = extra2;
						claim.body.pos     = target;
						claim.body.rot     = Quat{0.0f, 0.0f, 0.0f, 1.0f};
						client.Send(claim, CH_EVENT);
						carClaimSent = true;
						std::printf("claiming a car (model %u, extras %d/%d)\n",
						            carModel, static_cast<int>(extra1),
						            static_cast<int>(extra2));
					}
				} else if (carIsWrecked) {
					// Nothing. A wreck's driver has stopped sending, and
					// that is not laziness on the ghost's part - it is what
					// the real client does, and it is the case the observer
					// has to hold the wreck in place through.
					//
					// After long enough to watch the engine clear the wreck
					// away by itself, claim a fresh car. Two things get
					// looked at that way round: that an empty pool slot left
					// by a wreck is never respawned, and that a genuinely new
					// claim still is.
					if (reclaimAtMs != 0 && WallClock::NowMs() >= reclaimAtMs) {
						reclaimAtMs    = 0;
						carIsWrecked   = false;
						carClaimSent   = false;
						myVehicleNetId = INVALID_NETID;
						ridingNow      = true;
						std::printf("the wreck has had its minute; claiming a "
						            "fresh car\n");
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

					// Blow it up, at the position the last snapshot put it.
					// The real client reads the transform off the car inside
					// the BlowUpCar detour; this is the nearest a ghost can
					// get, and it is the same claim - "this is where my car
					// ended up" - so an observer that puts the wreck anywhere
					// else is wrong in a way that shows.
					//
					// Sent whether or not the ghost is currently "riding":
					// the server only takes a blast from the player it
					// believes is driving, so with -inout as well this also
					// exercises the server refusing one, which should print
					// nothing and change nothing.
					if (wantBlowUp && WallClock::NowMs() >= nextBlowUpMs) {
						C_VehicleBlowUp up{};
						InitHeader(up, WallClock::NowMs());
						up.body.netId = myVehicleNetId;
						up.body.pos   = v.body.pos;
						up.body.rot   = v.body.rot;
						client.Send(up, CH_EVENT);
						carIsWrecked = true;
						reclaimAtMs  = WallClock::NowMs() + WRECK_MS;
						std::printf("blowing up car %u at (%.1f %.1f %.1f)\n",
						            myVehicleNetId, v.body.pos.x, v.body.pos.y,
						            v.body.pos.z);
					}

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
