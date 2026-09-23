// Doing the install: download, check the hash, put the files where they go.
//
// On a thread of its own, because it is file IO and network and neither
// belongs on a frame loop. The window reads a snapshot under a lock and draws
// whatever it finds; nothing it draws can block.
#include "installer/core.h"

#include "launcher/core.h"

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

namespace coopiii::installer {
namespace {


std::wstring Widen(const std::string &utf8) {
	if (utf8.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::wstring out(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
	return out;
}

bool EnsureDir(const std::string &path) {
	if (path.empty() || launcher::DirExists(path))
		return true;
	const size_t slash = path.find_last_of("\\/");
	if (slash != std::string::npos && slash > 2 && !EnsureDir(path.substr(0, slash)))
		return false;
	return CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// One GET, straight into memory. The files in the Essential Pack are a few
// hundred kilobytes each; nothing here needs streaming to disk.
bool Download(const std::string &url, std::vector<uint8_t> *out, std::string *error) {
	auto fail = [&](const char *why) {
		if (error)
			*error = why;
		return false;
	};

	URL_COMPONENTS parts = {sizeof(parts)};
	wchar_t        host[256] = {0}, path[2048] = {0};
	parts.lpszHostName     = host;
	parts.dwHostNameLength = 255;
	parts.lpszUrlPath      = path;
	parts.dwUrlPathLength  = 2047;

	const std::wstring wide = Widen(url);
	if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
		return fail("that download address is not a URL");
	if (parts.nScheme != INTERNET_SCHEME_HTTPS)
		return fail("downloads have to be https");

	HINTERNET session = WinHttpOpen(L"CoopIII Setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return fail("could not start a connection");

	bool ok = false;
	if (HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0)) {
		HINTERNET request =
		    WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (request) {
			// Follow the redirect a release download always is.
			DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
			WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

			if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
			                       0, 0, 0) &&
			    WinHttpReceiveResponse(request, nullptr)) {
				DWORD status = 0, size = sizeof(status);
				WinHttpQueryHeaders(request,
				                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
				                    WINHTTP_NO_HEADER_INDEX);
				if (status == 200) {
					out->clear();
					DWORD available = 0;
					do {
						available = 0;
						if (!WinHttpQueryDataAvailable(request, &available))
							break;
						if (available == 0)
							break;
						const size_t at = out->size();
						out->resize(at + available);
						DWORD read = 0;
						if (!WinHttpReadData(request, out->data() + at, available, &read))
							break;
						out->resize(at + read);
					} while (available > 0);
					ok = !out->empty();
					if (!ok && error)
						*error = "the download was empty";
				} else if (error) {
					char buf[64];
					std::snprintf(buf, sizeof(buf), "the server answered %lu", status);
					*error = buf;
				}
			} else if (error) {
				*error = "the request failed";
			}
			WinHttpCloseHandle(request);
		} else if (error) {
			*error = "could not open the request";
		}
		WinHttpCloseHandle(connection);
	} else if (error) {
		*error = "could not reach that host";
	}

	WinHttpCloseHandle(session);
	return ok;
}

bool WriteBytes(const std::string &path, const std::vector<uint8_t> &bytes) {
	FILE *fh = std::fopen(path.c_str(), "wb");
	if (!fh)
		return false;
	const size_t put = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), fh);
	std::fclose(fh);
	return put == bytes.size();
}

// The name a download should be saved as: the last path segment of its URL.
std::string LeafOf(const std::string &url) {
	const size_t slash = url.find_last_of('/');
	std::string  leaf  = slash == std::string::npos ? url : url.substr(slash + 1);
	const size_t query = leaf.find('?');
	if (query != std::string::npos)
		leaf.resize(query);
	return leaf.empty() ? std::string("download.bin") : leaf;
}

PayloadReader g_payload;

// The Setup's own copy of a file, or the one beside it.
bool ReadPayload(const std::string &name, std::vector<uint8_t> *out) {
	if (g_payload && g_payload(name, out))
		return true;

	const std::string beside = launcher::Join(SetupDir(), name.c_str());
	if (!launcher::FileExists(beside))
		return false;
	FILE *fh = std::fopen(beside.c_str(), "rb");
	if (!fh)
		return false;
	std::fseek(fh, 0, SEEK_END);
	const long size = std::ftell(fh);
	std::fseek(fh, 0, SEEK_SET);
	out->resize(size > 0 ? static_cast<size_t>(size) : 0);
	const size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), fh);
	std::fclose(fh);
	return got == out->size() && !out->empty();
}

} // namespace

void SetPayloadReader(PayloadReader reader) { g_payload = std::move(reader); }

struct InstallJob::Impl {
	mutable std::mutex mutex;
	Progress           progress;
	std::thread        worker;
	std::atomic<bool>  cancel{false};

	void Detail(const char *prefix, const std::string &text) {
		std::lock_guard<std::mutex> lock(mutex);
		progress.details.push_back("[ " + std::string(prefix) + " ] " + text);
	}

	void SetStep(size_t index, StepState state, const std::string &note) {
		std::lock_guard<std::mutex> lock(mutex);
		if (index < progress.steps.size()) {
			progress.steps[index].state = state;
			progress.steps[index].note  = note;
		}
	}

	void Run(std::string gameDir, std::vector<Component> chosen);
};

void InstallJob::Impl::Run(std::string gameDir, std::vector<Component> chosen) {
	for (size_t i = 0; i < chosen.size(); ++i) {
		if (cancel.load()) {
			SetStep(i, StepState::Skipped, "Cancelled");
			continue;
		}

		const Component &c = chosen[i];
		SetStep(i, StepState::Working, "Installing");

		std::string error;
		bool        ok = false;

		if (c.source == "local") {
			// CoopIII itself: the files the Setup carries.
			Detail("..", c.name + ": writing files into the game folder");
			const std::string to = c.destination.empty()
			                           ? gameDir
			                           : launcher::Join(gameDir, c.destination.c_str());
			ok = EnsureDir(to);
			for (const std::string &file : c.files) {
				if (!ok)
					break;
				std::vector<uint8_t> bytes;
				if (!ReadPayload(file, &bytes)) {
					error = file + " is not in this Setup";
					ok    = false;
					break;
				}
				if (!WriteBytes(launcher::Join(to, file.c_str()), bytes)) {
					error = "could not write " + file + " into the game folder. Is GTA III "
					        "still running?";
					ok    = false;
				}
			}
		} else if (c.url.empty() || c.sha256.empty()) {
			// A component nobody has pointed at a download yet. Say so rather
			// than reporting an install that did not happen.
			error = "no download is set for this component yet";
		} else {
			Detail("..", c.name + ": downloading");
			std::vector<uint8_t> bytes;
			if (!Download(c.url, &bytes, &error)) {
				// Download filled in why.
			} else if (Sha256(bytes.data(), bytes.size()) != c.sha256) {
				error = "the download does not match the hash the manifest expects";
			} else {
				const std::string to = c.destination.empty()
				                           ? gameDir
				                           : launcher::Join(gameDir, c.destination.c_str());
				ok = EnsureDir(to) && WriteBytes(launcher::Join(to, LeafOf(c.url).c_str()), bytes);
				if (!ok)
					error = "could not write it into the game folder";
			}
		}

		if (ok) {
			SetStep(i, StepState::Done, "Done");
			Detail("ok", c.name + " installed");
			std::lock_guard<std::mutex> lock(mutex);
			++progress.finished;
		} else {
			SetStep(i, StepState::Failed, error.empty() ? "Failed" : error);
			Detail("!!", c.name + ": " + (error.empty() ? "failed" : error));
			std::lock_guard<std::mutex> lock(mutex);
			progress.failed = true;
		}
	}

	std::lock_guard<std::mutex> lock(mutex);
	progress.running = false;
}

InstallJob::InstallJob() : m_impl(new Impl()) {}

InstallJob::~InstallJob() {
	Cancel();
	if (m_impl->worker.joinable())
		m_impl->worker.join();
	delete m_impl;
}

void InstallJob::Start(const std::string &gameDir, const std::vector<std::string> &chosen) {
	if (Running())
		return;
	if (m_impl->worker.joinable())
		m_impl->worker.join();

	std::vector<Component> components;
	for (const std::string &id : chosen)
		if (const Component *c = BuiltInManifest().Find(id))
			components.push_back(*c);

	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		m_impl->progress = Progress{};
		m_impl->progress.total   = static_cast<int>(components.size());
		m_impl->progress.running = true;
		for (const Component &c : components)
			m_impl->progress.steps.push_back({c.id, c.name, StepState::Waiting, "Waiting"});
	}
	m_impl->cancel.store(false);

	Impl *impl     = m_impl;
	m_impl->worker = std::thread([impl, gameDir, components] {
		impl->Run(gameDir, components);
	});
}

void InstallJob::Cancel() { m_impl->cancel.store(true); }

bool InstallJob::Running() const {
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	return m_impl->progress.running;
}

Progress InstallJob::Snapshot() const {
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	Progress copy     = m_impl->progress;
	copy.cancelled    = m_impl->cancel.load();
	return copy;
}

// ---- odds and ends --------------------------------------------------------

std::string SetupDir() { return launcher::ExeDir(); }

bool CreateDesktopShortcut(const std::string &target, const std::string &name,
                           const std::string &workingDir) {
	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
		return false;

	bool ok = false;
	IShellLinkW *link = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
	                               IID_PPV_ARGS(&link))) &&
	    link) {
		link->SetPath(Widen(target).c_str());
		link->SetWorkingDirectory(Widen(workingDir).c_str());
		link->SetDescription(L"Start GTA III with CoopIII");

		IPersistFile *file = nullptr;
		if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
			PWSTR desktop = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) &&
			    desktop) {
				const std::wstring path = std::wstring(desktop) + L"\\" + Widen(name) + L".lnk";
				ok                      = SUCCEEDED(file->Save(path.c_str(), TRUE));
				CoTaskMemFree(desktop);
			}
			file->Release();
		}
		link->Release();
	}

	CoUninitialize();
	return ok;
}

} // namespace coopiii::installer
