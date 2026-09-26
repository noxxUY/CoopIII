// Two things a remote player's copy used to do on its own that its owner
// never did: step aside when something bumps it, and, sat as a passenger,
// turn its head toward whoever walks or drives past.
#pragma once

namespace coopiii::game {

// Call-site redirections, not detours: six calls to CPed::SetEvasiveStep and
// the two look-around calls in CPed::ProcessControl's PED_DRIVING arm. Each
// is only rewritten while it still calls the engine's function. Not fatal.
bool InstallReplicaCalm();
void RemoveReplicaCalm();

} // namespace coopiii::game
