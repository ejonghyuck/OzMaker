#include "pch.h"
#include "Clip.h"

size_t Clip::IndexAt(int64_t t) const {
    size_t n = Count();
    if (n == 0) return 0;
    size_t lo = 0, hi = n;  // first frame with Ts > t
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (Ts(mid) <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo == 0 ? 0 : lo - 1;
}

double Clip::AverageFps() const {
    size_t n = Count();
    if (n < 2 || EndTs() <= 0) return 0;
    return n / (EndTs() / 1e7);
}

// ---------------------------------------------------------------- FrameFetcher

void FrameFetcher::Start(std::shared_ptr<Clip> clip, HWND notify, UINT msg) {
    Stop();
    clip_ = std::move(clip);
    notify_ = notify;
    msg_ = msg;
    want_ = done_ = readyIndex_ = SIZE_MAX;
    ready_ = stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void FrameFetcher::Stop() {
    {
        std::lock_guard lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    clip_.reset();
}

void FrameFetcher::Want(size_t index) {
    {
        std::lock_guard lk(mtx_);
        if (want_ == index) return;
        want_ = index;
    }
    cv_.notify_all();
}

bool FrameFetcher::Take(std::vector<uint8_t>& px, size_t& index) {
    std::lock_guard lk(mtx_);
    if (!ready_) return false;
    px.swap(readyPx_);
    index = readyIndex_;
    ready_ = false;
    return true;
}

void FrameFetcher::Run() {
    auto reader = clip_->NewReader();
    const size_t bytes = (size_t)clip_->Width() * clip_->Height() * 4;
    std::vector<uint8_t> back;
    for (;;) {
        size_t target;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [&] { return stop_ || (want_ != SIZE_MAX && want_ != done_); });
            if (stop_) return;
            target = want_;
        }
        const uint8_t* px = reader->Get(target);
        if (px) {
            back.resize(bytes);
            memcpy(back.data(), px, bytes);
        }
        {
            std::lock_guard lk(mtx_);
            done_ = target;
            if (!px) continue;
            back.swap(readyPx_);
            readyIndex_ = target;
            ready_ = true;
        }
        PostMessageW(notify_, msg_, 0, 0);
    }
}
