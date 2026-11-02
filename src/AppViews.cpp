#include "pch.h"
#include "App.h"
#include "Util.h"
#include "Version.h"

using namespace theme;

namespace {

TextStyle Bold(uint8_t h = AlignStart) {
    TextStyle s;
    s.weight = kSemibold;
    s.h = h;
    return s;
}

TextStyle Align(uint8_t h, uint8_t v = AlignCenter) {
    TextStyle s;
    s.h = h;
    s.v = v;
    return s;
}

// The saved-video card along the bottom of the capture preview.
Rect VideoCardRect(Rect preview) { return Rect(preview.l + 12, preview.b - 12 - 64, preview.r - 12, preview.b - 12); }

const wchar_t* FormatBlurb(Format f) {
    switch (f) {
        case Format::Gif: return L"어디서나 재생 · 256색";
        case Format::WebP: return L"GIF보다 훨씬 작은 용량 · 대부분의 브라우저/메신저";
        case Format::Avif: return L"최고 압축률 · 최신 브라우저";
        default: return L"";
    }
}

}  // namespace

// ---------------------------------------------------------------- frame

void App::DrawFrame(Rect client) {
    ui_.Fill(client, Bg);
    // The files list takes all input while it is open; a click anywhere else closes it.
    if (screen_ == Screen::Settings || rec_ != RecState::Idle) filesOpen_ = false;
    if (rec_ != RecState::Idle) updateOpen_ = false;
    const bool popover = filesOpen_ || updateOpen_;
    if (popover) ui_.SetInputBlocked(true);
    Rect r = client;
    DrawHeader(r.CutTop(60));
    r = r.Inset(20, 0);
    r.b -= 20;
    switch (screen_) {
        case Screen::Home: DrawHome(r); break;
        case Screen::Editor: DrawEditor(r); break;
        case Screen::Settings: DrawSettings(r); break;
    }
    // Floating surfaces last, each in a float: that is what keeps them above the page and the live window preview
    // (a DWM thumbnail, which DWM draws above everything here) out from under them. See Ui::BeginFloat.
    if (popover) {
        ui_.SetInputBlocked(false);
        ui_.BeginFloat();
        if (filesOpen_)
            DrawFilesPopover(client);
        else
            DrawUpdatePopover(client);
        ui_.EndFloat();
    }
    ui_.BeginFloat();
    DrawToast(client);
    ui_.EndFloat();
}

void App::DrawHeader(Rect r) {
    Rect h = r.Inset(20, 0);
    ui_.Logo(Rect::XYWH(h.l, h.CY() - 14, 28, 28));
    float tw = ui_.Measure(L"OzMaker", 17.f, kBold);
    TextStyle title;
    title.weight = kBold;
    ui_.Text(Rect(h.l + 40, h.t, h.l + 40 + tw + 4, h.b), L"OzMaker", 17.f, TextColor, title);
    ui_.Text(Rect(h.l + 48 + tw, h.t + 2, h.l + 200 + tw, h.b), L"움짤 메이커", 12.5f, TextFaint);

    Rect right = h;
    if (screen_ != Screen::Settings) {
        Rect b = right.CutRight(36);
        if (ui_.IconButton(HashId("hdr.settings"), Rect::Centered(b.CX(), b.CY(), 36, 36), icon::Settings, L"설정"))
            OpenSettings();
        right.CutRight(8);
    } else {
        Rect b = right.CutRight(110);
        if (ui_.Button(HashId("hdr.back"), Rect::Centered(b.CX(), b.CY(), 110, 36), L"돌아가기", BtnStyle::Ghost,
                       icon::Back) ||
            ui_.In().Key(VK_ESCAPE)) {
            screen_ = settingsReturn_ == Screen::Editor && clip_ ? Screen::Editor : Screen::Home;
            SetCapturingHotkey(0);
            if (screen_ == Screen::Home) RefreshWindows();
        }
    }
    DrawUpdateButton(right);
    if (screen_ == Screen::Editor) {
        Rect b = right.CutRight(120);
        bool en = !exporter_.Busy();
        if (ui_.Button(HashId("hdr.new"), Rect::Centered(b.CX(), b.CY(), 120, 36), L"새 녹화", BtnStyle::Ghost,
                       icon::Back, en))
            CloseEditor();
        right.CutRight(8);
        DrawFilesButton(right);
    } else if (screen_ == Screen::Home) {
        if (clip_) {
            Rect b = right.CutRight(150);
            if (ui_.Button(HashId("hdr.resume"), Rect::Centered(b.CX(), b.CY(), 150, 36), L"이전 녹화 편집",
                           BtnStyle::Secondary, icon::Next))
                ResumeEditor();
            right.CutRight(8);
        }
        DrawFilesButton(right);
    }
    if (rec_ == RecState::Recording) {
        std::wstring t = (recPaused_ ? L"일시정지  " : L"REC  ") + util::FormatClock(capture_.Elapsed(), true);
        const D2D1_COLOR_F tint = recPaused_ ? Warning : Rec;
        Rect pill = Rect::Centered(r.CX(), r.CY(), recPaused_ ? 148.f : 120.f, 30);
        ui_.Fill(pill, WithAlpha(tint, 0.16f), 15);
        if (recPaused_) {
            ui_.Fill(Rect::XYWH(pill.l + 12, pill.CY() - 5, 3, 10), tint, 1);
            ui_.Fill(Rect::XYWH(pill.l + 17, pill.CY() - 5, 3, 10), tint, 1);
        } else {
            ui_.Circle(pill.l + 16, pill.CY(), 4.5f, Rec);
        }
        TextStyle st = Bold();
        st.tabular = true;
        ui_.Text(Rect(pill.l + 28, pill.t, pill.r, pill.b), t, 12.5f, recPaused_ ? Warning : RecHi, st);
    }
    ui_.Line(r.l, r.b - 0.5f, r.r, r.b - 0.5f, Border, 1.f);
}

void App::DrawToast(Rect client) {
    double now = ui_.Now();
    bool visible = !toast_.empty() && now < toastUntil_;
    float a = ui_.Anim(HashId("toast"), visible ? 1.f : 0.f, 12.f);
    if (visible) RequestWake(toastUntil_);
    if (a <= 0.01f) return;
    // One line when it fits, else wrapped onto as many as it needs (error messages can be long).
    const float maxW = std::min(600.f, client.W() - 80), lineW = ui_.Measure(toast_, 13.f);
    float tw = std::min(maxW, lineW + 60), th = 40;
    TextStyle st;
    if (lineW + 60 > maxW) {
        st.wrap = true;
        st.v = AlignStart;
        DWRITE_TEXT_METRICS m{};
        auto lay = Fonts::Layout(toast_, 13.f, st, tw - 58, 1000);
        if (lay && SUCCEEDED(lay->GetMetrics(&m))) th = std::max(40.f, std::ceil(m.height) + 22);
    }
    Rect r = Rect::Centered(client.CX(), client.b - 24 - th / 2 + (1 - a) * 16, tw, th);
    float radius = std::min(20.f, th / 2);
    ui_.Fill(r.Offset(0, 3), WithAlpha(Black, 0.35f * a), radius);
    ui_.Fill(r, WithAlpha(toastError_ ? Hex(0x3A1A20) : Hex(0x222733), a), radius);
    ui_.Stroke(r.Inset(0.5f), WithAlpha(toastError_ ? Rec : BorderStrong, 0.7f * a), radius);
    ui_.Icon(Rect::XYWH(r.l + 12, r.t, 24, 40), toastError_ ? icon::Warning : icon::Check, 14.f,
             WithAlpha(toastError_ ? RecHi : Success, a));
    ui_.Text(st.wrap ? Rect(r.l + 42, r.t + 11, r.r - 16, r.b - 11) : Rect(r.l + 42, r.t, r.r - 16, r.b), toast_, 13.f,
             WithAlpha(TextColor, a), st);
}

// ---------------------------------------------------------------- files

void App::DrawFilesButton(Rect& right) {
    Rect b = right.CutRight(104);
    filesBtn_ = Rect::Centered(b.CX(), b.CY(), 104, 36);
    bool en = rec_ == RecState::Idle && !exporter_.Busy();
    if (ui_.Button(HashId("hdr.files"), filesBtn_, L"파일", filesOpen_ ? BtnStyle::Secondary : BtnStyle::Ghost,
                   icon::OpenFile, en)) {
        filesOpen_ = true;
        updateOpen_ = false;
        filesScroll_ = 0;
        RefreshRecent();
    }
    // Recordings that would be lost on exit.
    if (!held_.empty()) ui_.Circle(filesBtn_.r - 12, filesBtn_.t + 9, 4.f, Accent2);
}

// A green round button, shown once a new version is downloaded and ready (or out, when it can't be put in place
// here). It opens a small popover to restart into it.
void App::DrawUpdateButton(Rect& right) {
    Updater::Status st = updater_.Get();
    bool ready = st.state == Updater::State::Ready, available = st.state == Updater::State::Available;
    if (!ready && !available) return;
    Rect b = right.CutRight(36);
    right.CutRight(8);
    updateBtn_ = Rect::Centered(b.CX(), b.CY(), 36, 36);
    UiId id = HashId("hdr.update");
    bool hot = ui_.Hover(updateBtn_);
    if (ui_.Click(id, updateBtn_)) {
        updateOpen_ = !updateOpen_;
        filesOpen_ = false;
    }
    float h = ui_.Anim(id, hot || updateOpen_ ? 1.f : 0.f);
    ui_.Circle(updateBtn_.CX(), updateBtn_.CY(), 13.f + h, Mix(Success, Hex(0x6EE7B7), h * 0.6f));
    // A bold download arrow over a base line (the MDL2 glyph is too thin at this size).
    const float x = updateBtn_.CX(), y = updateBtn_.CY();
    const D2D1_COLOR_F ink = Hex(0x06281C);
    ui_.Line(x, y - 6.5f, x, y + 3.f, ink, 2.f);
    ui_.Line(x - 4.5f, y - 1.f, x + 0.7f, y + 4.2f, ink, 2.f);
    ui_.Line(x + 4.5f, y - 1.f, x - 0.7f, y + 4.2f, ink, 2.f);
    ui_.Line(x - 5.5f, y + 7.f, x + 5.5f, y + 7.f, ink, 2.f);
    ui_.Tooltip(id, updateBtn_, ready ? L"업데이트 준비 완료!" : L"새 버전이 나왔습니다");
}

void App::DrawUpdatePopover(Rect client) {
    const auto& in = ui_.In();
    Updater::Status st = updater_.Get();
    const bool ready = st.state == Updater::State::Ready;
    const bool busy = rec_ != RecState::Idle || exporter_.Busy();
    const float W = std::min(320.f, client.W() - 40), H = ready && !busy ? 120.f : 142.f;
    Rect box = Rect::XYWH(std::max(client.l + 20, updateBtn_.r - W), updateBtn_.b + 8, W, H);
    if ((in.pressed && !box.Contains(in.mx, in.my)) || in.Key(VK_ESCAPE)) {
        updateOpen_ = false;
        return;
    }
    ui_.Fill(box.Offset(0, 6), WithAlpha(Black, 0.45f), 14);
    ui_.Fill(box, Surface2, 12);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 12);
    Rect c = box.Inset(16, 14);
    Rect title = c.CutTop(24);
    ui_.Icon(Rect::XYWH(title.l, title.t, 18, title.H()), ready ? icon::Check : icon::Download, 13.f, Success);
    ui_.Text(Rect(title.l + 26, title.t, title.r, title.b),
             ready ? L"새 버전 " + st.version + L" 준비 완료" : L"새 버전이 나왔습니다: " + st.version, 14.f, TextColor,
             Bold());
    Rect sub = c.CutTop(22);
    std::wstring now = L"지금 쓰는 버전 " OZ_VERSION_WSTR L" · ";
    float nw = ui_.Measure(now, 12.f);
    ui_.Text(sub, now, 12.f, TextDim);
    Rect link = Rect(sub.l + nw, sub.t, sub.l + nw + ui_.Measure(L"바뀐 점 보기", 12.f) + 4, sub.b);
    bool lh = ui_.Hover(link);
    if (ui_.Click(HashId("upd.notes"), link)) util::ShellOpen(Updater::ReleasePage(st.version));
    ui_.Text(link, L"바뀐 점 보기", 12.f, lh ? AccentHi : Accent);
    c.CutTop(6);
    if (ready && busy) {
        ui_.Text(c.CutTop(22), L"녹화 · 내보내기가 끝나면 업데이트할 수 있습니다", 12.f, Warning);
    } else if (!ready) {
        ui_.Text(c.CutTop(22), L"이 폴더에는 직접 바꿀 수 없어 내려받기 페이지에서 받아야 합니다", 11.5f, TextFaint);
    }
    Rect btns = c.CutBottom(36);
    Rect later = btns.CutRight(84);
    btns.CutRight(8);
    if (ready) {
        if (ui_.Button(HashId("upd.install"), btns, L"다시 시작해서 업데이트", BtnStyle::Primary, 0, !busy, 13.f))
            InstallUpdate();
    } else if (ui_.Button(HashId("upd.page"), btns, L"내려받기 페이지 열기", BtnStyle::Primary, 0, true, 13.f)) {
        util::ShellOpen(Updater::ReleasePage(st.version));
        updateOpen_ = false;
    }
    if (ui_.Button(HashId("upd.later"), later, L"나중에", BtnStyle::Secondary, 0, true, 13.f)) updateOpen_ = false;
}

void App::DrawFilesPopover(Rect client) {
    const auto& in = ui_.In();
    const float W = std::min(420.f, client.W() - 40), rowH = 52, secH = 30, footH = 64;
    float listH = 0;
    if (!held_.empty()) listH += secH + held_.size() * rowH;
    listH += secH + (recentFiles_.empty() ? 56 : recentFiles_.size() * rowH);
    float top = filesBtn_.b + 8;
    float H = std::min(listH + footH + 12, client.b - 16 - top);
    Rect box = Rect::XYWH(std::max(client.l + 20, filesBtn_.r - W), top, W, H);
    bool outside = !box.Contains(in.mx, in.my);
    if ((in.pressed && outside) || in.Key(VK_ESCAPE)) {
        filesOpen_ = false;
        return;
    }
    ui_.Fill(box.Offset(0, 6), WithAlpha(Black, 0.45f), 14);
    ui_.Fill(box, Surface2, 12);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 12);

    Rect body = box.Inset(8, 8);
    Rect foot = body.CutBottom(footH - 8);
    Rect list = body;
    float maxScroll = std::max(0.f, listH - list.H());
    if (!outside && in.wheel != 0) filesScroll_ -= in.wheel * 60;
    filesScroll_ = std::clamp(filesScroll_, 0.f, maxScroll);
    ui_.PushClip(list);
    Rect cur = Rect(list.l, list.t - filesScroll_, list.r, list.t - filesScroll_ + listH);
    auto section = [&](std::wstring_view title) {
        Rect s = cur.CutTop(secH);
        ui_.Text(s.Inset(10, 0), title, 12.f, TextFaint, Bold());
    };
    auto row = [&](UiId id, Rect r, wchar_t glyph, D2D1_COLOR_F tint, const std::wstring& name,
                   const std::wstring& sub, int buttons, const std::function<void(int, Rect)>& onButton) -> bool {
        bool hot = ui_.Hover(r);
        if (hot) ui_.Fill(r, Surface3, 8);
        Rect ic = Rect::XYWH(r.l + 8, r.CY() - 16, 32, 32);
        ui_.Fill(ic, WithAlpha(tint, 0.16f), 8);
        ui_.Icon(ic, glyph, 15.f, tint);
        Rect text = Rect(ic.r + 12, r.t, r.r - 8 - buttons * 34, r.b);
        ui_.Text(Rect(text.l, r.t + 7, text.r, r.CY() + 2), name, 13.f, TextColor, Bold());
        ui_.Text(Rect(text.l, r.CY() + 1, text.r, r.b - 7), sub, 11.5f, TextFaint);
        // Buttons first: they take the press before the row does.
        for (int k = 0; k < buttons; ++k) onButton(k, Rect::XYWH(r.r - 8 - (buttons - k) * 34, r.CY() - 15, 30, 30));
        return ui_.Click(id, r);
    };

    if (!held_.empty()) {
        section(L"저장하지 않은 녹화");
        for (size_t i = 0; i < held_.size(); ++i) {
            Rect r = cur.CutTop(rowH);
            const HeldClip& h = held_[i];
            const bool current = h.clip == clip_;
            const bool editing = current && screen_ == Screen::Editor;
            std::wstring sub = util::FormatClock(h.clip->Seconds(), true) + L" · " +
                               util::FormatAgo(util::NowSeconds() - h.when) + (editing ? L" · 편집 중" : L"");
            bool drop = false;
            bool open = row(HashId("files.held", (uint32_t)i), r, icon::Record, Accent2, h.name, sub, editing ? 0 : 1,
                            [&](int, Rect b) {
                                drop = ui_.IconButton(HashId("files.drop", (uint32_t)i), b, icon::Delete, L"버리기");
                            });
            if (drop) {
                if (current) {
                    StopThumbnails();
                    clip_.reset();
                    hasResult_ = false;
                }
                DropHeld(h.clip.get());
                Invalidate();
                break;
            }
            if (open) {
                filesOpen_ = false;
                auto clip = h.clip;
                clipName_ = h.name;
                if (clip != clip_) OpenEditor(clip);
                else ResumeEditor();
                break;
            }
        }
    }
    section(L"최근 파일");
    if (recentFiles_.empty()) {
        Rect r = cur.CutTop(56);
        ui_.Text(r.Inset(10, 0), L"저장하거나 연 파일이 여기에 표시됩니다.", 12.5f, TextFaint);
    }
    for (size_t i = 0; i < recentFiles_.size(); ++i) {
        Rect r = cur.CutTop(rowH);
        const RecentFile& f = recentFiles_[i];
        int clicked = -1;
        bool open = row(HashId("files.recent", (uint32_t)i), r, f.video ? icon::Video : icon::Photo,
                        f.video ? Accent : Success, util::FileNameOf(f.path),
                        util::FormatBytes(f.bytes) + L" · " + util::FormatAgo(f.age), 2, [&](int k, Rect b) {
                            if (k == 0 && ui_.IconButton(HashId("files.folder", (uint32_t)i), b, icon::Folder, L"폴더에서 보기"))
                                clicked = 0;
                            if (k == 1 && ui_.IconButton(HashId("files.copy", (uint32_t)i), b, icon::Copy, L"파일 복사"))
                                clicked = 1;
                        });
        if (ui_.Hover(r)) ui_.Tooltip(HashId("files.tip", (uint32_t)i), r, f.path);
        if (clicked == 0) {
            util::OpenFolderAndSelect(f.path);
        } else if (clicked == 1) {
            bool ok = util::CopyFileToClipboard(hwnd_, f.path);
            Toast(ok ? L"파일을 복사했습니다. 메신저나 폴더에 붙여넣으세요." : L"복사하지 못했습니다.", !ok);
        } else if (open) {
            // Videos open in the editor; 움짤 in whatever shows them.
            std::wstring path = f.path;
            if (f.video)
                OpenMediaFile(path);
            else
                util::ShellOpen(path);
            filesOpen_ = false;
            break;
        }
    }
    ui_.PopClip();
    if (maxScroll > 0) {
        float frac = list.H() / listH;
        float th = std::max(24.f, list.H() * frac);
        float ty = list.t + (list.H() - th) * (filesScroll_ / maxScroll);
        ui_.Fill(Rect(list.r - 3, ty, list.r, ty + th), BorderStrong, 1.5f);
    }

    ui_.Line(foot.l, foot.t + 0.5f, foot.r, foot.t + 0.5f, Border);
    Rect fb = foot.Inset(2, 12);
    Rect btn = fb.CutLeft(140);
    if (ui_.Button(HashId("files.open"), btn, L"파일 열기…", BtnStyle::Secondary, icon::OpenFile)) BrowseForVideo();
    ui_.Text(fb.Inset(14, 0), L"동영상을 창에 끌어다 놓아도 열립니다", 11.5f, TextFaint, Align(AlignEnd));
}

// ---------------------------------------------------------------- home

void App::DrawHome(Rect r) {
    r.CutTop(20);
    Rect left = r.CutLeft(360);
    r.CutLeft(20);
    DrawSourcePanel(left);
    DrawCapturePanel(r);
}

void App::DrawSourcePanel(Rect r) {
    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), Border, 14);
    Rect in = r.Inset(16);
    int src = settings_.source;
    bool idle = rec_ == RecState::Idle;
    if (ui_.Segmented(HashId("home.source"), in.CutTop(38), {L"창", L"전체 화면", L"영역"}, src, idle ? 0 : 0b111) &&
        idle)
        SetSource(src);
    in.CutTop(14);
    switch (settings_.source) {
        case kSourceDisplay: DrawMonitorList(in); break;
        case kSourceRegion: DrawRegionPanel(in); break;
        default: DrawWindowList(in); break;
    }
}

void App::DrawWindowList(Rect in) {
    Rect head = in.CutTop(36);
    ui_.Text(head, L"녹화할 창", 15.f, TextColor, Bold());
    Rect pickR = head.CutRight(124);
    {
        UiId id = HashId("home.pick");
        bool clicked = ui_.Click(id, pickR);
        float h = ui_.Anim(id, ui_.Hover(pickR) ? 1.f : 0.f);
        ui_.Fill(pickR, Mix(Surface2, Surface3, h), 9);
        ui_.Stroke(pickR.Inset(0.5f), Border, 9);
        ui_.Crosshair(pickR.l + 20, pickR.CY(), 14, Mix(TextDim, Accent, 0.6f + 0.4f * h));
        ui_.Text(Rect(pickR.l + 34, pickR.t, pickR.r, pickR.b), L"화면에서 선택", 13.f, TextColor, Bold());
        ui_.Tooltip(id, pickR, L"화면 위의 창을 직접 클릭해서 고릅니다");
        if (clicked) {
            HideMain();
            picker_.Start(EnumerateCaptureWindows(), [this](HWND h) {
                ShowMain(true);
                if (h) {
                    RefreshWindows();
                    SelectTarget(h);
                }
            });
        }
    }
    head.CutRight(6);
    Rect refR = head.CutRight(34);
    if (ui_.IconButton(HashId("home.refresh"), Rect::Centered(refR.CX(), refR.CY(), 34, 34), icon::Refresh,
                       L"목록 새로고침", true, 14.f))
        RefreshWindows();

    in.CutTop(10);
    Rect hint = in.CutBottom(22);
    ui_.Text(hint, L"더블클릭하면 바로 녹화합니다", 12.f, TextFaint, Align(AlignCenter, AlignEnd));
    in.CutBottom(6);

    Rect list = in;
    const float itemH = 58, step = 62;
    float content = windows_.size() * step - 4;
    float maxScroll = std::max(0.f, content - list.H());
    bool inList = ui_.Hover(list);
    if (inList && ui_.In().wheel != 0) listScroll_ -= ui_.In().wheel * step * 1.5f;
    listScroll_ = std::clamp(listScroll_, 0.f, maxScroll);
    float scroll = ui_.Anim(HashId("home.scroll"), listScroll_, 22.f);

    if (windows_.empty()) {
        ui_.Text(list, L"녹화할 수 있는 창이 없습니다", 13.f, TextFaint, Align(AlignCenter));
        return;
    }
    ui_.PushClip(list);
    auto* dc = gfx_.DC();
    for (size_t i = 0; i < windows_.size(); ++i) {
        const WinInfo& w = windows_[i];
        Rect item = Rect::XYWH(list.l, list.t + i * step - scroll, list.W() - (maxScroll > 0 ? 8 : 0), itemH);
        if (item.b < list.t || item.t > list.b) continue;
        UiId id = HashId("win", (uint32_t)(uintptr_t)w.hwnd);
        bool hov = ui_.Hover(item);
        if (ui_.Click(id, item)) SelectTarget(w.hwnd);
        if (hov && ui_.In().dblclick) StartRecording(WindowTarget(w.hwnd), false);
        bool sel = w.hwnd == target_;
        float h = ui_.Anim(id, hov ? 1.f : 0.f);
        ui_.Fill(item, sel ? Mix(Surface2, Accent, 0.16f) : WithAlpha(Surface2, h), 10);
        if (sel) ui_.Stroke(item.Inset(0.75f), WithAlpha(Accent, 0.85f), 10, 1.5f);

        Rect ic = Rect::XYWH(item.l + 12, item.CY() - 16, 32, 32);
        if (ID2D1Bitmap* bmp = IconFor(w)) {
            ui_.Painted(ic);
            dc->DrawBitmap(bmp, ic, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        } else {
            ui_.Icon(ic, icon::AppWindow, 20.f, TextDim);
        }

        ui_.Text(Rect(item.l + 56, item.t + 8, item.r - 12, item.t + 30), w.title, 13.5f, TextColor, Bold());
        std::wstring sub = w.exeName.empty() ? L"" : w.exeName;
        if (!w.minimized) {
            sub += util::Format(L"%s%d×%d", sub.empty() ? L"" : L" · ", w.frame.right - w.frame.left,
                                w.frame.bottom - w.frame.top);
        }
        if (w.fullscreen) sub += L" · 전체화면";
        if (w.minimized) sub += L" · 최소화됨";
        ui_.Text(Rect(item.l + 56, item.t + 30, item.r - 12, item.b - 8), sub, 12.f, TextFaint);
    }
    ui_.PopClip();
    if (maxScroll > 0) {
        float th = std::max(30.f, list.H() * list.H() / content);
        float ty = list.t + (list.H() - th) * (scroll / maxScroll);
        ui_.Fill(Rect::XYWH(list.r - 4, ty, 4, th), WithAlpha(TextFaint, 0.5f), 2);
    }
}

void App::DrawMonitorList(Rect in) {
    Rect head = in.CutTop(36);
    ui_.Text(head, L"녹화할 화면", 15.f, TextColor, Bold());
    Rect refR = head.CutRight(34);
    if (ui_.IconButton(HashId("mon.refresh"), Rect::Centered(refR.CX(), refR.CY(), 34, 34), icon::Refresh,
                       L"모니터 다시 찾기", true, 14.f))
        monitors_ = EnumerateMonitors();
    in.CutTop(10);
    Rect hint = in.CutBottom(22);
    ui_.Text(hint, L"더블클릭하면 바로 녹화합니다", 12.f, TextFaint, Align(AlignCenter, AlignEnd));
    in.CutBottom(6);

    const MonitorDesc* sel = FindMonitor(monitors_, settings_.monitor);
    const float itemH = 64, step = 70;
    for (size_t i = 0; i < monitors_.size(); ++i) {
        const MonitorDesc& m = monitors_[i];
        Rect item = Rect::XYWH(in.l, in.t + i * step, in.W(), itemH);
        if (item.b > in.b) break;
        UiId id = HashId("mon", (uint32_t)i);
        bool hov = ui_.Hover(item);
        if (ui_.Click(id, item)) {
            settings_.monitor = m.device;
            settingsDirty_ = true;
        }
        if (hov && ui_.In().dblclick) StartRecording(MonitorTarget(m.handle), false);
        bool isSel = &m == sel;
        float h = ui_.Anim(id, hov ? 1.f : 0.f);
        ui_.Fill(item, isSel ? Mix(Surface2, Accent, 0.16f) : WithAlpha(Surface2, h), 10);
        if (isSel) ui_.Stroke(item.Inset(0.75f), WithAlpha(Accent, 0.85f), 10, 1.5f);

        Rect ic = Rect::XYWH(item.l + 12, item.CY() - 17, 34, 34);
        ui_.Fill(ic, isSel ? WithAlpha(Accent, 0.22f) : Surface3, 8);
        ui_.Text(ic, std::to_wstring(m.number), 15.f, isSel ? AccentHi : TextDim, Bold(AlignCenter));

        std::wstring title = util::Format(L"모니터 %d", m.number);
        if (!m.name.empty()) title += L" · " + m.name;
        ui_.Text(Rect(item.l + 58, item.t + 10, item.r - 12, item.t + 32), title, 13.5f, TextColor, Bold());
        std::wstring sub = util::Format(L"%d×%d", m.rect.right - m.rect.left, m.rect.bottom - m.rect.top);
        if (m.hz > 1) sub += util::Format(L" · %dHz", m.hz);
        sub += util::Format(L" · 배율 %d%%", (int)std::lround(m.dpi * 100.0 / 96));
        if (m.primary) sub += L" · 주 모니터";
        ui_.Text(Rect(item.l + 58, item.t + 32, item.r - 12, item.b - 10), sub, 12.f, TextFaint);
    }
    if (monitors_.empty()) ui_.Text(in, L"모니터를 찾을 수 없습니다", 13.f, TextFaint, Align(AlignCenter));
}

void App::DrawRegionPanel(Rect in) {
    ui_.Text(in.CutTop(36), L"녹화할 영역", 15.f, TextColor, Bold());
    in.CutTop(10);

    // Big "select area" card.
    Rect card = in.CutTop(132);
    UiId id = HashId("region.pick");
    bool clicked = ui_.Click(id, card, rec_ == RecState::Idle);
    float h = ui_.Anim(id, ui_.Hover(card) ? 1.f : 0.f);
    ui_.Fill(card, Mix(Surface2, Mix(Surface2, Accent, 0.14f), h), 12);
    ui_.Stroke(card.Inset(0.75f), WithAlpha(Accent, 0.35f + 0.5f * h), 12, 1.5f);
    // Dashed-looking selection glyph.
    Rect g = Rect::Centered(card.CX(), card.t + 40, 46, 30);
    for (float x = g.l; x < g.r; x += 8) {
        ui_.Line(x, g.t, std::min(x + 4, g.r), g.t, Accent, 2.f);
        ui_.Line(x, g.b, std::min(x + 4, g.r), g.b, Accent, 2.f);
    }
    for (float y = g.t; y < g.b; y += 8) {
        ui_.Line(g.l, y, g.l, std::min(y + 4, g.b), Accent, 2.f);
        ui_.Line(g.r, y, g.r, std::min(y + 4, g.b), Accent, 2.f);
    }
    ui_.Text(Rect(card.l, card.t + 64, card.r, card.t + 90),
             settings_.HasRegion() ? L"영역 다시 지정" : L"영역 지정", 15.f, TextColor, Bold(AlignCenter));
    TextStyle st = Align(AlignCenter, AlignStart);
    ui_.Text(Rect(card.l + 12, card.t + 92, card.r - 12, card.b - 8),
             L"드래그해서 고르고 툴바에서 바로 녹화합니다", 12.f, TextDim, st);
    if (clicked) StartRegionPick();

    in.CutTop(14);
    if (settings_.HasRegion()) {
        const RECT& rg = settings_.region;
        Rect info = in.CutTop(92);
        ui_.Fill(info, Surface2, 12);
        Rect ii = info.Inset(14, 12);
        ui_.Text(ii.CutTop(18), L"현재 영역", 12.f, TextFaint);
        TextStyle big = Bold();
        big.tabular = true;
        ui_.Text(ii.CutTop(30), util::Format(L"%d × %d", rg.right - rg.left, rg.bottom - rg.top), 20.f, TextColor,
                 big);
        std::wstring where;
        HMONITOR mon = MonitorFromRect(&rg, MONITOR_DEFAULTTONULL);
        for (auto& m : monitors_)
            if (m.handle == mon)
                where = util::Format(L"모니터 %d · 위치 %d, %d", m.number, rg.left - m.rect.left, rg.top - m.rect.top);
        if (where.empty()) where = L"⚠ 지금 연결된 화면 밖에 있습니다";
        ui_.Text(ii, where, 12.f, TextDim);
    }
    Rect hint = in.CutBottom(40);
    TextStyle hs = Align(AlignCenter, AlignEnd);
    hs.wrap = true;
    ui_.Text(hint, L"녹화 중에는 영역 둘레에 얇은 테두리가 보입니다\n(녹화 결과에는 찍히지 않습니다)", 12.f, TextFaint, hs);
}

void App::DrawMonitorLayout(Rect r, const RECT* region, bool pickable) {
    if (monitors_.empty()) return;
    RECT bb = monitors_[0].rect;
    for (auto& m : monitors_) UnionRect(&bb, &bb, &m.rect);
    float bw = (float)(bb.right - bb.left), bh = (float)(bb.bottom - bb.top);
    float f = std::min(r.W() * 0.86f / bw, r.H() * 0.78f / bh);
    float ox = r.CX() - bw * f / 2, oy = r.CY() - bh * f / 2;
    auto map = [&](const RECT& x) {
        return Rect(ox + (x.left - bb.left) * f, oy + (x.top - bb.top) * f, ox + (x.right - bb.left) * f,
                    oy + (x.bottom - bb.top) * f);
    };
    const MonitorDesc* sel = region ? nullptr : FindMonitor(monitors_, settings_.monitor);
    for (size_t i = 0; i < monitors_.size(); ++i) {
        const MonitorDesc& m = monitors_[i];
        Rect mr = map(m.rect).Inset(3);
        UiId id = HashId("layout", (uint32_t)i);
        bool hov = pickable && ui_.Hover(mr);
        if (pickable && ui_.Click(id, mr)) {
            settings_.monitor = m.device;
            settingsDirty_ = true;
        }
        if (pickable && hov && ui_.In().dblclick) StartRecording(MonitorTarget(m.handle), false);
        bool isSel = &m == sel;
        float h = ui_.Anim(id, hov ? 1.f : 0.f);
        ui_.Fill(mr, isSel ? Mix(Surface2, Accent, 0.18f) : Mix(Surface2, Surface3, h), 8);
        ui_.Stroke(mr.Inset(0.75f), isSel ? Accent : BorderStrong, 8, isSel ? 2.f : 1.f);
        float fs = std::clamp(mr.H() * 0.28f, 12.f, 40.f);
        ui_.Text(Rect(mr.l, mr.t, mr.r, mr.CY() + fs * 0.4f), std::to_wstring(m.number), fs,
                 isSel ? AccentHi : TextDim, Bold(AlignCenter));
        if (mr.H() > 70)
            ui_.Text(Rect(mr.l, mr.CY() + fs * 0.4f, mr.r, mr.CY() + fs * 0.4f + 22),
                     util::Format(L"%d×%d", m.rect.right - m.rect.left, m.rect.bottom - m.rect.top), 12.f, TextFaint,
                     Align(AlignCenter));
    }
    if (region) {
        Rect rr = map(*region);
        ui_.Fill(rr, WithAlpha(Accent, 0.28f), 2);
        ui_.Stroke(rr, Accent, 2, 1.5f);
    }
}

void App::DrawCapturePreview(Rect preview) {
    ui_.Fill(preview, Hex(0x0A0B0E), 10);
    // The window preview is a DWM thumbnail, which DWM composites above everything we draw: everything here stays
    // above the result card instead of under it.
    const bool card = showVideo_ && rec_ == RecState::Idle;
    Rect view = preview;
    if (card) view.b = VideoCardRect(preview).t;
    std::wstring hk = HotkeyLabel();
    auto placeholder = [&](std::wstring_view title, std::wstring_view body) {
        float cy = view.CY();
        ui_.Ring(view.CX(), cy - 44, 30, WithAlpha(Accent, 0.35f), 2.f);
        ui_.Crosshair(view.CX(), cy - 44, 30, Accent);
        ui_.Text(Rect(view.l, cy - 4, view.r, cy + 22), title, 16.f, TextColor, Bold(AlignCenter));
        TextStyle st = Align(AlignCenter, AlignStart);
        st.wrap = true;
        ui_.Text(Rect(view.l + 30, cy + 30, view.r - 30, cy + 80), body, 12.5f, TextDim, st);
    };
    switch (settings_.source) {
        case kSourceDisplay:
            DrawMonitorLayout(view, nullptr, rec_ == RecState::Idle);
            break;
        case kSourceRegion:
            if (!settings_.HasRegion()) {
                placeholder(L"녹화할 영역을 지정하세요",
                            L"왼쪽의 ‘영역 지정’을 누르고 화면에서 드래그하세요.\n단축키 " + hk +
                                L" 은 지정한 영역을 바로 녹화합니다.");
            } else if (snapBmp_ && EqualRect(&snapRect_, &settings_.region)) {
                auto sz = snapBmp_->GetSize();
                Rect area = view.Inset(10);
                float s = Scale();
                float rw = (float)(snapRect_.right - snapRect_.left) / s, rh = (float)(snapRect_.bottom - snapRect_.top) / s;
                float f = std::min({area.W() / rw, area.H() / rh, 1.f});
                (void)sz;
                Rect dst = Rect::Centered(area.CX(), area.CY(), rw * f, rh * f);
                ui_.Painted(dst);
                gfx_.DC()->DrawBitmap(snapBmp_.Get(), dst, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
                ui_.Stroke(dst.Inset(-1.f), WithAlpha(Accent, 0.8f), 0, 1.5f);
            } else {
                DrawMonitorLayout(view, &settings_.region, false);
            }
            break;
        default:
            if (target_ && IsWindow(target_)) {
                thumbWanted_ = true;
                thumbArea_ = view.Inset(10);
                const WinInfo* info = FindWindowInfo(target_);
                if (info && info->minimized)
                    ui_.Text(view, L"최소화된 창입니다 · 녹화를 시작하면 복원됩니다", 13.f, TextFaint,
                             Align(AlignCenter));
                ui_.NativeLayer(thumbArea_);  // from here on, only floats may be drawn over it
            } else {
                placeholder(L"녹화할 창을 선택하세요", L"왼쪽 목록에서 고르거나 ‘화면에서 선택’을 누르세요.\n게임 중에는 " +
                                                         hk + L" 을 누르면 지금 보고 있는 창을 바로 녹화합니다.");
            }
            break;
    }
    if (card) DrawVideoResult(preview);
}

void App::DrawVideoResult(Rect preview) {
    Rect card = VideoCardRect(preview);
    float a = ui_.AnimFrom(HashId("vres.in"), 1.f, 0.f, 14.f);
    card = card.Offset(0, (1 - a) * 12);
    ui_.Fill(card.Offset(0, 3), WithAlpha(Black, 0.4f * a), 12);
    ui_.Fill(card, WithAlpha(Hex(0x14231D), 0.97f * a), 12);
    ui_.Stroke(card.Inset(0.5f), WithAlpha(Success, 0.45f * a), 12);
    Rect in = card.Inset(14, 10);
    Rect close = in.CutRight(28);
    if (ui_.IconButton(HashId("vres.close"), Rect::Centered(close.CX(), close.CY(), 28, 28), icon::Close, L"닫기",
                       true, 11.f))
        showVideo_ = false;
    in.CutRight(6);
    const float bw = 76;
    Rect btns = in.CutRight(bw * 4 + 18);
    Rect b = Rect::Centered(0, btns.CY(), bw, 34);
    auto at = [&](int i) { return Rect::XYWH(btns.l + i * (bw + 6), b.t, bw, b.H()); };
    if (ui_.Button(HashId("vres.edit"), at(0), L"편집", BtnStyle::Primary, icon::Cut, true, 12.f)) {
        if (clip_ && _wcsicmp(clip_->SourcePath().c_str(), lastVideo_.path.c_str()) == 0)
            ResumeEditor();
        else
            OpenVideo(lastVideo_.path);
    }
    if (ui_.Button(HashId("vres.folder"), at(1), L"폴더", BtnStyle::Secondary, icon::Folder, true, 12.f))
        util::OpenFolderAndSelect(lastVideo_.path);
    if (ui_.Button(HashId("vres.copy"), at(2), L"복사", BtnStyle::Secondary, icon::Copy, true, 12.f)) {
        if (util::CopyFileToClipboard(hwnd_, lastVideo_.path))
            Toast(L"파일을 클립보드에 복사했습니다 · 메신저나 탐색기에 붙여넣으세요");
        else
            Toast(L"클립보드에 복사할 수 없습니다.", true);
    }
    if (ui_.Button(HashId("vres.open"), at(3), L"재생", BtnStyle::Secondary, icon::Play, true, 12.f))
        util::ShellOpen(lastVideo_.path);
    in.CutRight(12);
    Rect icon = in.CutLeft(28);
    ui_.Circle(icon.CX(), icon.CY(), 12, WithAlpha(Success, 0.2f));
    ui_.Icon(icon, icon::Check, 12.f, Success);
    in.CutLeft(8);
    ui_.Text(Rect(in.l, in.t, in.r, in.CY() + 1), util::FileNameOf(lastVideo_.path), 13.f, TextColor, Bold());
    std::wstring sub = util::FormatClock(lastVideo_.seconds, false) + L" · " + util::FormatBytes(lastVideo_.bytes) +
                       (lastVideo_.audio ? L" · 소리 포함" : L" · 소리 없음");
    if (!lastVideo_.encoder.empty()) {  // only when it fits: the buttons leave little room in a narrow window
        std::wstring enc =
            lastVideo_.hardware ? L" · " + Mp4Writer::Vendor(lastVideo_.encoder) + L" 가속" : L" · CPU 인코딩";
        if (ui_.Measure(sub + enc, 11.5f) <= in.W()) sub += enc;
    }
    ui_.Text(Rect(in.l, in.CY() + 1, in.r, in.b), sub, 11.5f, TextDim);
}

void App::DrawCapturePanel(Rect r) {
    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), Border, 14);
    Rect in = r.Inset(20);
    const bool video = settings_.output == kOutputVideo;
    if (video) ProbeEncoders();

    Rect controls = in.CutBottom(326);
    in.CutBottom(16);
    DrawCapturePreview(in);

    RecTarget t;
    bool ready = CurrentTarget(t, nullptr);
    const int src = settings_.source;

    // Info row: what + resulting size.
    Rect row = controls.CutTop(24);
    if (ready) {
        int sw = 0, sh = 0, ow = 0, oh = 0;
        SourceSize(t, sw, sh);
        PlannedSize(sw, sh, ow, oh);
        std::wstring title;
        std::wstring badge = L"창 캡처";
        D2D1_COLOR_F badgeColor = AccentHi;
        if (src == kSourceWindow) {
            const WinInfo* info = FindWindowInfo(t.hwnd);
            title = info ? info->title : L"";
            if (UseMonitorCapture(t.hwnd)) {
                badge = L"화면 캡처";
                badgeColor = Warning;
            }
        } else {
            const MonitorDesc* md = nullptr;
            for (auto& m : monitors_)
                if (m.handle == t.monitor) md = &m;
            title = md ? util::Format(L"모니터 %d", md->number) : L"모니터";
            if (md && !md->name.empty() && src == kSourceDisplay) title += L" · " + md->name;
            if (src == kSourceRegion) title = L"영역 · " + title;
            badge = src == kSourceRegion ? L"영역" : L"전체 화면";
        }
        std::wstring dims = sw > 0 ? util::Format(L"%d×%d  →  %d×%d", sw, sh, ow, oh) : L"";
        float dw = ui_.Measure(dims, 12.5f) + 4;
        float bw = ui_.Measure(badge, 11.5f) + 16;
        Rect b = Rect::XYWH(row.r - bw, row.CY() - 11, bw, 22);
        ui_.Fill(b, WithAlpha(badgeColor, 0.15f), 11);
        ui_.Text(b, badge, 11.5f, badgeColor, Bold(AlignCenter));
        ui_.Text(Rect(b.l - dw - 10, row.t, b.l - 10, row.b), dims, 12.5f, TextDim, Align(AlignEnd));
        ui_.Text(Rect(row.l, row.t, b.l - dw - 20, row.b), title, 14.f, TextColor, Bold());
    } else {
        const wchar_t* none = src == kSourceDisplay ? L"선택된 화면 없음"
                              : src == kSourceRegion ? L"지정된 영역 없음"
                                                     : L"선택된 창 없음";
        ui_.Text(row, none, 14.f, TextFaint, Bold());
    }
    controls.CutTop(14);

    auto labeled = [&](std::wstring_view label, float h) {
        Rect line = controls.CutTop(h);
        Rect l = line.CutLeft(84);
        ui_.Text(l, label, 13.f, TextDim);
        controls.CutTop(10);
        return line;
    };
    bool changed = false;
    bool idle = rec_ == RecState::Idle;
    int output = settings_.output;
    if (ui_.Segmented(HashId("home.output"), labeled(L"결과물", 38), {L"움짤 (GIF · WebP …)", L"동영상 (MP4)"}, output,
                      idle ? 0 : 0b11) &&
        idle) {
        settings_.output = output;
        changed = true;
    }
    if (video) {
        changed |= ui_.Segmented(HashId("home.vfps"), labeled(L"프레임", 38), {L"24", L"30", L"60"}, settings_.vFpsIdx);
        changed |= ui_.Segmented(HashId("home.vres"), labeled(L"해상도", 38),
                                 {L"원본", L"2160p", L"1440p", L"1080p", L"720p"}, settings_.vResIdx);
    } else {
        changed |= ui_.Segmented(HashId("home.fps"), labeled(L"프레임", 38),
                                 {L"10", L"15", L"20", L"30", L"50", L"60"}, settings_.fpsIdx);
        changed |= ui_.Segmented(HashId("home.size"), labeled(L"최대 크기", 38),
                                 {L"원본", L"1280", L"960", L"720", L"480"}, settings_.sizeIdx);
    }

    // Toggles: cursor, (window) title bar, (video) sound and mic. Each as wide as its label needs, with the room
    // left shared out evenly (or all narrowed alike when there is too little).
    struct ToggleItem {
        UiId id;
        const wchar_t* label;
        bool* v;
    };
    std::vector<ToggleItem> items{{HashId("home.cursor"), L"마우스 커서", &settings_.cursor}};
    if (src == kSourceWindow) items.push_back({HashId("home.client"), L"제목 표시줄 제외", &settings_.clientOnly});
    if (video) {
        items.push_back({HashId("home.audio"), L"소리 녹음", &settings_.audio});
        items.push_back({HashId("home.mic"), L"마이크", &settings_.mic});
    }
    Rect toggles = controls.CutTop(44);
    const float n = (float)items.size(), avail = toggles.W() - 10 * (n - 1);
    float need = 0;
    for (const auto& it : items) need += ui_.Measure(it.label, 13.f) + 64;  // ToggleRow: padding, label, switch
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) toggles.CutLeft(10);
        float w = ui_.Measure(items[i].label, 13.f) + 64;
        w = need <= avail ? w + (avail - need) / n : w * avail / need;
        changed |= ui_.ToggleRow(items[i].id, i + 1 == items.size() ? toggles : toggles.CutLeft(w), items[i].label,
                                 *items[i].v);
    }
    controls.CutTop(10);

    // One-line summary of what will happen, with a shortcut to the detailed settings.
    Rect cap = controls.CutTop(20);
    if (video) {
        Rect link = cap.CutRight(80);
        UiId lid = HashId("home.vsettings");
        bool lh = ui_.Hover(link);
        if (ui_.Click(lid, link)) OpenSettings(kTabVideo);
        ui_.Text(link, L"세부 설정 ›", 12.f, lh ? AccentHi : Accent, Align(AlignEnd));
        std::wstring audio = AudioSummary(src);
        bool warn = settings_.audio && src == kSourceWindow && !AudioCapture::ProcessLoopbackSupported();
        float aw = ui_.Measure(audio, 12.f) + 4;
        Rect ar = cap.CutRight(aw + 16);
        ui_.Icon(Rect::XYWH(ar.l, ar.t, 14, ar.H()), warn ? icon::Warning : icon::Info, 11.f,
                 warn ? Warning : TextFaint);
        ui_.Text(Rect(ar.l + 16, ar.t, ar.r, ar.b), audio, 12.f, warn ? Warning : TextDim);
        ui_.Text(cap, VideoSummary(), 12.f, TextFaint);
    } else {
        ui_.Text(cap, L"녹화가 끝나면 편집 화면에서 구간 · 크기 · 형식을 정해 저장합니다", 12.f, TextFaint);
    }
    controls.CutTop(14);
    if (changed) settingsDirty_ = true;

    // Record button
    Rect btn = controls.CutTop(56);
    UiId id = HashId("home.rec");
    bool enabled = rec_ != RecState::Idle || ready;
    bool clicked = ui_.Click(id, btn, enabled);
    float h = ui_.Anim(id, enabled && ui_.Hover(btn) ? 1.f : 0.f);
    D2D1_COLOR_F bg = enabled ? Mix(Rec, RecHi, h) : Surface2;
    if (rec_ != RecState::Idle) bg = Mix(Hex(0x2A2F3A), Hex(0x343B48), h);
    ui_.Fill(btn, bg, 14);
    std::wstring hk = HotkeyLabel();
    if (rec_ == RecState::Idle) {
        const wchar_t* label = video ? L"동영상 녹화" : L"녹화 시작";
        float lw = ui_.Measure(label, 16.f, kSemibold);
        float cx = btn.CX() - (lw + 28) / 2 + 9;
        ui_.Circle(cx, btn.CY(), 9, enabled ? White : TextFaint);
        if (enabled) ui_.Circle(cx, btn.CY(), 4, Mix(Rec, RecHi, h));
        ui_.Text(Rect(cx + 18, btn.t, btn.r, btn.b), label, 16.f, enabled ? White : TextFaint, Bold());
    } else {
        ui_.Fill(Rect::Centered(btn.CX() - 70, btn.CY(), 14, 14), Rec, 3);
        std::wstring t2 = rec_ == RecState::Countdown ? L"카운트다운 취소"
                                                 : L"녹화 중지  " + util::FormatClock(capture_.Elapsed(), true);
        TextStyle st = Bold();
        st.tabular = true;
        ui_.Text(Rect(btn.CX() - 52, btn.t, btn.r, btn.b), t2, 16.f, White, st);
    }
    float kw = ui_.Measure(hk, 11.5f) + 18;
    Rect kb = Rect::XYWH(btn.r - kw - 14, btn.CY() - 12, kw, 24);
    ui_.Fill(kb, WithAlpha(Black, 0.22f), 8);
    ui_.Text(kb, hk, 11.5f, WithAlpha(White, enabled ? 0.85f : 0.4f), Align(AlignCenter));
    if (clicked) {
        if (rec_ == RecState::Idle)
            StartRecording(t, false);
        else
            StopRecording();
    }
}

// ---------------------------------------------------------------- editor

void App::DrawEditor(Rect r) {
    if (!clip_) {
        screen_ = Screen::Home;
        return;
    }
    r.CutTop(20);
    Rect right = r.CutRight(316);
    r.CutRight(20);
    Rect left = r;
    Rect tl = left.CutBottom(84);
    left.CutBottom(8);
    Rect tr = left.CutBottom(44);
    left.CutBottom(10);

    // Keyboard shortcuts
    const UiInput& in = ui_.In();
    size_t count = clip_->Count();
    if (in.Key(VK_SPACE)) SetPlaying(!playing_);
    if (in.Key(VK_LEFT) && playhead_ > 0) {
        SetPlaying(false);
        Seek(playhead_ - 1);
    }
    if (in.Key(VK_RIGHT) && playhead_ + 1 < count) {
        SetPlaying(false);
        Seek(playhead_ + 1);
    }
    if (in.Key(VK_HOME)) Seek(inF_);
    if (in.Key(VK_END)) Seek(outF_);
    if (in.Key('I')) {
        inF_ = std::min(playhead_, outF_);
        hasResult_ = false;
        Seek(inF_);
    }
    if (in.Key('O')) {
        outF_ = std::max(playhead_, inF_);
        hasResult_ = false;
    }
    if (in.Key(VK_ESCAPE)) cropMode_ = false;

    if (playing_) playhead_ = PlayheadFromClock();
    playhead_ = std::clamp(playhead_, (size_t)0, count - 1);

    DrawPreview(left);
    DrawTransport(tr);
    DrawTimeline(tl);
    DrawExportPanel(right);
}

void App::DrawPreview(Rect r) {
    ui_.Fill(r, Hex(0x0A0B0E), 14);
    Rect area = r.Inset(14);
    const int W = clip_->Width(), H = clip_->Height();
    float f = std::min({area.W() / W, area.H() / H, 3.f});
    Rect dst = Rect::Centered(area.CX(), area.CY(), W * f, H * f);
    dst = Rect(std::round(dst.l), std::round(dst.t), std::round(dst.l) + W * f, std::round(dst.t) + H * f);
    previewDst_ = dst;
    ShowFrame(playhead_);
    if (frameBmp_) {
        float px = f * Scale();  // device pixels per frame pixel
        auto mode = px < 0.99f ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                    : px > 1.99f ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                 : D2D1_INTERPOLATION_MODE_LINEAR;
        ui_.Painted(dst);
        gfx_.DC()->DrawBitmap(frameBmp_.Get(), dst, 1.f, mode, nullptr);
    }

    auto toDip = [&](const RECT& c) {
        return Rect(dst.l + c.left * f, dst.t + c.top * f, dst.l + c.right * f, dst.t + c.bottom * f);
    };
    bool fullCrop = crop_.left == 0 && crop_.top == 0 && crop_.right == W && crop_.bottom == H;
    Rect cr = toDip(crop_);
    const UiInput& in = ui_.In();
    UiId id = HashId("preview");

    if (cropMode_) {
        // Hit-test handles: 0-3 corners (TL, TR, BR, BL), 4-7 edges (T, R, B, L), 8 = move.
        auto hitHandle = [&](float x, float y) -> int {
            const float g = 10;
            float pts[8][2] = {{cr.l, cr.t}, {cr.r, cr.t}, {cr.r, cr.b}, {cr.l, cr.b},
                               {cr.CX(), cr.t}, {cr.r, cr.CY()}, {cr.CX(), cr.b}, {cr.l, cr.CY()}};
            for (int i = 0; i < 8; ++i) {
                if (i >= 4 && cropAspect_ > 0) break;
                if (std::fabs(x - pts[i][0]) <= g && std::fabs(y - pts[i][1]) <= g) return i;
            }
            if (cr.Contains(x, y)) return 8;
            return -1;
        };
        int hover = hitHandle(in.mx, in.my);
        if (in.pressed && hover >= 0 && dst.Inset(-12).Contains(in.mx, in.my)) {
            ui_.Click(id, dst.Inset(-12));
            cropDrag_ = hover;
            cropStart_ = crop_;
            dragX_ = in.mx;
            dragY_ = in.my;
        }
        if (cropDrag_ >= 0 && in.down) {
            int dx = (int)std::lround((in.mx - dragX_) / f), dy = (int)std::lround((in.my - dragY_) / f);
            RECT c = cropStart_;
            const int minSz = 16;
            if (cropDrag_ == 8) {
                int w = c.right - c.left, h = c.bottom - c.top;
                int x = std::clamp((int)c.left + dx, 0, W - w), y = std::clamp((int)c.top + dy, 0, H - h);
                crop_ = {x, y, x + w, y + h};
            } else {
                int l = c.left, t = c.top, rr = c.right, b = c.bottom;
                bool L = cropDrag_ == 0 || cropDrag_ == 3 || cropDrag_ == 7;
                bool R = cropDrag_ == 1 || cropDrag_ == 2 || cropDrag_ == 5;
                bool T = cropDrag_ == 0 || cropDrag_ == 1 || cropDrag_ == 4;
                bool B = cropDrag_ == 2 || cropDrag_ == 3 || cropDrag_ == 6;
                if (L) l = std::clamp((int)c.left + dx, 0, rr - minSz);
                if (R) rr = std::clamp((int)c.right + dx, l + minSz, W);
                if (T) t = std::clamp((int)c.top + dy, 0, b - minSz);
                if (B) b = std::clamp((int)c.bottom + dy, t + minSz, H);
                if (cropAspect_ > 0 && cropDrag_ < 4) {
                    static const double aspects[] = {0, 1.0, 4.0 / 3.0, 16.0 / 9.0, 9.0 / 16.0};
                    double a = aspects[cropAspect_];
                    int w = rr - l;
                    int h = (int)std::lround(w / a);
                    int maxH = T ? b : H - t;
                    if (h > maxH) {
                        h = maxH;
                        w = (int)std::lround(h * a);
                        if (L) l = rr - w; else rr = l + w;
                    }
                    if (T) t = b - h; else b = t + h;
                }
                crop_ = {l, t, rr, b};
            }
            cr = toDip(crop_);
            hasResult_ = false;
        }
        if (!in.down) cropDrag_ = -1;
        int hh = cropDrag_ >= 0 ? cropDrag_ : hover;
        if (hh == 0 || hh == 2) ui_.SetCursor(IDC_SIZENWSE);
        else if (hh == 1 || hh == 3) ui_.SetCursor(IDC_SIZENESW);
        else if (hh == 4 || hh == 6) ui_.SetCursor(IDC_SIZENS);
        else if (hh == 5 || hh == 7) ui_.SetCursor(IDC_SIZEWE);
        else if (hh == 8) ui_.SetCursor(IDC_SIZEALL);
    } else if (ui_.Click(id, dst)) {
        SetPlaying(!playing_);
    }

    if (!fullCrop || cropMode_) {
        D2D1_COLOR_F dim = WithAlpha(Black, 0.6f);
        ui_.Fill(Rect(dst.l, dst.t, dst.r, cr.t), dim);
        ui_.Fill(Rect(dst.l, cr.b, dst.r, dst.b), dim);
        ui_.Fill(Rect(dst.l, cr.t, cr.l, cr.b), dim);
        ui_.Fill(Rect(cr.r, cr.t, dst.r, cr.b), dim);
        ui_.Stroke(cr, WithAlpha(White, 0.9f), 0, 1.f);
    }
    if (cropMode_) {
        for (int i = 1; i <= 2; ++i) {
            float x = cr.l + cr.W() * i / 3, y = cr.t + cr.H() * i / 3;
            ui_.Line(x, cr.t, x, cr.b, WithAlpha(White, 0.22f));
            ui_.Line(cr.l, y, cr.r, y, WithAlpha(White, 0.22f));
        }
        float pts[8][2] = {{cr.l, cr.t}, {cr.r, cr.t}, {cr.r, cr.b}, {cr.l, cr.b},
                           {cr.CX(), cr.t}, {cr.r, cr.CY()}, {cr.CX(), cr.b}, {cr.l, cr.CY()}};
        for (int i = 0; i < (cropAspect_ > 0 ? 4 : 8); ++i) {
            Rect hr = Rect::Centered(pts[i][0], pts[i][1], 10, 10);
            ui_.Fill(hr, White, 2);
            ui_.Stroke(hr, WithAlpha(Black, 0.4f), 2);
        }
        std::wstring sz = util::Format(L"%d × %d", crop_.right - crop_.left, crop_.bottom - crop_.top);
        float w = ui_.Measure(sz, 12.f) + 16;
        Rect lab = Rect::XYWH(cr.l, std::max(dst.t, cr.t - 28), w, 22);
        ui_.Fill(lab, WithAlpha(Black, 0.7f), 6);
        ui_.Text(lab, sz, 12.f, White, Align(AlignCenter));
    }

    if (!playing_ && !cropMode_) {
        float a = ui_.Anim(HashId("preview.pausehint"), ui_.Hover(dst) ? 1.f : 0.f);
        if (a > 0.01f) {
            ui_.Circle(dst.CX(), dst.CY(), 30, WithAlpha(Black, 0.5f * a));
            ui_.Icon(Rect::Centered(dst.CX() + 2, dst.CY(), 60, 60), icon::Play, 22.f, WithAlpha(White, a));
        }
    }
}

void App::DrawTransport(Rect r) {
    Rect playR = r.CutLeft(44);
    if (ui_.IconButton(HashId("tr.play"), Rect::Centered(playR.CX(), playR.CY(), 44, 44),
                       playing_ ? icon::Pause : icon::Play, playing_ ? L"일시정지 (Space)" : L"재생 (Space)", true,
                       18.f))
        SetPlaying(!playing_);
    r.CutLeft(4);
    Rect prevR = r.CutLeft(34), nextR = r.CutLeft(34);
    if (ui_.IconButton(HashId("tr.prev"), Rect::Centered(prevR.CX(), prevR.CY(), 34, 34), icon::Prev,
                       L"이전 프레임 (←)", playhead_ > 0, 12.f)) {
        SetPlaying(false);
        Seek(playhead_ - 1);
    }
    if (ui_.IconButton(HashId("tr.next"), Rect::Centered(nextR.CX(), nextR.CY(), 34, 34), icon::Next,
                       L"다음 프레임 (→)", playhead_ + 1 < clip_->Count(), 12.f)) {
        SetPlaying(false);
        Seek(playhead_ + 1);
    }
    if (clip_->HasAudio()) {
        Rect volR = r.CutLeft(34);
        bool slow = kSpeedOptions[settings_.speedIdx] != 1.0;
        const int vol = previewMute_ ? 0 : settings_.previewVolume;
        const wchar_t* tip = slow ? L"재생 속도가 1×일 때만 소리가 나옵니다"
                             : vol == 0 ? L"미리보기 소리 켜기" : L"미리보기 소리 끄기";
        if (ui_.IconButton(HashId("tr.mute"), Rect::Centered(volR.CX(), volR.CY(), 34, 34),
                           vol == 0 || slow ? icon::Mute : icon::Volume, tip, true, 13.f))
            TogglePreviewMute();
        // The volume (#36). Crop mode pauses the preview and needs the room.
        if (!cropMode_) {
            Rect volS = r.CutLeft(78);
            Rect track(volS.l + 8, volS.t + 8, volS.r - 8, volS.b - 8);
            UiId id = HashId("tr.vol");
            float v = (float)vol;
            if (ui_.Slider(id, track, v, 0.f, 100.f)) SetPreviewVolume((int)std::lround(v / 5) * 5);
            if (ui_.In().wheel != 0 && ui_.Hover(Rect(volR.l, volS.t, volS.r, volS.b)))
                SetPreviewVolume(vol + (ui_.In().wheel > 0 ? 5 : -5));
            ui_.Tooltip(id, track, util::Format(L"미리보기 음량 %d%% · 휠로도 조절", vol));
        }
    }
    r.CutLeft(10);
    std::wstring time = util::FormatClock(clip_->Ts(playhead_) / 1e7, true) + L"  /  " +
                        util::FormatClock(clip_->Seconds(), true);
    TextStyle st;
    st.tabular = true;
    ui_.Text(r.CutLeft(150), time, 13.f, TextDim, st);

    // Crop controls on the right.
    Rect cropBtn = r.CutRight(92);
    if (ui_.Button(HashId("tr.crop"), Rect::Centered(cropBtn.CX(), cropBtn.CY(), 92, 36), L"자르기",
                   cropMode_ ? BtnStyle::Primary : BtnStyle::Secondary, icon::Crop, true, 13.f)) {
        cropMode_ = !cropMode_;
        if (cropMode_) SetPlaying(false);
    }
    if (cropMode_) {
        r.CutRight(8);
        Rect reset = r.CutRight(68);
        if (ui_.Button(HashId("tr.cropreset"), Rect::Centered(reset.CX(), reset.CY(), 68, 36), L"초기화",
                       BtnStyle::Ghost, 0, true, 13.f)) {
            cropAspect_ = 0;
            ResetCrop();
            hasResult_ = false;
        }
        r.CutRight(6);
        Rect seg = r.CutRight(std::min(270.f, r.W()));
        if (ui_.Segmented(HashId("tr.aspect"), Rect::Centered(seg.CX(), seg.CY(), seg.W(), 36),
                          {L"자유", L"1:1", L"4:3", L"16:9", L"9:16"}, cropAspect_)) {
            ApplyCropAspect();
            hasResult_ = false;
        }
    }
}

void App::DrawTimeline(Rect r) {
    const int64_t total = std::max<int64_t>(1, clip_->EndTs());
    Rect labels = r.CutTop(22);
    Rect track = r.Inset(0, 2);
    track.b -= 4;
    auto X = [&](int64_t t) { return track.l + track.W() * (float)((double)t / total); };
    auto T = [&](float x) { return (int64_t)((double)std::clamp((x - track.l) / track.W(), 0.f, 1.f) * total); };

    double selStart = clip_->Ts(inF_) / 1e7, selEnd = RangeEndTs() / 1e7;
    TextStyle st;
    st.tabular = true;
    ui_.Text(labels, L"구간  " + util::FormatClock(selStart, true) + L" – " + util::FormatClock(selEnd, true), 12.f,
             TextDim, st);
    st.h = AlignEnd;
    ui_.Text(labels, util::Format(L"%.1f초 · I / O 키로 구간 지정", selEnd - selStart), 12.f, TextFaint, st);

    ui_.Fill(track, Surface2, 8);
    ui_.PushClip(track);
    if (!thumbs_.empty()) {
        float slot = track.W() / thumbs_.size();
        for (size_t i = 0; i < thumbs_.size(); ++i) {
            if (!thumbs_[i]) continue;
            Rect d = Rect::XYWH(track.l + i * slot, track.t, slot + 0.5f, track.H());
            // Fill the slot (center crop) keeping the thumbnail's aspect.
            float bw = (float)thumbW_, bh = (float)thumbH_;
            float s = std::max(d.W() / bw, d.H() / bh);
            float sw = d.W() / s, sh = d.H() / s;
            D2D1_RECT_F src{(bw - sw) / 2, (bh - sh) / 2, (bw + sw) / 2, (bh + sh) / 2};
            ui_.Painted(d);
            gfx_.DC()->DrawBitmap(thumbs_[i].Get(), d, 0.9f, D2D1_INTERPOLATION_MODE_LINEAR, &src);
        }
    }
    float x0 = X(clip_->Ts(inF_)), x1 = X(RangeEndTs());
    ui_.Fill(Rect(track.l, track.t, x0, track.b), WithAlpha(Bg, 0.72f));
    ui_.Fill(Rect(x1, track.t, track.r, track.b), WithAlpha(Bg, 0.72f));
    ui_.PopClip();

    // Trim handles (checked before the scrub area so they win the press).
    const UiInput& in = ui_.In();
    Rect hIn = Rect(x0, track.t, x0 + 12, track.b), hOut = Rect(x1 - 12, track.t, x1, track.b);
    UiId idIn = HashId("tl.in"), idOut = HashId("tl.out"), idScrub = HashId("tl.scrub");
    ui_.Click(idIn, hIn.Inset(-6, 0));
    ui_.Click(idOut, hOut.Inset(-6, 0));
    if (ui_.Hover(hIn.Inset(-6, 0)) || ui_.Hover(hOut.Inset(-6, 0)) || ui_.Active(idIn) || ui_.Active(idOut))
        ui_.SetCursor(IDC_SIZEWE);
    if (ui_.Active(idIn) && in.down) {
        size_t idx = std::min(clip_->IndexAt(T(in.mx)), outF_);
        if (idx != inF_) {
            inF_ = idx;
            hasResult_ = false;
            Seek(inF_);
        }
    }
    if (ui_.Active(idOut) && in.down) {
        size_t idx = std::max(clip_->IndexAt(T(in.mx) - 1), inF_);
        if (idx != outF_) {
            outF_ = idx;
            hasResult_ = false;
            Seek(outF_);
        }
    }
    ui_.Click(idScrub, track);
    if (ui_.Active(idScrub)) {
        if (in.pressed) {
            resumeAfterScrub_ = playing_;
            playing_ = false;
            scrubbing_ = true;
        }
        if (in.down) Seek(std::clamp(clip_->IndexAt(T(in.mx)), (size_t)0, clip_->Count() - 1));
    }
    if (scrubbing_ && !in.down) {
        scrubbing_ = false;
        if (resumeAfterScrub_) SetPlaying(true);
    }

    Rect sel(x0, track.t, x1, track.b);
    ui_.Stroke(sel.Inset(1), Accent, 6, 2.f);
    for (Rect h : {hIn, hOut}) {
        ui_.Fill(h, Accent, 4);
        ui_.Line(h.CX(), h.CY() - 8, h.CX(), h.CY() + 8, WithAlpha(White, 0.85f), 1.5f);
    }
    float px = X(clip_->Ts(playhead_));
    ui_.Line(px, track.t - 4, px, track.b + 2, White, 2.f);
    ui_.Circle(px, track.t - 4, 4.5f, White);
}

void App::DrawExportPanel(Rect r) {
    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), Border, 14);
    Rect in = r.Inset(18);
    bool busy = exporter_.Busy();

    ui_.Text(in.CutTop(28), L"내보내기", 16.f, TextColor, Bold());
    in.CutTop(8);

    uint32_t disabled = 0;
    for (int i = 0; i < (int)Format::Count; ++i)
        if (!FormatAvailable((Format)i)) disabled |= 1u << i;
    Format fmt = (Format)fmt_;
    const bool mp4 = fmt == Format::Mp4;
    const double speed = kSpeedOptions[settings_.speedIdx];
    int ow = 0, oh = 0;
    OutputSize(ow, oh);
    double fpsLimit = EffectiveOutFps(), outSec = 0, outFps = 0;
    size_t outFrames = 0;
    if (mp4) {
        outFrames = PlanVideoFrames(*clip_, inF_, outF_, fpsLimit, speed).size();
        outFps = fpsLimit;
        outSec = outFrames / fpsLimit;
    } else {
        auto plan = PlanFrames(*clip_, inF_, outF_, fpsLimit, speed);
        for (auto& p : plan) outSec += p.durationMs / 1000.0;
        outFrames = plan.size();
        outFps = outSec > 0 ? outFrames / outSec : 0;
        if (settings_.holdLast) outSec += kHoldLastMs / 1000.0;
    }
    if (!busy) UpdateEstimate();
    const uint64_t target = (uint64_t)kTargetMB[settings_.targetIdx] * 1'000'000;

    // Bottom: what will be written and the actions, always in view.
    Rect saveAs = in.CutBottom(36);
    in.CutBottom(8);
    Rect save = in.CutBottom(46);
    in.CutBottom(10);
    bool showResult = hasResult_ && !busy;
    Rect card, summary;
    if (showResult)
        card = in.CutBottom(76);
    else
        summary = in.CutBottom(20);
    in.CutBottom(8);

    // Options above, scrolling when the window is too short for all of them.
    Rect view = in;
    float maxScroll = std::max(0.f, exportContentH_ - view.H());
    if (ui_.Hover(view) && ui_.In().wheel != 0) exportScroll_ -= ui_.In().wheel * 60.f;
    exportScroll_ = std::clamp(exportScroll_, 0.f, maxScroll);
    ui_.PushClip(view);
    in = Rect(view.l, view.t - exportScroll_, view.r - (maxScroll > 0 ? 8 : 0), view.t - exportScroll_ + 10000);
    const float top = in.t;

    auto label = [&](std::wstring_view name, std::wstring_view value) {
        Rect l = in.CutTop(20);
        ui_.Text(l, name, 12.5f, TextDim);
        if (!value.empty()) ui_.Text(l, value, 12.5f, TextColor, Align(AlignEnd));
        in.CutTop(4);
    };
    bool changed = false;

    label(L"형식", {});
    if (ui_.Segmented(HashId("ex.fmt"), in.CutTop(34), {L"GIF", L"WebP", L"AVIF", L"MP4"}, fmt_,
                      disabled)) {
        changed = true;
        hasResult_ = false;
        if (fmt_ != (int)Format::Mp4) settings_.format = fmt_;  // the 움짤 format the next clip starts with
    }
    if (mp4) ProbeEncoders();
    std::wstring blurb = (disabled & (1u << fmt_)) ? L"이 빌드에는 포함되어 있지 않습니다"
                         : !mp4                    ? std::wstring(FormatBlurb(fmt))
                         : ExportAudio()           ? L"어디서나 재생되는 동영상 · 소리 포함"
                                                   : L"어디서나 재생되는 동영상";
    ui_.Text(in.CutTop(20), blurb, 11.5f, TextFaint);
    in.CutTop(8);

    // 용량 맞추기: size, frame rate and quality (MP4: the bitrate) are picked to land under the size.
    {
        const bool missed = target && fitMissed_ && !fitting_;
        label(L"용량 맞추기", !target ? L"" : fitting_ ? L"맞추는 중…" : missed ? L"" : L"맞춤");
        if (ui_.Segmented(HashId("ex.target"), in.CutTop(32), {L"끔", L"5MB", L"10MB", L"15MB", L"20MB"},
                          settings_.targetIdx)) {
            changed = true;
            fitContent_.clear();  // fit again even when nothing else changed
            fitCorrection_ = 1;
            if (!settings_.targetIdx) fitBitrate_ = 0;
        }
        if (int mb = kTargetMB[settings_.targetIdx]) {
            const wchar_t* hint = missed     ? L"가장 작게 해도 넘습니다 · 구간을 줄여 보세요"
                                  : mp4      ? L"비트레이트를 이 용량에 맞춥니다"
                                  : mb == 20 ? L"크기·프레임·품질을 맞춤 · 디스코드 무료 한도"
                                  : mb == 15 ? L"크기·프레임·품질을 맞춤 · X(트위터) GIF 한도"
                                             : L"크기·프레임 레이트·품질을 이 용량에 맞춥니다";
            ui_.Text(in.CutTop(20), hint, 11.5f, missed ? Warning : TextFaint);
        }
        in.CutTop(8);
    }

    if (mp4) {
        // Codec and bitrate follow the video recording settings.
        Rect head = in.CutTop(20);
        ui_.Text(head, L"인코딩", 12.5f, TextDim);
        Rect link = head.CutRight(90);
        bool lh = ui_.Hover(link);
        if (ui_.Click(HashId("ex.vsettings"), link)) OpenSettings(kTabVideo);
        ui_.Text(link, L"세부 설정 ›", 12.f, lh ? AccentHi : Accent, Align(AlignEnd));
        in.CutTop(4);
        ui_.Text(in.CutTop(22), VideoSummary(), 12.5f, TextColor);
    } else {
        int& q = settings_.quality[fmt_];
        label(L"품질", q >= 100 && fmt == Format::WebP ? L"무손실" : std::to_wstring(q));
        float qv = (float)q;
        if (ui_.Slider(HashId("ex.q"), in.CutTop(22).Inset(7, 0), qv, 1.f, 100.f)) {
            q = (int)std::lround(qv);
            changed = true;
        }
    }
    in.CutTop(8);

    label(L"크기", util::Format(L"%d × %d", ow, oh));
    changed |= ui_.Segmented(HashId("ex.scale"), in.CutTop(32), {L"100%", L"75%", L"50%", L"33%", L"640px", L"480px"},
                             settings_.scaleIdx);
    in.CutTop(8);

    label(L"프레임 레이트", util::Format(L"%.0f fps", outFps));
    changed |= ui_.Segmented(HashId("ex.fps"), in.CutTop(32), {L"원본", L"30", L"25", L"20", L"15", L"10"},
                             settings_.outFpsIdx);
    in.CutTop(8);

    label(L"재생 속도", {});
    int prevSpeed = settings_.speedIdx;
    if (ui_.Segmented(HashId("ex.speed"), in.CutTop(32), {L"0.5×", L"1×", L"1.5×", L"2×"}, settings_.speedIdx)) {
        changed = true;
        // Re-base the preview clock so the playhead doesn't jump.
        std::swap(prevSpeed, settings_.speedIdx);
        size_t ph = playing_ ? PlayheadFromClock() : playhead_;
        std::swap(prevSpeed, settings_.speedIdx);
        playhead_ = ph;
        SetPlaying(playing_);
    }
    in.CutTop(8);
    if (!mp4) {
        Rect row = in.CutTop(32);
        ui_.Text(row, L"반복", 12.5f, TextDim);
        changed |= ui_.Segmented(HashId("ex.loop"), row.CutRight(std::min(186.f, row.W() - 60)),
                                 {L"계속", L"1번", L"3번"}, settings_.loopIdx);
        in.CutTop(6);
        changed |= ui_.ToggleRow(HashId("ex.hold"), in.CutTop(46).Inset(-10, 0), L"끝에서 잠깐 멈춤",
                                 settings_.holdLast, L"마지막 장면을 1초 더 보여 줍니다");
    } else if (clip_->HasAudio()) {
        auto* video = dynamic_cast<const VideoClip*>(clip_.get());
        if (video && !video->AudioExportable()) {
            bool off = false;
            ui_.ToggleRow(HashId("ex.audio"), in.CutTop(46).Inset(-10, 0), L"소리 포함", off,
                          util::Format(L"%.1fkHz 소리는 넣을 수 없습니다", video->AudioRate() / 1000.0), false);
        } else {
            bool slow = speed != 1.0;
            changed |= ui_.ToggleRow(HashId("ex.audio"), in.CutTop(46).Inset(-10, 0), L"소리 포함", exportAudio_,
                                     slow ? L"속도를 바꾸면 소리는 빠집니다" : L"", !slow);
        }
    }
    exportContentH_ = in.t - top;
    ui_.PopClip();
    if (maxScroll > 0) {
        float th = std::max(24.f, view.H() * view.H() / exportContentH_);
        float ty = view.t + (view.H() - th) * (exportScroll_ / maxScroll);
        ui_.Fill(Rect(view.r - 3, ty, view.r, ty + th), BorderStrong, 1.5f);
    }
    if (changed) {
        hasResult_ = false;
        settingsDirty_ = true;
    }

    if (!showResult) {
        std::wstring sum = mp4 ? util::Format(L"%d×%d · %.0ffps · %.1f초 · %s", ow, oh, outFps, outSec,
                                              ExportAudio() ? L"소리 포함" : L"소리 없음")
                               : util::Format(L"%d×%d · %.0ffps · %.1f초", ow, oh, outFps, outSec);
        // The estimate, once it matches what is on screen.
        bool current = estValid_ && estKey_ == EstimateKey();
        bool over = current && target && estBytes_ > target;
        if (!busy && FormatAvailable(fmt))
            sum += current ? L" · 약 " + util::FormatBytes(estBytes_) : std::wstring(L" · 크기 계산 중…");
        ui_.Text(summary, sum, 12.f, over ? Warning : current ? TextDim : TextFaint, Align(AlignCenter));
        if (!current && !busy) RequestWake(ui_.Now() + 0.25);
    }

    if (busy) {
        float p = exporter_.Progress();
        ui_.Fill(save, Surface2, 12);
        Rect bar = Rect(save.l + 14, save.CY() + 8, save.r - 14, save.CY() + 12);
        ui_.ProgressBar(bar, p, Accent);
        ui_.Text(Rect(save.l + 14, save.t + 4, save.r - 14, save.CY() + 4),
                 util::Format(L"%s 인코딩 중…  %d%%", FormatName(fmt), (int)(p * 100)), 13.f, TextColor, Bold());
        if (ui_.Button(HashId("ex.cancel"), saveAs, L"취소", BtnStyle::Ghost, icon::Close, true, 13.f))
            exporter_.Cancel();
        RequestWake(ui_.Now() + 0.1);
    } else {
        bool avail = !(disabled & (1u << fmt_));
        std::wstring label = std::wstring(FormatName(fmt)) + L"로 저장";
        if (ui_.Button(HashId("ex.save"), save, label, BtnStyle::Primary, icon::Save, avail, 15.f))
            StartExport(false);
        if (ui_.Button(HashId("ex.saveas"), saveAs, L"다른 이름으로 저장…", BtnStyle::Secondary, 0, avail, 13.f))
            StartExport(true);
    }

    if (showResult) {
        ui_.Fill(card, WithAlpha(Success, 0.08f), 12);
        ui_.Stroke(card.Inset(0.5f), WithAlpha(Success, 0.35f), 12);
        Rect top = Rect(card.l + 12, card.t + 4, card.r - 12, card.t + 32);
        ui_.Icon(Rect::XYWH(top.l, top.t, 18, top.H()), icon::Check, 13.f, Success);
        // The recording itself (just saved) or the last export.
        std::wstring size = (resultOriginal_ ? L"원본 · " : L"") + util::FormatBytes(lastResult_.bytes);
        float sw = ui_.Measure(size, 12.f) + 4;
        ui_.Text(Rect(top.l + 24, top.t, top.r - sw - 8, top.b), util::FileNameOf(lastResult_.path), 12.5f,
                 TextColor, Bold());
        ui_.Text(top, size, 12.f, TextDim, Align(AlignEnd));
        Rect btns = Rect(card.l + 10, card.b - 40, card.r - 10, card.b - 8);
        float bw = (btns.W() - 12) / 3;
        if (ui_.Button(HashId("res.folder"), Rect::XYWH(btns.l, btns.t, bw, btns.H()), L"폴더", BtnStyle::Secondary,
                       icon::Folder, true, 12.f))
            util::OpenFolderAndSelect(lastResult_.path);
        if (ui_.Button(HashId("res.copy"), Rect::XYWH(btns.l + bw + 6, btns.t, bw, btns.H()), L"복사",
                       BtnStyle::Secondary, icon::Copy, true, 12.f)) {
            if (util::CopyFileToClipboard(hwnd_, lastResult_.path))
                Toast(L"파일을 클립보드에 복사했습니다 · 메신저나 탐색기에 붙여넣으세요");
            else
                Toast(L"클립보드에 복사할 수 없습니다.", true);
        }
        if (ui_.Button(HashId("res.open"), Rect::XYWH(btns.l + 2 * (bw + 6), btns.t, bw, btns.H()), L"열기",
                       BtnStyle::Secondary, icon::OpenFile, true, 12.f))
            util::ShellOpen(lastResult_.path);
    }
}

// ---------------------------------------------------------------- settings

void App::SettingRow(Rect& area, std::wstring_view title, std::wstring_view desc, float controlW,
                     const std::function<void(Rect)>& control) {
    Rect row = area.CutTop(68);
    Rect ctl = row.CutRight(controlW);
    row.CutRight(16);
    ui_.Text(Rect(row.l, row.t + 12, row.r, row.t + 34), title, 14.f, TextColor, Bold());
    ui_.Text(Rect(row.l, row.t + 34, row.r, row.b - 10), desc, 12.f, TextFaint);
    control(Rect::Centered(ctl.CX(), row.CY(), ctl.W(), 36));
    ui_.Line(area.l, area.t - 0.5f, area.r, area.t - 0.5f, Border);
}

void App::SettingSection(Rect& area, std::wstring_view title) {
    Rect h = area.CutTop(46);
    TextStyle st = Bold();
    st.v = AlignEnd;
    ui_.Text(Rect(h.l, h.t, h.r, h.b - 10), title, 12.5f, AccentHi, st);
    ui_.Line(area.l, area.t - 0.5f, area.r, area.t - 0.5f, Border);
}

void App::OpenSettings(int tab) {
    if (screen_ != Screen::Settings) settingsReturn_ = screen_;
    screen_ = Screen::Settings;
    SetCapturingHotkey(0);
    micListed_ = false;
    if (tab >= 0 && tab != settings_.settingsTab) {
        settings_.settingsTab = tab;
        settingsScroll_ = 0;
        settingsDirty_ = true;
    }
}

void App::DrawSettings(Rect r) {
    r.CutTop(20);
    float w = std::min(980.f, r.W());
    Rect card = Rect(r.CX() - w / 2, r.t, r.CX() + w / 2, r.b);
    ui_.Fill(card, Surface, 14);
    ui_.Stroke(card.Inset(0.5f), Border, 14);
    Rect in = card.Inset(28, 20);
    ui_.Text(in.CutTop(36), L"설정", 20.f, TextColor, Bold());
    in.CutTop(10);

    // Tabs down the left; the one viewed last comes back next time. Ctrl+Tab steps through them.
    static constexpr struct {
        const wchar_t* name;
        wchar_t glyph;
    } kTabs[kTabCount] = {{L"일반", icon::Settings},
                          {L"녹화", icon::Record},
                          {L"동영상", icon::Video},
                          {L"소리", icon::Volume},
                          {L"정보", icon::Info}};
    int tab = settings_.settingsTab;
    Rect list = in.CutLeft(150);
    ui_.Line(list.r + 12.5f, list.t, list.r + 12.5f, list.b, Border);
    in.CutLeft(28);
    for (int i = 0; i < kTabCount; ++i) {
        Rect item = list.CutTop(40);
        list.CutTop(4);
        bool on = i == tab;
        if (ui_.Click(HashId("set.tab", i), item)) tab = i;
        float hl = ui_.Anim(HashId("set.tabHl", i), on ? 1.f : ui_.Hover(item) ? 0.5f : 0.f);
        if (hl > 0.01f) ui_.Fill(item, WithAlpha(Surface3, hl), 8);
        if (on) ui_.Fill(Rect::XYWH(item.l, item.CY() - 9, 3, 18), Accent, 1.5f);
        ui_.Icon(Rect::XYWH(item.l + 12, item.t, 20, item.H()), kTabs[i].glyph, 14.f, on ? AccentHi : TextDim);
        ui_.Text(Rect(item.l + 42, item.t, item.r - 8, item.b), kTabs[i].name, 14.f, on ? TextColor : TextDim,
                 on ? Bold() : TextStyle{});
    }
    if (ui_.In().ctrl && ui_.In().Key(VK_TAB)) tab = (tab + (ui_.In().shift ? kTabCount - 1 : 1)) % kTabCount;
    if (tab != settings_.settingsTab) {
        settings_.settingsTab = tab;
        settingsScroll_ = 0;
        settingsDirty_ = true;
        SetCapturingHotkey(0);
    }

    // Scroll view, one per tab
    Rect view = in;
    float maxScroll = std::max(0.f, settingsContentH_ - view.H());
    if (ui_.Hover(view) && ui_.In().wheel != 0) settingsScroll_ -= ui_.In().wheel * 90.f;
    settingsScroll_ = std::clamp(settingsScroll_, 0.f, maxScroll);
    float scroll = ui_.AnimFrom(HashId("set.scroll", tab), settingsScroll_, settingsScroll_, 22.f);
    ui_.PushClip(view);
    Rect area(view.l, view.t - scroll, view.r - (maxScroll > 0 ? 12 : 0), view.t - scroll + 100000);
    const float top = area.t;
    bool changed = false;
    switch (tab) {
        case kTabGeneral: SettingsGeneral(area, changed); break;
        case kTabRecord: SettingsRecord(area, changed); break;
        case kTabVideo: SettingsVideo(area, changed); break;
        case kTabAudio: SettingsAudio(area, changed); break;
        case kTabAbout: SettingsAbout(area); break;
    }
    settingsContentH_ = area.t - top + 8;
    ui_.PopClip();
    if (maxScroll > 0) {
        float th = std::max(30.f, view.H() * view.H() / settingsContentH_);
        float ty = view.t + (view.H() - th) * (scroll / maxScroll);
        ui_.Fill(Rect::XYWH(view.r - 4, ty, 4, th), WithAlpha(TextFaint, 0.5f), 2);
        // Edges show that the list continues.
        if (scroll > 1) ui_.Line(view.l, view.t + 0.5f, view.r, view.t + 0.5f, BorderStrong);
        if (scroll < maxScroll - 1) ui_.Line(view.l, view.b - 0.5f, view.r, view.b - 0.5f, BorderStrong);
    }
    if (changed) settingsDirty_ = true;
}

void App::SettingsGeneral(Rect& area, bool& changed) {
    SettingRow(area, L"저장 폴더", settings_.outDir, 196, [&](Rect c) {
        Rect a = c.CutLeft(94);
        c.CutLeft(8);
        if (ui_.Button(HashId("set.openDir"), a, L"열기", BtnStyle::Secondary, icon::Folder, true, 13.f)) {
            util::EnsureDir(settings_.outDir);
            util::ShellOpen(settings_.outDir);
        }
        if (ui_.Button(HashId("set.changeDir"), c, L"변경", BtnStyle::Secondary, 0, true, 13.f)) {
            std::wstring dir;
            if (util::PickFolder(hwnd_, settings_.outDir, dir)) {
                settings_.outDir = dir;
                changed = true;
            }
        }
    });

    int nameIdx = -1;
    for (int i = 0; i < (int)_countof(kNameTemplates); ++i)
        if (settings_.nameTemplate == kNameTemplates[i]) nameIdx = i;
    std::wstring nameDesc = (nameIdx < 0 ? L"settings.ini의 형식: " + settings_.nameTemplate + L" · 예: " : L"예: ") +
                            FileStem(L"리그 오브 레전드") + L".gif";
    SettingRow(area, L"파일 이름", nameDesc, 330, [&](Rect c) {
        if (ui_.Segmented(HashId("set.name"), c, {L"OzMaker_날짜", L"창 이름_날짜", L"날짜"}, nameIdx)) {
            settings_.nameTemplate = kNameTemplates[nameIdx];
            changed = true;
        }
    });

    SettingRow(area, L"Windows 시작 시 실행", L"로그인하면 창 없이 트레이에서 시작해 단축키를 바로 쓸 수 있습니다", 60,
               [&](Rect c) {
                   bool on = autostart_;
                   if (ui_.Toggle(HashId("set.autostart"), Rect::Centered(c.r - 22, c.CY(), 44, 24), on)) {
                       if (util::SetAutostart(on))
                           autostart_ = on;
                       else
                           Toast(L"시작 프로그램에 등록하지 못했습니다.", true);
                   }
               });

    int closeIdx = settings_.closeToTray ? 0 : 1;
    SettingRow(area, L"닫기 버튼", L"트레이로 숨기면 창을 닫아도 단축키로 녹화할 수 있습니다", 260, [&](Rect c) {
        if (ui_.Segmented(HashId("set.close"), c, {L"트레이로 숨기기", L"종료"}, closeIdx)) {
            settings_.closeToTray = closeIdx == 0;
            changed = true;
        }
    });

    SettingRow(area, L"효과음", L"녹화 시작 · 중지 · 저장 시 짧은 알림음", 60, [&](Rect c) {
        changed |= ui_.Toggle(HashId("set.sound"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.sounds);
    });

    SettingRow(area, L"새 버전 자동 확인", L"시작할 때와 하루 한 번 GitHub에서 확인하고 새 버전을 미리 받아 둡니다", 60,
               [&](Rect c) {
                   if (ui_.Toggle(HashId("set.autoUpdate"), Rect::Centered(c.r - 22, c.CY(), 44, 24),
                                  settings_.autoUpdate)) {
                       changed = true;
                       CheckForUpdate();
                   }
               });
}

void App::SettingsRecord(Rect& area, bool& changed) {
    std::wstring hkDesc = hotkeyOk_ ? L"게임 중에도 이 키로 녹화를 시작/중지합니다 (창 탭: 지금 보고 있는 창)"
                                    : L"⚠ 다른 프로그램이 사용 중이라 등록하지 못했습니다. 다른 조합을 지정하세요";
    auto hotkeyButton = [&](Rect c, int which, UINT mods, UINT vk) {
        bool on = capturingHotkey_ == which;
        std::wstring label = on ? L"키 조합을 누르세요…" : util::HotkeyToString(mods, vk);
        if (ui_.Button(HashId("set.hotkey", which), c, label, on ? BtnStyle::Primary : BtnStyle::Secondary,
                       icon::Keyboard, true, 13.f))
            SetCapturingHotkey(on ? 0 : which);
    };
    SettingRow(area, L"녹화 단축키", hkDesc, 230,
               [&](Rect c) { hotkeyButton(c, kHotkeyRecord, settings_.hkMods, settings_.hkVk); });

    SettingRow(area, L"일시정지 단축키",
               pauseHotkeyOk_ ? L"녹화 중에만 쓰입니다 · 녹화 중 표시의 ❚❚ 버튼으로도 멈추고 이어 갑니다"
                              : L"⚠ 지난 녹화 때 다른 프로그램이 쓰고 있어 쓰지 못했습니다. 다른 조합을 지정하세요",
               230, [&](Rect c) { hotkeyButton(c, kHotkeyPause, settings_.hkPauseMods, settings_.hkPauseVk); });

    SettingRow(area, L"최대 녹화 길이", L"이 길이가 되면 저장하고 편집 화면을 엽니다 · 움짤과 동영상 모두", 300,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.maxlen"), c, {L"끔", L"10분", L"30분", L"1시간"},
                                            settings_.maxLengthIdx);
               });

    SettingRow(area, L"시작 카운트다운", L"녹화를 누른 뒤 시작까지의 시간 · 영역 지정 막대에서도 바꿈 (단축키는 즉시 시작)", 230,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.cd"), c, {L"없음", L"3초", L"5초"}, settings_.countdown);
               });

    SettingRow(area, L"창 캡처 방식", L"자동: 전체화면 게임만 화면 캡처 · 검은 화면이 녹화되면 ‘화면’으로 바꾸세요",
               230, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.mode"), c, {L"자동", L"창", L"화면"}, settings_.captureMode);
               });

    SettingRow(area, L"녹화 중 표시", L"녹화 시간 · 일시정지 · 중지 버튼 · 영역 테두리 (녹화 결과에는 찍히지 않습니다)", 300,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.overlay"), c, {L"항상", L"전체화면 제외", L"숨김"},
                                            settings_.overlayMode);
               });

    SettingRow(area, L"클릭 강조", L"마우스를 누른 곳에 퍼지는 원을 함께 녹화합니다 · 왼쪽 버튼 노랑, 오른쪽 파랑", 60,
               [&](Rect c) {
                   changed |= ui_.Toggle(HashId("set.clickfx"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.clickFx);
               });
}

void App::SettingsVideo(Rect& area, bool& changed) {
    ProbeEncoders();
    const bool hevcOk = !encName_[1][1].empty() || !encName_[1][0].empty();
    SettingRow(area, L"코덱",
               hevcOk ? L"H.264: 어디서나 재생 · HEVC: 같은 화질에 용량 약 절반 (일부 기기 · 메신저 미지원)"
                      : L"H.264: 어디서나 재생 · 이 PC에는 HEVC 인코더가 없습니다",
               230, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.vcodec"), c, {L"H.264", L"HEVC"}, settings_.vCodec,
                                            hevcOk ? 0 : 0b10);
               });
    if (!hevcOk && settings_.vCodec == 1) settings_.vCodec = 0;

    const std::wstring& hwName = encName_[settings_.vCodec][1];
    std::wstring hwDesc = hwName.empty() ? L"그래픽카드 인코더를 찾지 못해 CPU로 인코딩합니다"
                                         : L"그래픽카드 인코더 사용: " + hwName + L" (게임 성능에 영향이 거의 없음)";
    SettingRow(area, L"하드웨어 인코딩", hwDesc, 60, [&](Rect c) {
        changed |= ui_.Toggle(HashId("set.vhw"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.vHardware);
    });

    SettingRow(area, L"프레임 레이트", L"게임은 60, 화면 설명 · 강의는 30이면 충분합니다", 230, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.vfps"), c, {L"24", L"30", L"60"}, settings_.vFpsIdx);
    });

    SettingRow(area, L"해상도", L"원본보다 크게 키우지는 않습니다 · 세로 화면은 짧은 쪽 기준", 330, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.vres"), c, {L"원본", L"2160p", L"1440p", L"1080p", L"720p"},
                                 settings_.vResIdx);
    });

    std::wstring brDesc = L"자동: 해상도 · 프레임에 맞춰 정합니다 (Mbps)";
    {
        RecTarget t;
        if (CurrentTarget(t, nullptr)) {
            int sw, sh, ow, oh;
            SourceSize(t, sw, sh);
            PlannedSize(sw, sh, ow, oh);
            UINT32 auto_ = Mp4Writer::AutoBitrate(ow, oh, kVideoFpsOptions[settings_.vFpsIdx], settings_.vCodec == 1);
            brDesc = util::Format(L"자동: 지금 대상(%d×%d)이면 약 %.1f Mbps · 1분에 %.0f MB", ow, oh, auto_ / 1e6,
                                  auto_ / 8.0 * 60 / 1e6);
        }
    }
    SettingRow(area, L"비트레이트", brDesc, 360, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.vbr"), c, {L"자동", L"6", L"10", L"16", L"25", L"40", L"60"},
                                 settings_.vBitrateIdx);
    });

    SettingRow(area, L"비트레이트 방식", L"VBR: 움직임에 따라 조절해 화질 우선 · CBR: 일정한 용량 (스트리밍 · 업로드 규격)",
               230, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.vrc"), c, {L"VBR", L"CBR"}, settings_.vRateCtrl);
               });

    SettingRow(area, L"키프레임 간격", L"짧을수록 편집 프로그램에서 탐색이 빠르고, 용량은 조금 늘어납니다", 230,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.vkey"), c, {L"1초", L"2초", L"5초"}, settings_.vKeyIdx);
               });

    SettingRow(area, L"인코딩 품질", L"고품질은 같은 용량에서 더 선명하지만 GPU/CPU를 더 씁니다", 260, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.vpreset"), c, {L"빠르게", L"균형", L"고품질"}, settings_.vPresetIdx);
    });
}

void App::SettingsAudio(Rect& area, bool& changed) {
    SettingRow(area, L"소리 녹음",
               AudioCapture::ProcessLoopbackSupported()
                   ? L"창 캡처: 그 창(프로그램)의 소리만 · 전체 화면 · 영역: 시스템 전체 소리"
                   : L"전체 화면 · 영역: 시스템 전체 소리 · 창 캡처는 Windows 10 2004 이상에서만 녹음됩니다",
               60, [&](Rect c) {
                   changed |= ui_.Toggle(HashId("set.audio"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.audio);
               });

    SettingRow(area, L"마이크 녹음", L"내 목소리를 동영상(MP4)에 함께 녹음합니다 · 위의 소리와 한 트랙으로 섞입니다", 60,
               [&](Rect c) {
                   changed |= ui_.Toggle(HashId("set.mic"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.mic);
               });
    if (!micListed_) {
        // Tests show a made-up device, not this PC's.
        micDevices_ = testWindowsOnly_ ? std::vector<AudioCapture::Device>{{L"test", L"마이크(USB 테스트 장치)", true}}
                                       : AudioCapture::InputDevices();
        micListed_ = true;
    }
    bool micKnown = settings_.micDevice.empty();
    for (const auto& d : micDevices_) micKnown |= d.id == settings_.micDevice;
    std::wstring micDesc = micDevices_.empty() ? L"연결된 마이크가 없습니다"
                           : !micKnown         ? L"고른 장치가 연결되어 있지 않습니다 · 연결하거나 다른 장치를 고르세요"
                                               : L"기본 장치로 두면 Windows에서 기본 장치를 바꿀 때 함께 바뀝니다";
    SettingRow(area, L"마이크 장치", micDesc, 300, [&](Rect c) {
        UiId id = HashId("set.micDevice");
        bool hov = ui_.Hover(c);
        if (ui_.Click(id, c) && MicMenu(c)) {
            changed = true;
            if (micTest_.Running()) StartMicTest();  // now on the new device
        }
        ui_.Fill(c, hov ? Surface3 : Surface2, 8);
        ui_.Stroke(c.Inset(0.5f), hov ? BorderStrong : Border, 8);
        Rect t(c.l + 12, c.t, c.r - 30, c.b);
        ui_.PushClip(t);
        ui_.Text(t, MicDeviceName(), 13.f, micKnown ? TextColor : Warning);
        ui_.PopClip();
        ui_.Icon(Rect(c.r - 30, c.t, c.r - 8, c.b), icon::ChevronDown, 10.f, TextDim);
    });
    SettingRow(area, L"마이크 음량", L"목소리가 작거나 크게 들리면 조절하세요 · 100%는 장치 그대로", 300, [&](Rect c) {
        Rect label = c.CutRight(52);
        float v = (float)settings_.micVolume;
        if (ui_.Slider(HashId("set.micVol"), c.Inset(8, 0), v, 0.f, 200.f)) {
            int nv = (int)std::lround(v / 5) * 5;  // in steps of 5 %
            changed |= nv != settings_.micVolume;
            settings_.micVolume = nv;
        }
        TextStyle st = Align(AlignEnd);
        st.tabular = true;
        ui_.Text(label, util::Format(L"%d%%", settings_.micVolume), 13.f, TextColor, st);
    });
    const bool testing = micTest_.Running() || micTestShow_ >= 0;
    SettingRow(area, L"마이크 확인",
               testing ? L"말해 보세요 · 녹음하지 않습니다 · 10초 뒤 저절로 멈춥니다"
                       : L"누르는 동안 마이크를 잠시 열어 소리 크기를 보여 줍니다 · 녹음하지 않습니다",
               300, [&](Rect c) {
                   Rect btn = c.CutRight(84);
                   if (ui_.Button(HashId("set.micTest"), btn.Inset(0, 4), testing ? L"그만" : L"테스트",
                                  BtnStyle::Secondary, testing ? icon::Stop : 0, !micDevices_.empty(), 13.f) &&
                       micTestShow_ < 0) {
                       if (testing)
                           StopMicTest();
                       else
                           StartMicTest();
                   }
                   // 16 steps over -60..0 dB: green, then yellow from -19 dB, red from -7.5 dB.
                   const int n = 16;
                   const float gap = 3, h = 14, w0 = c.l + 4, w = (c.W() - 16 - gap * (n - 1)) / n;
                   const float level = micTestShow_ >= 0 ? micTestShow_ : micLevel_;
                   for (int i = 0; i < n; ++i) {
                       Rect s = Rect::XYWH(w0 + i * (w + gap), c.CY() - h / 2, w, h);
                       D2D1_COLOR_F on = i >= n - 2 ? Rec : i >= n - 5 ? Warning : Success;
                       ui_.Fill(s, testing && level >= (i + 0.5f) / n ? on : Surface3, 2);
                   }
               });

    SettingRow(area, L"오디오 비트레이트", L"AAC · 160 kbps면 게임 · 음악 모두 충분합니다", 260, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.abr"), c, {L"96", L"128", L"160", L"192"}, settings_.aBitrateIdx);
    });

    SettingRow(area, L"샘플레이트", L"영상 편집에는 48 kHz가 표준입니다", 230, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.arate"), c, {L"44.1 kHz", L"48 kHz"}, settings_.aRateIdx);
    });
}

void App::SettingsAbout(Rect& area) {
    Rect head = area.CutTop(84);
    ui_.Logo(Rect::XYWH(head.l, head.CY() - 24, 48, 48));
    ui_.Text(Rect(head.l + 64, head.CY() - 24, head.r, head.CY() + 2), L"OzMaker", 20.f, TextColor, Bold());
    ui_.Text(Rect(head.l + 64, head.CY() + 2, head.r, head.CY() + 24), L"움짤 메이커 · 버전 " OZ_VERSION_WSTR, 13.f,
             TextDim);
    ui_.Line(area.l, area.t - 0.5f, area.r, area.t - 0.5f, Border);

    std::wstring formats;
    for (int i = 0; i < (int)Format::Mp4; ++i)
        if (FormatAvailable((Format)i)) formats += (formats.empty() ? L"" : L", ") + std::wstring(FormatName((Format)i));
    SettingRow(area, L"지원 형식", L"움짤: " + formats + L" · 동영상: MP4", 0, [](Rect) {});

    Updater::Status st = updater_.Get();
    std::wstring desc;
    switch (st.state) {
        case Updater::State::Idle: desc = L"아직 확인하지 않았습니다"; break;
        case Updater::State::Checking: desc = L"확인 중…"; break;
        case Updater::State::UpToDate: desc = L"최신 버전입니다"; break;
        case Updater::State::Downloading:
            desc = util::Format(L"새 버전 %s 받는 중… %d%%", st.version.c_str(), (int)(st.progress * 100));
            break;
        case Updater::State::Ready: desc = L"새 버전 " + st.version + L" 준비 완료 · 다시 시작하면 바뀝니다"; break;
        case Updater::State::Available:
            desc = L"새 버전 " + st.version + L" · 이 폴더에는 직접 바꿀 수 없어 내려받기 페이지에서 받아 주세요";
            break;
        case Updater::State::Failed: desc = L"확인하지 못했습니다: " + st.error; break;
    }
    if (st.state == Updater::State::Checking || st.state == Updater::State::Downloading) RequestWake(ui_.Now() + 0.2);
    SettingRow(area, L"업데이트", desc, 200, [&](Rect c) {
        if (st.state == Updater::State::Ready) {
            bool en = rec_ == RecState::Idle && !exporter_.Busy();
            if (ui_.Button(HashId("set.install"), c, L"다시 시작해서 업데이트", BtnStyle::Primary, 0, en, 13.f))
                InstallUpdate();
        } else if (st.state == Updater::State::Available) {
            if (ui_.Button(HashId("set.updPage"), c, L"내려받기 페이지", BtnStyle::Secondary, icon::OpenFile, true, 13.f))
                util::ShellOpen(Updater::ReleasePage(st.version));
        } else if (ui_.Button(HashId("set.check"), c, L"지금 확인", BtnStyle::Secondary, icon::Refresh,
                              !updater_.Busy(), 13.f)) {
            updater_.Check();
        }
    });

    SettingRow(area, L"프로젝트", L"소스 코드 · 바뀐 점 · 오픈소스 라이선스", 260, [&](Rect c) {
        Rect a = c.CutLeft((c.W() - 8) / 2);
        c.CutLeft(8);
        if (ui_.Button(HashId("set.github"), a, L"GitHub", BtnStyle::Secondary, icon::OpenFile, true, 13.f))
            util::ShellOpen(kRepoUrl);
        if (ui_.Button(HashId("set.licenses"), c, L"라이선스", BtnStyle::Secondary, 0, true, 13.f))
            util::ShellOpen(std::wstring(kRepoUrl) + L"/blob/main/THIRD-PARTY-NOTICES.md");
    });
}
