// Downloading: WinHTTP, https only, straight to disk.
//
// Three things make it survive a bad connection. A download is written to
// <name>.part and only renamed once it is whole, so a half file never looks
// like a finished one. A .part left by an earlier attempt is continued with a
// Range request rather than started again. And each attempt that fails is
// retried a few times, backing off, before the next source is tried.
//
// None of that is trusted: FetchVerified keeps a file only if its SHA-256 is
// the one the manifest carries, whichever source it came from.
#include "installer/core.h"

#include "launcher/core.h"

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>

#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace coopiii::installer {
namespace {

constexpr int      kAttempts = 4;
constexpr uint64_t kMaxBytes = 256ull << 20;   // nothing in the manifest is near this

std::wstring Widen(const std::string &utf8) {
	if (utf8.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::wstring out(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
	return out;
}

uint64_t SizeOf(const std::string &path) {
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d))
		return 0;
	return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

struct Handle {
	HINTERNET h = nullptr;
	explicit Handle(HINTERNET handle) : h(handle) {}
	~Handle() {
		if (h)
			WinHttpCloseHandle(h);
	}
	operator HINTERNET() const { return h; }
};

enum class Outcome { Done, Retry, GiveUp };

// One request. Appends to `part` when the server honours the Range.
Outcome Attempt(const std::string &url, const std::string &part, const ByteProgress &progress,
                const std::function<bool()> &cancelled, std::string *error) {
	URL_COMPONENTS parts = {sizeof(parts)};
	wchar_t        host[256] = {0}, path[2048] = {0};
	parts.lpszHostName     = host;
	parts.dwHostNameLength = 255;
	parts.lpszUrlPath      = path;
	parts.dwUrlPathLength  = 2047;

	const std::wstring wide = Widen(url);
	if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
		*error = "that download address is not a URL";
		return Outcome::GiveUp;
	}
	if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
		*error = "downloads have to be https";
		return Outcome::GiveUp;
	}

	Handle session(WinHttpOpen(L"CoopIII-Setup/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session) {
		// Windows before 8.1 has no automatic proxy; the default one is next best.
		session.h = WinHttpOpen(L"CoopIII-Setup/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	}
	if (!session) {
		*error = "could not start a connection";
		return Outcome::Retry;
	}
	WinHttpSetTimeouts(session, 15000, 15000, 20000, 30000);

	Handle connection(WinHttpConnect(session, host, parts.nPort, 0));
	if (!connection) {
		*error = "could not reach that host";
		return Outcome::Retry;
	}
	Handle request(WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
	                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!request) {
		*error = "could not open the request";
		return Outcome::Retry;
	}
	// A release download is always a redirect; never let one drop to http.
	DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

	const uint64_t have = SizeOf(part);
	std::wstring   range;
	if (have > 0)
		range = L"Range: bytes=" + std::to_wstring(have) + L"-";

	if (!WinHttpSendRequest(request, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
	                        range.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0,
	                        0, 0) ||
	    !WinHttpReceiveResponse(request, nullptr)) {
		*error = "the connection failed (error " + std::to_string(GetLastError()) + ")";
		return Outcome::Retry;
	}

	DWORD status = 0, size = sizeof(status);
	WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

	bool append = false;
	if (status == 206 && have > 0) {
		append = true;
	} else if (status == 416 && have > 0) {
		// Nothing left to send: the .part is already whole.
		return Outcome::Done;
	} else if (status != 200) {
		*error = "the server answered " + std::to_string(status);
		// 404 and friends will not get better by asking again.
		return (status >= 500 || status == 429 || status == 408) ? Outcome::Retry : Outcome::GiveUp;
	}

	uint64_t total = 0;
	wchar_t  length[32] = {0};
	DWORD    lengthSize = sizeof(length);
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
	                        length, &lengthSize, WINHTTP_NO_HEADER_INDEX))
		total = _wcstoui64(length, nullptr, 10) + (append ? have : 0);
	if (total > kMaxBytes) {
		*error = "the download is far larger than expected";
		return Outcome::GiveUp;
	}

	FILE *fh = std::fopen(part.c_str(), append ? "ab" : "wb");
	if (!fh) {
		*error = "could not write the download to " + part;
		return Outcome::GiveUp;
	}
	uint64_t done = append ? have : 0;
	if (progress)
		progress(done, total);

	std::vector<uint8_t> buffer(64 * 1024);
	Outcome              outcome = Outcome::Done;
	for (;;) {
		if (cancelled && cancelled()) {
			*error  = "cancelled";
			outcome = Outcome::GiveUp;
			break;
		}
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available)) {
			*error  = "the connection dropped";
			outcome = Outcome::Retry;
			break;
		}
		if (available == 0)
			break;
		DWORD read = 0;
		if (!WinHttpReadData(request, buffer.data(),
		                     available < buffer.size() ? available : static_cast<DWORD>(buffer.size()),
		                     &read)) {
			*error  = "the connection dropped";
			outcome = Outcome::Retry;
			break;
		}
		if (read == 0)
			break;
		if (std::fwrite(buffer.data(), 1, read, fh) != read) {
			*error  = "could not write the download. Is the disk full?";
			outcome = Outcome::GiveUp;
			break;
		}
		done += read;
		if (done > kMaxBytes) {
			*error  = "the download is far larger than expected";
			outcome = Outcome::GiveUp;
			break;
		}
		if (progress)
			progress(done, total);
	}
	std::fclose(fh);

	if (outcome == Outcome::Done && total > 0 && done != total) {
		*error  = "the download ended early";
		outcome = Outcome::Retry;
	}
	return outcome;
}

} // namespace

bool DownloadFile(const std::string &url, const std::string &dest, const ByteProgress &progress,
                  const std::function<bool()> &cancelled, std::string *error) {
	const std::string part = dest + ".part";
	std::string       why;
	for (int attempt = 0; attempt < kAttempts; ++attempt) {
		if (attempt > 0) {
			// 1, 2, 4 seconds, in slices so a cancel is not kept waiting.
			for (int slice = 0; slice < (1 << (attempt - 1)) * 10; ++slice) {
				if (cancelled && cancelled())
					break;
				Sleep(100);
			}
		}
		if (cancelled && cancelled()) {
			why = "cancelled";
			break;
		}
		const Outcome outcome = Attempt(url, part, progress, cancelled, &why);
		if (outcome == Outcome::Done) {
			if (!MoveFileExA(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
				why = "could not move the finished download into place";
				break;
			}
			return true;
		}
		if (outcome == Outcome::GiveUp)
			break;
	}
	if (error)
		*error = why;
	return false;
}

bool FetchVerified(const std::vector<std::string> &sources, const std::string &sha256,
                   const std::string &dest, const ByteProgress &progress,
                   const std::function<bool()> &cancelled,
                   const std::function<void(const std::string &)> &log, std::string *error) {
	if (launcher::FileExists(dest)) {
		if (Sha256File(dest) == sha256)
			return true;
		DeleteFileA(dest.c_str());
	}
	if (sources.empty()) {
		if (error)
			*error = "no download is set for this yet";
		return false;
	}

	std::string last;
	for (const std::string &source : sources) {
		// Twice per source: the second time from scratch, in case what went
		// wrong was a stale .part that the resume stitched onto.
		for (int fresh = 0; fresh < 2; ++fresh) {
			if (cancelled && cancelled()) {
				if (error)
					*error = "cancelled";
				return false;
			}
			if (fresh)
				DeleteFileA((dest + ".part").c_str());
			std::string why;
			if (!DownloadFile(source, dest, progress, cancelled, &why)) {
				last = why;
				if (log)
					log(source + ": " + why);
				break;   // the download itself failed; the next source, not a retry
			}
			if (Sha256File(dest) == sha256)
				return true;
			DeleteFileA(dest.c_str());
			last = "the download does not match the SHA-256 the Setup expects";
			if (log)
				log(source + ": " + last);
		}
	}
	if (error)
		*error = last.empty() ? "the download failed" : last;
	return false;
}

std::string DefaultCacheDir() {
	char base[MAX_PATH] = {0};
	if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base)))
		GetTempPathA(MAX_PATH, base);
	return launcher::Join(launcher::Join(launcher::Join(base, "CoopIII"), "Setup"), "downloads");
}

} // namespace coopiii::installer
