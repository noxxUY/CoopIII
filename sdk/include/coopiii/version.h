// Which CoopIII this is, as the player reads it: the mark in the corner of the
// HUD and the first line of each log.
//
// The same number as set_version in xmake.lua and the "coopiii" component in
// installer/assets/components.json, bumped together; tools/basetest checks it
// against xmake.lua. Not the protocol version - that one is PROTOCOL_VERSION in
// protocol.h, and it moves on its own schedule.
#pragma once

#define COOPIII_VERSION "0.0.1"
