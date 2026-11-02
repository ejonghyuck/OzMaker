#pragma once

// New versions from GitHub releases (#31). Each release carries update.txt next to its OzMaker.exe:
//   version=0.4.2
//   exe=OzMaker.exe
//   size=5212672
//   sha256=<64 hex digits>
// Check() reads releases/latest/download/update.txt and, when it names a newer version, downloads that exe next to
// ours as OzMaker.exe.new and checks its size and SHA-256. Install() then swaps it in; the caller restarts.
// OZMAKER_UPDATE_URL points tests at another base: a URL, or a local folder holding the same files.
class Updater {
public:
    enum class State {
        Idle,
        Checking,
        UpToDate,
        Downloading,
        Ready,      // downloaded and verified: Install() can swap it in
        Available,  // newer, but our folder can't be written (Program Files): the release page has it
        Failed,
    };
    struct Status {
        State state = State::Idle;
        std::wstring version;  // the newest release, once known
        float progress = 0;    // Downloading
        std::wstring error;    // Failed
        double checkedAt = 0;  // util::NowSeconds() of the last finished check, 0 = never
    };

    ~Updater();
    // exe: what Install() replaces (the running exe; tests use a copy).
    void Init(const std::wstring& exe, HWND notify, UINT msg);
    // Starts a check unless one is running. notify gets msg posted whenever the status changes.
    void Check();
    Status Get() const;
    bool Busy() const;
    void SetForTest(const Status& s) {
        std::lock_guard lk(mtx_);
        status_ = s;
    }
    // Renames the exe to .old and puts the downloaded one in its place. On failure nothing has changed.
    bool Install(std::wstring& err);
    // After an update: removes the .old (and any half-finished download) left next to exe.
    static void CleanUp(const std::wstring& exe);

    // "1.2.3" → one comparable number; 0 if it isn't a version.
    static uint64_t ParseVersion(std::wstring_view v);
    static uint64_t CurrentVersion();
    static std::wstring Sha256File(const std::wstring& path, uint64_t* size = nullptr);  // lowercase hex, "" = error
    static std::wstring Sha256(const void* data, size_t n);
    static std::wstring ReleasePage(const std::wstring& version);  // where a person downloads it by hand

private:
    struct Info {
        std::wstring version, exe, sha256;
        uint64_t size = 0;
    };
    void Run();
    bool Fetch(const std::wstring& name, const std::function<bool(const uint8_t*, size_t, uint64_t total)>& sink,
               std::wstring& err);
    static bool Parse(const std::string& text, Info& info);
    void Set(const std::function<void(Status&)>& change);

    std::wstring exe_, base_;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    mutable std::mutex mtx_;
    Status status_;
    Info ready_;  // what the staged download is (State::Ready)
    std::thread thread_;
    std::atomic<bool> running_{false}, cancel_{false};
    HINTERNET request_ = nullptr;  // under mtx_: closed by the destructor to abort a stuck download
};
