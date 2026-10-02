// What the owner's mission shows, run again on every participant's machine by
// the participant's own engine (docs/missions.md 5.4). Pure: the list of what
// is replayed, how one instruction the owner's script ran becomes one the
// participant's interpreter can run, and the table from the owner's blip
// handles to the participant's. game/mission.cpp does the reading and the
// running.
//
// **Every operand is sent as a value.** The owner's instruction names its
// operands however the script wrote them, as literals, globals or locals of a
// script the participant does not have. What travels is what the owner's
// engine read: each value as a 32-bit literal (type 1), whose four bytes
// CollectParameters copies into ScriptParams untouched, so an int arrives an
// int and a float arrives the same float. Text labels, which have no type
// byte, go as their eight bytes. A global the handler reads by *offset* (the
// on-screen timer and counter) keeps its offset, since every machine runs the
// same main.scm. An output goes to the running script's local 0.
#pragma once

#include "missionaddr.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coopiii::game::replay {

// What each operand of an instruction on the replay list is.
enum class Arg : uint8_t {
	Value,    // read it, send its value
	Text,     // eight bytes of text label, no type byte
	Global,   // a global the handler takes by offset: send the offset
	Output,   // where the handler stores its result: the runner's local 0
	Blip,     // the owner's blip handle, translated to the participant's
	Object,   // the owner's cutscene object or head, translated the same way
	Char,     // a pedestrian the session names: its netId on the wire
	Car,      // a car the session names, the same
	Pickup,   // the owner's pickup handle, translated like a blip
	Fire,     // the owner's script fire, the same
	// An object named by the global that holds it, sent as that global: every
	// machine's own global holds its own object, the main script's doors
	// and the mission's objects alike (mission-audit.md R3).
	ObjGlobal,
	OutGlobal,   // an object made into a global, which each machine fills itself
	// A blip made into a global, which each machine fills itself: a
	// contact's marker, which the main script and later missions take off
	// again by that global. Never in the blip map, so it outlives the mission.
	OutBlip,
	// A number sent as a 16-bit literal (type 5), for an instruction whose
	// operands would not fit the code as 32-bit ones: SET_ZONE_CAR_INFO's
	// sixteen. One that does not fit sixteen bits is not sent.
	Short,
	Sphere,   // the owner's script sphere handle, translated like a blip
	// A number the script names by a global goes as that global, for each
	// machine to read its own; a literal goes as a Value. What a mission
	// saved of its player at the start and gives back at the end: each
	// participant's own wanted level, not the owner's.
	OwnGlobal,
	// A continuous sound made into a global, which each machine fills itself,
	// and one named by that global: every machine's own handle into its own
	// audio script object pool (ADD_CONTINUOUS_SOUND, REMOVE_SOUND).
	SoundOut,
	SoundGlobal,
};

// What running it means for the participant.
enum class Kind : uint8_t {
	Plain,     // run it
	BlipNew,   // run it, and remember which of our blips stands for the owner's
	BlipUse,   // translate the blip first; drop it for a blip we never made
	Pay,       // ADD_SCORE: run it, unless the session keeps one shared wallet
	World,     // run it, and it is in the campaign: a save keeps what it did
	ObjectNew, // run it, and remember which of our objects stands for the owner's
	Teleport,  // SET_PLAYER_COORDINATES: our own player, beside the owner's spot
	PickupNew, // run it, and remember which of our pickups stands for the owner's
	FireNew,   // run it, and remember which of our script fires stands for the owner's
	// Run it where the car it names is simulated, when that is not the
	// owner's machine: its driver's, or its custodian's while nobody drives
	// it (mission-audit.md R9). The owner's own engine runs it too, on a copy
	// the next snapshot from the car's holder puts right.
	Holder,
	// The mission's player changes clothes: each participant changes its own
	// player's, when it safely can (game/outfit.h). Only ever sent for the
	// owner's own player, and its char operand goes as 0; it is never run as
	// it stands.
	Outfit,
	SphereNew, // run it, and remember which of our spheres stands for the owner's
	// Not run live: it counts once, so it goes in the campaign delta alone,
	// which every machine applies once, a participant in the mission too, and
	// the owner's own skips. Run live as well, a participant would count it
	// twice, once now and once from the delta (missions.md 12.1).
	Campaign,
};

constexpr size_t MAX_ARGS = 17;

struct Entry {
	uint16_t opcode;
	Kind     kind;
	uint8_t  count;
	Arg      args[MAX_ARGS];
};

// The replay list. An opcode that is not here is not replayed: a participant
// misses something visible, a failure anybody can see and fix, never an
// effect twice.
inline const Entry *Find(uint16_t opcode) {
	using A = Arg;
	using K = Kind;
	static const Entry kList[] = {
	    // The words on the screen.
	    {0x00BA, K::Plain, 3, {A::Text, A::Value, A::Value}},                 // PRINT_BIG
	    {0x00BB, K::Plain, 3, {A::Text, A::Value, A::Value}},                 // PRINT
	    {0x00BC, K::Plain, 3, {A::Text, A::Value, A::Value}},                 // PRINT_NOW
	    {0x00BD, K::Plain, 3, {A::Text, A::Value, A::Value}},                 // PRINT_SOON
	    {0x00BE, K::Plain, 0, {}},                                            // CLEAR_PRINTS
	    {0x01E3, K::Plain, 4, {A::Text, A::Value, A::Value, A::Value}},       // PRINT_WITH_NUMBER_BIG
	    {0x01E4, K::Plain, 4, {A::Text, A::Value, A::Value, A::Value}},       // PRINT_WITH_NUMBER
	    {0x01E5, K::Plain, 4, {A::Text, A::Value, A::Value, A::Value}},       // PRINT_WITH_NUMBER_NOW
	    {0x02FC, K::Plain, 5, {A::Text, A::Value, A::Value, A::Value, A::Value}},   // .._2_NUMBERS
	    {0x02FD, K::Plain, 5, {A::Text, A::Value, A::Value, A::Value, A::Value}},   // .._2_NUMBERS_NOW
	    {0x036D, K::Plain, 5, {A::Text, A::Value, A::Value, A::Value, A::Value}},   // .._2_NUMBERS_BIG
	    {0x03D5, K::Plain, 1, {A::Text}},                                     // CLEAR_THIS_PRINT
	    {0x03D6, K::Plain, 1, {A::Text}},                                     // CLEAR_THIS_BIG_PRINT
	    {0x03E5, K::Plain, 1, {A::Text}},                                     // PRINT_HELP
	    {0x03E6, K::Plain, 0, {}},                                            // CLEAR_HELP
	    {0x03EB, K::Plain, 0, {}},                                            // CLEAR_SMALL_PRINTS
	    // The queued big words, a label and two or three (0x00443437,
	    // 0x00443482): the Vigilante's bonus and its reward. And the time
	    // Salvatore leaves Luigi's, a label and four (0x00447959).
	    {0x0217, K::Plain, 3, {A::Text, A::Value, A::Value}},                 // PRINT_BIG_Q
	    {0x0218, K::Plain, 4, {A::Text, A::Value, A::Value, A::Value}},       // PRINT_WITH_NUMBER_BIG_Q
	    {0x02FE, K::Plain, 5, {A::Text, A::Value, A::Value, A::Value, A::Value}},   // .._2_NUMBERS_SOON
	    // A payphone's message, the phone then a label (0x00444710): Payday
	    // For Ray's phones. A phone is an index init.sc made in the same
	    // order on every machine.
	    {0x024C, K::Plain, 2, {A::Value, A::Text}},                           // SET_PHONE_MESSAGE
	    // And the same phones switched off at the end (0x004447B2, the phone:
	    // SetPhoneMessage_JustOnce with no message), so a phone the guest
	    // never lifted does not ring on with Ray's old directions.
	    {0x024E, K::Plain, 1, {A::Value}},                                    // TURN_PHONE_OFF
	    // Everybody is paid, and every machine's stats count the mission.
	    {0x0109, K::Pay, 2, {A::Value, A::Value}},                            // ADD_SCORE
	    {0x0317, K::Plain, 0, {}},                                            // INCREMENT_MISSION_ATTEMPTS
	    // The progress and the pass are the campaign's (0x00447D80 adds its
	    // one operand to CStats::ProgressMade; 0x00447F95 reads an eight-byte
	    // label, increments CStats::MissionsPassed and calls
	    // CheckPointReachedSuccessfully), and so are the three islands done
	    // (0x0044980C, 0x0044983D, 0x0044986E, no operands: each sets its
	    // CStats flag, and the first two play the radio's "island open"
	    // announcement, 13 and 14; SUBURBAN_PASSED plays none).
	    {0x030C, K::Campaign, 1, {A::Value}},                                 // PLAYER_MADE_PROGRESS
	    {0x0318, K::Campaign, 1, {A::Text}},                                  // REGISTER_MISSION_PASSED
	    {0x034A, K::Campaign, 0, {}},                                         // INDUSTRIAL_PASSED
	    {0x034B, K::Campaign, 0, {}},                                         // COMMERCIAL_PASSED
	    {0x034C, K::Campaign, 0, {}},                                         // SUBURBAN_PASSED
	    {0x0394, K::Plain, 1, {A::Value}},                                    // PLAY_MISSION_PASSED_TUNE
	    // And the odd jobs' own stats, which every machine's stats count the
	    // way they count the mission (handlers 0x00447F44, 0x00447F59 and
	    // 0x00588987..0x00588A5B: CStats' counters and records).
	    {0x0315, K::Plain, 0, {}},                                            // REGISTER_PASSENGER_DROPPED_OFF_TAXI
	    {0x0316, K::Plain, 1, {A::Value}},                                    // REGISTER_MONEY_MADE_TAXI
	    {0x03FD, K::Plain, 1, {A::Value}},                                    // REGISTER_4X4_ONE_TIME
	    {0x03FE, K::Plain, 1, {A::Value}},                                    // REGISTER_4X4_TWO_TIME
	    {0x03FF, K::Plain, 1, {A::Value}},                                    // REGISTER_4X4_THREE_TIME
	    {0x0400, K::Plain, 1, {A::Value}},                                    // REGISTER_4X4_MAYHEM_TIME
	    {0x0401, K::Plain, 0, {}},                                            // REGISTER_LIFE_SAVED
	    {0x0402, K::Plain, 0, {}},                                            // REGISTER_CRIMINAL_CAUGHT
	    {0x0403, K::Plain, 1, {A::Value}},                                    // REGISTER_AMBULANCE_LEVEL
	    {0x0404, K::Plain, 0, {}},                                            // REGISTER_FIRE_EXTINGUISHED
	    // Where to go.
	    {0x018A, K::BlipNew, 4, {A::Value, A::Value, A::Value, A::Output}},   // ADD_BLIP_FOR_COORD
	    {0x0167, K::BlipNew, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Output}},
	    {0x02A8, K::BlipNew, 5, {A::Value, A::Value, A::Value, A::Value, A::Output}},
	    {0x0186, K::BlipNew, 2, {A::Car, A::Output}},                         // ADD_BLIP_FOR_CAR
	    {0x0187, K::BlipNew, 2, {A::Char, A::Output}},                        // ADD_BLIP_FOR_CHAR
	    // The old blip on a car, with its colour and display (0x0043F8BD,
	    // three, then the handle): Turismo's three racers.
	    {0x0161, K::BlipNew, 4, {A::Car, A::Value, A::Value, A::Output}},   // ADD_BLIP_FOR_CAR_OLD
	    // A blip on a pickup (0x0044F600, one, then the handle) and a sprite
	    // one (0x0044F696, the pickup and the sprite): the briefcases, the
	    // free guns. Both read aPickUps[i].m_pObject unchecked, so a
	    // participant runs one only on a pickup of its own still standing
	    // (game/mission.cpp, RunEffect).
	    {0x03DC, K::BlipNew, 2, {A::Pickup, A::Output}},                      // ADD_BLIP_FOR_PICKUP
	    {0x03DD, K::BlipNew, 3, {A::Pickup, A::Value, A::Output}},            // ADD_SPRITE_BLIP_FOR_PICKUP
	    {0x0164, K::BlipUse, 1, {A::Blip}},                                   // REMOVE_BLIP
	    {0x0165, K::BlipUse, 2, {A::Blip, A::Value}},                         // CHANGE_BLIP_COLOUR
	    {0x0166, K::BlipUse, 2, {A::Blip, A::Value}},                         // DIM_BLIP
	    {0x0168, K::BlipUse, 2, {A::Blip, A::Value}},                         // CHANGE_BLIP_SCALE
	    {0x018B, K::BlipUse, 2, {A::Blip, A::Value}},                         // CHANGE_BLIP_DISPLAY
	    // A contact's marker (handler 0x004453EC: SetCoordBlip as
	    // BLIP_CONTACT_POINT, stored in its global), in the campaign: every
	    // machine makes its own into the same global, where the main script
	    // and later missions take it off again. A blip instruction above that
	    // names such a global, and no blip the mission made, goes by the
	    // global too (Encode, `blipByGlobal`) and is in the campaign as well.
	    {0x02A7, K::World, 5, {A::Value, A::Value, A::Value, A::Value, A::OutBlip}},
	    // What the world keeps after the mission (missions.md 3.4), which a
	    // save carries: parked-car generators, the building swaps and hidden
	    // objects, the road network and the garages' types. A generator and
	    // a garage are indices main.scm created in the same order on every
	    // machine, and a model the script names is its own table's.
	    {0x014C, K::World, 2, {A::Value, A::Value}},                          // SWITCH_CAR_GENERATOR
	    {0x0363, K::World, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x03B6, K::World, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x01E7, K::World, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x01E8, K::World, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x022A, K::World, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x022B, K::World, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x02FA, K::World, 2, {A::Value, A::Value}},                          // CHANGE_GARAGE_TYPE
	    {0x0299, K::World, 1, {A::Value}},                                    // ACTIVATE_GARAGE
	    {0x02B9, K::World, 1, {A::Value}},                                    // DEACTIVATE_GARAGE
	    {0x03A5, K::World, 3, {A::Value, A::Value, A::Value}},                // .._WITH_CAR_MODEL
	    // The mission's objects (mission-audit.md R3), each machine's own in
	    // its own global: made, moved, turned, let go of and taken away.
	    {0x0107, K::Plain, 5, {A::Value, A::Value, A::Value, A::Value, A::OutGlobal}},   // CREATE_OBJECT
	    {0x029B, K::Plain, 5, {A::Value, A::Value, A::Value, A::Value, A::OutGlobal}},   // .._NO_OFFSET
	    {0x0108, K::Plain, 1, {A::ObjGlobal}},                                // DELETE_OBJECT
	    {0x01BC, K::Plain, 4, {A::ObjGlobal, A::Value, A::Value, A::Value}},  // SET_OBJECT_COORDINATES
	    {0x0177, K::Plain, 2, {A::ObjGlobal, A::Value}},                      // SET_OBJECT_HEADING
	    {0x034E, K::Plain, 8, {A::ObjGlobal, A::Value, A::Value, A::Value, A::Value, A::Value,
	                           A::Value, A::Value}},                          // SLIDE_OBJECT
	    {0x034D, K::Plain, 4, {A::ObjGlobal, A::Value, A::Value, A::Value}},  // ROTATE_OBJECT
	    {0x035C, K::Plain, 5, {A::ObjGlobal, A::Car, A::Value, A::Value, A::Value}},
	    {0x0382, K::Plain, 2, {A::ObjGlobal, A::Value}},                      // SET_OBJECT_COLLISION
	    {0x0392, K::Plain, 2, {A::ObjGlobal, A::Value}},                      // SET_OBJECT_DYNAMIC
	    {0x0381, K::Plain, 4, {A::ObjGlobal, A::Value, A::Value, A::Value}},  // SET_OBJECT_VELOCITY
	    {0x038C, K::Plain, 4, {A::ObjGlobal, A::Value, A::Value, A::Value}},  // ADD_TO_OBJECT_VELOCITY
	    {0x0240, K::Plain, 2, {A::ObjGlobal, A::Value}},                      // FLASH_OBJECT
	    {0x01C4, K::Plain, 1, {A::ObjGlobal}},                                // .._NO_LONGER_NEEDED
	    // Kept past the mission, as the owner's is (game/mission.cpp,
	    // KeepMadeObject).
	    {0x01C7, K::Plain, 1, {A::ObjGlobal}},                                // DONT_REMOVE_OBJECT
	    {0x0188, K::BlipNew, 2, {A::ObjGlobal, A::Output}},                   // ADD_BLIP_FOR_OBJECT
	    // The mission's own explosions and fires (mission-audit.md R8), at
	    // their place on every machine, where each hurts what that machine
	    // decides the damage of: its own player and its own entities.
	    {0x020C, K::Plain, 4, {A::Value, A::Value, A::Value, A::Value}},      // ADD_EXPLOSION
	    {0x02CF, K::FireNew, 4, {A::Value, A::Value, A::Value, A::Output}},   // START_SCRIPT_FIRE
	    {0x0325, K::FireNew, 2, {A::Car, A::Output}},                         // START_CAR_FIRE
	    {0x0326, K::FireNew, 2, {A::Char, A::Output}},                        // START_CHAR_FIRE
	    {0x02D1, K::Plain, 1, {A::Fire}},                                     // REMOVE_SCRIPT_FIRE
	    // And the smoke and flames a mission sets burning at a place: Give Me
	    // Liberty's wrecked police cars at the bridge, Frank's steam, Ray's
	    // smoke. Otherwise only the owner's screen has them (0x00445185, five
	    // operands: the type, the place and a flag).
	    {0x02A2, K::Plain, 5, {A::Value, A::Value, A::Value, A::Value, A::Value}},   // ADD_PARTICLE_EFFECT
	    // And put out again, the way the mission's cleanup puts out its own
	    // (Give Me Liberty's, after a failure too): left burning here, a retry
	    // would light a second set over the first.
	    {0x03AE, K::Plain, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    // The pickups the mission lays out (mission-audit.md R2), each
	    // machine's own, which the pickup sync then names by where they are.
	    {0x0213, K::PickupNew, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Output}},
	    {0x032B, K::PickupNew, 7, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value,
	                               A::Output}},                           // .._WITH_AMMO
	    {0x02E1, K::PickupNew, 5, {A::Value, A::Value, A::Value, A::Value, A::Output}},   // MONEY
	    // A Drop In The Ocean's packages, where the plane is as it drops them;
	    // each machine's float where they land (mission.cpp, the floating ones).
	    {0x035B, K::PickupNew, 4, {A::Value, A::Value, A::Value, A::Output}},   // FLOATING_PACKAGE
	    {0x0215, K::Plain, 1, {A::Pickup}},                                   // REMOVE_PICKUP
	    // The mission's own HUD (mission-audit.md R16). What they show is the
	    // global's value, which the owner streams.
	    {0x014E, K::Plain, 1, {A::Global}},                                   // DISPLAY_ONSCREEN_TIMER
	    {0x014F, K::Plain, 1, {A::Global}},                                   // CLEAR_ONSCREEN_TIMER
	    {0x0150, K::Plain, 2, {A::Global, A::Value}},                         // DISPLAY_ONSCREEN_COUNTER
	    {0x03C4, K::Plain, 3, {A::Global, A::Value, A::Text}},                // .._COUNTER_WITH_STRING
	    {0x0151, K::Plain, 1, {A::Global}},                                   // CLEAR_ONSCREEN_COUNTER
	    {0x0396, K::Plain, 1, {A::Value}},                                    // FREEZE_ONSCREEN_TIMER
	    // And the HUD item a tutorial flashes while its help box explains it:
	    // the help box went to everybody and the flashing did not, so the
	    // owner's radar blinked for four seconds and nobody else's did.
	    {0x03E7, K::Plain, 1, {A::Value}},                                    // FLASH_HUD_OBJECT
	    // A cutscene (missions.md 11.3), which every machine plays from its
	    // own files: the models it needs, loaded the moment they are asked
	    // for and let go of when the mission does, then the scene, its
	    // objects and their animations. A cutscene object is a handle into
	    // each machine's own object pool.
	    {0x023C, K::Plain, 2, {A::Value, A::Text}},                           // LOAD_SPECIAL_CHARACTER
	    {0x02F3, K::Plain, 2, {A::Value, A::Text}},                           // LOAD_SPECIAL_MODEL
	    {0x0247, K::Plain, 1, {A::Value}},                                    // REQUEST_MODEL
	    {0x038B, K::Plain, 0, {}},                                            // LOAD_ALL_MODELS_NOW
	    {0x0249, K::Plain, 1, {A::Value}},                                    // MARK_MODEL_AS_NO_LONGER_NEEDED
	    {0x0296, K::Plain, 1, {A::Value}},                                    // UNLOAD_SPECIAL_CHARACTER
	    {0x02E4, K::Plain, 1, {A::Text}},                                     // LOAD_CUTSCENE
	    {0x0244, K::Plain, 3, {A::Value, A::Value, A::Value}},                // SET_CUTSCENE_OFFSET
	    {0x02E5, K::ObjectNew, 2, {A::Value, A::Output}},                     // CREATE_CUTSCENE_OBJECT
	    {0x02E6, K::Plain, 2, {A::Object, A::Text}},                          // SET_CUTSCENE_ANIM
	    {0x02F4, K::ObjectNew, 3, {A::Object, A::Value, A::Output}},          // CREATE_CUTSCENE_HEAD
	    {0x02F5, K::Plain, 2, {A::Object, A::Text}},                          // SET_HEAD_ANIM
	    {0x02E7, K::Plain, 0, {}},                                            // START_CUTSCENE
	    {0x02EA, K::Plain, 0, {}},                                            // CLEAR_CUTSCENE
	    // The screen and the camera around it, and the player held still.
	    {0x016A, K::Plain, 2, {A::Value, A::Value}},                          // DO_FADE
	    {0x0169, K::Plain, 3, {A::Value, A::Value, A::Value}},                // SET_FADING_COLOUR
	    {0x02A3, K::Plain, 1, {A::Value}},                                    // SWITCH_WIDESCREEN
	    {0x01B4, K::Plain, 2, {A::Value, A::Value}},                          // SET_PLAYER_CONTROL
	    {0x015F, K::Plain, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x0160, K::Plain, 4, {A::Value, A::Value, A::Value, A::Value}},      // POINT_CAMERA_AT_POINT
	    {0x015A, K::Plain, 0, {}},                                            // RESTORE_CAMERA
	    {0x02EB, K::Plain, 0, {}},                                            // RESTORE_CAMERA_JUMPCUT
	    {0x0373, K::Plain, 0, {}},                                            // SET_CAMERA_BEHIND_PLAYER
	    {0x03C8, K::Plain, 0, {}},                                            // SET_CAMERA_IN_FRONT_OF_PLAYER
	    {0x0157, K::Plain, 3, {A::Value, A::Value, A::Value}},                // CAMERA_ON_PLAYER
	    {0x0158, K::Plain, 3, {A::Car, A::Value, A::Value}},                  // CAMERA_ON_VEHICLE
	    {0x0159, K::Plain, 3, {A::Char, A::Value, A::Value}},                 // CAMERA_ON_PED
	    // A sound at a place, and a frenzy the mission starts, which every
	    // machine's own CDarkel then runs (docs/rampage.md).
	    {0x018C, K::Plain, 4, {A::Value, A::Value, A::Value, A::Value}},      // ADD_ONE_OFF_SOUND
	    {0x01F9, K::Plain, 9, {A::Text, A::Value, A::Value, A::Value, A::Value, A::Value,
	                           A::Value, A::Value, A::Value}},                // START_KILL_FRENZY
	    // Where the mission puts its player. Every machine's player is player
	    // 0 on that machine, so the owner's operand is everybody's.
	    {0x0055, K::Teleport, 4, {A::Value, A::Value, A::Value, A::Value}},   // SET_PLAYER_COORDINATES
	    {0x0171, K::Plain, 2, {A::Value, A::Value}},                          // SET_PLAYER_HEADING
	    {0x012A, K::Teleport, 4, {A::Value, A::Value, A::Value, A::Value}},   // WARP_PLAYER_FROM_CAR_TO_COORD
	    // What the story does to the player happens to every participant
	    // (mission-audit.md R12): the weapons it hands over and takes, the
	    // wanted level, health, being seen, being left alone, and where
	    // everybody wakes up after a death or an arrest.
	    {0x01B1, K::Plain, 3, {A::Value, A::Value, A::Value}},                // GIVE_WEAPON_TO_PLAYER
	    {0x017A, K::Plain, 3, {A::Value, A::Value, A::Value}},                // SET_PLAYER_AMMO
	    {0x01B8, K::Plain, 2, {A::Value, A::Value}},                          // SET_CURRENT_PLAYER_WEAPON
	    {0x03B8, K::Plain, 1, {A::Value}},                                    // REMOVE_ALL_PLAYER_WEAPONS
	    // The RC, 4x4 and Mayhem runs and two of the Yardies' save the
	    // player's level into a global at the start and give it back from it
	    // at the end: each machine keeps and gets back its own (0x004416D0
	    // collects the player and stores the level where the operand says).
	    {0x01C0, K::Plain, 2, {A::Value, A::Global}},                         // STORE_WANTED_LEVEL
	    {0x010D, K::Plain, 2, {A::Value, A::OwnGlobal}},                      // ALTER_WANTED_LEVEL
	    {0x010E, K::Plain, 2, {A::Value, A::Value}},                          // .._NO_DROP
	    {0x0110, K::Plain, 1, {A::Value}},                                    // CLEAR_WANTED_LEVEL
	    {0x0222, K::Plain, 2, {A::Value, A::Value}},                          // SET_PLAYER_HEALTH
	    {0x0336, K::Plain, 2, {A::Value, A::Value}},                          // SET_PLAYER_VISIBLE
	    {0x03BF, K::Plain, 2, {A::Value, A::Value}},                          // SET_EVERYONE_IGNORE_PLAYER
	    {0x01F7, K::Plain, 2, {A::Value, A::Value}},                          // SET_POLICE_IGNORE_PLAYER
	    // Paramedic's reward (11_ambulance, level 12 or 15), kept like the
	    // campaign: CPlayerInfo::m_bInfiniteSprint (+0x114, handler
	    // 0x00448C37), which a save carries. No mission turns it off again.
	    {0x0330, K::World, 2, {A::Value, A::Value}},                          // SET_PLAYER_NEVER_GETS_TIRED
	    // And the Vigilante's (13_copcar, ten bonus kills), kept the same way:
	    // CPlayerInfo::m_bGetOutOfJailFree (+0x116, handler 0x00588CEF, the
	    // player and the flag). No mission turns it off either.
	    {0x0413, K::World, 2, {A::Value, A::Value}},                          // SET_GET_OUT_OF_JAIL_FREE
	    // And the clothes it puts the player in (game/outfit.h). DRESS_CHAR
	    // isn't on the list: each machine builds its own player again when
	    // its own model 0 is in.
	    {0x0352, K::Outfit, 2, {A::Value, A::Text}},                          // UNDRESS_CHAR
	    {0x016E, K::Plain, 4, {A::Value, A::Value, A::Value, A::Value}},      // OVERRIDE_NEXT_RESTART
	    {0x01F6, K::Plain, 0, {}},                                            // CANCEL_OVERRIDE_RESTART
	    // The mission's word to a car somebody else drives (mission-audit.md
	    // R9) goes to the machine that simulates it.
	    {0x00AB, K::Holder, 4, {A::Car, A::Value, A::Value, A::Value}},       // SET_CAR_COORDINATES
	    {0x0175, K::Holder, 2, {A::Car, A::Value}},                           // SET_CAR_HEADING
	    {0x0224, K::Holder, 2, {A::Car, A::Value}},                           // SET_CAR_HEALTH
	    // And blowing it up (0x00442DE4: GetAt, then BlowUpCar(null) through
	    // the vtable). A wreck is decided once, where the car is simulated:
	    // the owner's own BlowUpCar refuses a car somebody else drives or
	    // settles, so Blow Fish's truck at a participant's wheel never went up
	    // when the timer ran out. To its holder alone, never to everybody for
	    // a car nobody holds (carauthority.h, WhereTheWreckGoes).
	    {0x020B, K::Holder, 1, {A::Car}},                                     // EXPLODE_CAR
	    // And to every machine: a car's doors are locked on the copy each
	    // player's own engine tries to get into, its colour is what everybody
	    // sees, and the brakes are each player's own pad's, the car they drive.
	    {0x020A, K::Plain, 2, {A::Car, A::Value}},                            // LOCK_CAR_DOORS
	    // The other way the script says the same thing: handler 0x0043ED92
	    // stores its second operand at +0x224 exactly as LOCK_CAR_DOORS'
	    // 0x00442DAC does (Meat Business, Luigi's dealer's Stallion).
	    {0x0135, K::Plain, 2, {A::Car, A::Value}},                            // CHANGE_CAR_LOCK
	    {0x0229, K::Plain, 3, {A::Car, A::Value, A::Value}},                  // CHANGE_CAR_COLOUR
	    // What the mission makes a car stand up to is on every copy, for the
	    // reason a bomb is: whoever ends up driving it simulates it, and their
	    // engine decides what it takes (0x004455DF sets +0x53 bit 4,
	    // 0x0044E1A1 sets +0x1F7 bit 0, two operands each).
	    {0x02AA, K::Plain, 2, {A::Car, A::Value}},                            // SET_CAR_ONLY_DAMAGED_BY_PLAYER
	    {0x03AB, K::Plain, 2, {A::Car, A::Value}},                            // SET_CAR_STRONG
	    {0x0221, K::Plain, 2, {A::Value, A::Value}},                          // APPLY_BRAKES_TO_PLAYERS_CAR
	    // A bomb the mission fits to a car is on every copy of it, so whoever
	    // ends up driving it has it (mission-audit.md R6), and a free bomb
	    // shop is free for everybody who drives in.
	    {0x0242, K::Plain, 2, {A::Car, A::Value}},                            // ARM_CAR_WITH_BOMB
	    {0x021D, K::Plain, 1, {A::Value}},                                    // SET_FREE_BOMB_SHOP
	    // The mission's detonator in the player's hand (0x0044C9A7, none:
	    // CGarages::GivePlayerDetonator), I Scream, You Scream's remote: every
	    // participant may press it (mission-audit.md R6), and the cleanup's
	    // SET_PLAYER_AMMO 12 to 0 takes it back from everybody.
	    {0x037F, K::Plain, 0, {}},                                            // GIVE_PLAYER_DETONATOR
	    // A car that keeps its paint through a Pay'n'Spray (0x00444C25, two:
	    // bFixedColour, bit 5 of +0x4D9), Lips' car: on every copy, since the
	    // spray it goes through may be a helper's.
	    {0x0294, K::Plain, 2, {A::Car, A::Value}},                            // SET_CAN_RESPRAY_CAR
	    // And a mine it drops is in every participant's world, where each
	    // engine arms its own and the first to go off takes the others with it
	    // (game/mine.h): Gone Fishing's, in the partner's wake.
	    {0x02F0, K::Plain, 3, {A::Value, A::Value, A::Value}},                // DROP_MINE
	    {0x02F1, K::Plain, 3, {A::Value, A::Value, A::Value}},                // DROP_NAUTICAL_MINE
	    // A mission garage runs on every machine, so each machine's own state
	    // machine sees the car it waits for when that machine's player brings
	    // it (mission-audit.md R5): the car it takes, and its door.
	    {0x021B, K::Plain, 2, {A::Value, A::Car}},                            // SET_TARGET_CAR_FOR_MISSION_GARAGE
	    {0x0360, K::Plain, 1, {A::Value}},                                    // OPEN_GARAGE
	    {0x0361, K::Plain, 1, {A::Value}},                                    // CLOSE_GARAGE
	    {0x03BB, K::Plain, 1, {A::Value}},                                    // SET_GARAGE_DOOR_TYPE_TO_SWING_OPEN
	    // The mission Cessnas fly on every machine, each machine's own copy,
	    // which game/planes.cpp keeps on the session's clock (mission-audit.md
	    // R7), so a participant can follow one and bring theirs down.
	    {0x033A, K::Plain, 0, {}},                                            // START_DRUG_RUN
	    {0x0358, K::Plain, 0, {}},                                            // START_DRUG_DROP_OFF
	    // And The Exchange's Catalina helicopter, the same way: each machine
	    // flies its own copy of it along the engine's fixed path, and a
	    // participant's copy brought down answers the owner's
	    // HAS_CATALINA_HELI_BEEN_SHOT_DOWN. None has operands; the start sets
	    // CHeli::CatalinaHeliOn (0x0054A980), the removal takes the slot
	    // away (0x0054A9D0). The take-off and the flying away write the path
	    // state through pHelis[3] with no null test (0x0054A9B0, 0x0054A9C0),
	    // so mission.cpp holds them until our copy is in its slot.
	    {0x03B2, K::Plain, 0, {}},                                            // START_CATALINA_HELI
	    {0x03B3, K::Plain, 0, {}},                                            // CATALINA_HELI_TAKE_OFF
	    {0x03B4, K::Plain, 0, {}},                                            // REMOVE_CATALINA_HELI
	    {0x03BE, K::Plain, 0, {}},                                            // CATALINA_HELI_FLY_AWAY
	    // The mission's script fires, all put out at once (0x00448050:
	    // CFireManager::RemoveAllScriptFires, no operands), each machine its
	    // own: Silence The Sneak, the fire truck's.
	    {0x031A, K::Plain, 0, {}},                                            // REMOVE_ALL_SCRIPT_FIRES
	    // The streets the mission clears and thins, which every machine does to
	    // what it hosts itself (mission-audit.md R15): the owner's engine clears
	    // its own traffic out of a cutscene, and the traffic a participant
	    // hosts is on everybody's screen. CLEAR_AREA is the place, the radius
	    // and a flag (0x0044D84A, five); each density is one float (0x0044F730
	    // into 0x005FA56C, 0x004426FA into 0x005EC8B4). game/missionworld.h puts
	    // the densities back when the mission ends.
	    {0x0395, K::Plain, 5, {A::Value, A::Value, A::Value, A::Value, A::Value}},   // CLEAR_AREA
	    {0x03DE, K::Plain, 1, {A::Value}},                                    // SET_PED_DENSITY_MULTIPLIER
	    {0x01EB, K::Plain, 1, {A::Value}},                                    // SET_CAR_DENSITY_MULTIPLIER
	    // What each machine's own zones generate (R15): the RC missions' Diablo
	    // and Mafia targets, the Triads Toni's mission thins out. A save keeps
	    // the zones and the gangs (CTheZones and CGangs are in it), so these are
	    // the campaign's too. A zone is its name, a label, then day or night and
	    // the numbers: sixteen in all for the cars (0x0043F27C), ten for the
	    // gangs (0x0043F65C); a gang's weapons are three (0x00443F26).
	    {0x0152, K::World, 17, {A::Text, A::Short, A::Short, A::Short, A::Short, A::Short, A::Short,
	                            A::Short, A::Short, A::Short, A::Short, A::Short, A::Short, A::Short,
	                            A::Short, A::Short, A::Short}},               // SET_ZONE_CAR_INFO
	    {0x015C, K::World, 11, {A::Text, A::Short, A::Short, A::Short, A::Short, A::Short, A::Short,
	                            A::Short, A::Short, A::Short, A::Short}},     // SET_ZONE_PED_INFO
	    {0x0237, K::World, 3, {A::Value, A::Value, A::Value}},                // SET_GANG_WEAPONS
	    // A gang turned on the player, or called off (0x00588677, 0x005886A7:
	    // two, the ped type and the threat, ORed into or taken out of the
	    // type's m_threats). A save keeps CPedType, so the campaign's too.
	    {0x03F1, K::World, 2, {A::Value, A::Value}},                          // SET_THREAT_FOR_PED_TYPE
	    {0x03F2, K::World, 2, {A::Value, A::Value}},                          // CLEAR_THREAT_FOR_PED_TYPE
	    // What the mission says out loud (game/missionworld.h): its one line of
	    // dialogue at a time, loaded by name, played once it is in, at a place
	    // when the mission says where and in the ear otherwise, and let go of.
	    {0x03CF, K::Plain, 1, {A::Text}},                                     // LOAD_MISSION_AUDIO
	    {0x03D1, K::Plain, 0, {}},                                            // PLAY_MISSION_AUDIO
	    {0x03D7, K::Plain, 3, {A::Value, A::Value, A::Value}},                // SET_MISSION_AUDIO_POSITION
	    {0x040D, K::Plain, 0, {}},                                            // CLEAR_MISSION_AUDIO
	    // The pager (0x0043F12A: a label, then three), and the fire truck's
	    // "burning vehicle reported in X": a label with a label in it, then
	    // the time and the flag (0x0044CBAB).
	    {0x014D, K::Plain, 4, {A::Text, A::Value, A::Value, A::Value}},       // ADD_PAGER_MESSAGE
	    {0x0384, K::Plain, 4, {A::Text, A::Text, A::Value, A::Value}},        // PRINT_STRING_IN_STRING
	    // Arms Shortage's attackers' blips, the old form: the char, colour and
	    // display, then the handle (0x0043F92A).
	    {0x0162, K::BlipNew, 4, {A::Char, A::Value, A::Value, A::Output}},   // ADD_BLIP_FOR_CHAR_OLD
	    // Luigi's second mission's cylinders on the ground: the place and the
	    // radius, then the handle (0x0044E927); taken down by it (0x0044EA3D).
	    {0x03BC, K::SphereNew, 5, {A::Value, A::Value, A::Value, A::Value, A::Output}},   // ADD_SPHERE
	    {0x03BD, K::Plain, 1, {A::Sphere}},                                   // REMOVE_SPHERE
	    // The world loaded where the mission is about to put its player
	    // (0x0044F0B6, three floats), so a participant moved there after it
	    // does not stand in the half that has not streamed in.
	    {0x03CB, K::Plain, 3, {A::Value, A::Value, A::Value}},                // LOAD_SCENE
	    // The Exchange's credits, started and stopped (0x005896AA, 0x005896BA).
	    {0x0434, K::Plain, 0, {}},                                            // START_CREDITS
	    {0x0435, K::Plain, 0, {}},                                            // STOP_CREDITS
	    // Give Me Liberty failed: the player starts again at the bridge, or at
	    // the hideout, after the fade the engine's critical-mission failure
	    // plays (0x004449E6: four floats, OverrideNextRestart, then
	    // PlayerFailedCriticalMission). Every participant starts there too,
	    // each beside the spot the way SET_PLAYER_COORDINATES puts them.
	    {0x0255, K::Teleport, 4, {A::Value, A::Value, A::Value, A::Value}},   // RESTART_CRITICAL_MISSION
	    // The 1100 range's, which went nowhere until that range was hooked.
	    // The island the mission loads behind its loading screen (0x00589D2D,
	    // one operand): Last Requests' Staunton before it moves its player
	    // there, S.A.M.'s Shoreside as its player nears the platform. Run only
	    // where it cannot take a participant's ground away (mission.cpp,
	    // LoadIslandHere).
	    {0x044C, K::Plain, 1, {A::Value}},                                    // LOAD_COLLISION_WITH_SCREEN
	    // Lips' car stands up to more on every copy, the way SET_CAR_STRONG
	    // does, since a helper may be the one driving it (0x00589EE3: the car
	    // and a flag into +0x4DA bit 1).
	    {0x044F, K::Plain, 2, {A::Car, A::Value}},                            // MAKE_CRAIGS_CAR_A_BIT_STRONGER
	    // Bait's cartel car put back on the road after the player (0x00589F42,
	    // the car), where that car is simulated.
	    {0x0450, K::Holder, 1, {A::Car}},                                     // SET_JAMES_CAR_ON_PATH_TO_PLAYER
	    // The Exchange's end-of-game music, loaded for its END cutscene
	    // (0x00589FA5, no operands).
	    {0x0451, K::Plain, 0, {}},                                            // LOAD_END_OF_GAME_TUNE
	    // The launch's make-safe, which the trigger runs between the grant and
	    // START_MISSION: frozen and unhurt through the title's fade, as the
	    // owner is (0x005885F9, the player). game/mission.cpp gives it back
	    // at the end when no cutscene has.
	    {0x03EF, K::Plain, 1, {A::Value}},                                    // MAKE_PLAYER_SAFE_FOR_CUTSCENE
	    // What a mission sets for a while, each one operand, and each put back
	    // at the end (mission.cpp, EndEffects): Pay'n'Spray free (0x00448E0A,
	    // a byte at 0x0095CD1D), the police's eye for crime (0x0044EFD5,
	    // CWanted +0x0C), where the next death or arrest wakes up (0x00589083,
	    // 0x005890A5), the world held still for a cutscene (0x0044E75E), every
	    // car unhurt (0x00588729), traffic made round the camera (0x005884F5),
	    // the camera's near clip (0x0058902C), the music through a fade
	    // (0x00589880).
	    {0x0335, K::Plain, 1, {A::Value}},                                    // SET_FREE_RESPRAYS
	    {0x03C7, K::Plain, 1, {A::Value}},                                    // SET_WANTED_MULTIPLIER
	    {0x041F, K::Plain, 1, {A::Value}},                                    // OVERRIDE_HOSPITAL_LEVEL
	    {0x0420, K::Plain, 1, {A::Value}},                                    // .._POLICE_STATION_LEVEL
	    {0x03B7, K::Plain, 1, {A::Value}},                                    // SWITCH_WORLD_PROCESSING
	    {0x03F4, K::Plain, 1, {A::Value}},                                    // SET_ALL_CARS_CAN_BE_DAMAGED
	    {0x03EA, K::Plain, 1, {A::Value}},                                    // .._CARS_AROUND_CAMERA
	    {0x041D, K::Plain, 1, {A::Value}},                                    // SET_NEAR_CLIP
	    {0x043C, K::Plain, 1, {A::Value}},                                    // SET_MUSIC_DOES_FADE
	    // The peds cleared out of a box (0x0058941F, six floats), which each
	    // machine does to the ones it hosts, as CLEAR_AREA.
	    {0x042B, K::Plain, 6, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    // What is only seen and heard once: the camera shaking (0x004396D9,
	    // one), a moving particle effect (0x0044DB4E, twelve), one particle
	    // (0x005896EA, eight), the end of the game's tune started and stopped
	    // (0x005899AA, 0x005899BF, none).
	    {0x0003, K::Plain, 1, {A::Value}},                                    // SHAKE_CAM
	    {0x039D, K::Plain, 12, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value,
	                            A::Value, A::Value, A::Value, A::Value, A::Value, A::Value}},
	    {0x0437, K::Plain, 8, {A::Value, A::Value, A::Value, A::Value, A::Value, A::Value,
	                           A::Value, A::Value}},                          // CREATE_SINGLE_PARTICLE
	    {0x043F, K::Plain, 0, {}},                                            // PLAY_END_OF_GAME_TUNE
	    {0x0440, K::Plain, 0, {}},                                            // STOP_END_OF_GAME_TUNE
	    // A sound that plays until it is taken off (0x00440B70: the place and
	    // the sound, then the handle; 0x00440BF2 takes it off), kept in a
	    // global, each machine's own: Frank's party, 8-Ball's burning building.
	    {0x018D, K::Plain, 5, {A::Value, A::Value, A::Value, A::Value, A::SoundOut}},
	    {0x018E, K::Plain, 1, {A::SoundGlobal}},                              // REMOVE_SOUND
	};
	for (const Entry &e : kList)
		if (e.opcode == opcode)
			return &e;
	return nullptr;
}

// Everything on the list, for installing: which ranges have work in them.
inline bool Listed(int32_t command) { return command >= 0 && Find(static_cast<uint16_t>(command)); }

// ---- reading the owner's instruction ------------------------------------------------

// One operand's value as the engine reads it: a literal, a float literal of
// 1/16ths, a global out of the script space, or a local out of `locals` (the
// running script's sixteen and its two timers). False for anything else or
// past the end.
inline bool ReadValue(const uint8_t *space, uint32_t size, uint32_t ip, const int32_t *locals,
                      uint32_t *bits, uint32_t *length) {
	if (ip >= size)
		return false;
	const uint8_t type = space[ip];
	auto u16 = [&](uint32_t at) {
		return static_cast<uint32_t>(space[at] | (space[at + 1] << 8));
	};
	switch (type) {
	case scripts::PARAM_INT32:
		if (ip + 5 > size)
			return false;
		std::memcpy(bits, space + ip + 1, 4);
		*length = 5;
		return true;
	case scripts::PARAM_INT8:
		if (ip + 2 > size)
			return false;
		*bits   = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(space[ip + 1])));
		*length = 2;
		return true;
	case scripts::PARAM_INT16:
		if (ip + 3 > size)
			return false;
		*bits   = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(u16(ip + 1))));
		*length = 3;
		return true;
	case scripts::PARAM_FLOAT: {
		if (ip + 3 > size)
			return false;
		const float f = static_cast<float>(static_cast<int16_t>(u16(ip + 1))) / 16.0f;
		std::memcpy(bits, &f, 4);
		*length = 3;
		return true;
	}
	case scripts::PARAM_GLOBAL: {
		if (ip + 3 > size)
			return false;
		const uint32_t at = u16(ip + 1);
		if (at + 4 > size)
			return false;
		std::memcpy(bits, space + at, 4);
		*length = 3;
		return true;
	}
	case scripts::PARAM_LOCAL: {
		if (ip + 3 > size || !locals)
			return false;
		const uint32_t index = u16(ip + 1);
		if (index >= 18)
			return false;
		*bits   = static_cast<uint32_t>(locals[index]);
		*length = 3;
		return true;
	}
	default:
		return false;
	}
}

// One instruction, ready for a participant's interpreter.
constexpr size_t MAX_CODE = 64;

struct Encoded {
	Kind     kind       = Kind::Plain;
	uint8_t  length     = 0;
	uint8_t  handleAt   = 0xFF;   // where a Blip operand's four bytes are, for BlipUse
	int32_t  ownerBlip  = -1;     // the owner's handle a BlipUse names
	uint8_t  code[MAX_CODE] = {};
};

// The owner's instruction at `ip` (where its operands start; the opcode was
// read already) as the participant will run it. False for an opcode not on
// the list or operands this cannot read.
//
// `blipByGlobal`: a blip operand the script names by a global goes as that
// global, `02 lo hi`, for each machine to read its own - a blip the main
// script keeps there, a contact's marker, which no blip map has. It is in the
// campaign then (Kind::World), whatever the instruction.
//
// `pickupByGlobal`, the same for a pickup: one the mission makes into a
// global goes into that global on every machine, and one it takes away by a
// global goes by it. A shop's guns and Phil's armour are the world's for
// good, not the mission's stash, and the main script's own out-of-stock sign
// is in no participant's map (game/mission.cpp, KeptPickup).
inline bool Encode(uint16_t opcode, const uint8_t *space, uint32_t size, uint32_t ip,
                   const int32_t *locals, Encoded *out, bool blipByGlobal = false,
                   bool pickupByGlobal = false) {
	const Entry *e = Find(opcode);
	if (!e)
		return false;
	Encoded enc;
	enc.kind   = e->kind;
	size_t   n = 0;
	auto put = [&](const void *bytes, size_t len) {
		if (n + len > MAX_CODE)
			return false;
		std::memcpy(enc.code + n, bytes, len);
		n += len;
		return true;
	};
	if (!put(&opcode, 2))
		return false;
	uint32_t at = ip;
	for (uint8_t i = 0; i < e->count; ++i) {
		switch (e->args[i]) {
		case Arg::Text:
			if (at + 8 > size || !put(space + at, 8))
				return false;
			at += 8;
			break;
		case Arg::Global:
		case Arg::ObjGlobal:
		case Arg::OutGlobal:
		case Arg::OutBlip:
		case Arg::SoundOut:
		case Arg::SoundGlobal: {
			if (at + 3 > size || space[at] != scripts::PARAM_GLOBAL || !put(space + at, 3))
				return false;
			at += 3;
			break;
		}
		case Arg::Output: {
			const uint32_t len = at < size ? (space[at] == scripts::PARAM_GLOBAL ||
			                                          space[at] == scripts::PARAM_LOCAL
			                                      ? 3u
			                                      : 0u)
			                               : 0u;
			if (pickupByGlobal && e->kind == Kind::PickupNew && len == 3 &&
			    space[at] == scripts::PARAM_GLOBAL) {
				if (!put(space + at, 3))
					return false;
				enc.kind = Kind::World;
				at += len;
				break;
			}
			const uint8_t local0[3] = {scripts::PARAM_LOCAL, 0, 0};
			if (len == 0 || !put(local0, 3))
				return false;
			at += len;
			break;
		}
		case Arg::Short: {
			uint32_t bits = 0, len = 0;
			if (at >= size || space[at] == scripts::PARAM_FLOAT ||
			    !ReadValue(space, size, at, locals, &bits, &len))
				return false;
			const int32_t v = static_cast<int32_t>(bits);
			if (v < -32768 || v > 32767)
				return false;
			const uint8_t short16[3] = {scripts::PARAM_INT16, static_cast<uint8_t>(v & 0xFF),
			                            static_cast<uint8_t>((v >> 8) & 0xFF)};
			if (!put(short16, 3))
				return false;
			at += len;
			break;
		}
		case Arg::Value:
		case Arg::Blip:
		case Arg::Object:
		case Arg::Char:
		case Arg::Car:
		case Arg::Pickup:
		case Arg::Fire:
		case Arg::Sphere:
		case Arg::OwnGlobal: {
			if (((e->args[i] == Arg::Blip && blipByGlobal) || e->args[i] == Arg::OwnGlobal) &&
			    at + 3 <= size && space[at] == scripts::PARAM_GLOBAL) {
				if (!put(space + at, 3))
					return false;
				if (e->args[i] == Arg::Blip)
					enc.kind = Kind::World;
				at += 3;
				break;
			}
			if (e->args[i] == Arg::Pickup && pickupByGlobal && at + 3 <= size &&
			    space[at] == scripts::PARAM_GLOBAL) {
				if (!put(space + at, 3))
					return false;
				enc.kind = Kind::World;
				at += 3;
				break;
			}
			uint32_t bits = 0, len = 0;
			if (!ReadValue(space, size, at, locals, &bits, &len))
				return false;
			const uint8_t type = scripts::PARAM_INT32;
			if (e->args[i] == Arg::Blip) {
				enc.handleAt  = static_cast<uint8_t>(n + 1);
				enc.ownerBlip = static_cast<int32_t>(bits);
			}
			if (!put(&type, 1) || !put(&bits, 4))
				return false;
			at += len;
			break;
		}
		}
	}
	enc.length = static_cast<uint8_t>(n);
	*out       = enc;
	return true;
}

// The global a sound instruction names (Arg::SoundOut or Arg::SoundGlobal):
// `02 lo hi`, each machine's own. `makes` for the one that makes it.
inline bool SoundGlobalOf(const uint8_t *code, size_t length, uint16_t *out, bool *makes);

// ---- the blips, owner's to ours --------------------------------------------------

constexpr size_t MAX_BLIPS = 48;

class BlipMap {
public:
	void Add(int32_t owner, int32_t ours) {
		for (size_t i = 0; i < m_count; ++i)
			if (m_owner[i] == owner) {
				m_ours[i] = ours;
				return;
			}
		if (m_count < MAX_BLIPS) {
			m_owner[m_count] = owner;
			m_ours[m_count]  = ours;
			++m_count;
		}
	}
	// Ours for the owner's, or -1 for one we never made.
	int32_t Ours(int32_t owner) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_owner[i] == owner)
				return m_ours[i];
		return -1;
	}
	void Remove(int32_t owner) {
		for (size_t i = 0; i < m_count; ++i)
			if (m_owner[i] == owner) {
				--m_count;
				m_owner[i] = m_owner[m_count];
				m_ours[i]  = m_ours[m_count];
				return;
			}
	}
	size_t  Count() const { return m_count; }
	int32_t OursAt(size_t i) const { return m_ours[i]; }
	void    Clear() { m_count = 0; }

private:
	int32_t m_owner[MAX_BLIPS] = {};
	int32_t m_ours[MAX_BLIPS]  = {};
	size_t  m_count            = 0;
};

// ---- an encoded instruction's operands ------------------------------------------

// How many bytes an operand takes once encoded: a literal is its type byte
// and four, a text label eight, a global or the runner's local three.
inline size_t EncodedSize(Arg a) {
	switch (a) {
	case Arg::Text:      return 8;
	case Arg::Global:
	case Arg::ObjGlobal:
	case Arg::OutGlobal:
	case Arg::OutBlip:
	case Arg::SoundOut:
	case Arg::SoundGlobal:
	case Arg::Output:
	case Arg::Short:     return 3;
	default:             return 5;
	}
}

// The same for the operand at `at` of an instruction as it was encoded: a
// blip or a pickup sent by its global (Encode, `blipByGlobal`,
// `pickupByGlobal`) is the global's three bytes.
inline size_t OperandSize(Arg a, const uint8_t *code, size_t at, size_t length) {
	if ((a == Arg::Blip || a == Arg::Pickup || a == Arg::OwnGlobal) && at < length && code[at] == scripts::PARAM_GLOBAL)
		return 3;
	return EncodedSize(a);
}

// The global an instruction makes a blip into (Arg::OutBlip), or the one a
// blip operand sent by its global names: `02 lo hi`, each machine's own.
// False for one that names neither.
inline bool BlipGlobalOf(const uint8_t *code, size_t length, uint16_t *out, bool *makes) {
	if (length < 2)
		return false;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e)
		return false;
	size_t at = 2;
	for (uint8_t i = 0; i < e->count; ++i) {
		const size_t size = OperandSize(e->args[i], code, at, length);
		if (at + size > length)
			return false;
		if ((e->args[i] == Arg::OutBlip || e->args[i] == Arg::Blip) && size == 3 &&
		    code[at] == scripts::PARAM_GLOBAL) {
			*out = static_cast<uint16_t>(code[at + 1] | (code[at + 2] << 8));
			if (makes)
				*makes = e->args[i] == Arg::OutBlip;
			return true;
		}
		at += size;
	}
	return false;
}

// The global a pickup instruction makes its pickup into, or takes one away
// by (Encode, `pickupByGlobal`): `02 lo hi`, each machine's own. False for
// one that names neither: a pickup the mission's map translates.
inline bool PickupGlobalOf(const uint8_t *code, size_t length, uint16_t *out, bool *makes) {
	if (length < 2)
		return false;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e)
		return false;
	size_t at = 2;
	for (uint8_t i = 0; i < e->count; ++i) {
		const size_t size = OperandSize(e->args[i], code, at, length);
		if (at + size > length)
			return false;
		const bool output = e->args[i] == Arg::Output && e->kind == Kind::PickupNew;
		if ((output || e->args[i] == Arg::Pickup) && size == 3 &&
		    code[at] == scripts::PARAM_GLOBAL) {
			*out = static_cast<uint16_t>(code[at + 1] | (code[at + 2] << 8));
			if (makes)
				*makes = output;
			return true;
		}
		at += size;
	}
	return false;
}

// The global an instruction makes an object into, for a participant to know
// which of its objects the mission made. False for one that makes none.
inline bool OutGlobalOf(const uint8_t *code, size_t length, uint16_t *out) {
	if (length < 2)
		return false;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e)
		return false;
	size_t at = 2;
	for (uint8_t i = 0; i < e->count; ++i) {
		const size_t size = OperandSize(e->args[i], code, at, length);
		if (at + size > length)
			return false;
		if (e->args[i] == Arg::OutGlobal && code[at] == scripts::PARAM_GLOBAL) {
			*out = static_cast<uint16_t>(code[at + 1] | (code[at + 2] << 8));
			return true;
		}
		at += size;
	}
	return false;
}

// The global an object operand names, for a participant to check that its
// own holds a live object before running it: `02 lo hi` at the operand.
// Every ObjGlobal of the instruction, at most `max`.
inline size_t ObjectGlobals(const uint8_t *code, size_t length, uint16_t *out, size_t max) {
	if (length < 2)
		return 0;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e)
		return 0;
	size_t at = 2, n = 0;
	for (uint8_t i = 0; i < e->count; ++i) {
		const size_t size = OperandSize(e->args[i], code, at, length);
		if (at + size > length)
			return n;
		if (e->args[i] == Arg::ObjGlobal && n < max && code[at] == scripts::PARAM_GLOBAL)
			out[n++] = static_cast<uint16_t>(code[at + 1] | (code[at + 2] << 8));
		at += size;
	}
	return n;
}

// A DELETE_OBJECT the world keeps: one whose global the mission never wrote,
// so it names an object the main script made (init.sc's barriers), not one
// of the mission's own. Last Requests takes Portland's subway gate and tunnel
// block away for good, and Love's third mission Staunton's; a save
// carries that, so the campaign has to. `missionWrote` says whether the
// mission wrote a global (game/mission.h, CampaignWrites::Known).
template <class Wrote>
inline bool DeletesWorldObject(const uint8_t *code, size_t length, Wrote missionWrote) {
	if (length < 2 || static_cast<uint16_t>(code[0] | (code[1] << 8)) != 0x0108)
		return false;
	uint16_t global = 0;
	return ObjectGlobals(code, length, &global, 1) == 1 && !missionWrote(global);
}

// A literal operand's four bytes: a Value, Blip or Object at `index`. False
// for anything else or past the end.
inline bool LiteralAt(const uint8_t *code, size_t length, uint8_t index, int32_t *out) {
	if (length < 2)
		return false;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e || index >= e->count)
		return false;
	size_t at = 2;
	for (uint8_t i = 0; i < index; ++i)
		at += OperandSize(e->args[i], code, at, length);
	if (at + 5 > length || OperandSize(e->args[index], code, at, length) != 5)
		return false;
	std::memcpy(out, code + at + 1, 4);
	return true;
}

inline bool SetLiteralAt(uint8_t *code, size_t length, uint8_t index, int32_t value) {
	int32_t was = 0;
	if (!LiteralAt(code, length, index, &was))
		return false;
	const Entry *e  = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	size_t       at = 2;
	for (uint8_t i = 0; i < index; ++i)
		at += OperandSize(e->args[i], code, at, length);
	std::memcpy(code + at + 1, &value, 4);
	return true;
}

// A pickup the world keeps once it is made: one that comes back after it is
// taken, in a shop (1) or on the street (2, and 14 the slow one). CREATE_PICKUP
// and CREATE_PICKUP_WITH_AMMO name the type second. Cipriani's Chauffeur's
// Uzi, Phil's guns and his armour are these; everything a mission lays out
// for the taking once is not (PICKUP_ONCE, the money, the packages).
inline bool KeepsPickup(const uint8_t *code, size_t length) {
	if (length < 2)
		return false;
	const uint16_t opcode = static_cast<uint16_t>(code[0] | (code[1] << 8));
	if (opcode != 0x0213 && opcode != 0x032B)
		return false;
	int32_t type = 0;
	if (!LiteralAt(code, length, 1, &type))
		return false;
	return type == 1 || type == 2 || type == 14;
}

// Every handle an instruction names, owner's to ours. A blip and a cutscene
// object are the owner's handles on the wire and mapped here as they are
// made; a pedestrian or a car is the netId the session gave it, and ours is
// whatever this machine's replica of it is. Each answers -1 for one this
// machine does not have.
struct Handles {
	const BlipMap *blips   = nullptr;
	const BlipMap *objects = nullptr;
	const BlipMap *pickups = nullptr;
	const BlipMap *fires   = nullptr;
	const BlipMap *spheres = nullptr;
	int32_t (*charOf)(int32_t netId) = nullptr;
	int32_t (*carOf)(int32_t netId)  = nullptr;
};

// Walks the literal operands of an encoded instruction that name a handle,
// by the list's own shape: `fn(arg, value)` gives what to write in its place,
// or false to give up.
template <class Fn>
inline bool EachHandle(uint8_t *code, size_t length, Fn fn) {
	if (length < 2)
		return false;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e)
		return false;
	size_t at = 2;
	for (uint8_t i = 0; i < e->count; ++i) {
		const size_t size = OperandSize(e->args[i], code, at, length);
		if (at + size > length)
			return false;
		const Arg a = e->args[i];
		// A blip sent by its global is each machine's own, not a handle.
		if (size == 5 && (a == Arg::Blip || a == Arg::Object || a == Arg::Char || a == Arg::Car ||
		                  a == Arg::Pickup || a == Arg::Fire ||
		                  a == Arg::Sphere)) {
			int32_t value = 0;
			std::memcpy(&value, code + at + 1, 4);
			if (!fn(a, &value))
				return false;
			std::memcpy(code + at + 1, &value, 4);
		}
		at += size;
	}
	return true;
}

// An instruction whose car may be none. A negative operand there is the
// engine's "no car", not a handle: SET_TARGET_CAR_FOR_MISSION_GARAGE's
// handler passes null for one (missionaddr.h), and every mission that points
// a garage at its car ends with one in its cleanup. It goes to everybody as
// -1 and is run as -1, where any other car nobody can name is dropped - which
// used to leave a participant's garage pointing at the copy of a car the
// session was about to take away.
constexpr bool CarMayBeNone(uint16_t opcode) {
	return opcode == scripts::op::SET_TARGET_CAR_FOR_MISSION_GARAGE;
}

// On the owner, before it is sent: each pedestrian or car the instruction
// names becomes its netId. False, not to be sent, for one the session has no
// name for yet (still inside its naming round trip, or never hosted).
inline bool ToWire(Encoded *enc, int32_t (*charNet)(int32_t handle),
                   int32_t (*carNet)(int32_t handle)) {
	const bool noneOk = CarMayBeNone(static_cast<uint16_t>(enc->code[0] | (enc->code[1] << 8)));
	return EachHandle(enc->code, enc->length, [&](Arg a, int32_t *v) {
		if (a != Arg::Char && a != Arg::Car)
			return true;
		if (a == Arg::Car && noneOk && *v < 0) {
			*v = -1;
			return true;
		}
		int32_t (*const fn)(int32_t) = a == Arg::Char ? charNet : carNet;
		if (!fn)
			return false;
		*v = fn(*v);
		return *v >= 0;
	});
}

// The participant's copy of an instruction, with ours written over every
// handle of the owner's it names. False, to be dropped, for one this machine
// does not have.
inline bool Translate(const Encoded &in, const Handles &h, Encoded *out) {
	*out = in;
	const bool noneOk = in.length >= 2 &&
	                    CarMayBeNone(static_cast<uint16_t>(in.code[0] | (in.code[1] << 8)));
	return EachHandle(out->code, out->length, [&](Arg a, int32_t *v) {
		switch (a) {
		case Arg::Blip:   *v = h.blips ? h.blips->Ours(*v) : -1; break;
		case Arg::Object: *v = h.objects ? h.objects->Ours(*v) : -1; break;
		case Arg::Pickup: *v = h.pickups ? h.pickups->Ours(*v) : -1; break;
		case Arg::Fire:   *v = h.fires ? h.fires->Ours(*v) : -1; break;
		case Arg::Sphere: *v = h.spheres ? h.spheres->Ours(*v) : -1; break;
		case Arg::Char:   *v = h.charOf ? h.charOf(*v) : -1; break;
		case Arg::Car:
			if (noneOk && *v < 0) {
				*v = -1;
				return true;
			}
			*v = h.carOf ? h.carOf(*v) : -1;
			break;
		default:          break;
		}
		return *v != -1;
	});
}

// Whether every pedestrian, car and object an instruction about to run here
// names is one `live(arg, handle)` still finds. The engine's handlers take
// GetAt's answer on trust and call through it - SET_CAR_HEADING's, at
// 0x004402BF, is `call 0043EAF0 / mov ecx,eax / call CPlaceable::SetHeading`
// with nothing in between - and a handle kept in a map or a roster row can
// outlive its entity by a frame: a replica the engine took away, a cutscene
// object a DELETE_OBJECT ended. The car an instruction may name as none, -1,
// is not asked about.
template <class Fn>
inline bool EntitiesLive(const uint8_t *code, size_t length, Fn live) {
	if (length > MAX_CODE)
		return false;
	uint8_t copy[MAX_CODE];
	std::memcpy(copy, code, length);
	const bool noneOk = length >= 2 &&
	                    CarMayBeNone(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	bool ok = true;
	const bool walked = EachHandle(copy, length, [&](Arg a, int32_t *v) {
		if (a != Arg::Char && a != Arg::Car && a != Arg::Object)
			return true;
		if (a == Arg::Car && noneOk && *v < 0)
			return true;
		ok = live(a, *v);
		return ok;
	});
	return walked && ok;
}

} // namespace coopiii::game::replay

namespace coopiii::game::replay {

inline bool SoundGlobalOf(const uint8_t *code, size_t length, uint16_t *out, bool *makes) {
	if (length < 2)
		return false;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e)
		return false;
	size_t at = 2;
	for (uint8_t i = 0; i < e->count; ++i) {
		const size_t size = EncodedSize(e->args[i]);
		if (at + size > length)
			return false;
		if ((e->args[i] == Arg::SoundOut || e->args[i] == Arg::SoundGlobal) &&
		    code[at] == scripts::PARAM_GLOBAL) {
			*out = static_cast<uint16_t>(code[at + 1] | (code[at + 2] << 8));
			if (makes)
				*makes = e->args[i] == Arg::SoundOut;
			return true;
		}
		at += size;
	}
	return false;
}

} // namespace coopiii::game::replay

namespace coopiii::game::replay {

// ---- an instruction off the wire ------------------------------------------------
//
// What a participant runs came from another machine, and the interpreter it
// is handed to checks none of it: CRunningScript::StoreParameters writes an
// output local at whatever index the operand names (m_anLocalVariables[i],
// our runner being a 0x100-byte buffer), a global at whatever offset, and a
// handler reads a text label up to its NUL (LOAD_CUTSCENE copies it into an
// eight-byte name). So an instruction is run only when it has exactly the
// shape Encode gives it, or the ones built by hand in its image: an opcode on
// the list, each operand of the type its Arg says, every global inside
// main.scm's variables (`globalsEnd`, mission.h GlobalsEnd), an output our
// local 0, every label ending inside its eight bytes, and nothing past the
// last operand.
inline bool WellFormed(const uint8_t *code, size_t length, uint32_t globalsEnd) {
	if (!code || length < 2 || length > MAX_CODE)
		return false;
	const Entry *e = Find(static_cast<uint16_t>(code[0] | (code[1] << 8)));
	if (!e)
		return false;
	auto global = [&](size_t at) {
		if (at + 3 > length || code[at] != scripts::PARAM_GLOBAL)
			return false;
		const uint32_t offset = static_cast<uint32_t>(code[at + 1] | (code[at + 2] << 8));
		return offset >= 8 && offset + 4u <= globalsEnd;
	};
	auto literal = [&](size_t at) { return at + 5 <= length && code[at] == scripts::PARAM_INT32; };
	size_t at = 2;
	for (uint8_t i = 0; i < e->count; ++i) {
		const Arg a = e->args[i];
		switch (a) {
		case Arg::Text: {
			if (at + 8 > length || std::memchr(code + at, 0, 8) == nullptr)
				return false;
			at += 8;
			break;
		}
		case Arg::Global:
		case Arg::ObjGlobal:
		case Arg::OutGlobal:
		case Arg::OutBlip:
		case Arg::SoundOut:
		case Arg::SoundGlobal:
			if (!global(at))
				return false;
			at += 3;
			break;
		case Arg::Output:
			if (at + 3 <= length && code[at] == scripts::PARAM_LOCAL) {
				if (code[at + 1] != 0 || code[at + 2] != 0)
					return false;
			} else if (e->kind != Kind::PickupNew || !global(at)) {
				return false;
			}
			at += 3;
			break;
		case Arg::Short:
			if (at + 3 > length || code[at] != scripts::PARAM_INT16)
				return false;
			at += 3;
			break;
		case Arg::Blip:
		case Arg::Pickup:
		case Arg::OwnGlobal:
			if (at < length && code[at] == scripts::PARAM_GLOBAL) {
				if (!global(at))
					return false;
				at += 3;
				break;
			}
			if (!literal(at))
				return false;
			at += 5;
			break;
		default:   // Value, Object, Char, Car, Fire, Sphere
			if (!literal(at))
				return false;
			at += 5;
			break;
		}
	}
	return at == length;
}

// Operands the engine's handler uses as an index into a table of fixed size
// and never checks, with the range the table allows. A peer's instruction
// outside it is not run: GiveWeapon writes m_weapons[type] (13 of them),
// CGarages' calls index aGarages[32], SetPhoneMessage_JustOnce writes
// m_aPhones[50], RequestSpecialChar builds a model id from its slot (four),
// CParticle::AddParticle reads m_aParticles[type] (68, re3 ParticleType.h),
// CPedType::ms_apPedType has 23 and CGangs 9, and StartFrenzy gives the
// player its weapon when it is below the weapon count, so a negative one too.
struct OperandRange {
	uint16_t opcode;
	uint8_t  index;
	int32_t  lo, hi;
};

inline bool OperandsInRange(const uint8_t *code, size_t length) {
	static const OperandRange kRanges[] = {
	    {0x01B1, 1, 0, 12},    // GIVE_WEAPON_TO_PLAYER, the weapon
	    {0x017A, 1, 0, 12},    // SET_PLAYER_AMMO
	    {0x01B8, 1, 0, 12},    // SET_CURRENT_PLAYER_WEAPON
	    {0x0237, 0, 0, 8},     // SET_GANG_WEAPONS, the gang
	    {0x0237, 1, 0, 12},    //   and its two weapons
	    {0x0237, 2, 0, 12},
	    {0x03F1, 0, 0, 22},    // SET_THREAT_FOR_PED_TYPE
	    {0x03F2, 0, 0, 22},    // CLEAR_THREAT_FOR_PED_TYPE
	    {0x02FA, 0, 0, 31},    // CHANGE_GARAGE_TYPE
	    {0x0299, 0, 0, 31},    // ACTIVATE_GARAGE
	    {0x02B9, 0, 0, 31},    // DEACTIVATE_GARAGE
	    {0x03A5, 0, 0, 31},    // CHANGE_GARAGE_TYPE_WITH_CAR_MODEL
	    {0x021B, 0, 0, 31},    // SET_TARGET_CAR_FOR_MISSION_GARAGE
	    {0x0360, 0, 0, 31},    // OPEN_GARAGE
	    {0x0361, 0, 0, 31},    // CLOSE_GARAGE
	    {0x03BB, 0, 0, 31},    // SET_GARAGE_DOOR_TYPE_TO_SWING_OPEN
	    {0x024C, 0, 0, 49},    // SET_PHONE_MESSAGE
	    {0x024E, 0, 0, 49},    // TURN_PHONE_OFF
	    {0x023C, 0, 1, 4},     // LOAD_SPECIAL_CHARACTER
	    {0x0296, 0, 1, 4},     // UNLOAD_SPECIAL_CHARACTER
	    {0x0437, 0, 0, 67},    // CREATE_SINGLE_PARTICLE
	    {0x03DD, 1, 0, 20},    // ADD_SPRITE_BLIP_FOR_PICKUP, RadarSprites[21]
	    {0x01F9, 1, 0, 0x7FFFFFFF},   // START_KILL_FRENZY, the weapon
	    // The player, CWorld::Players[i]: four of them, and only the first
	    // has a ped on any machine, which each of these reads through.
	    {0x01B4, 0, 0, 0},     // SET_PLAYER_CONTROL
	    {0x0157, 0, 0, 0},     // CAMERA_ON_PLAYER
	    {0x0055, 0, 0, 0},     // SET_PLAYER_COORDINATES
	    {0x0171, 0, 0, 0},     // SET_PLAYER_HEADING
	    {0x012A, 0, 0, 0},     // WARP_PLAYER_FROM_CAR_TO_COORD
	    {0x01B1, 0, 0, 0},     // GIVE_WEAPON_TO_PLAYER
	    {0x017A, 0, 0, 0},     // SET_PLAYER_AMMO
	    {0x01B8, 0, 0, 0},     // SET_CURRENT_PLAYER_WEAPON
	    {0x03B8, 0, 0, 0},     // REMOVE_ALL_PLAYER_WEAPONS
	    {0x01C0, 0, 0, 0},     // STORE_WANTED_LEVEL
	    {0x010D, 0, 0, 0},     // ALTER_WANTED_LEVEL
	    {0x0110, 0, 0, 0},     // CLEAR_WANTED_LEVEL
	    {0x0222, 0, 0, 0},     // SET_PLAYER_HEALTH
	    {0x0336, 0, 0, 0},     // SET_PLAYER_VISIBLE
	    {0x03BF, 0, 0, 0},     // SET_EVERYONE_IGNORE_PLAYER
	    {0x01F7, 0, 0, 0},     // SET_POLICE_IGNORE_PLAYER
	    {0x0330, 0, 0, 0},     // SET_PLAYER_NEVER_GETS_TIRED
	    {0x0413, 0, 0, 0},     // SET_GET_OUT_OF_JAIL_FREE
	    {0x0221, 0, 0, 0},     // APPLY_BRAKES_TO_PLAYERS_CAR
	    {0x03EF, 0, 0, 0},     // MAKE_PLAYER_SAFE_FOR_CUTSCENE
	};
	if (length < 2)
		return false;
	const uint16_t opcode = static_cast<uint16_t>(code[0] | (code[1] << 8));
	for (const OperandRange &r : kRanges) {
		if (r.opcode != opcode)
			continue;
		int32_t v = 0;
		if (!LiteralAt(code, length, r.index, &v))
			continue;   // sent by a global, which is this machine's own
		if (v < r.lo || v > r.hi)
			return false;
	}
	return true;
}

// Which operand of an instruction is a model id the handler builds or
// streams from, or -1. `usedObjects`: a negative one is main.scm's
// used-object table's (UsedObjectArray[-model]), which the handler reads
// without looking at the table's size.
inline int ModelOperand(uint16_t opcode, bool *usedObjects) {
	*usedObjects = true;
	switch (opcode) {
	case 0x0247:   // REQUEST_MODEL
	case 0x0249:   // MARK_MODEL_AS_NO_LONGER_NEEDED
	case 0x0107:   // CREATE_OBJECT
	case 0x029B:   // CREATE_OBJECT_NO_OFFSET
	case 0x0213:   // CREATE_PICKUP
	case 0x032B:   // CREATE_PICKUP_WITH_AMMO
		return 0;
	case 0x02E5:   // CREATE_CUTSCENE_OBJECT
	case 0x02F3:   // LOAD_SPECIAL_MODEL
		*usedObjects = false;
		return 0;
	case 0x02F4:   // CREATE_CUTSCENE_HEAD
		*usedObjects = false;
		return 1;
	default:
		return -1;
	}
}

// An instruction that builds something in the world out of its model, and
// has to find the model loaded when it runs: an object made from a model that
// is not in has no clump, and CREATE_OBJECT's handler adds it to the world
// all the same.
constexpr bool BuildsFromModel(uint16_t opcode) {
	return opcode == 0x0107 || opcode == 0x029B;
}

} // namespace coopiii::game::replay

namespace coopiii::game::replay {

// ---- The Exchange's Catalina helicopter (mission-audit.md R7) -----------------------
//
// Each machine flies its own, in CHeli::pHelis[3], from the replayed
// START_CATALINA_HELI; nothing streams it. A car operand that names the
// owner's goes as this in place of a netId, and a participant reads it as its
// own copy: the marker over the chopper and the camera on it. Past every
// netId, beside WIRE_OWN_PLAYER (mission.h).
constexpr int32_t WIRE_CATALINA_HELI = 0x10001;

// ---- the power pills (mission-audit.md R7) --------------------------------------
//
// CPacManPickups, a table of its own outside CPickups: Big'N'Veiny's magazines
// (START_PACMAN_RACE, fixed places) and Bullion Run's gold (START_PACMAN_SCRAMBLE,
// scattered by rand(), so no two machines would lay them in the same places).
// The owner's are drawn on every participant's screen from what the owner's
// table holds, one slot at a time, as a MISSION_EFFECT_RUN whose code is
// ADD_POWER_PILL's opcode with five literals: the place, the slot and the
// type (0 when the slot emptied). ADD_POWER_PILL itself is not on the list and
// has three, so nothing the owner's script runs can look like one. A
// participant writes the slot into its own table and never collects it: only
// the owner's car carries the gold and eats the pills its script counts.
//
// The table, verified against the retail image: CPacManPickups::aPMPickUps at
// 0x00731618, 0x100 of 0x14 bytes (ClearPMPickUps, 0x00433150: `cmp bp,100h`,
// `add esi,14h`, m_pObject at +0x0C, m_eType at +0x10), bPMActive at
// 0x0095CD6F; race pills are type 2 (0x00432D88), a scramble's is its argument,
// 1 from START_PACMAN_SCRAMBLE (0x004333D4). CPacManPickup::Update
// (0x004331B0, thiscall) has one caller, CPacManPickups::Update's loop at
// 0x00432AA0.
constexpr uint16_t PILL_OPCODE      = 0x02DA;   // ADD_POWER_PILL
constexpr size_t   PILL_OPERANDS    = 5;
constexpr size_t   PILL_CODE_LENGTH = 2 + PILL_OPERANDS * 5;
constexpr size_t   PILL_SLOTS       = 0x100;
constexpr uint8_t  PILL_NONE        = 0;
constexpr uint8_t  PILL_SCRAMBLE    = 1;
constexpr uint8_t  PILL_RACE        = 2;
constexpr float    PILL_WORLD_LIMIT = 5000.0f;
// How many slots the owner tells of in one frame. Bullion Run lays out 90
// at once; each goes as a packet of its own.
constexpr size_t   PILLS_PER_FRAME  = 16;

struct PillRow {
	uint8_t type = PILL_NONE;
	float   x = 0.0f, y = 0.0f, z = 0.0f;
};

inline bool PillChanged(const PillRow &a, const PillRow &b) {
	if (a.type != b.type)
		return true;
	return a.type != PILL_NONE && (a.x != b.x || a.y != b.y || a.z != b.z);
}

// The code for one slot. How long, 0 for no room.
inline size_t PillCode(uint8_t *code, size_t room, uint16_t slot, const PillRow &row) {
	if (room < PILL_CODE_LENGTH)
		return 0;
	int32_t v[PILL_OPERANDS];
	std::memcpy(&v[0], &row.x, 4);
	std::memcpy(&v[1], &row.y, 4);
	std::memcpy(&v[2], &row.z, 4);
	v[3] = slot;
	v[4] = row.type;
	code[0]   = static_cast<uint8_t>(PILL_OPCODE & 0xFF);
	code[1]   = static_cast<uint8_t>(PILL_OPCODE >> 8);
	size_t at = 2;
	for (int32_t value : v) {
		code[at++] = scripts::PARAM_INT32;
		std::memcpy(code + at, &value, 4);
		at += 4;
	}
	return at;
}

// And back. False for anything that is not one, a slot past the table, a
// type the engine has no arm for, or a place that is no number or off the map.
inline bool ReadPillCode(const uint8_t *code, size_t length, uint16_t *slot, PillRow *row) {
	if (length != PILL_CODE_LENGTH || static_cast<uint16_t>(code[0] | (code[1] << 8)) != PILL_OPCODE)
		return false;
	int32_t v[PILL_OPERANDS];
	for (size_t i = 0; i < PILL_OPERANDS; ++i) {
		if (code[2 + i * 5] != scripts::PARAM_INT32)
			return false;
		std::memcpy(&v[i], code + 3 + i * 5, 4);
	}
	if (v[3] < 0 || static_cast<size_t>(v[3]) >= PILL_SLOTS)
		return false;
	if (v[4] != PILL_NONE && v[4] != PILL_SCRAMBLE && v[4] != PILL_RACE)
		return false;
	PillRow r;
	std::memcpy(&r.x, &v[0], 4);
	std::memcpy(&r.y, &v[1], 4);
	std::memcpy(&r.z, &v[2], 4);
	r.type = static_cast<uint8_t>(v[4]);
	if (r.type != PILL_NONE) {
		const float at[3] = {r.x, r.y, r.z};
		for (float f : at)
			if (!(f > -PILL_WORLD_LIMIT && f < PILL_WORLD_LIMIT))   // NaN fails both
				return false;
	}
	*slot = static_cast<uint16_t>(v[3]);
	*row  = r;
	return true;
}

} // namespace coopiii::game::replay
