#include "pch.h"
#include "Updater.h"
#include "Util.h"
#include "Version.h"

namespace {
constexpr const wchar_t* kDefaultBase = L"https://github.com/ejonghyuck/OzMaker/releases/latest/download";
constexpr uint64_t kMaxExe = 64ull << 20;

constexpr uint64_t MakeVersion(uint64_t a, uint64_t b, uint64_t c) { return (a << 40) | (b << 20) | c; }

// Version.h writes the version twice; they must agree.
constexpr bool SameVersion(const char* s, uint64_t v) {
    uint64_t part[3] = {}, n = 0;
    for (; *s; ++s) {
        if (*s == '.') {
            if (++n > 2) return false;
        } else if (*s >= '0' && *s <= '9') {
            part[n] = part[n] * 10 + (*s - '0');
        } else {
            return false;
        }
    }
    return n == 2 && MakeVersion(part[0], part[1], part[2]) == v;
}
static_assert(SameVersion(OZ_VERSION_STR, MakeVersion(OZ_VERSION_MAJOR, OZ_VERSION_MINOR, OZ_VERSION_PATCH)));

std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (char c : s) w += (wchar_t)(unsigned char)c;  // update.txt is ASCII
    return w;
}

bool IsHex64(const std::wstring& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return iswxdigit(c); });
}

bool IsLocal(const std::wstring& base) { return base.find(L"://") == std::wstring::npos; }

std::wstring LastError(const wchar_t* what, DWORD e = GetLastError()) {
    return util::Format(tr(L"%s (오류 %lu)"), what, e);
}
}  // namespace

Updater::~Updater() {
    cancel_ = true;
    {
        std::lock_guard lk(mtx_);
        if (request_) {
            WinHttpCloseHandle(request_);  // a blocked read returns at once
            request_ = nullptr;
        }
    }
    if (thread_.joinable()) thread_.join();
}

void Updater::Init(const std::wstring& exe, HWND notify, UINT msg) {
    exe_ = exe;
    notify_ = notify;
    msg_ = msg;
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(L"OZMAKER_UPDATE_URL", buf, _countof(buf));
    base_ = n > 0 && n < _countof(buf) ? std::wstring(buf, n) : kDefaultBase;
    while (!base_.empty() && (base_.back() == L'/' || base_.back() == L'\\')) base_.pop_back();
}

void Updater::Check() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    Set([](Status& s) {
        if (s.state != State::Ready) s.state = State::Checking;
        s.error.clear();
    });
    thread_ = std::thread([this] {
        Run();
        running_ = false;
    });
}

Updater::Status Updater::Get() const {
    std::lock_guard lk(mtx_);
    return status_;
}

bool Updater::Busy() const { return running_; }

void Updater::Set(const std::function<void(Status&)>& change) {
    {
        std::lock_guard lk(mtx_);
        change(status_);
    }
    if (notify_) PostMessageW(notify_, msg_, 0, 0);
}

uint64_t Updater::ParseVersion(std::wstring_view v) {
    uint64_t part[3] = {};
    int n = 0;
    bool digit = false;
    for (wchar_t c : v) {
        if (c == L'.') {
            if (!digit || ++n > 2) return 0;
            digit = false;
        } else if (c >= L'0' && c <= L'9') {
            part[n] = part[n] * 10 + (c - L'0');
            if (part[n] >= (1u << 20)) return 0;
            digit = true;
        } else {
            return 0;
        }
    }
    if (n != 2 || !digit) return 0;
    return MakeVersion(part[0], part[1], part[2]);
}

uint64_t Updater::CurrentVersion() { return MakeVersion(OZ_VERSION_MAJOR, OZ_VERSION_MINOR, OZ_VERSION_PATCH); }

std::wstring Updater::ReleasePage(const std::wstring& version) {
    return std::wstring(kRepoUrl) + (version.empty() ? L"/releases/latest" : L"/releases/tag/v" + version);
}

namespace {
struct Hasher {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    Hasher() {
        ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
             BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    }
    ~Hasher() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void Add(const void* p, size_t n) {
        ok = ok && BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)p, (ULONG)n, 0));
    }
    std::wstring Hex() {
        uint8_t d[32];
        if (!ok || !BCRYPT_SUCCESS(BCryptFinishHash(hash, d, sizeof(d), 0))) return {};
        std::wstring s;
        for (uint8_t b : d) s += util::Format(L"%02x", b);
        return s;
    }
};
}  // namespace

std::wstring Updater::Sha256(const void* data, size_t n) {
    Hasher h;
    h.Add(data, n);
    return h.Hex();
}

std::wstring Updater::Sha256File(const std::wstring& path, uint64_t* size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    Hasher h;
    std::vector<uint8_t> buf(1 << 16);
    uint64_t total = 0;
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) != FALSE) && got > 0) {
        h.Add(buf.data(), got);
        total += got;
    }
    CloseHandle(f);
    if (size) *size = total;
    return ok ? h.Hex() : std::wstring();
}

bool Updater::Parse(const std::string& text, Info& info) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || line[0] == '#' || eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "version") info.version = Widen(value);
        else if (key == "exe") info.exe = Widen(value);
        else if (key == "sha256") info.sha256 = Widen(value);
        else if (key == "size") info.size = _strtoui64(value.c_str(), nullptr, 10);
    }
    std::transform(info.sha256.begin(), info.sha256.end(), info.sha256.begin(), towlower);
    // The exe name goes into a URL and nothing else, but keep it a plain file name.
    bool plainName = !info.exe.empty() && info.exe.size() < 64 &&
                     info.exe.find_first_of(L"/\\:?#%") == std::wstring::npos && info.exe.find(L"..") == std::wstring::npos;
    return ParseVersion(info.version) != 0 && plainName && IsHex64(info.sha256) && info.size > 0 &&
           info.size <= kMaxExe;
}

bool Updater::Fetch(const std::wstring& name, const std::function<bool(const uint8_t*, size_t, uint64_t)>& sink,
                    std::wstring& err) {
    if (IsLocal(base_)) {
        std::wstring path = base_ + L"\\" + name;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            err = LastError(tr(L"업데이트 정보를 읽지 못했습니다"));
            return false;
        }
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        std::vector<uint8_t> buf(1 << 16);
        DWORD got = 0;
        bool ok = true;
        while (!cancel_ && (ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) != FALSE) && got > 0)
            if (!sink(buf.data(), got, (uint64_t)sz.QuadPart)) break;
        CloseHandle(f);
        if (!ok) err = LastError(tr(L"업데이트 파일을 읽지 못했습니다"));
        return ok && !cancel_;
    }

    std::wstring url = base_ + L"/" + name;
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        err = tr(L"업데이트 주소가 올바르지 않습니다.");
        return false;
    }
    std::wstring agent = L"OzMaker/" OZ_VERSION_WSTR;
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        err = LastError(tr(L"인터넷에 연결하지 못했습니다"));
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET req = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                            : nullptr;
    {
        std::lock_guard lk(mtx_);
        request_ = cancel_ ? nullptr : req;
    }
    bool ok = false;
    DWORD status = 0, len = sizeof(status);
    if (req && !cancel_ && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &len, WINHTTP_NO_HEADER_INDEX)) {
        if (status == 200) {
            uint64_t total = 0;
            wchar_t clen[32] = {};
            DWORD cl = sizeof(clen);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &cl,
                                    WINHTTP_NO_HEADER_INDEX))
                total = _wcstoui64(clen, nullptr, 10);
            std::vector<uint8_t> buf(1 << 16);
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (cancel_ || !WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) {
                    ok = false;
                    break;
                }
                if (got == 0) break;
                if (!sink(buf.data(), got, total)) break;
            }
            if (!ok && !cancel_) err = LastError(tr(L"내려받다가 연결이 끊겼습니다"));
        } else {
            err = status == 404 ? tr(L"업데이트 정보가 없는 릴리스입니다.") : util::Format(tr(L"서버 응답 %lu"), status);
        }
    } else if (!cancel_) {
        err = LastError(tr(L"GitHub에 연결하지 못했습니다"));
    }
    {
        std::lock_guard lk(mtx_);
        if (request_ == req && req) WinHttpCloseHandle(req);
        request_ = nullptr;
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok && !cancel_;
}

void Updater::Run() {
    std::wstring err;
    std::string text;
    bool got = Fetch(L"update.txt", [&](const uint8_t* p, size_t n, uint64_t) {
        text.append((const char*)p, n);
        return text.size() < 4096;
    }, err);
    Info info;
    if (got && !Parse(text, info)) {
        got = false;
        err = tr(L"업데이트 정보를 알아볼 수 없습니다.");
    }
    if (!got) {
        if (cancel_) return;
        Set([&](Status& s) {
            if (s.state == State::Ready) return;  // the download from earlier still stands
            s.state = State::Failed;
            s.error = err;
            s.checkedAt = util::NowSeconds();
        });
        return;
    }
    if (ParseVersion(info.version) <= CurrentVersion()) {
        DeleteFileW((exe_ + L".new").c_str());  // a download this version has caught up with
        Set([&](Status& s) {
            s.state = State::UpToDate;
            s.version = info.version;
            s.checkedAt = util::NowSeconds();
        });
        return;
    }

    // Already downloaded (an earlier check, or an earlier run that wasn't restarted)?
    const std::wstring staged = exe_ + L".new", part = exe_ + L".new.part";
    uint64_t have = 0;
    if (Sha256File(staged, &have) == info.sha256 && have == info.size) {
        {
            std::lock_guard lk(mtx_);
            ready_ = info;
        }
        Set([&](Status& s) {
            s.state = State::Ready;
            s.version = info.version;
            s.checkedAt = util::NowSeconds();
        });
        return;
    }

    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        // Most likely Program Files or another folder that needs administrator rights.
        Set([&](Status& s) {
            s.state = State::Available;
            s.version = info.version;
            s.checkedAt = util::NowSeconds();
        });
        return;
    }
    Set([&](Status& s) {
        s.state = State::Downloading;
        s.version = info.version;
        s.progress = 0;
    });
    Hasher hash;
    uint64_t written = 0;
    bool writeOk = true;
    float shown = 0;
    got = Fetch(info.exe, [&](const uint8_t* p, size_t n, uint64_t) {
        DWORD w = 0;
        if (written + n > info.size || !WriteFile(f, p, (DWORD)n, &w, nullptr) || w != n) {
            writeOk = false;
            return false;
        }
        hash.Add(p, n);
        written += n;
        float pr = (float)written / info.size;
        if (pr - shown >= 0.02f) {
            shown = pr;
            Set([&](Status& s) { s.progress = pr; });
        }
        return true;
    }, err);
    CloseHandle(f);
    std::wstring sum = got && writeOk ? hash.Hex() : std::wstring();
    if (got && writeOk && written == info.size && sum == info.sha256 &&
        MoveFileExW(part.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        {
            std::lock_guard lk(mtx_);
            ready_ = info;
        }
        Set([&](Status& s) {
            s.state = State::Ready;
            s.checkedAt = util::NowSeconds();
        });
        return;
    }
    DeleteFileW(part.c_str());
    if (cancel_) return;
    if (got && writeOk) err = tr(L"받은 파일이 손상되었습니다(크기 또는 SHA-256이 다름).");
    else if (!writeOk) err = tr(L"받은 파일을 저장하지 못했습니다.");
    Set([&](Status& s) {
        s.state = State::Failed;
        s.error = err;
        s.checkedAt = util::NowSeconds();
    });
}

bool Updater::Install(std::wstring& err) {
    Info info;
    {
        std::lock_guard lk(mtx_);
        if (status_.state != State::Ready || running_) {
            err = tr(L"업데이트가 아직 준비되지 않았습니다.");
            return false;
        }
        info = ready_;
    }
    const std::wstring staged = exe_ + L".new", old = exe_ + L".old";
    uint64_t size = 0;
    if (Sha256File(staged, &size) != info.sha256 || size != info.size) {
        err = tr(L"받아 둔 업데이트 파일이 바뀌었습니다. 다시 확인합니다.");
        DeleteFileW(staged.c_str());
        Set([](Status& s) { s.state = State::Idle; });
        return false;
    }
    // A running exe can't be overwritten or deleted, but it can be renamed.
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe_.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = LastError(tr(L"지금 실행 파일의 이름을 바꾸지 못했습니다"));
        return false;
    }
    if (!MoveFileExW(staged.c_str(), exe_.c_str(), 0)) {
        DWORD e = GetLastError();
        MoveFileExW(old.c_str(), exe_.c_str(), 0);
        err = LastError(tr(L"새 실행 파일을 넣지 못했습니다"), e);
        return false;
    }
    Set([](Status& s) { s.state = State::Idle; });
    return true;
}

void Updater::CleanUp(const std::wstring& exe) {
    std::wstring old = exe + L".old";
    // The old instance may take a moment to let go of its file.
    for (int i = 0; i < 20 && GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        if (DeleteFileW(old.c_str())) break;
        Sleep(250);
    }
    DeleteFileW((exe + L".new.part").c_str());
}
