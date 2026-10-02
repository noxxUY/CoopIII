// The thread behind presence.h: finds Discord's pipe, says who we are, and
// keeps the activity the game thread last handed it on the profile.
//
// It never blocks the game: the game thread only copies an Activity under a
// lock. And it never blocks itself for long: reads only take what
// PeekNamedPipe says is already there, and the one wait, for the answer to a
// handshake, gives up after a few seconds. Without Discord it looks again every
// RETRY_MS and says so in the log once.
#include "presence.h"

#include "log.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <windows.h>

namespace coopiii::presence {

namespace {

constexpr uint32_t TICK_MS            = 100;
constexpr uint32_t HANDSHAKE_WAIT_MS  = 5000;
constexpr int      PIPE_COUNT         = 10;

std::mutex        g_lock;
Activity          g_want;
// Never destroyed at process exit: by then the thread was ended where it
// stood, and a joinable std::thread going out of scope ends the process.
std::thread      *g_thread = nullptr;
std::atomic<bool> g_stop{false};
uint64_t          g_appId = 0;

// Only the thread touches these.
HANDLE   g_pipe          = INVALID_HANDLE_VALUE;
uint32_t g_nonce         = 0;
bool     g_saidNoDiscord = false;
bool     g_saidRefused   = false;
bool     g_saidError     = false;

HANDLE OpenPipe() {
	for (int i = 0; i < PIPE_COUNT; ++i) {
		wchar_t name[64];
		swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
		const HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		                             0, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			return h;
	}
	return INVALID_HANDLE_VALUE;
}

void Drop() {
	if (g_pipe != INVALID_HANDLE_VALUE)
		CloseHandle(g_pipe);
	g_pipe = INVALID_HANDLE_VALUE;
}

bool Send(uint32_t op, const std::string &json) {
	const std::string bytes = Frame(op, json);
	DWORD             done  = 0;
	return WriteFile(g_pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) &&
	       done == bytes.size();
}

// One frame, when a whole one is waiting. 1 for a frame, 0 for nothing yet,
// -1 for a pipe that broke or a stream that makes no sense.
int ReadFrame(uint32_t *op, std::string *body) {
	DWORD avail = 0;
	if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr))
		return -1;
	if (avail < FRAME_HEADER)
		return 0;
	uint8_t header[FRAME_HEADER];
	DWORD   got = 0;
	if (!PeekNamedPipe(g_pipe, header, FRAME_HEADER, &got, &avail, nullptr) || got != FRAME_HEADER)
		return -1;
	uint32_t length = 0;
	if (!ParseHeader(header, op, &length))
		return -1;
	if (avail < FRAME_HEADER + length)
		return 0;
	std::string all(FRAME_HEADER + length, '\0');
	if (!ReadFile(g_pipe, &all[0], static_cast<DWORD>(all.size()), &got, nullptr) ||
	    got != all.size())
		return -1;
	body->assign(all, FRAME_HEADER, length);
	return 1;
}

// Whatever Discord sent since the last pass: a ping is answered, a close ends
// the pipe. False when the pipe is done.
bool Pump(bool *ready) {
	for (;;) {
		uint32_t    op = 0;
		std::string body;
		const int   r = ReadFrame(&op, &body);
		if (r == 0)
			return true;
		if (r < 0)
			return false;
		if (op == OP_PING) {
			if (!Send(OP_PONG, body))
				return false;
		} else if (op == OP_CLOSE) {
			// The usual reason is an application id Discord does not know.
			if (!g_saidRefused) {
				g_saidRefused = true;
				Log("discord: Discord closed the connection (%.200s); trying again every %u s",
				    body.c_str(), RETRY_MS / 1000);
			}
			return false;
		} else if (op == OP_FRAME && ready && IsReady(body)) {
			*ready = true;
		} else if (op == OP_FRAME && IsError(body)) {
			// A command it did not take. Once per connection: the next one
			// would most likely say the same.
			if (!g_saidError) {
				g_saidError = true;
				Log("discord: Discord did not take the activity (%.200s)", body.c_str());
			}
		}
	}
}

bool Connect() {
	g_pipe = OpenPipe();
	if (g_pipe == INVALID_HANDLE_VALUE) {
		if (!g_saidNoDiscord) {
			g_saidNoDiscord = true;
			Log("discord: Discord is not running here; looking again every %u s",
			    RETRY_MS / 1000);
		}
		return false;
	}
	if (!Send(OP_HANDSHAKE, HandshakeJson(g_appId))) {
		Drop();
		return false;
	}
	bool ready = false;
	for (uint32_t waited = 0; waited < HANDSHAKE_WAIT_MS && !g_stop; waited += TICK_MS) {
		if (!Pump(&ready)) {
			Drop();
			return false;
		}
		if (ready)
			break;
		Sleep(TICK_MS);
	}
	if (!ready) {
		Drop();
		return false;
	}
	g_saidNoDiscord = false;
	g_saidRefused   = false;
	g_saidError     = false;
	Log("discord: connected, showing CoopIII on the profile");
	return true;
}

void Run() {
	const uint32_t pid        = GetCurrentProcessId();
	uint32_t       nextTry    = GetTickCount();
	bool           sent       = false;
	uint32_t       lastSentMs = 0;
	Activity       shown;

	while (!g_stop) {
		const uint32_t now = GetTickCount();
		if (g_pipe == INVALID_HANDLE_VALUE) {
			if (static_cast<int32_t>(now - nextTry) >= 0) {
				nextTry = now + RETRY_MS;
				sent    = false;
				Connect();
			}
		} else if (!Pump(nullptr)) {
			Drop();
			nextTry = now + RETRY_MS;
		} else {
			Activity want;
			{
				std::lock_guard<std::mutex> hold(g_lock);
				want = g_want;
			}
			if (ShouldSend(want != shown, sent, now, lastSentMs)) {
				if (Send(OP_FRAME, ActivityJson(pid, want, ++g_nonce))) {
					shown      = want;
					sent       = true;
					lastSentMs = now;
				} else {
					Drop();
					nextTry = now + RETRY_MS;
				}
			}
		}
		Sleep(TICK_MS);
	}

	if (g_pipe != INVALID_HANDLE_VALUE) {
		Send(OP_FRAME, ClearJson(pid, ++g_nonce));
		Drop();
	}
}

} // namespace

void Start(uint64_t appId) {
	if (appId == 0 || g_thread)
		return;
	g_appId = appId;
	g_stop  = false;
	try {
		g_thread = new std::thread(Run);
	} catch (...) {
		Log("discord: could not start the presence thread");
	}
}

void Update(const Activity &activity) {
	std::lock_guard<std::mutex> hold(g_lock);
	g_want = activity;
}

void Stop() {
	if (!g_thread)
		return;
	g_stop = true;
	g_thread->join();
	delete g_thread;
	g_thread = nullptr;
}

} // namespace coopiii::presence
