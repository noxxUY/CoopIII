// The options dialog. options.h says what it is; this is every setting in it
// and how each one is drawn.
//
// One table, Settings(), holds every setting: its section, its name, the line
// under it, its control and how to read and write it in a ServerConfig. The
// dialog, the change count on each section, the console's "what changed" and
// the main screen's rules card all read the same table, so a setting added
// to it is added to all four.
#include "options.h"

#include "ui/anim.h"
#include "ui/fonts.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <imgui_internal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ui;

namespace coopiii {
namespace {

// ---- the table ---------------------------------------------------------------

enum class Kind : uint8_t { Toggle, Choice, Number, Port, Password };

struct Setting {
	int         section = SECTION_SERVER;
	Kind        kind    = Kind::Toggle;
	const char *key     = "";       // its name in CoopIII-Server.ini
	const char *title   = "";
	const char *detail  = "";       // the line under the title
	const char *note    = nullptr;  // a second, quieter line, when there is one
	bool        restart = false;    // only takes effect when the server starts

	// Choice: the segments, and a line of its own for each, which replaces
	// `detail` when there is one.
	const char *const *options       = nullptr;
	int                optionCount   = 0;
	const char *const *optionDetails = nullptr;

	// Number: the range and the step, and how a value reads. `scale` turns
	// the stored number into the one shown (centimetres into metres); `zero`
	// is what 0 reads as, when 0 means "never" rather than a number.
	int         lo = 0, hi = 0, step = 1;
	const char *format = "%.0f";
	const char *zero   = nullptr;
	float       scale  = 1.0f;

	int  (*get)(const ServerConfig &) = nullptr;
	void (*set)(ServerConfig &, int)  = nullptr;
	// When it says no, the setting does nothing under the others as they
	// stand, and is drawn faded and cannot be changed.
	bool (*applies)(const ServerConfig &) = nullptr;
};

const char *const kSectionNames[SECTION_COUNT] = {
    "Server", "Players & combat", "Wanted level", "Missions", "Money & progress", "World",
};

const char *const kOnOff[]     = {"Off", "On"};
const char *const kWanted[]    = {"Per player", "Shared", "Off"};
const char *const kWantedWhy[] = {
    "Per player: everyone keeps their own stars, shared while riding in the same car.",
    "Shared: the whole session has the highest wanted level anybody has.",
    "Off: nobody ever gets a wanted level.",
};
const char *const kEnemies[]    = {"Original", "Tougher", "More"};
const char *const kEnemiesWhy[] = {
    "Original: as many and as tough as in single player.",
    "Tougher: their health and armour grow with every extra player.",
    "More: tougher, and extra copies of the ordinary ones.",
};
const char *const kVotes[]   = {"75%", "Half", "Everyone", "Anyone"};
const char *const kMoney[]   = {"Off", "Own wallets", "Shared"};
const char *const kMoneyWhy[] = {
    "Off: every game pays its own player for what it saw, as it always has.",
    "Own wallets: the reward for a wreck or a helicopter goes to whoever earned it.",
    "Shared: one wallet for everybody, for every reward, purchase and fine.",
};
const char *const kPackages[]    = {"Shared", "Per player"};
const char *const kPackagesWhy[] = {
    "Shared: one player picks a package up and it counts for everybody.",
    "Per player: everybody finds their own hundred.",
};
const char *const kRampages[]    = {"Shared", "Scaled", "Off"};
const char *const kRampagesWhy[] = {
    "Shared: one rampage for everybody, and every player's kills count toward it.",
    "Scaled: shared, with the kill target multiplied by the number of players.",
    "Off: every player's rampage is their own.",
};
const char *const kCheats[]    = {"Shared", "Personal", "Off"};
const char *const kCheatsWhy[] = {
    "Shared: every cheat works, and the weather, speed and riot ones hit everybody.",
    "Personal: only the cheats about whoever typed them, like health or weapons.",
    "Off: no cheats at all while connected.",
};
const char *const kCoopCheats[]    = {"Outside missions", "Always", "Off"};
const char *const kCoopCheatsWhy[] = {
    "Outside missions: TPTO1 to TPTO8 put you beside that player, but not in a mission.",
    "Always: the same, during a mission too.",
    "Off: CoopIII's own cheats do nothing.",
};

Setting Toggle(int section, const char *key, const char *title, const char *detail,
               int (*get)(const ServerConfig &), void (*set)(ServerConfig &, int)) {
	Setting s;
	s.section     = section;
	s.kind        = Kind::Toggle;
	s.key         = key;
	s.title       = title;
	s.detail      = detail;
	s.options     = kOnOff;
	s.optionCount = 2;
	s.get         = get;
	s.set         = set;
	return s;
}

Setting Choice(int section, const char *key, const char *title, const char *detail,
               const char *const *options, int count, const char *const *details,
               int (*get)(const ServerConfig &), void (*set)(ServerConfig &, int)) {
	Setting s;
	s.section       = section;
	s.kind          = Kind::Choice;
	s.key           = key;
	s.title         = title;
	s.detail        = detail;
	s.options       = options;
	s.optionCount   = count;
	s.optionDetails = details;
	s.get           = get;
	s.set           = set;
	return s;
}

Setting Number(int section, const char *key, const char *title, const char *detail, int lo,
               int hi, int step, const char *format, int (*get)(const ServerConfig &),
               void (*set)(ServerConfig &, int)) {
	Setting s;
	s.section = section;
	s.kind    = Kind::Number;
	s.key     = key;
	s.title   = title;
	s.detail  = detail;
	s.lo      = lo;
	s.hi      = hi;
	s.step    = step;
	s.format  = format;
	s.get     = get;
	s.set     = set;
	return s;
}

const std::vector<Setting> &Settings() {
	static const std::vector<Setting> table = [] {
		std::vector<Setting> t;

		// ---- server
		{
			Setting s;
			s.section = SECTION_SERVER;
			s.kind    = Kind::Port;
			s.key     = "port";
			s.title   = "Port";
			s.detail  = "The UDP port players connect to. Friends outside your network need "
			            "it forwarded on your router.";
			s.restart = true;
			s.get     = [](const ServerConfig &c) { return static_cast<int>(c.port); };
			s.set     = [](ServerConfig &c, int v) { c.port = static_cast<uint16_t>(v); };
			t.push_back(s);
		}
		{
			Setting s;
			s.section = SECTION_SERVER;
			s.kind    = Kind::Password;
			s.key     = "password";
			s.title   = "Password";
			s.detail  = "What players put in their CoopIII.ini to join. Empty lets in anybody "
			            "with the address.";
			s.note    = "It keeps strangers out and no more: it crosses the network as typed.";
			t.push_back(s);
		}
		t.push_back(Number(
		    SECTION_SERVER, "maxPlayers", "Player slots",
		    "How many players the session takes. Lowering it turns nobody out.", 1, MAX_PLAYERS,
		    1, "%.0f", [](const ServerConfig &c) { return static_cast<int>(c.maxPlayers); },
		    [](ServerConfig &c, int v) { c.maxPlayers = static_cast<uint8_t>(v); }));
		{
			Setting s = Toggle(
			    SECTION_SERVER, "lookUpPublicAddress", "Show the public address",
			    "Asks a what-is-my-IP service for the address friends on the internet type, and "
			    "shows it up top.",
			    [](const ServerConfig &c) { return c.lookUpPublicAddress ? 1 : 0; },
			    [](ServerConfig &c, int v) { c.lookUpPublicAddress = v != 0; });
			s.note    = "Only this machine's address is looked up. Players see none of it.";
			s.restart = true;
			t.push_back(s);
		}
		{
			Setting s = Toggle(
			    SECTION_SERVER, "openRouterPort", "Open the port on the router",
			    "Asks the router over UPnP to forward the port here, and closes it again when the "
			    "server stops.",
			    [](const ServerConfig &c) { return c.openRouterPort ? 1 : 0; },
			    [](ServerConfig &c, int v) { c.openRouterPort = v != 0; });
			s.note    = "A router with UPnP turned off says no; then it has to be forwarded by hand.";
			s.restart = true;
			t.push_back(s);
		}

		// ---- players and combat
		t.push_back(Toggle(
		    SECTION_PLAYERS, "friendlyFire", "Friendly fire", "Players can hurt and kill each other.",
		    [](const ServerConfig &c) { return c.friendlyFire ? 1 : 0; },
		    [](ServerConfig &c, int v) { c.friendlyFire = v != 0; }));
		t.push_back(Toggle(
		    SECTION_PLAYERS, "ammoSync", "Real ammunition",
		    "Everybody's gun holds what its owner really has, so it can run dry on your screen.",
		    [](const ServerConfig &c) { return c.ammoSync ? 1 : 0; },
		    [](ServerConfig &c, int v) { c.ammoSync = v != 0; }));
		t.push_back(Toggle(
		    SECTION_PLAYERS, "syncCustomSkins", "Custom skins",
		    "Everybody sees the skin each player picked in Player Setup.",
		    [](const ServerConfig &c) { return c.syncCustomSkins ? 1 : 0; },
		    [](ServerConfig &c, int v) { c.syncCustomSkins = v != 0; }));

		// ---- wanted level
		t.push_back(Choice(
		    SECTION_WANTED, "wantedLevel", "Wanted level", "", kWanted, 3, kWantedWhy,
		    [](const ServerConfig &c) { return static_cast<int>(c.wantedLevel); },
		    [](ServerConfig &c, int v) { c.wantedLevel = static_cast<WantedLevelRule>(v); }));
		{
			Setting s = Number(
			    SECTION_WANTED, "maxWantedLevel", "Most stars",
			    "Nobody's wanted level goes higher than this. Six is the game's own.",
			    CONFIG_MAX_WANTED_MIN, WANTED_LEVEL_CEILING, 1, "%.0f of 6",
			    [](const ServerConfig &c) { return static_cast<int>(c.maxWanted); },
			    [](ServerConfig &c, int v) { c.maxWanted = static_cast<uint8_t>(v); });
			s.applies = [](const ServerConfig &c) { return c.wantedLevel != WantedLevelRule::Off; };
			t.push_back(s);
		}

		// ---- missions
		t.push_back(Toggle(
		    SECTION_MISSIONS, "missionFailOnDeath", "Mission fails on death",
		    "If anyone dies or is busted during a mission, it fails for everyone.",
		    [](const ServerConfig &c) { return c.missionFailOnDeath ? 1 : 0; },
		    [](ServerConfig &c, int v) { c.missionFailOnDeath = v != 0; }));
		{
			Setting s = Number(
			    SECTION_MISSIONS, "missionMargin", "Marker range",
			    "How far outside a mission's checkpoint still counts as being there. "
			    "A start counts anybody within 50 m, or within this when it is wider.",
			    0, 5000, 50, "%.1f m",
			    [](const ServerConfig &c) { return static_cast<int>(c.missionMarginCm); },
			    [](ServerConfig &c, int v) { c.missionMarginCm = static_cast<uint16_t>(v); });
			s.scale = 0.01f;
			t.push_back(s);
		}
		{
			Setting s = Number(
			    SECTION_MISSIONS, "missionCheckpointWait", "Checkpoint wait",
			    "How long a checkpoint waits for players who are not at it before the mission "
			    "goes on.",
			    0, MISSION_CHECKPOINT_WAIT_S_MAX, 5, "%.0f s",
			    [](const ServerConfig &c) { return static_cast<int>(c.missionCheckpointWaitS); },
			    [](ServerConfig &c, int v) { c.missionCheckpointWaitS = static_cast<uint16_t>(v); });
			s.zero = "Never";
			t.push_back(s);
		}
		{
			Setting s = Toggle(
			    SECTION_MISSIONS, "missionTimedCheckpoints", "Races and timed missions wait too",
			    "Checkpoints wait in races, side jobs and missions with a clock as well. The "
			    "clock keeps running.",
			    [](const ServerConfig &c) { return c.missionTimedCheckpoints ? 1 : 0; },
			    [](ServerConfig &c, int v) { c.missionTimedCheckpoints = v != 0; });
			s.applies = [](const ServerConfig &c) { return c.missionCheckpointWaitS != 0; };
			t.push_back(s);
		}
		{
			Setting s = Number(
			    SECTION_MISSIONS, "missionCatchUp", "Bring latecomers",
			    "Anybody who joins mid-mission, or respawns, further than this from its owner "
			    "is moved beside them.",
			    0, MISSION_DISTANCE_M_MAX, 10, "%.0f m",
			    [](const ServerConfig &c) { return static_cast<int>(c.missionCatchUpM); },
			    [](ServerConfig &c, int v) { c.missionCatchUpM = static_cast<uint16_t>(v); });
			s.zero = "Off";
			t.push_back(s);
		}
		{
			Setting s = Number(
			    SECTION_MISSIONS, "missionFallBehind", "Bring stragglers",
			    "Where checkpoints do not wait, anybody this far behind the owner is brought "
			    "along.",
			    0, MISSION_DISTANCE_M_MAX, 10, "%.0f m",
			    [](const ServerConfig &c) { return static_cast<int>(c.missionBehindM); },
			    [](ServerConfig &c, int v) { c.missionBehindM = static_cast<uint16_t>(v); });
			s.zero = "Off";
			t.push_back(s);
		}
		{
			Setting s = Number(
			    SECTION_MISSIONS, "missionFallBehindTime", "Straggler time",
			    "How long somebody has to be that far behind before they are brought along.",
			    CONFIG_BEHIND_S_MIN, MISSION_BEHIND_S_MAX, 1, "%.0f s",
			    [](const ServerConfig &c) { return static_cast<int>(c.missionBehindS); },
			    [](ServerConfig &c, int v) { c.missionBehindS = static_cast<uint16_t>(v); });
			s.applies = [](const ServerConfig &c) { return c.missionBehindM != 0; };
			t.push_back(s);
		}
		t.push_back(Number(
		    SECTION_MISSIONS, "missionIntroWait", "Wait for a busy game",
		    "How long a start waits for a player whose game is in an intro or mission of its "
		    "own.",
		    CONFIG_INTRO_WAIT_S_MIN, CONFIG_INTRO_WAIT_S_MAX, 5, "%.0f s",
		    [](const ServerConfig &c) { return static_cast<int>(c.missionIntroWaitS); },
		    [](ServerConfig &c, int v) { c.missionIntroWaitS = static_cast<uint16_t>(v); }));
		t.push_back(Choice(
		    SECTION_MISSIONS, "missionEnemies", "Enemies", "", kEnemies, 3, kEnemiesWhy,
		    [](const ServerConfig &c) { return static_cast<int>(c.missionEnemies); },
		    [](ServerConfig &c, int v) { c.missionEnemies = static_cast<uint8_t>(v); }));
		{
			Setting s = Number(
			    SECTION_MISSIONS, "missionScale", "Toughness per player",
			    "What each player after the first adds to the enemies' health and armour.", 0,
			    MISSION_SCALE_MAX, 10, "+%.0f%%",
			    [](const ServerConfig &c) { return static_cast<int>(c.missionScale); },
			    [](ServerConfig &c, int v) { c.missionScale = static_cast<uint16_t>(v); });
			s.applies = [](const ServerConfig &c) {
				return c.missionEnemies != MISSION_ENEMIES_ORIGINAL;
			};
			t.push_back(s);
		}
		t.push_back(Toggle(
		    SECTION_MISSIONS, "missionPayHelpers", "Helpers are paid",
		    "Everybody in a mission gets its reward, not only the player who started it.",
		    [](const ServerConfig &c) { return c.missionPayHelpers ? 1 : 0; },
		    [](ServerConfig &c, int v) { c.missionPayHelpers = v != 0; }));
		t.push_back(Choice(
		    SECTION_MISSIONS, "cutsceneSkip", "Cutscene skip",
		    "How many of the players watching a cutscene have to press skip.", kVotes, 4, nullptr,
		    [](const ServerConfig &c) { return static_cast<int>(c.cutsceneSkip); },
		    [](ServerConfig &c, int v) { c.cutsceneSkip = static_cast<VoteRule>(v); }));

		// ---- money and progress
		t.push_back(Choice(
		    SECTION_MONEY, "money", "Money", "", kMoney, 3, kMoneyWhy,
		    [](const ServerConfig &c) { return static_cast<int>(c.money); },
		    [](ServerConfig &c, int v) { c.money = static_cast<MoneyMode>(v); }));
		t.push_back(Choice(
		    SECTION_MONEY, "hiddenPackages", "Hidden packages", "", kPackages, 2, kPackagesWhy,
		    [](const ServerConfig &c) { return static_cast<int>(c.hiddenPackages); },
		    [](ServerConfig &c, int v) { c.hiddenPackages = static_cast<PackageMode>(v); }));
		{
			Setting s = Toggle(
			    SECTION_MONEY, "keepProgress", "Keep progress",
			    "The missions passed, the packages found and the car lists carry over to the "
			    "next time the server starts, and whoever is behind catches up when he joins.",
			    [](const ServerConfig &c) { return c.keepProgress ? 1 : 0; },
			    [](ServerConfig &c, int v) { c.keepProgress = v != 0; });
			s.note    = "Kept in CoopIII-Progress.dat. Forget progress, below, starts it over.";
			s.restart = true;
			t.push_back(s);
		}
		{
			Setting s = Choice(
			    SECTION_MONEY, "rampages", "Rampages", "", kRampages, 3, kRampagesWhy,
			    [](const ServerConfig &c) { return static_cast<int>(c.rampage); },
			    [](ServerConfig &c, int v) { c.rampage = static_cast<RampageMode>(v); });
			s.note = "A change made while a rampage runs waits for it to end.";
			t.push_back(s);
		}
		{
			Setting s = Choice(
			    SECTION_MONEY, "rampageVote", "Rampage vote",
			    "How many players have to agree before a rampage starts for everybody.", kVotes, 4,
			    nullptr, [](const ServerConfig &c) { return static_cast<int>(c.rampageVote); },
			    [](ServerConfig &c, int v) { c.rampageVote = static_cast<VoteRule>(v); });
			s.applies = [](const ServerConfig &c) { return c.rampage != RampageMode::Off; };
			t.push_back(s);
		}
		{
			Setting s = Number(
			    SECTION_MONEY, "rampageVoteTime", "Rampage vote time",
			    "How long the players have to answer it.", RAMPAGE_VOTE_MS_MIN / 1000,
			    RAMPAGE_VOTE_MS_MAX / 1000, 1, "%.0f s",
			    [](const ServerConfig &c) { return static_cast<int>(c.rampageVoteS); },
			    [](ServerConfig &c, int v) { c.rampageVoteS = static_cast<uint16_t>(v); });
			s.applies = [](const ServerConfig &c) { return c.rampage != RampageMode::Off; };
			t.push_back(s);
		}

		// ---- world
		t.push_back(Choice(
		    SECTION_WORLD, "cheats", "Cheats", "", kCheats, 3, kCheatsWhy,
		    [](const ServerConfig &c) { return static_cast<int>(c.cheats); },
		    [](ServerConfig &c, int v) { c.cheats = static_cast<CheatMode>(v); }));
		t.push_back(Choice(
		    SECTION_WORLD, "coopCheats", "CoopIII cheats", "", kCoopCheats, 3, kCoopCheatsWhy,
		    [](const ServerConfig &c) { return static_cast<int>(c.coopCheats); },
		    [](ServerConfig &c, int v) { c.coopCheats = static_cast<CoopCheatMode>(v); }));
		t.push_back(Number(
		    SECTION_WORLD, "abandonedCars", "Abandoned cars",
		    "How long a car stays once everybody has left it and gone more than 200 m away.",
		    CONFIG_ABANDONED_S_MIN, CONFIG_ABANDONED_S_MAX, 10, "%.0f s",
		    [](const ServerConfig &c) { return static_cast<int>(c.abandonedCarS); },
		    [](ServerConfig &c, int v) { c.abandonedCarS = static_cast<uint16_t>(v); }));
		return t;
	}();
	return table;
}

bool Differs(const Setting &s, const ServerConfig &a, const ServerConfig &b) {
	if (s.kind == Kind::Password)
		return a.password != b.password;
	return s.get(a) != s.get(b);
}

// What a setting's value reads as, in the dialog and in the console.
std::string ValueText(const Setting &s, const ServerConfig &c) {
	char out[64];
	switch (s.kind) {
	case Kind::Password:
		return c.password.empty() ? "none" : "set";
	case Kind::Port:
		std::snprintf(out, sizeof out, "%d", s.get(c));
		return out;
	case Kind::Toggle:
	case Kind::Choice: {
		const int v = s.get(c);
		return v >= 0 && v < s.optionCount ? s.options[v] : "?";
	}
	case Kind::Number: {
		const int v = s.get(c);
		if (v == 0 && s.zero)
			return s.zero;
		std::snprintf(out, sizeof out, s.format, static_cast<double>(v * s.scale));
		return out;
	}
	}
	return "";
}

// Wrapped text's height, the way TextWrapped breaks it, without drawing it.
float WrappedHeight(Type role, float width, const char *text) {
	const TypeStyle style = StyleOf(role);
	const char     *p     = text;
	const char     *end   = text + std::strlen(text);
	float           h     = 0.0f;
	while (p < end) {
		const char *stop = style.font->CalcWordWrapPosition(style.pixelSize, p, end, width);
		if (stop == p)
			stop = p + 1;
		h += style.lineHeight;
		p = stop;
		while (p < end && (*p == ' ' || *p == '\n'))
			++p;
	}
	return h > 0.0f ? h : style.lineHeight;
}

// ---- the controls -------------------------------------------------------------

constexpr float kControlH = 34.0f;
constexpr float kStepperW = 34.0f + 4.0f + 84.0f + 4.0f + 34.0f;
constexpr float kFieldW   = 200.0f;

float ChoiceWidth(const Setting &s) {
	float widest = 0.0f;
	for (int i = 0; i < s.optionCount; ++i)
		widest = ImMax(widest, MeasureText(Type::ButtonSmall, s.options[i]).x);
	return (widest + 26.0f) * static_cast<float>(s.optionCount) + 6.0f;
}

float ControlWidth(const Setting &s) {
	switch (s.kind) {
	case Kind::Toggle:   return 48.0f;
	case Kind::Choice:   return ChoiceWidth(s);
	case Kind::Number:   return kStepperW;
	case Kind::Port:     return 110.0f;
	case Kind::Password: return kFieldW;
	}
	return 48.0f;
}

float ControlHeight(const Setting &s) { return s.kind == Kind::Toggle ? 28.0f : kControlH; }

ImDrawList *Draw() { return ImGui::GetWindowDrawList(); }

// [-] value [+], held down to run. True when the value moved.
bool Stepper(const char *id, ImVec2 pos, const Setting &s, int *value, const Theme &theme) {
	const float side = kControlH;
	const Rect  minus{pos, ImVec2(side, side)};
	const Rect  mid{ImVec2(pos.x + side + 4.0f, pos.y), ImVec2(84.0f, side)};
	const Rect  plus{ImVec2(mid.Max().x + 4.0f, pos.y), ImVec2(side, side)};

	Draw()->AddRectFilled(mid.pos, mid.Max(), theme.bgWindow, radius::kInput);
	ServerConfig shown;
	s.set(shown, *value);
	const std::string text = ValueText(s, shown);
	const float       w    = MeasureText(Type::Mono, text.c_str()).x;
	TextMiddle(ImVec2(mid.pos.x + (mid.size.x - w) * 0.5f, mid.pos.y), side, Type::Mono,
	           theme.textPrimary, text.c_str());

	const bool canDown = *value > s.lo;
	const bool canUp   = *value < s.hi;
	bool       moved   = false;
	char       key[64];
	ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
	std::snprintf(key, sizeof key, "%s-", id);
	if (IconButton(key, minus, Icon::Minus, theme, canDown ? theme.textButton : theme.textMuted,
	               14.0f) &&
	    canDown) {
		// Back onto the step's grid first, so 333 cm goes to 300 and not 283.
		const int off = (*value - s.lo) % s.step;
		*value        = ImMax(s.lo, *value - (off != 0 ? off : s.step));
		moved         = true;
	}
	std::snprintf(key, sizeof key, "%s+", id);
	if (IconButton(key, plus, Icon::Plus, theme, canUp ? theme.textButton : theme.textMuted,
	               14.0f) &&
	    canUp) {
		const int off = (*value - s.lo) % s.step;
		*value        = ImMin(s.hi, *value + (s.step - off));
		moved         = true;
	}
	ImGui::PopItemFlag();
	return moved;
}

bool PortValid(const char *text, uint16_t *out) {
	if (!text[0])
		return false;
	char      *end = nullptr;
	const long v   = std::strtol(text, &end, 10);
	if (*end != '\0' || v < 1 || v > 65535)
		return false;
	*out = static_cast<uint16_t>(v);
	return true;
}

// ---- layout ---------------------------------------------------------------------

constexpr float kRowPadX    = 18.0f;
constexpr float kRowPadY    = 14.0f;
constexpr float kRowMinH    = 66.0f;
constexpr float kSectionGap = 22.0f;
constexpr float kHeadingH   = 30.0f;
constexpr float kRailW      = 176.0f;
constexpr float kRailGap    = 20.0f;
constexpr float kRailItemH  = 38.0f;

struct RowLayout {
	float       height;
	bool        below;   // the control goes under the text, not beside it
	float       textW;
	const char *detail;
};

RowLayout LayOut(const Setting &s, const ServerConfig &c, float innerW, bool portBad) {
	RowLayout   r{};
	const float cw = ControlWidth(s);
	r.below        = cw > innerW * 0.5f;
	r.textW        = r.below ? innerW : innerW - cw - 24.0f;
	r.detail       = s.detail;
	if (s.kind == Kind::Choice && s.optionDetails) {
		const int v = s.get(c);
		if (v >= 0 && v < s.optionCount)
			r.detail = s.optionDetails[v];
	}
	if (s.kind == Kind::Port && portBad)
		r.detail = "A port is a number from 1 to 65535.";
	float h = kRowPadY + 20.0f + 3.0f + WrappedHeight(Type::BodySmall, r.textW, r.detail);
	if (s.note)
		h += 2.0f + WrappedHeight(Type::Hint, r.textW, s.note);
	h += kRowPadY;
	if (r.below)
		h += ControlHeight(s) + 4.0f;
	r.height = ImMax(h, r.below ? 0.0f : ControlHeight(s) + kRowPadY * 2.0f);
	r.height = ImMax(r.height, kRowMinH);
	return r;
}

} // namespace

// ---- the dialog -------------------------------------------------------------------

void OptionsDialog::Open(const ServerConfig &from) {
	open     = true;
	editing  = from;
	scroll   = 0.0f;
	dragging = false;
	forgetArmed = false;
	std::snprintf(port, sizeof port, "%u", static_cast<unsigned>(from.port));
	std::snprintf(password, sizeof password, "%s", from.password.c_str());
}

OptionsResult DrawOptions(OptionsDialog &d, const ServerConfig &saved, App &app, ImVec2 screen,
                          float topInset) {
	const Theme &theme = app.CurrentTheme();
	const float  t     = Appear("##optionsdialog", d.open, 17.0f);
	if (t <= 0.0f)
		return OptionsResult::None;

	OptionsResult result = OptionsResult::None;
	const auto   &table  = Settings();

	// As big as the design would like and no bigger than the window: the
	// server window can be made small, and the list scrolls inside it.
	const ImVec2 size(ImClamp(screen.x - 32.0f, 360.0f, 820.0f),
	                  ImClamp(screen.y - topInset - 24.0f, 300.0f, 700.0f));
	const Dialog dialog(screen, size, theme, t, topInset);
	ImGui::BeginDisabled(!d.open);

	ImDrawList *draw = ImGui::GetWindowDrawList();
	const Rect  box  = dialog.box;
	Card(box, theme, radius::kDialog, theme.bgPanel, theme.borderControl);

	const float dx = box.pos.x + 28.0f;
	const float dw = box.size.x - 56.0f;
	float       dy = box.pos.y + 24.0f;

	DrawIcon(draw, Icon::Gear, ImVec2(dx, dy + 4.0f), 20.0f, theme.textSecondary);
	Text(ImVec2(dx + 30.0f, dy), Type::DialogHeading, theme.textPrimary, "Server options");
	if (IconButton("##optclose",
	               {ImVec2(box.Max().x - 28.0f - 36.0f, dy - 4.0f), ImVec2(36.0f, 36.0f)},
	               Icon::Close, theme, theme.textSecondary, 18.0f, false))
		result = OptionsResult::Close;
	dy += 28.0f + 8.0f;
	Text(ImVec2(dx, dy), Type::Body, theme.textSecondary,
	     "Synced to every player in the session. Defaults match single player.");
	dy += 20.0f + 18.0f;

	const float actionsY  = box.Max().y - 24.0f - 44.0f;
	const float bodyTop   = dy;
	const float bodyBot   = actionsY - 18.0f;
	// The rail wants the width for itself and the height for its six names.
	const bool  withRail  = dw >= 600.0f && bodyBot - bodyTop >= SECTION_COUNT * kRailItemH;
	const float listX     = withRail ? dx + kRailW + kRailGap : dx;
	const float listW     = withRail ? dw - kRailW - kRailGap : dw;
	const float cardW     = listW - 12.0f;   // room for the scroll bar
	const float innerW    = cardW - kRowPadX * 2.0f;
	const Rect  view{ImVec2(listX, bodyTop), ImVec2(listW, bodyBot - bodyTop)};

	uint16_t   portValue = 0;
	const bool portOk    = PortValid(d.port, &portValue);
	if (portOk)
		d.editing.port = portValue;
	d.editing.password = ServerConfig::CleanPassword(d.password);

	// How many settings each section has that are not what is saved.
	int changed[SECTION_COUNT] = {};
	int changedAll             = 0;
	for (const Setting &s : table)
		if (Differs(s, d.editing, saved)) {
			++changed[s.section];
			++changedAll;
		}

	// ---- the scroll
	const float maxScroll = ImMax(0.0f, d.contentH - view.size.y);
	const bool  overList  = d.open && ImGui::IsMouseHoveringRect(view.pos, view.Max(), false);
	if (overList && ImGui::GetIO().MouseWheel != 0.0f)
		d.scroll -= ImGui::GetIO().MouseWheel * 72.0f;
	d.scroll           = ImClamp(d.scroll, 0.0f, maxScroll);
	const float scroll = ImClamp(Animate("##optscroll", d.scroll, 20.0f), 0.0f, maxScroll);

	// ---- the rail
	if (withRail) {
		int here = 0;
		for (int i = 0; i < SECTION_COUNT; ++i)
			if (d.sectionY[i] - scroll <= 40.0f)
				here = i;
		if (scroll >= maxScroll - 1.0f && maxScroll > 0.0f)
			here = SECTION_COUNT - 1;
		if (d.contentH <= 0.0f)
			here = 0;   // nothing laid out yet to say where the sections are

		const float sel = Animate("##optrailsel", static_cast<float>(here), 20.0f);
		draw->AddRectFilled(ImVec2(dx, bodyTop + sel * kRailItemH),
		                    ImVec2(dx + kRailW, bodyTop + sel * kRailItemH + kRailItemH - 4.0f),
		                    theme.bgSelected, radius::kInput);
		for (int i = 0; i < SECTION_COUNT; ++i) {
			const Rect item{ImVec2(dx, bodyTop + i * kRailItemH), ImVec2(kRailW, kRailItemH - 4.0f)};
			char       key[32];
			std::snprintf(key, sizeof key, "##optrail%d", i);
			const Touched f = Hotspot(key, item);
			if (f.clicked)
				d.scroll = ImMin(d.sectionY[i], maxScroll);
			if (f.hovered)
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (i != here && f.t > 0.0f)
				draw->AddRectFilled(item.pos, item.Max(),
				                    WithAlpha(Lift(theme.bgPanel, theme, f.t), ImMin(1.0f, f.t * 2.0f)),
				                    radius::kInput);
			TextMiddle(ImVec2(item.pos.x + 12.0f, item.pos.y), item.size.y, Type::FieldLabel,
			           i == here ? theme.textPrimary : theme.textSecondary, kSectionNames[i]);
			// Unsaved changes in it: a count in a small round badge.
			const float shown = Appear(ImGui::GetID(key) ^ 0x51u, changed[i] > 0, 14.0f);
			if (shown > 0.0f) {
				char n[8];
				std::snprintf(n, sizeof n, "%d", changed[i] > 0 ? changed[i] : 1);
				const float  nw = MeasureText(Type::MonoSmall, n).x;
				const float  bw = ImMax(20.0f, nw + 12.0f);
				const ImVec2 c(item.Max().x - 10.0f - bw * 0.5f, item.Centre().y);
				const DrawGroup badge(draw);
				draw->AddRectFilled(ImVec2(c.x - bw * 0.5f, c.y - 10.0f),
				                    ImVec2(c.x + bw * 0.5f, c.y + 10.0f), theme.actionBg, 10.0f);
				TextMiddle(ImVec2(c.x - nw * 0.5f, c.y - 10.0f), 20.0f, Type::MonoSmall,
				           theme.actionFg, n);
				badge.Fade(shown);
				badge.Scale(c, 0.6f + 0.4f * EaseBack(shown));
			}
		}
	}

	// ---- the list
	ImGui::PushClipRect(view.pos, view.Max(), true);
	float y = 0.0f;   // in the list, from its top
	static int   s_tipRow   = -1;
	static float s_tipSince = 0.0f;
	bool         tipHovered = false;
	for (int sec = 0; sec < SECTION_COUNT; ++sec) {
		d.sectionY[sec] = y;
		const float headY = bodyTop + y - scroll;
		if (headY + kHeadingH > view.pos.y && headY < view.Max().y)
			TextMiddle(ImVec2(listX + 2.0f, headY), kHeadingH - 8.0f, Type::SectionTitle,
			           theme.textPrimary, kSectionNames[sec]);
		y += kHeadingH;

		// The rows first, so the card knows how tall it is.
		struct Placed {
			int       index;
			RowLayout layout;
		};
		std::vector<Placed> rows;
		float               cardH = 0.0f;
		for (int i = 0; i < static_cast<int>(table.size()); ++i) {
			if (table[i].section != sec)
				continue;
			const RowLayout r = LayOut(table[i], d.editing, innerW, !portOk);
			rows.push_back({i, r});
			cardH += r.height;
		}
		const float cardTop = bodyTop + y - scroll;
		const Rect  card{ImVec2(listX, cardTop), ImVec2(cardW, cardH)};
		if (card.Max().y > view.pos.y && card.pos.y < view.Max().y)
			Card(card, theme, radius::kCard, theme.bgCard, theme.border);

		float rowY = cardTop;
		for (size_t k = 0; k < rows.size(); ++k) {
			const Setting   &s = table[rows[k].index];
			const RowLayout &r = rows[k].layout;
			const Rect       row{ImVec2(listX, rowY), ImVec2(cardW, r.height)};
			rowY += r.height;
			if (row.Max().y <= view.pos.y || row.pos.y >= view.Max().y)
				continue;

			const bool first = k == 0, last = k + 1 == rows.size();
			const bool live  = !s.applies || s.applies(d.editing);
			char       id[40];
			std::snprintf(id, sizeof id, "##opt_%s", s.key);

			// The one hover state: the row's surface lifts a little. Tested
			// rather than submitted, so it never takes a click from the
			// control on it.
			const bool  over = d.open && ImGui::IsMouseHoveringRect(row.pos, row.Max(), true);
			const float lift = Animate(ImGui::GetID(id) ^ 0x1Fu, over ? 1.0f : 0.0f, 14.0f);
			if (lift > 0.0f) {
				const ImDrawFlags corners = (first ? ImDrawFlags_RoundCornersTop : 0) |
				                            (last ? ImDrawFlags_RoundCornersBottom : 0);
				draw->AddRectFilled(ImVec2(row.pos.x + 1.0f, row.pos.y + (first ? 1.0f : 0.0f)),
				                    ImVec2(row.Max().x - 1.0f, row.Max().y - (last ? 1.0f : 0.0f)),
				                    WithAlpha(Lift(theme.bgCard, theme, 0.5f), lift),
				                    corners ? radius::kCard - 1.0f : 0.0f,
				                    corners ? corners : ImDrawFlags_RoundCornersNone);
			}
			if (!first)
				draw->AddLine(ImVec2(row.pos.x + kRowPadX, row.pos.y + 0.5f),
				              ImVec2(row.Max().x - kRowPadX, row.pos.y + 0.5f), theme.border, 1.0f);
			// Changed and not saved yet: a bar down the row's left edge.
			const float mark =
			    Animate(ImGui::GetID(id) ^ 0x2Eu, Differs(s, d.editing, saved) ? 1.0f : 0.0f, 16.0f);
			if (mark > 0.0f)
				draw->AddRectFilled(ImVec2(row.pos.x + 1.0f, row.pos.y + 10.0f),
				                    ImVec2(row.pos.x + 4.0f, row.Max().y - 10.0f),
				                    WithAlpha(theme.actionBg, mark), 1.5f);

			const DrawGroup faded(draw);
			const float     tx = row.pos.x + kRowPadX;
			float           ty = row.pos.y + kRowPadY;
			const float     tw = Text(ImVec2(tx, ty), Type::SectionTitle, theme.textPrimary, s.title);
			if (s.restart)
				Chip(ImVec2(tx + tw + 10.0f, ty - 1.0f), "Restart", theme);
			const Rect titleBox{ImVec2(tx, ty), ImVec2(tw, 20.0f)};
			ty += 20.0f + 3.0f;
			ty += TextWrapped(ImVec2(tx, ty), r.textW, Type::BodySmall,
			                  s.kind == Kind::Port && !portOk ? theme.statusFail : theme.textTertiary,
			                  r.detail);
			if (s.note) {
				ty += 2.0f;
				ty += TextWrapped(ImVec2(tx, ty), r.textW, Type::Hint, theme.textMuted, s.note);
			}

			// The control: on the right, centred on the row, or under the text
			// when it is too wide to sit beside it.
			const float  cw = ControlWidth(s), ch = ControlHeight(s);
			const ImVec2 at = r.below ? ImVec2(tx, row.Max().y - kRowPadY - ch)
			                          : ImVec2(row.Max().x - kRowPadX - cw,
			                                   row.pos.y + (r.height - ch) * 0.5f);
			ImGui::BeginDisabled(!live);
			switch (s.kind) {
			case Kind::Toggle: {
				bool on = s.get(d.editing) != 0;
				if (Switch(id, at, &on, theme))
					s.set(d.editing, on ? 1 : 0);
				break;
			}
			case Kind::Choice: {
				int v = s.get(d.editing);
				if (Segmented(id, {at, ImVec2(cw, ch)}, s.options, s.optionCount, &v, theme))
					s.set(d.editing, v);
				break;
			}
			case Kind::Number: {
				int v = s.get(d.editing);
				if (Stepper(id, at, s, &v, theme))
					s.set(d.editing, v);
				break;
			}
			case Kind::Port:
				TextInput(id, {at, ImVec2(cw, ch)}, d.port, sizeof d.port, theme, "2001",
				          ImGuiInputTextFlags_CharsDecimal);
				break;
			case Kind::Password:
				TextInput(id, {at, ImVec2(cw, ch)}, d.password, sizeof d.password, theme,
				          "No password");
				break;
			}
			ImGui::EndDisabled();
			if (!live)
				faded.Fade(0.45f);

			// Hovering the name says what the setting is called in the file
			// and what it is by default.
			if (d.open && ImGui::IsMouseHoveringRect(titleBox.pos, titleBox.Max(), true)) {
				tipHovered = true;
				if (s_tipRow != rows[k].index) {
					s_tipRow   = rows[k].index;
					s_tipSince = app.Seconds();
				}
				if (app.Seconds() - s_tipSince > 0.45f) {
					const std::string def = ValueText(s, ServerConfig{});
					ImGui::SetTooltip("%s in CoopIII-Server.ini\nDefault: %s%s", s.key,
					                  s.kind == Kind::Password ? "no password" : def.c_str(),
					                  s.restart ? "\nTakes effect when the server starts again."
					                  : !live   ? "\nDoes nothing with the settings above as they are."
					                            : "");
				}
			}
		}
		y += cardH + (sec + 1 < SECTION_COUNT ? kSectionGap : 0.0f);
	}
	if (!tipHovered)
		s_tipRow = -1;
	d.contentH = y + 4.0f;
	ImGui::PopClipRect();

	// The list's edges fade when there is more of it past them.
	const float fadeH = 18.0f;
	if (scroll > 0.5f)
		draw->AddRectFilledMultiColor(view.pos, ImVec2(view.Max().x, view.pos.y + fadeH),
		                              theme.bgPanel, theme.bgPanel, WithAlpha(theme.bgPanel, 0.0f),
		                              WithAlpha(theme.bgPanel, 0.0f));
	if (scroll < maxScroll - 0.5f)
		draw->AddRectFilledMultiColor(ImVec2(view.pos.x, view.Max().y - fadeH), view.Max(),
		                              WithAlpha(theme.bgPanel, 0.0f), WithAlpha(theme.bgPanel, 0.0f),
		                              theme.bgPanel, theme.bgPanel);

	// The scroll bar, which can be dragged.
	if (maxScroll > 0.0f) {
		const float trackX = view.Max().x - 5.0f;
		const float thumbH = ImMax(36.0f, view.size.y * view.size.y / d.contentH);
		const float thumbY = view.pos.y + (view.size.y - thumbH) * (scroll / maxScroll);
		const Rect  thumb{ImVec2(trackX - 4.0f, thumbY), ImVec2(12.0f, thumbH)};
		const Touched f = Hotspot("##optthumb", thumb);
		if (f.held) {
			if (!d.dragging) {
				d.dragging = true;
				d.dragFrom = ImGui::GetIO().MousePos.y - thumbY;
			}
			const float at = ImGui::GetIO().MousePos.y - d.dragFrom - view.pos.y;
			d.scroll       = ImClamp(at / ImMax(1.0f, view.size.y - thumbH), 0.0f, 1.0f) * maxScroll;
		} else {
			d.dragging = false;
		}
		draw->AddRectFilled(ImVec2(trackX, thumbY), ImVec2(trackX + 4.0f, thumbY + thumbH),
		                    Mix(theme.borderControl, theme.borderStrong, ImMin(1.0f, f.t * 2.0f)),
		                    2.0f);
	}

	// ---- the footer
	draw->AddLine(ImVec2(dx, actionsY - 9.5f), ImVec2(dx + dw, actionsY - 9.5f), theme.border, 1.0f);
	if (LinkText("##optreset", ImVec2(dx, actionsY + 13.0f), Type::ButtonSmall,
	             theme.textSecondary, "Reset to defaults")) {
		// Where the server is and who may join are not rules; they stay.
		ServerConfig defaults;
		defaults.port     = d.editing.port;
		defaults.password = d.editing.password;
		d.editing         = defaults;
	}
	ImGui::SetItemTooltip("Every rule back to single player's. The port and the password stay.");
	float noteX = dx + MeasureText(Type::ButtonSmall, "Reset to defaults").x + 18.0f;
	// Starting the campaign over, beside it when the dialog is wide enough to
	// have room. Two clicks, since the first one only asks.
	if (dw >= 560.0f) {
		const char *label = d.forgetArmed ? "Click again to forget" : "Forget progress";
		if (LinkText("##optforget", ImVec2(noteX, actionsY + 13.0f), Type::ButtonSmall,
		             d.forgetArmed ? theme.statusFail : theme.textSecondary, label)) {
			if (d.forgetArmed)
				result = OptionsResult::ForgetProgress;
			d.forgetArmed = !d.forgetArmed;
		}
		ImGui::SetItemTooltip("Starts the campaign over: the missions passed, the packages found "
		                      "and the car lists.\nOnly with nobody connected. The old file is "
		                      "kept as CoopIII-Progress.dat.old.");
		noteX += MeasureText(Type::ButtonSmall, label).x + 18.0f;
	}
	if (changedAll > 0) {
		char note[48];
		std::snprintf(note, sizeof note, "%d unsaved change%s", changedAll,
		              changedAll == 1 ? "" : "s");
		TextMiddle(ImVec2(noteX, actionsY), 44.0f, Type::Hint, theme.textTertiary, note);
	}

	if (PrimaryButton("##optsave", {ImVec2(box.Max().x - 28.0f - 92.0f, actionsY), ImVec2(92.0f, 44.0f)},
	                  "Save", Icon::Check, portOk, theme, Type::ButtonSmall) &&
	    portOk)
		result = OptionsResult::Save;
	if (OutlineButton("##optcancel",
	                  {ImVec2(box.Max().x - 28.0f - 92.0f - 10.0f - 96.0f, actionsY),
	                   ImVec2(96.0f, 44.0f)},
	                  "Cancel", Icon::Close, theme, false, Type::ButtonSmall))
		result = OptionsResult::Close;

	if (d.open && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput)
		result = OptionsResult::Close;

	ImGui::EndDisabled();
	dialog.End();

	if (result != OptionsResult::None && result != OptionsResult::ForgetProgress)
		d.open = false;
	return result;
}

std::vector<std::string> DescribeChanges(const ServerConfig &before, const ServerConfig &after) {
	std::vector<std::string> lines;
	for (const Setting &s : Settings()) {
		if (!Differs(s, before, after))
			continue;
		std::string line;
		if (s.kind == Kind::Password) {
			line = after.password.empty() ? "password removed: anybody with the address can join"
			       : before.password.empty() ? "password set: players need it to join"
			                                 : "password changed";
		} else {
			line = s.title;
			line += ": ";
			line += ValueText(s, after);
			line += " (was ";
			line += ValueText(s, before);
			line += ")";
			if (s.restart)
				line += ", from the next start of the server";
		}
		lines.push_back(line);
	}
	return lines;
}

size_t SettingCount() { return Settings().size(); }

std::vector<RuleSummary> SummariseRules(const ServerConfig &c) {
	static const char *const kKeys[] = {"friendlyFire", "wantedLevel", "missionFailOnDeath",
	                                    "money",        "rampages",    "cheats",
	                                    "hiddenPackages"};
	std::vector<RuleSummary> out;
	for (const char *key : kKeys)
		for (const Setting &s : Settings())
			if (std::strcmp(s.key, key) == 0)
				out.push_back({s.title, ValueText(s, c)});
	return out;
}

} // namespace coopiii
