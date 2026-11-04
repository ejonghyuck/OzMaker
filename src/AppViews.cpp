#include "pch.h"
#include "App.h"
#include "AnimClip.h"
#include "Caption.h"
#include "Crash.h"
#include "Mp4Cut.h"
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

// A file name pattern with its tokens in the UI's language: both work, the field shows these.
std::wstring NameInLanguage(std::wstring_view p) {
    const int l = Language() == Lang::English ? 1 : 0;
    std::wstring s;
    for (size_t i = 0, len = 0; i < p.size();) {
        if (int k = NameTokenAt(p, i, len); k >= 0) {
            s += kNameTokens[k][l];
            i += len;
        } else {
            s += p[i++];
        }
    }
    return s;
}

// The line under a pattern of the user's own: what becomes of it, in Warning when it isn't what was written.
std::pair<std::wstring, bool> NameHint(const std::wstring& p) {
    if (p.find_first_not_of(L' ') == std::wstring::npos) return {tr(L"비워 두면 OzMaker_날짜로 저장합니다."), false};
    if (p.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos)
        return {tr(L"파일 이름에 쓸 수 없는 글자(\\ / : * ? \" < > |)는 _로 바뀝니다."), true};
    std::wstring odd;  // {…} that is no token
    for (size_t a = p.find(L'{'); a != std::wstring::npos; a = p.find(L'{', a + 1)) {
        size_t b = p.find_first_of(L"{}", a + 1), len = 0;
        if (b == std::wstring::npos) break;
        if (p[b] == L'}' && NameTokenAt(p, a, len) < 0) odd += (odd.empty() ? L"" : L" ") + p.substr(a, b - a + 1);
    }
    if (!odd.empty()) return {tr(L"모르는 이름표는 글자 그대로 들어갑니다: ") + odd, true};
    return {tr(L"같은 이름의 파일이 있으면 끝에 _2, _3을 붙입니다."), false};
}

const wchar_t* FormatBlurb(Format f) {
    switch (f) {
        case Format::Gif: return tr(L"어디서나 재생 · 256색");
        case Format::WebP: return tr(L"GIF보다 훨씬 작은 용량 · 대부분의 브라우저/메신저");
        case Format::Avif: return tr(L"최고 압축률 · 최신 브라우저");
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
    if (screen_ != Screen::Home) timerOpen_ = false;
    if (screen_ != Screen::Replay) rpAddOpen_ = false, rpProgMenu_ = rpFileMenu_ = rpRenaming_ = -1;
    const bool modal = question_.has_value();
    if (modal) menu_ = {};
    const bool rpFloat = rpAddOpen_ || rpProgMenu_ >= 0 || rpFileMenu_ >= 0;
    const bool popover = !modal && (filesOpen_ || updateOpen_ || timerOpen_ || menu_.owner || rpFloat);
    menu_.seen = false;
    // An Alert's buttons lie over the page (the record button is under it on Home): while the pointer is on the
    // alert, where it was drawn last frame, the page under it takes no input.
    const bool onAlert = toastSticky_ && ui_.Now() < toastUntil_ && toastRect_.Contains(ui_.In().mx, ui_.In().my);
    if (popover || modal || onAlert) ui_.SetInputBlocked(true);
    Rect r = client;
    DrawHeader(r.CutTop(60));
    r = r.Inset(20, 0);
    r.b -= 20;
    switch (screen_) {
        case Screen::Home: DrawHome(r); break;
        case Screen::Editor: DrawEditor(r); break;
        case Screen::Settings: DrawSettings(r); break;
        case Screen::Replay: DrawReplay(r); break;
    }
    // Floating surfaces last, each in a float: that is what keeps them above the page and the live window preview
    // (a DWM thumbnail, which DWM draws above everything here) out from under them. See Ui::BeginFloat.
    if (menu_.owner && !menu_.seen) menu_ = {};  // its field is gone (another screen, scrolled away)
    if (popover) {
        ui_.SetInputBlocked(false);
        ui_.BeginFloat();
        if (filesOpen_)
            DrawFilesPopover(client);
        else if (updateOpen_)
            DrawUpdatePopover(client);
        else if (timerOpen_)
            DrawTimerPopover(client);
        else if (menu_.owner)
            DrawMenuPopover(client);
        else if (rpAddOpen_)
            DrawReplayAddPopover(client);
        else if (rpFloat)
            DrawReplayMenus(client);
        ui_.EndFloat();
    }
    if (modal) {
        ui_.SetInputBlocked(false);
        ui_.BeginFloat();
        DrawQuestion(client);
        ui_.EndFloat();
    }
    ui_.SetInputBlocked(false);
    ui_.BeginFloat();
    DrawToast(client);
    ui_.EndFloat();
    if (dragKind_ != DragKind::None) {
        ui_.BeginFloat();
        DrawDropCue(client);
        ui_.EndFloat();
    }
}

void App::DrawQuestion(Rect client) {
    const auto& in = ui_.In();
    const Question& q = *question_;
    const float a = ui_.AnimFrom(HashId("ask", questionSeq_), 1.f, 0.f, 18.f);
    ui_.Fill(client, WithAlpha(P.Scrim, 0.55f * a));
    const float W = std::min(q.lists.empty() ? 440.f : 520.f, client.W() - 40), pad = 22, iconW = 40;
    TextStyle body;
    body.wrap = true;
    body.v = AlignStart;
    auto height = [&](std::wstring_view s, float w) {
        DWRITE_TEXT_METRICS m{};
        auto lay = Fonts::Layout(s, 13.f, body, w, 1000);
        return lay && SUCCEEDED(lay->GetMetrics(&m)) ? std::ceil(m.height) : 20.f;
    };
    const float textW = W - pad * 2 - iconW, bullet = 14;
    float bodyH = 0;
    if (q.lists.empty()) bodyH = height(q.body, textW);
    for (const auto& [head, lines] : q.lists) {
        bodyH += 24;
        for (const std::wstring& line : lines) bodyH += height(line, textW - bullet) + 4;
        bodyH += 8;
    }
    const float H = pad + 26 + 6 + bodyH + 24 + 38 + pad;
    Rect box = Rect::Centered(client.CX(), client.CY() - 24 + (1 - a) * 10, W, H);
    ui_.Fill(box.Offset(0, 8), WithAlpha(Shadow, 0.45f * a), 16);
    ui_.Fill(box, WithAlpha(Surface2, a), 14);
    ui_.Stroke(box.Inset(0.5f), WithAlpha(BorderStrong, a), 14);
    Rect c = box.Inset(pad);
    const D2D1_COLOR_F tint = q.danger && !q.glyph ? RecHi : Accent;
    Rect mark = Rect::XYWH(c.l, c.t - 2, 30, 30);
    ui_.Circle(mark.CX(), mark.CY(), 15, WithAlpha(tint, 0.18f * a));
    ui_.Icon(mark, q.glyph ? q.glyph : q.danger ? icon::Warning : icon::Check, 14.f, WithAlpha(tint, a));
    Rect text(c.l + iconW, c.t, c.r, c.b);
    ui_.Text(text.CutTop(26), q.title, 15.f, WithAlpha(TextColor, a), Bold());
    text.CutTop(6);
    if (q.lists.empty()) ui_.Text(text.CutTop(bodyH), q.body, 13.f, WithAlpha(TextDim, a), body);
    for (const auto& [head, lines] : q.lists) {
        TextStyle h = Bold();
        h.v = AlignStart;
        ui_.Text(text.CutTop(24), head, 13.f, WithAlpha(TextColor, a), h);
        for (const std::wstring& line : lines) {
            Rect l = text.CutTop(height(line, textW - bullet));
            ui_.Text(Rect(l.l, l.t, l.l + bullet, l.t + 18), L"•", 13.f, WithAlpha(TextFaint, a), Align(AlignStart, AlignStart));
            ui_.Text(Rect(l.l + bullet, l.t, l.r, l.b), line, 13.f, WithAlpha(TextDim, a), body);
            text.CutTop(4);
        }
        text.CutTop(8);
    }

    Rect btns = c.CutBottom(38);
    const float yesW = std::max(96.f, ui_.Measure(q.yes, 13.f, kSemibold) + 40);
    Rect yes = btns.CutRight(yesW);
    btns.CutRight(8);
    Rect no = btns.CutRight(88);
    bool said = ui_.Button(HashId("ask.yes"), yes, q.yes, q.danger ? BtnStyle::Danger : BtnStyle::Primary, 0, true,
                           13.f) ||
                in.Key(VK_RETURN);
    bool not_ = ui_.Button(HashId("ask.no"), no, tr(L"취소"), BtnStyle::Secondary, 0, true, 13.f) ||
                in.Key(VK_ESCAPE) || (in.pressed && !box.Contains(in.mx, in.my));
    if (said) {
        answer_ = std::move(question_->onYes);
        question_.reset();
        PostMessageW(hwnd_, WM_APP_ANSWER, 0, 0);
    } else if (not_) {
        question_.reset();
    }
    if (said || not_) Invalidate();
}

void App::DrawHeader(Rect r) {
    Rect h = r.Inset(20, 0);
    ui_.Logo(Rect::XYWH(h.l, h.CY() - 14, 28, 28));
    float tw = ui_.Measure(L"OzMaker", 17.f, kBold);
    TextStyle title;
    title.weight = kBold;
    ui_.Text(Rect(h.l + 40, h.t, h.l + 40 + tw + 4, h.b), L"OzMaker", 17.f, TextColor, title);
    ui_.Text(Rect(h.l + 48 + tw, h.t + 2, h.l + 200 + tw, h.b), tr(L"움짤 메이커"), 12.5f, TextFaint);

    // The tabs, and in their place while recording, how long it has been going. Drawn first: Tab and screen readers
    // go in drawing order, so the header reads left to right.
    if (rec_ != RecState::Recording) {
        if (screen_ != Screen::Settings) DrawTabs(r);
    } else {
        std::wstring t = (recPaused_ ? tr(L"일시정지  ") : L"REC  ") + util::FormatClock(RecElapsed(), true);
        const int timer = settings_.TimerMinutes() * 60;
        if (timer > 0) t += util::Format(L"  / %d:%02d:%02d", timer / 3600, timer / 60 % 60, timer % 60);
        const D2D1_COLOR_F tint = recPaused_ ? Warning : Rec;
        Rect pill = Rect::Centered(r.CX(), r.CY(), (recPaused_ ? 148.f : 120.f) + (timer > 0 ? 76.f : 0.f), 30);
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

    // The settings button (돌아가기 in the settings) has the corner; the rest goes left of it.
    Rect right = h;
    const bool settings = screen_ == Screen::Settings;
    Rect corner = right.CutRight(settings ? 110.f : 36.f);
    if (!settings) right.CutRight(8);
    DrawUpdateButton(right);
    if (!settings) DrawFilesButton(right);
    if (!settings) {
        if (ui_.IconButton(HashId("hdr.settings"), Rect::Centered(corner.CX(), corner.CY(), 36, 36), icon::Settings,
                           tr(L"설정")))
            OpenSettings();
    } else if (ui_.Button(HashId("hdr.back"), Rect::Centered(corner.CX(), corner.CY(), 110, 36), tr(L"돌아가기"),
                          BtnStyle::Ghost, icon::Back) ||
               ui_.In().Key(VK_ESCAPE)) {
        SetCapturingHotkey(0);
        hkConflict_ = {};
        if (settingsReturn_ == Screen::Editor) {
            ResumeEditor();
        } else if (settingsReturn_ == Screen::Replay) {
            OpenReplayTab();
        } else {
            screen_ = Screen::Home;
            RefreshWindows();
        }
    }
    ui_.Line(r.l, r.b - 0.5f, r.r, r.b - 0.5f, Border, 1.f);
}

void App::DrawTabs(Rect r) {
    const float tabW = 120, tabH = 30, pad = 3, gap = 2;
    Rect box = Rect::Centered(r.CX(), r.CY(), tabW * 3 + gap * 2 + pad * 2, tabH + pad * 2);
    ui_.Fill(box, Surface, 11);
    ui_.Stroke(box.Inset(0.5f), Border, 11);
    // Not while a countdown runs or a file is being saved: the editor has to stay as it is for that.
    const bool en = rec_ == RecState::Idle && !exporter_.Busy();
    ui_.Declare(HashId("hdr.tabs"), box, Role::Group, tr(L"녹화 · 편집 · 리플레이"));
    const RpState rp = ReplayState();
    for (int i = 0; i < 3; ++i) {
        const Screen tab = i == 0 ? Screen::Home : i == 1 ? Screen::Editor : Screen::Replay;
        const wchar_t* label = i == 0 ? tr(L"녹화") : i == 1 ? tr(L"편집") : tr(L"리플레이");
        Rect t = Rect::XYWH(box.l + pad + i * (tabW + gap), box.t + pad, tabW, tabH);
        const UiId id = HashId("hdr.tab", (uint32_t)i);
        const bool on = screen_ == tab;
        const bool clicked = ui_.Click(id, t, en && !on);
        const std::wstring spoken = i < 2 ? std::wstring(label)
                                    : rp == RpState::Holding ? std::wstring(label) + tr(L", 담는 중")
                                    : rp == RpState::Waiting ? std::wstring(label) + tr(L", 기다리는 중")
                                                             : std::wstring(label);
        if (UiNode* n = ui_.Describe(id, spoken, Role::Tab))
            n->enabled = en, n->on = on, n->pos = i, n->count = 3, n->group = HashId("hdr.tabs");
        const float hv = ui_.Anim(id, en && !on && ui_.Hover(t) ? 1.f : 0.f);
        if (on) {
            ui_.Fill(t, Contrast() ? Accent : Surface3, 8);
            ui_.Stroke(t.Inset(0.5f), Contrast() ? Accent : BorderStrong, 8);
        }
        const D2D1_COLOR_F fg = on ? (Contrast() ? OnFill : TextColor) : en ? Mix(TextDim, TextColor, hv) : P.Disabled;
        const float lw = ui_.Measure(label, 13.f, kSemibold), iw = 14, x = std::round(t.CX() - (iw + 7 + lw) / 2);
        if (i == 0)
            ui_.Circle(x + iw / 2, t.CY(), 4.5f, fg);
        else
            ui_.Icon(Rect::XYWH(x, t.t, iw, t.H()), i == 1 ? icon::Cut : icon::Replay, 13.f, fg);
        if (i == 2 && (rp == RpState::Holding || rp == RpState::Waiting)) {
            // Red while it holds what goes by, grey while it waits for a program.
            ui_.Circle(t.r - 12, t.t + 9, 3.5f, Contrast() ? fg : rp == RpState::Holding ? Rec : TextFaint);
            ui_.Tooltip(id, t, rp == RpState::Holding ? tr(L"리플레이 담는 중") : tr(L"리플레이 기다리는 중"));
        }
        ui_.Text(Rect(x + iw + 7, t.t, t.r, t.b), label, 13.f, fg, Bold());
        if (i == 1 && clip_) {
            // A file is open in the 편집 tab, waiting there.
            if (!on) ui_.Circle(t.r - 19, t.t + 10, 3.f, AccentHi);
            ui_.Tooltip(id, t, tr(L"열린 파일: ") + ClipTitle());
        }
        if (clicked) {
            if (i == 0) {
                if (screen_ == Screen::Editor)
                    CloseEditor();
                else
                    screen_ = Screen::Home, RefreshWindows();
            } else if (i == 1) {
                ResumeEditor();
            } else {
                OpenReplayTab();
            }
        }
    }
}

void App::DrawToast(Rect client) {
    double now = ui_.Now();
    bool visible = !toast_.empty() && now < toastUntil_;
    float a = ui_.Anim(HashId("toast"), visible ? 1.f : 0.f, 12.f);
    if (visible && !toastSticky_) RequestWake(toastUntil_);
    if (a <= 0.01f) return;
    // An Alert keeps its buttons at the right: the settings tab that fixes it, if any, then close.
    static const wchar_t* const kTabNames[kTabCount] = {L"일반", L"녹화", L"동영상", L"소리", L"정보", L"리플레이"};
    const std::wstring act = toastSticky_ && toastTab_ >= 0 && toastTab_ < kTabCount
                                 ? util::Format(tr(L"%s 설정 열기"), tr(kTabNames[toastTab_]))
                                 : std::wstring();
    const float actW = act.empty() ? 0 : ui_.Measure(act, 12.5f) + 24;
    const float btnsW = !toastSticky_ ? 0 : act.empty() ? 34 : actW + 40;
    // One line when it fits, else wrapped onto as many as it needs (error messages can be long).
    const float maxW = std::min(toastSticky_ ? 640.f : 600.f, client.W() - 80),
                lineW = ui_.Measure(toast_, 13.f) + btnsW;
    float tw = std::min(maxW, lineW + 60), th = 40;
    TextStyle st;
    if (lineW + 60 > maxW) {
        st.wrap = true;
        st.v = AlignStart;
        DWRITE_TEXT_METRICS m{};
        auto lay = Fonts::Layout(toast_, 13.f, st, tw - 58 - btnsW, 1000);
        if (lay && SUCCEEDED(lay->GetMetrics(&m))) th = std::max(40.f, std::ceil(m.height) + 22);
    }
    Rect r = Rect::Centered(client.CX(), client.b - 24 - th / 2 + (1 - a) * 16, tw, th);
    toastRect_ = r;
    float radius = std::min(20.f, th / 2);
    ui_.Fill(r.Offset(0, 3), WithAlpha(Shadow, 0.35f * a), radius);
    ui_.Fill(r, WithAlpha(toastError_ ? P.ToastError : P.Toast, a), radius);
    ui_.Stroke(r.Inset(0.5f), WithAlpha(toastError_ ? Rec : BorderStrong, 0.7f * a), radius);
    ui_.Icon(Rect::XYWH(r.l + 12, r.t, 24, 40), toastError_ ? icon::Warning : icon::Check, 14.f,
             WithAlpha(toastError_ ? RecHi : Success, a));
    ui_.Text(st.wrap ? Rect(r.l + 42, r.t + 11, r.r - 16 - btnsW, r.b - 11) : Rect(r.l + 42, r.t, r.r - 16 - btnsW, r.b),
             toast_, 13.f, WithAlpha(TextColor, a), st);
    if (!toastSticky_ || !visible) return;
    Rect close = Rect::XYWH(r.r - 6 - 28, r.t + 6, 28, 28);
    if (ui_.IconButton(HashId("toast.close"), close, icon::Close, tr(L"닫기"), true, 12.f)) toastUntil_ = 0;
    if (!act.empty() &&
        ui_.Button(HashId("toast.act"), Rect::XYWH(close.l - 6 - actW, r.t + 6, actW, 28), act, BtnStyle::Secondary, 0,
                   true, 12.5f)) {
        toastUntil_ = 0;
        OpenSettings(toastTab_);
    }
}

// ---------------------------------------------------------------- files

void App::DrawFilesButton(Rect& right) {
    // The count: recordings that would be lost on exit.
    const std::wstring count = held_.empty() ? L"" : std::to_wstring(held_.size());
    const float cw = count.empty() ? 0 : std::max(18.f, ui_.Measure(count, 11.f, kBold) + 10);
    const float w = 28 + 13 + 8 + ui_.Measure(tr(L"최근 파일"), 13.f, kSemibold) + (cw ? cw + 8 : 0);
    Rect b = right.CutRight(w);
    filesBtn_ = Rect::Centered(b.CX(), b.CY(), w, 36);
    const bool en = rec_ == RecState::Idle && !exporter_.Busy();
    Rect label = filesBtn_;
    if (cw) label.r -= cw + 8;
    const UiId id = HashId("hdr.files");
    const bool clicked = ui_.Click(id, filesBtn_, en);
    ui_.Describe(id, held_.empty() ? tr(L"최근 파일") : util::Format(tr(L"최근 파일, 저장하지 않은 녹화 %zu개"), held_.size()));
    const float hv = ui_.Anim(id, en && ui_.Hover(filesBtn_) ? 1.f : 0.f);
    ui_.Fill(filesBtn_, filesOpen_ ? Surface2 : WithAlpha(Surface3, hv), 9);
    if (filesOpen_) ui_.Stroke(filesBtn_.Inset(0.5f), Border, 9);
    const D2D1_COLOR_F fg = !en ? TextFaint : filesOpen_ ? TextColor : Mix(TextDim, TextColor, hv);
    ui_.Icon(Rect::XYWH(filesBtn_.l + 14, filesBtn_.t, 13, filesBtn_.H()), icon::OpenFile, 13.f, fg);
    ui_.Text(Rect(filesBtn_.l + 35, filesBtn_.t, label.r, filesBtn_.b), tr(L"최근 파일"), 13.f, fg, Bold());
    if (cw) {
        Rect c = Rect::XYWH(filesBtn_.r - 14 - cw, filesBtn_.CY() - 9, cw, 18);
        ui_.Fill(c, en ? Badge : Surface3, 9);
        TextStyle st = Bold(AlignCenter);
        st.weight = kBold;
        ui_.Text(c, count, 11.f, en ? OnFill : TextFaint, st);
        ui_.Tooltip(id, filesBtn_, util::Format(tr(L"저장하지 않은 녹화 %zu개"), held_.size()));
    }
    if (clicked) {
        filesOpen_ = true;
        updateOpen_ = false;
        filesScroll_ = 0;
        RefreshRecent();
    }
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
    ui_.Describe(id, ready ? tr(L"업데이트 준비 완료") : tr(L"새 버전이 나왔습니다"));
    float h = ui_.Anim(id, hot || updateOpen_ ? 1.f : 0.f);
    ui_.Circle(updateBtn_.CX(), updateBtn_.CY(), 13.f + h, Mix(P.UpdateFill, P.UpdateFillHi, h * 0.6f));
    // A bold download arrow over a base line (the MDL2 glyph is too thin at this size).
    const float x = updateBtn_.CX(), y = updateBtn_.CY();
    const D2D1_COLOR_F ink = P.UpdateInk;
    ui_.Line(x, y - 6.5f, x, y + 3.f, ink, 2.f);
    ui_.Line(x - 4.5f, y - 1.f, x + 0.7f, y + 4.2f, ink, 2.f);
    ui_.Line(x + 4.5f, y - 1.f, x - 0.7f, y + 4.2f, ink, 2.f);
    ui_.Line(x - 5.5f, y + 7.f, x + 5.5f, y + 7.f, ink, 2.f);
    ui_.Tooltip(id, updateBtn_, ready ? tr(L"업데이트 준비 완료!") : tr(L"새 버전이 나왔습니다"));
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
    ui_.Fill(box.Offset(0, 6), WithAlpha(Shadow, 0.45f), 14);
    ui_.Fill(box, Surface2, 12);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 12);
    Rect c = box.Inset(16, 14);
    Rect title = c.CutTop(24);
    ui_.Icon(Rect::XYWH(title.l, title.t, 18, title.H()), ready ? icon::Check : icon::Download, 13.f, Success);
    ui_.Text(Rect(title.l + 26, title.t, title.r, title.b),
             ready ? util::Format(tr(L"새 버전 %s 준비 완료"), st.version.c_str()) : tr(L"새 버전이 나왔습니다: ") + st.version, 14.f, TextColor,
             Bold());
    Rect sub = c.CutTop(22);
    std::wstring now = util::Format(tr(L"지금 쓰는 버전 %s · "), OZ_VERSION_WSTR);
    float nw = ui_.Measure(now, 12.f);
    ui_.Text(sub, now, 12.f, TextDim);
    Rect link = Rect(sub.l + nw, sub.t, sub.l + nw + ui_.Measure(tr(L"바뀐 점 보기"), 12.f) + 4, sub.b);
    bool lh = ui_.Hover(link);
    if (ui_.Click(HashId("upd.notes"), link)) util::ShellOpen(Updater::ReleasePage(st.version));
    ui_.Describe(HashId("upd.notes"), tr(L"바뀐 점 보기"));
    ui_.Text(link, tr(L"바뀐 점 보기"), 12.f, lh ? LinkHi : Link);
    c.CutTop(6);
    if (ready && busy) {
        ui_.Text(c.CutTop(22), tr(L"녹화 · 내보내기가 끝나면 업데이트할 수 있습니다"), 12.f, Warning);
    } else if (!ready) {
        ui_.Text(c.CutTop(22), tr(L"이 폴더에는 직접 바꿀 수 없어 내려받기 페이지에서 받아야 합니다"), 11.5f, TextFaint);
    }
    Rect btns = c.CutBottom(36);
    Rect later = btns.CutRight(84);
    btns.CutRight(8);
    if (ready) {
        if (ui_.Button(HashId("upd.install"), btns, tr(L"다시 시작해서 업데이트"), BtnStyle::Primary, 0, !busy, 13.f))
            InstallUpdate();
    } else if (ui_.Button(HashId("upd.page"), btns, tr(L"내려받기 페이지 열기"), BtnStyle::Primary, 0, true, 13.f)) {
        util::ShellOpen(Updater::ReleasePage(st.version));
        updateOpen_ = false;
    }
    if (ui_.Button(HashId("upd.later"), later, tr(L"나중에"), BtnStyle::Secondary, 0, true, 13.f)) updateOpen_ = false;
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
    ui_.Fill(box.Offset(0, 6), WithAlpha(Shadow, 0.45f), 14);
    ui_.Fill(box, Surface2, 12);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 12);

    Rect body = box.Inset(8, 8);
    Rect foot = body.CutBottom(footH - 8);
    Rect list = body;
    float maxScroll = std::max(0.f, listH - list.H());
    if (!outside && in.wheel != 0) filesScroll_ -= in.wheel * 60;
    ui_.KeepInView(list, filesScroll_);
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
        ui_.Text(Rect(text.l, r.CY() + 1, text.r, r.b - 7), sub, 12.f, TextFaint);
        // Buttons first: they take the press before the row does.
        for (int k = 0; k < buttons; ++k) onButton(k, Rect::XYWH(r.r - 8 - (buttons - k) * 34, r.CY() - 15, 30, 30));
        const bool clicked = ui_.Click(id, r);
        ui_.Describe(id, name + L", " + sub, Role::Item);
        return clicked;
    };

    if (!held_.empty()) {
        section(tr(L"저장하지 않은 녹화"));
        for (size_t i = 0; i < held_.size(); ++i) {
            Rect r = cur.CutTop(rowH);
            const HeldClip& h = held_[i];
            const bool current = h.clip == clip_;
            const bool editing = current && screen_ == Screen::Editor;
            std::wstring sub = util::FormatClock(h.clip->Seconds(), true) + L" · " +
                               util::FormatAgo(util::NowSeconds() - h.when) + (editing ? tr(L" · 편집 중") : L"");
            bool drop = false;
            bool open = row(HashId("files.held", (uint32_t)i), r, icon::Record, Accent2, h.name, sub, editing ? 0 : 1,
                            [&](int, Rect b) {
                                drop = ui_.IconButton(HashId("files.drop", (uint32_t)i), b, icon::Delete, tr(L"버리기"));
                            });
            if (drop) {
                ConfirmDropHeld(i);
                break;
            }
            if (open) {
                OpenHeld(i);
                break;
            }
        }
    }
    section(tr(L"최근 파일"));
    if (recentFiles_.empty()) {
        Rect r = cur.CutTop(56);
        ui_.Text(r.Inset(10, 0), tr(L"저장하거나 연 파일이 여기에 표시됩니다."), 12.5f, TextFaint);
    }
    for (size_t i = 0; i < recentFiles_.size(); ++i) {
        Rect r = cur.CutTop(rowH);
        const RecentFile& f = recentFiles_[i];
        int clicked = -1;
        bool open = row(HashId("files.recent", (uint32_t)i), r, f.video ? icon::Video : icon::Photo,
                        f.video ? Accent : Success, util::FileNameOf(f.path),
                        util::FormatBytes(f.bytes) + L" · " + util::FormatAgo(f.age), 2, [&](int k, Rect b) {
                            if (k == 0 && ui_.IconButton(HashId("files.folder", (uint32_t)i), b, icon::Folder, tr(L"폴더에서 보기")))
                                clicked = 0;
                            if (k == 1 && ui_.IconButton(HashId("files.copy", (uint32_t)i), b, icon::Copy, tr(L"파일 복사")))
                                clicked = 1;
                        });
        if (ui_.Hover(r)) ui_.Tooltip(HashId("files.tip", (uint32_t)i), r, f.path);
        if (clicked == 0) {
            util::OpenFolderAndSelect(f.path);
        } else if (clicked == 1) {
            bool ok = util::CopyFileToClipboard(hwnd_, f.path);
            Toast(ok ? tr(L"파일을 복사했습니다. 메신저나 폴더에 붙여넣으세요.") : tr(L"복사하지 못했습니다."), !ok);
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
    if (ui_.Button(HashId("files.open"), btn, tr(L"동영상 열기…"), BtnStyle::Secondary, icon::OpenFile)) BrowseForVideo();
    ui_.Text(fb.Inset(14, 0), tr(L"전체 목록은 편집 탭에 있습니다"), 12.f, TextFaint, Align(AlignEnd));
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
    ui_.Name(tr(L"녹화할 대상"));
    if (ui_.Segmented(HashId("home.source"), in.CutTop(38), {tr(L"창"), tr(L"전체 화면"), tr(L"영역")}, src, idle ? 0 : 0b111) &&
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
    ui_.Text(head, tr(L"녹화할 창"), 15.f, TextColor, Bold());
    Rect pickR = head.CutRight(124);
    {
        UiId id = HashId("home.pick");
        bool clicked = ui_.Click(id, pickR);
        ui_.Describe(id, tr(L"화면에서 선택"));
        float h = ui_.Anim(id, ui_.Hover(pickR) ? 1.f : 0.f);
        ui_.Fill(pickR, Mix(Surface2, Surface3, h), 9);
        ui_.Stroke(pickR.Inset(0.5f), Border, 9);
        ui_.Crosshair(pickR.l + 20, pickR.CY(), 14, Mix(TextDim, Accent, 0.6f + 0.4f * h));
        ui_.Text(Rect(pickR.l + 34, pickR.t, pickR.r, pickR.b), tr(L"화면에서 선택"), 13.f, TextColor, Bold());
        ui_.Tooltip(id, pickR, tr(L"화면 위의 창을 직접 클릭해서 고릅니다"));
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
                       tr(L"목록 새로고침"), true, 14.f))
        RefreshWindows();

    in.CutTop(10);
    Rect hint = in.CutBottom(22);
    ui_.Text(hint, tr(L"창을 더블클릭하면 바로 녹화합니다"), 12.f, TextFaint, Align(AlignCenter, AlignEnd));
    in.CutBottom(6);

    Rect list = in;
    const float itemH = 58, step = 62;
    float content = windows_.size() * step - 4;
    float maxScroll = std::max(0.f, content - list.H());
    bool inList = ui_.Hover(list);
    if (inList && ui_.In().wheel != 0) listScroll_ -= ui_.In().wheel * step * 1.5f;
    ui_.KeepInView(list, listScroll_);
    listScroll_ = std::clamp(listScroll_, 0.f, maxScroll);
    float scroll = ui_.Anim(HashId("home.scroll"), listScroll_, 22.f);

    if (windows_.empty()) {
        ui_.Text(list, tr(L"녹화할 수 있는 창이 없습니다"), 13.f, TextFaint, Align(AlignCenter));
        return;
    }
    ui_.PushClip(list);
    auto* dc = gfx_.DC();
    for (size_t i = 0; i < windows_.size(); ++i) {
        const WinInfo& w = windows_[i];
        Rect item = Rect::XYWH(list.l, list.t + i * step - scroll, list.W() - (maxScroll > 0 ? 8 : 0), itemH);
        UiId id = HashId("win", (uint32_t)(uintptr_t)w.hwnd);
        ui_.Declare(id, item, Role::Item, w.title).on = w.hwnd == target_;  // Tab reaches it scrolled out too
        if (item.b < list.t || item.t > list.b) continue;
        bool hov = ui_.Hover(item);
        if (ui_.Click(id, item)) SelectTarget(w.hwnd);
        if (hov && ui_.In().dblclick) StartRecording(WindowTarget(w.hwnd), false);
        bool sel = w.hwnd == target_;
        float h = ui_.Anim(id, hov ? 1.f : 0.f);
        ui_.Fill(item, sel ? (Contrast() ? Accent : Mix(Surface2, Accent, 0.16f)) : WithAlpha(Surface2, h), 10);
        if (sel) ui_.Stroke(item.Inset(0.75f), WithAlpha(Accent, 0.85f), 10, 1.5f);
        const bool onHighlight = sel && Contrast();

        Rect ic = Rect::XYWH(item.l + 12, item.CY() - 16, 32, 32);
        if (ID2D1Bitmap* bmp = IconFor(w)) {
            ui_.Painted(ic);
            dc->DrawBitmap(bmp, ic, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        } else {
            ui_.Icon(ic, icon::AppWindow, 20.f, onHighlight ? OnFill : TextDim);
        }

        ui_.Text(Rect(item.l + 56, item.t + 8, item.r - 12, item.t + 30), w.title, 13.5f,
                 onHighlight ? OnFill : TextColor, Bold());
        std::wstring sub = w.exeName.empty() ? L"" : w.exeName;
        if (!w.minimized) {
            sub += util::Format(L"%s%d×%d", sub.empty() ? L"" : L" · ", w.frame.right - w.frame.left,
                                w.frame.bottom - w.frame.top);
        }
        if (w.fullscreen) sub += tr(L" · 전체화면");
        if (w.minimized) sub += tr(L" · 최소화됨");
        ui_.Text(Rect(item.l + 56, item.t + 30, item.r - 12, item.b - 8), sub, 12.f, onHighlight ? OnFill : TextFaint);
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
    ui_.Text(head, tr(L"녹화할 화면"), 15.f, TextColor, Bold());
    Rect refR = head.CutRight(34);
    if (ui_.IconButton(HashId("mon.refresh"), Rect::Centered(refR.CX(), refR.CY(), 34, 34), icon::Refresh,
                       tr(L"모니터 다시 찾기"), true, 14.f))
        monitors_ = EnumerateMonitors();
    in.CutTop(10);
    Rect hint = in.CutBottom(22);
    ui_.Text(hint, tr(L"화면을 더블클릭하면 바로 녹화합니다"), 12.f, TextFaint, Align(AlignCenter, AlignEnd));
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
        if (UiNode* n = ui_.Describe(id, util::Format(tr(L"모니터 %d"), m.number) + (m.name.empty() ? L"" : L" · " + m.name),
                                     Role::Item))
            n->on = isSel;
        float h = ui_.Anim(id, hov ? 1.f : 0.f);
        const bool onHighlight = isSel && Contrast();
        ui_.Fill(item, isSel ? (Contrast() ? Accent : Mix(Surface2, Accent, 0.16f)) : WithAlpha(Surface2, h), 10);
        if (isSel) ui_.Stroke(item.Inset(0.75f), WithAlpha(Accent, 0.85f), 10, 1.5f);

        Rect ic = Rect::XYWH(item.l + 12, item.CY() - 17, 34, 34);
        if (Contrast())
            ui_.Stroke(ic.Inset(0.5f), onHighlight ? OnFill : Border, 8);
        else
            ui_.Fill(ic, isSel ? WithAlpha(Accent, 0.22f) : Surface3, 8);
        ui_.Text(ic, std::to_wstring(m.number), 15.f, onHighlight ? OnFill : isSel ? AccentHi : TextDim,
                 Bold(AlignCenter));

        std::wstring title = util::Format(tr(L"모니터 %d"), m.number);
        if (!m.name.empty()) title += L" · " + m.name;
        ui_.Text(Rect(item.l + 58, item.t + 10, item.r - 12, item.t + 32), title, 13.5f,
                 onHighlight ? OnFill : TextColor, Bold());
        std::wstring sub = util::Format(L"%d×%d", m.rect.right - m.rect.left, m.rect.bottom - m.rect.top);
        if (m.hz > 1) sub += util::Format(L" · %dHz", m.hz);
        sub += util::Format(tr(L" · 배율 %d%%"), (int)std::lround(m.dpi * 100.0 / 96));
        if (m.primary) sub += tr(L" · 주 모니터");
        ui_.Text(Rect(item.l + 58, item.t + 32, item.r - 12, item.b - 10), sub, 12.f, onHighlight ? OnFill : TextFaint);
    }
    if (monitors_.empty()) ui_.Text(in, tr(L"모니터를 찾을 수 없습니다"), 13.f, TextFaint, Align(AlignCenter));
}

void App::DrawRegionPanel(Rect in) {
    ui_.Text(in.CutTop(36), tr(L"녹화할 영역"), 15.f, TextColor, Bold());
    in.CutTop(10);

    // Big "select area" card.
    Rect card = in.CutTop(132);
    UiId id = HashId("region.pick");
    bool clicked = ui_.Click(id, card, rec_ == RecState::Idle);
    ui_.Describe(id, settings_.HasRegion() ? tr(L"영역 다시 지정") : tr(L"영역 지정"));
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
             settings_.HasRegion() ? tr(L"영역 다시 지정") : tr(L"영역 지정"), 15.f, TextColor, Bold(AlignCenter));
    TextStyle st = Align(AlignCenter, AlignStart);
    ui_.Text(Rect(card.l + 12, card.t + 92, card.r - 12, card.b - 8),
             tr(L"드래그해서 고르고 툴바에서 바로 녹화합니다"), 12.f, TextDim, st);
    if (clicked) StartRegionPick();

    in.CutTop(14);
    if (settings_.HasRegion()) {
        const RECT& rg = settings_.region;
        Rect info = in.CutTop(92);
        ui_.Fill(info, Surface2, 12);
        Rect ii = info.Inset(14, 12);
        ui_.Text(ii.CutTop(18), tr(L"현재 영역"), 12.f, TextFaint);
        TextStyle big = Bold();
        big.tabular = true;
        ui_.Text(ii.CutTop(30), util::Format(L"%d × %d", rg.right - rg.left, rg.bottom - rg.top), 20.f, TextColor,
                 big);
        std::wstring where;
        HMONITOR mon = MonitorFromRect(&rg, MONITOR_DEFAULTTONULL);
        for (auto& m : monitors_)
            if (m.handle == mon)
                where = util::Format(tr(L"모니터 %d · 위치 %d, %d"), m.number, rg.left - m.rect.left, rg.top - m.rect.top);
        if (where.empty()) where = tr(L"⚠ 지금 연결된 화면 밖에 있습니다");
        ui_.Text(ii, where, 12.f, TextDim);
    }
    Rect hint = in.CutBottom(40);
    TextStyle hs = Align(AlignCenter, AlignEnd);
    hs.wrap = true;
    ui_.Text(hint, tr(L"녹화 중에는 영역 둘레에 얇은 테두리가 보입니다\n(녹화 결과에는 찍히지 않습니다)"), 12.f, TextFaint, hs);
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
        if (UiNode* n = ui_.Describe(id, util::Format(tr(L"모니터 %d"), m.number), Role::Item)) n->on = isSel;
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
    ui_.Fill(preview, P.Preview, 10);
    // The window preview is a DWM thumbnail, which DWM composites above everything we draw: everything here stays
    // above the result card instead of under it.
    const bool card = showVideo_ && rec_ == RecState::Idle;
    Rect view = preview;
    if (card) view.b = VideoCardRect(preview).t;
    std::wstring hk = HotkeyLabel();
    auto placeholder = [&](std::wstring_view title, std::wstring_view body, bool editHint) {
        // The line about the 편집 tab only where it fits under the rest.
        const std::wstring_view a = tr(L"갖고 있는 동영상이나 움짤은 "), link = tr(L"편집"),
                                b = tr(L" 탭에서 엽니다. 창에 끌어다 놓아도 됩니다.");
        const float wa = ui_.Measure(a, 13.f), wl = ui_.Measure(link, 13.f, kBold), wb = ui_.Measure(b, 13.f);
        const float hintW = wa + wl + wb + 32;
        editHint = editHint && hintW <= view.W() - 20 && view.H() >= 230;
        float cy = view.CY() - (editHint ? 26 : 0);
        ui_.Ring(view.CX(), cy - 44, 30, WithAlpha(Accent, 0.35f), 2.f);
        ui_.Crosshair(view.CX(), cy - 44, 30, Accent);
        ui_.Text(Rect(view.l, cy - 4, view.r, cy + 22), title, 16.f, TextColor, Bold(AlignCenter));
        TextStyle st = Align(AlignCenter, AlignStart);
        st.wrap = true;
        ui_.Text(Rect(view.l + 30, cy + 30, view.r - 30, cy + 80), body, 13.f, TextDim, st);
        if (!editHint) return;
        Rect pill = Rect::Centered(view.CX(), cy + 112, hintW, 36);
        ui_.Fill(pill, Surface, 9);
        ui_.Stroke(pill.Inset(0.5f), Border, 9);
        float x = std::round(pill.l + 16);
        ui_.Text(Rect(x, pill.t, x + wa + 2, pill.b), a, 13.f, TextDim);
        x += wa;
        Rect lr(x - 2, pill.t + 4, x + wl + 2, pill.b - 4);
        const UiId id = HashId("home.edithint");
        const bool clicked = ui_.Click(id, lr);
        ui_.Describe(id, link);
        ui_.Text(Rect(x, pill.t, x + wl + 2, pill.b), link, 13.f, ui_.Hover(lr) ? LinkHi : Link, Bold());
        x += wl;
        ui_.Text(Rect(x, pill.t, pill.r, pill.b), b, 13.f, TextDim);
        if (clicked) ResumeEditor();
    };
    switch (settings_.source) {
        case kSourceDisplay:
            DrawMonitorLayout(view, nullptr, rec_ == RecState::Idle);
            break;
        case kSourceRegion:
            if (!settings_.HasRegion()) {
                placeholder(tr(L"녹화할 영역을 지정하세요"),
                            tr(L"왼쪽의 ‘영역 지정’을 누르고 화면에서 드래그하세요.\n") + hk +
                                tr(L" 단축키를 누르면 지정한 영역을 바로 녹화합니다."),
                            false);
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
                    ui_.Text(view, tr(L"최소화된 창입니다 · 녹화를 시작하면 복원됩니다"), 13.f, TextFaint,
                             Align(AlignCenter));
                ui_.NativeLayer(thumbArea_);  // from here on, only floats may be drawn over it
            } else {
                placeholder(tr(L"녹화할 창을 고르세요"),
                            tr(L"왼쪽 목록에서 고르거나 ‘화면에서 선택’을 누르세요.\n게임 중에는 ") + hk +
                                tr(L" 단축키로 바로 녹화할 수 있습니다."),
                            true);
            }
            break;
    }
    if (card) DrawVideoResult(preview);
}

void App::DrawVideoResult(Rect preview) {
    Rect card = VideoCardRect(preview);
    float a = ui_.AnimFrom(HashId("vres.in"), 1.f, 0.f, 14.f);
    card = card.Offset(0, (1 - a) * 12);
    ui_.Fill(card.Offset(0, 3), WithAlpha(Shadow, 0.4f * a), 12);
    ui_.Fill(card, WithAlpha(P.SavedCard, 0.97f * a), 12);
    ui_.Stroke(card.Inset(0.5f), WithAlpha(Success, 0.45f * a), 12);
    Rect in = card.Inset(14, 10);
    Rect close = in.CutRight(28);
    if (ui_.IconButton(HashId("vres.close"), Rect::Centered(close.CX(), close.CY(), 28, 28), icon::Close, tr(L"닫기"),
                       true, 11.f))
        showVideo_ = false;
    in.CutRight(6);
    const float bw = 76;
    Rect btns = in.CutRight(bw * 4 + 18);
    Rect b = Rect::Centered(0, btns.CY(), bw, 34);
    auto at = [&](int i) { return Rect::XYWH(btns.l + i * (bw + 6), b.t, bw, b.H()); };
    if (ui_.Button(HashId("vres.edit"), at(0), tr(L"편집"), BtnStyle::Primary, icon::Cut, true, 12.f)) EditLastVideo();
    if (ui_.Button(HashId("vres.folder"), at(1), tr(L"폴더"), BtnStyle::Secondary, icon::Folder, true, 12.f))
        util::OpenFolderAndSelect(lastVideo_.path);
    if (ui_.Button(HashId("vres.copy"), at(2), tr(L"복사"), BtnStyle::Secondary, icon::Copy, true, 12.f)) {
        if (util::CopyFileToClipboard(hwnd_, lastVideo_.path))
            Toast(tr(L"파일을 클립보드에 복사했습니다 · 메신저나 탐색기에 붙여넣으세요"));
        else
            Toast(tr(L"클립보드에 복사할 수 없습니다."), true);
    }
    if (ui_.Button(HashId("vres.open"), at(3), tr(L"재생"), BtnStyle::Secondary, icon::Play, true, 12.f))
        util::ShellOpen(lastVideo_.path);
    in.CutRight(12);
    Rect icon = in.CutLeft(28);
    ui_.Circle(icon.CX(), icon.CY(), 12, WithAlpha(Success, 0.2f));
    ui_.Icon(icon, icon::Check, 12.f, Success);
    in.CutLeft(8);
    ui_.Text(Rect(in.l, in.t, in.r, in.CY() + 1), util::FileNameOf(lastVideo_.path), 13.f, TextColor, Bold());
    std::wstring sub = util::FormatClock(lastVideo_.seconds, false) + L" · " + util::FormatBytes(lastVideo_.bytes) +
                       (lastVideo_.audio ? tr(L" · 소리 포함") : tr(L" · 소리 없음"));
    if (!lastVideo_.encoder.empty()) {  // only when it fits: the buttons leave little room in a narrow window
        std::wstring enc =
            lastVideo_.hardware ? L" · " + Mp4Writer::Vendor(lastVideo_.encoder) + tr(L" 가속") : tr(L" · CPU 인코딩");
        if (ui_.Measure(sub + enc, 12.f) <= in.W()) sub += enc;
    }
    ui_.Text(Rect(in.l, in.CY() + 1, in.r, in.b), sub, 12.f, TextDim);
}

void App::DrawCapturePanel(Rect r) {
    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), Border, 14);
    Rect in = r.Inset(20);
    const bool video = settings_.output == kOutputVideo;
    if (video) ProbeEncoders();

    Rect controls = in.CutBottom(346);
    in.CutBottom(16);
    DrawCapturePreview(in);

    RecTarget t;
    bool ready = CurrentTarget(t, nullptr);
    const int src = settings_.source;
    int sw = 0, sh = 0, ow = 0, oh = 0;
    bool hevc = false;
    if (ready) {
        SourceSize(t, sw, sh);
        PlannedSize(sw, sh, ow, oh, &hevc);
    }

    // Info row: what + resulting size.
    Rect row = controls.CutTop(24);
    if (ready) {
        std::wstring title;
        std::wstring badge = tr(L"창 캡처");
        D2D1_COLOR_F badgeColor = AccentHi;
        if (src == kSourceWindow) {
            const WinInfo* info = FindWindowInfo(t.hwnd);
            title = info ? info->title : L"";
            if (UseMonitorCapture(t.hwnd)) {
                badge = tr(L"화면 캡처");
                badgeColor = Warning;
            }
        } else {
            const MonitorDesc* md = nullptr;
            for (auto& m : monitors_)
                if (m.handle == t.monitor) md = &m;
            title = md ? util::Format(tr(L"모니터 %d"), md->number) : tr(L"모니터");
            if (md && !md->name.empty() && src == kSourceDisplay) title += L" · " + md->name;
            if (src == kSourceRegion) title = tr(L"영역 · ") + title;
            badge = src == kSourceRegion ? tr(L"영역") : tr(L"전체 화면");
        }
        std::wstring dims = sw > 0 ? util::Format(L"%d×%d  →  %d×%d", sw, sh, ow, oh) + (video && hevc ? L" · HEVC" : L"") : L"";
        float dw = ui_.Measure(dims, 12.5f) + 4;
        float bw = ui_.Measure(badge, 11.5f) + 16;
        Rect b = Rect::XYWH(row.r - bw, row.CY() - 11, bw, 22);
        ui_.Fill(b, WithAlpha(badgeColor, 0.15f), 11);
        ui_.Text(b, badge, 11.5f, badgeColor, Bold(AlignCenter));
        ui_.Text(Rect(b.l - dw - 10, row.t, b.l - 10, row.b), dims, 12.5f, TextDim, Align(AlignEnd));
        ui_.Text(Rect(row.l, row.t, b.l - dw - 20, row.b), title, 14.f, TextColor, Bold());
    } else {
        const wchar_t* none = src == kSourceDisplay ? tr(L"선택된 화면 없음")
                              : src == kSourceRegion ? tr(L"지정된 영역 없음")
                                                     : tr(L"선택된 창 없음");
        ui_.Text(row, none, 14.f, TextFaint, Bold());
    }
    controls.CutTop(14);

    auto labeled = [&](std::wstring_view label, float h) {
        Rect line = controls.CutTop(h);
        Rect l = line.CutLeft(84);
        ui_.Text(l, label, 13.f, TextDim);
        ui_.Name(label);
        controls.CutTop(10);
        return line;
    };
    bool changed = false;
    bool idle = rec_ == RecState::Idle;
    int output = settings_.output;
    if (ui_.Segmented(HashId("home.output"), labeled(tr(L"결과물"), 38), {tr(L"움짤 (GIF · WebP …)"), tr(L"동영상 (MP4)")}, output,
                      idle ? 0 : 0b11) &&
        idle) {
        settings_.output = output;
        changed = true;
    }
    if (video) {
        changed |= ui_.Segmented(HashId("home.vfps"), labeled(tr(L"프레임"), 38), {L"24", L"30", L"60"}, settings_.vFpsIdx);
        changed |= ui_.Segmented(HashId("home.vres"), labeled(tr(L"해상도"), 38),
                                 {tr(L"원본"), L"2160p", L"1440p", L"1080p", L"720p"}, settings_.vResIdx);
    } else {
        changed |= ui_.Segmented(HashId("home.fps"), labeled(tr(L"프레임"), 38),
                                 {L"10", L"15", L"20", L"30", L"50", L"60"}, settings_.fpsIdx);
        changed |= ui_.Segmented(HashId("home.size"), labeled(tr(L"최대 크기"), 38),
                                 {tr(L"원본"), L"1280", L"960", L"720", L"480"}, settings_.sizeIdx);
    }

    // Toggles: cursor, (window) title bar, (video) sound and mic. Each as wide as its label needs, with the room
    // left shared out evenly (or all narrowed alike when there is too little).
    struct ToggleItem {
        UiId id;
        const wchar_t* label;
        bool* v;
    };
    std::vector<ToggleItem> items{{HashId("home.cursor"), tr(L"마우스 커서"), &settings_.cursor}};
    if (src == kSourceWindow) items.push_back({HashId("home.client"), tr(L"제목 표시줄 제외"), &settings_.clientOnly});
    if (video) {
        items.push_back({HashId("home.audio"), tr(L"소리 녹음"), &settings_.audio});
        items.push_back({HashId("home.mic"), tr(L"마이크"), &settings_.mic});
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

    // What will come out, with a shortcut to the detailed settings; then what happens when the recording stops.
    Rect cap = controls.CutTop(20);
    Rect then = controls.CutTop(20);
    if (video) {
        Rect link = cap.CutRight(80);
        UiId lid = HashId("home.vsettings");
        bool lh = ui_.Hover(link);
        if (ui_.Click(lid, link)) OpenSettings(kTabVideo);
        ui_.Describe(lid, tr(L"동영상 세부 설정"));
        ui_.Text(link, tr(L"세부 설정 ›"), 12.f, lh ? LinkHi : Link, Align(AlignEnd));
        std::wstring audio = AudioSummary(src);
        bool warn = settings_.audio && src == kSourceWindow && !AudioCapture::ProcessLoopbackSupported();
        float aw = ui_.Measure(audio, 12.f) + 4;
        Rect ar = cap.CutRight(aw + 16);
        ui_.Icon(Rect::XYWH(ar.l, ar.t, 14, ar.H()), warn ? icon::Warning : icon::Info, 11.f,
                 warn ? Warning : TextFaint);
        ui_.Text(Rect(ar.l + 16, ar.t, ar.r, ar.b), audio, 12.f, warn ? Warning : TextDim);
        const bool autoCodec = settings_.vCodec == kCodecAuto;
        ui_.Text(cap, VideoSummary(hevc, hevc && autoCodec), 12.f, TextFaint);
        DrawTimerChip(then);
        then.CutRight(10);
        // HEVC is written as a plain MP4 (no fragments): a crash leaves nothing of it. 자동 without it shrinks.
        std::wstring warnLine;
        if (ready && hevc)
            warnLine = tr(L"HEVC 녹화는 OzMaker가 비정상 종료되면 남지 않습니다.");
        else if (ready && autoCodec && std::max(sw, sh) > 4096 && std::max(ow, oh) == 4096)
            warnLine = util::Format(tr(L"HEVC 비디오 확장이 없어 %d×%d로 줄여 녹화합니다."), ow, oh);
        if (!warnLine.empty()) {
            ui_.Icon(Rect::XYWH(then.l, then.t, 14, then.H()), icon::Warning, 11.f, Warning);
            ui_.Text(Rect(then.l + 18, then.t, then.r, then.b), warnLine, 12.5f, Warning);
        } else {
            ui_.Text(then, tr(L"녹화를 멈추면 파일로 저장됩니다. ‘편집’을 누르면 편집 탭에서 엽니다."), 12.5f, TextFaint);
        }
    } else {
        ui_.Text(cap, tr(L"녹화를 멈추면 편집 탭이 열립니다."), 12.5f, TextFaint);
        DrawTimerChip(then);
        ui_.Text(then, tr(L"거기서 자르고 저장하세요."), 12.5f, TextFaint);
    }
    controls.CutTop(14);
    if (changed) settingsDirty_ = true;

    // Record button
    Rect btn = controls.CutTop(56);
    UiId id = HashId("home.rec");
    bool enabled = rec_ != RecState::Idle || ready;
    bool clicked = ui_.Click(id, btn, enabled);
    const wchar_t* name = rec_ == RecState::Idle        ? (video ? tr(L"동영상 녹화") : tr(L"녹화 시작"))
                          : rec_ == RecState::Countdown ? tr(L"카운트다운 취소")
                                                        : tr(L"녹화 중지");
    if (UiNode* n = ui_.Describe(id, name)) n->keys = HotkeyLabel();
    float h = ui_.Anim(id, enabled && ui_.Hover(btn) ? 1.f : 0.f);
    D2D1_COLOR_F bg = enabled ? Mix(RecFill, RecFillHi, h) : Surface2;
    if (rec_ != RecState::Idle) bg = Mix(Surface3, BorderStrong, h);
    ui_.Fill(btn, bg, 14);
    if (rec_ != RecState::Idle)
        ui_.Stroke(btn.Inset(0.75f), Rec, 14, 1.5f);
    else if (Contrast())
        ui_.Stroke(btn.Inset(0.5f), enabled ? Border : P.Disabled, 14);
    // The text: on the red fill, or (stopping) on the gray.
    const D2D1_COLOR_F ink = rec_ != RecState::Idle ? TextColor : enabled ? OnFill : P.Disabled;
    std::wstring hk = HotkeyLabel();
    if (rec_ == RecState::Idle) {
        const wchar_t* label = video ? tr(L"동영상 녹화") : tr(L"녹화 시작");
        float lw = ui_.Measure(label, 16.f, kSemibold);
        float cx = btn.CX() - (lw + 28) / 2 + 9;
        ui_.Circle(cx, btn.CY(), 9, ink);
        if (enabled) ui_.Circle(cx, btn.CY(), 4, Mix(RecFill, RecFillHi, h));
        ui_.Text(Rect(cx + 18, btn.t, btn.r, btn.b), label, 16.f, ink, Bold());
    } else {
        ui_.Fill(Rect::Centered(btn.CX() - 70, btn.CY(), 14, 14), Rec, 3);
        std::wstring t2 = rec_ == RecState::Countdown ? tr(L"카운트다운 취소")
                                                 : tr(L"녹화 중지  ") + util::FormatClock(RecElapsed(), true);
        TextStyle st = Bold();
        st.tabular = true;
        ui_.Text(Rect(btn.CX() - 52, btn.t, btn.r, btn.b), t2, 16.f, ink, st);
        if (const int timer = settings_.TimerMinutes() * 60; timer > 0 && rec_ == RecState::Recording) {
            std::wstring zeros = t2;  // tabular digits are all as wide as 0
            std::replace_if(zeros.begin(), zeros.end(), iswdigit, L'0');
            const float x = btn.CX() - 52 + ui_.Measure(zeros, 16.f, kSemibold) + 6;
            const bool up = timerFiredFor_ == timer;
            ui_.Text(Rect(x, btn.t, btn.r, btn.b), util::Format(L"/ %d:%02d:%02d", timer / 3600, timer / 60 % 60, timer % 60),
                     14.f, up ? Warning : WithAlpha(ink, 0.6f), st);
        }
    }
    float kw = ui_.Measure(hk, 11.5f) + 18;
    Rect kb = Rect::XYWH(btn.r - kw - 14, btn.CY() - 12, kw, 24);
    if (Contrast())
        ui_.Stroke(kb.Inset(0.5f), ink, 8);
    else
        ui_.Fill(kb, WithAlpha(Black, P.light && !(enabled && rec_ == RecState::Idle) ? 0.08f : 0.22f), 8);
    ui_.Text(kb, hk, 11.5f, WithAlpha(ink, enabled ? 0.85f : 0.4f), Align(AlignCenter));
    if (clicked) {
        if (rec_ == RecState::Idle)
            StartRecording(t, false);
        else
            StopRecording();
    }
}

// The record timer (#47): a button at the right of the line under the options, and the popover it opens.
void App::DrawTimerChip(Rect& line) {
    const int minutes = settings_.TimerMinutes();
    const std::wstring label =
        !minutes ? std::wstring(tr(L"타이머 끔"))
                 : util::Format(settings_.timerStop ? tr(L"타이머 %s · 멈춤") : tr(L"타이머 %s · 알림"),
                                TimerWords(minutes).c_str());
    const float w = ui_.Measure(label, 12.f, kSemibold) + 40;
    Rect slot = line.CutRight(w);
    timerChip_ = Rect::Centered(slot.CX(), slot.CY(), w, 26);
    const UiId id = HashId("home.timer");
    if (ui_.Click(id, timerChip_)) {
        timerOpen_ = true;
        filesOpen_ = updateOpen_ = false;
    }
    ui_.Describe(id, tr(L"녹화 타이머, ") + label);
    const float h = ui_.Anim(id, ui_.Hover(timerChip_) || timerOpen_ ? 1.f : 0.f);
    const bool set = minutes > 0;
    ui_.Fill(timerChip_, set ? WithAlpha(Accent, Contrast() ? 0.f : 0.16f) : Surface2, 8);
    ui_.Stroke(timerChip_.Inset(0.5f), set ? Accent : Mix(Border, BorderStrong, h), 8);
    const D2D1_COLOR_F ink = set || h > 0.5f ? TextColor : TextDim;
    ui_.Icon(Rect::XYWH(timerChip_.l + 9, timerChip_.t, 14, timerChip_.H()), icon::Timer, 12.f, ink);
    TextStyle st = Bold();
    st.tabular = true;
    ui_.Text(Rect(timerChip_.l + 28, timerChip_.t, timerChip_.r - 6, timerChip_.b), label, 12.f, ink, st);
}

bool App::TimerFields(Rect r) {
    const int total = std::clamp(settings_.timerMinutes, 1, kTimerMaxMinutes);
    const int values[2] = {total / 60, total % 60};
    const wchar_t* units[2] = {tr(L"시간"), tr(L"분")};
    const wchar_t* names[2] = {tr(L"타이머 시간"), tr(L"타이머 분")};
    bool changed = false;
    for (int i = 0; i < 2; ++i) {
        const UiId id = HashId("timer.field", i);
        if (ui_.FocusedField() != id) timerText_[i] = std::to_wstring(values[i]);  // as typed only while typing
        if (ui_.TextField(id, r.CutLeft(56), timerText_[i], L"0", 2)) {
            std::erase_if(timerText_[i], [](wchar_t c) { return c < L'0' || c > L'9'; });
            changed = true;
        }
        ui_.Describe(id, names[i], Role::Edit);
        r.CutLeft(8);
        ui_.Text(r.CutLeft(ui_.Measure(units[i], 13.f) + 4), units[i], 13.f, TextDim);
        r.CutLeft(14);
    }
    if (changed) {
        const int m = _wtoi(timerText_[0].c_str()) * 60 + _wtoi(timerText_[1].c_str());
        settings_.timerMinutes = std::clamp(m, 1, kTimerMaxMinutes);
        settingsDirty_ = true;
    }
    return changed;
}

void App::DrawTimerPopover(Rect client) {
    const auto& in = ui_.In();
    const bool custom = settings_.timerIdx == kTimerCustom;
    const float W = std::min(352.f, client.W() - 40), pad = 16, w = W - pad * 2;
    // What the length does, and (MP4) the space it takes against what the drive has.
    auto noteFor = [&](int minutes) {
        if (!minutes) return std::wstring(tr(L"끄면 녹화 중지를 누를 때까지 녹화합니다."));
        const std::wstring words = TimerWords(minutes);
        return util::Format(settings_.timerStop
                                ? tr(L"%s이 되면 녹화를 멈추고 저장합니다. 일시정지한 시간은 세지 않습니다.")
                                : tr(L"%s이 되면 알림과 효과음으로 알립니다. 녹화는 계속됩니다. 일시정지한 시간은 세지 않습니다."),
                            words.c_str());
    };
    const int minutes = settings_.TimerMinutes();
    const std::wstring note = noteFor(minutes);
    std::wstring space;
    bool spaceWarn = false;
    RecTarget t;
    if (minutes && settings_.output == kOutputVideo && CurrentTarget(t, nullptr)) {
        int sw, sh, ow, oh;
        bool hevc = false;
        SourceSize(t, sw, sh);
        PlannedSize(sw, sh, ow, oh, &hevc);
        const double video = settings_.VideoBitrate()
                                 ? settings_.VideoBitrate()
                                 : Mp4Writer::AutoBitrate(ow, oh, kVideoFpsOptions[settings_.vFpsIdx], hevc);
        const double audio = settings_.audio || settings_.mic ? kAudioBitrateOptions[settings_.aBitrateIdx] * 1000.0 : 0;
        const uint64_t bytes = (uint64_t)((video + audio) / 8 * minutes * 60), free = util::FreeBytes(settings_.outDir);
        // Variable frame rate puts fewer frames in while the screen is still: the bitrate is then the most it takes.
        space = util::Format(settings_.vFrameMode == 1 ? tr(L"이 길이면 최대 %s · 남은 공간 %s") : tr(L"이 길이면 약 %s · 남은 공간 %s"),
                             util::FormatBytes(bytes).c_str(), util::FormatBytes(free).c_str());
        spaceWarn = bytes > free;
    }
    TextStyle wrap;
    wrap.wrap = true;
    wrap.v = AlignStart;
    DWRITE_TEXT_METRICS m{};
    float noteH = 34;
    if (auto lay = Fonts::Layout(note, 12.f, wrap, w, 200); lay && SUCCEEDED(lay->GetMetrics(&m))) noteH = std::ceil(m.height);
    const float H = pad + 24 + 8 + 36 + (custom ? 8 + 32 : 0) + 14 + 18 + 6 + 36 + 12 + noteH + (space.empty() ? 0 : 6 + 18) + pad;
    // Above the button (the record button is under it), or under it when there is no room.
    float top = timerChip_.t - 8 - H;
    if (top < client.t + 68) top = timerChip_.b + 8;
    Rect box = Rect::XYWH(std::clamp(timerChip_.r - W, client.l + 20, client.r - 20 - W), top, W, H);
    if ((in.pressed && !box.Contains(in.mx, in.my)) || (in.Key(VK_ESCAPE) && !ui_.TextFocused())) {
        timerOpen_ = false;
        return;
    }
    ui_.Fill(box.Offset(0, 6), WithAlpha(Shadow, 0.45f), 14);
    ui_.Fill(box, Surface2, 12);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 12);
    ui_.Declare(HashId("timer.box"), box, Role::Group, tr(L"녹화 타이머"));
    Rect c = box.Inset(pad);
    ui_.Text(c.CutTop(24), tr(L"녹화 타이머"), 14.f, TextColor, Bold());
    c.CutTop(8);
    bool changed = false;
    ui_.Name(tr(L"녹화 타이머"));
    if (ui_.Segmented(HashId("timer.pick"), c.CutTop(36), {tr(L"끔"), tr(L"30분"), tr(L"1시간"), tr(L"2시간"), tr(L"직접 입력")},
                      settings_.timerIdx)) {
        changed = true;
        if (settings_.timerIdx == kTimerCustom) ui_.Focus(HashId("timer.field", 0));
    }
    if (custom) {
        c.CutTop(8);
        changed |= TimerFields(c.CutTop(32));
    }
    c.CutTop(14);
    ui_.Text(c.CutTop(18), tr(L"시간이 되면"), 12.5f, TextDim);
    c.CutTop(6);
    int end = settings_.timerStop ? 1 : 0;
    ui_.Name(tr(L"시간이 되면"));
    if (ui_.Segmented(HashId("timer.end"), c.CutTop(36), {tr(L"알림만"), tr(L"멈추고 저장")}, end, minutes ? 0 : 0b11)) {
        settings_.timerStop = end == 1;
        changed = true;
    }
    c.CutTop(12);
    // Drawn as it is after this frame's changes (the box was sized for the text before them).
    ui_.Text(c.CutTop(noteH), noteFor(settings_.TimerMinutes()), 12.f, TextDim, wrap);
    if (!space.empty()) {
        c.CutTop(6);
        ui_.Text(c.CutTop(18), space, 12.f, spaceWarn ? Warning : TextFaint);
    }
    if (changed) {
        settingsDirty_ = true;
        Invalidate();
    }
}

// ---------------------------------------------------------------- drop-down lists

bool App::Dropdown(UiId id, const Rect& r, std::wstring_view label, std::wstring_view name,
                   std::vector<MenuItem> items, int& value, D2D1_COLOR_F ink) {
    bool picked = false;
    if (menuPicked_ == id) {
        menuPicked_ = 0;
        picked = menuPickedValue_ != value;
        value = menuPickedValue_;
    }
    const bool open = menu_.owner == id;
    const bool hov = ui_.Hover(r);
    if (ui_.Click(id, r)) {
        menu_ = {id, r, std::wstring(name), std::move(items), value, true, ui_.FocusRing()};
        filesOpen_ = updateOpen_ = timerOpen_ = false;
    } else if (open) {
        menu_.anchor = r;
        menu_.name = name;
        menu_.items = std::move(items);
        menu_.value = value;
        menu_.seen = true;
    }
    ui_.Describe(id, std::wstring(name) + L", " + std::wstring(label));
    const bool lit = hov || menu_.owner == id;
    ui_.Fill(r, lit ? Surface3 : Surface2, 8);
    ui_.Stroke(r.Inset(0.5f), menu_.owner == id ? Accent : lit ? BorderStrong : Border, 8);
    Rect t(r.l + 12, r.t, r.r - 30, r.b);
    ui_.PushClip(t);
    ui_.Text(t, label, 13.f, ink);
    ui_.PopClip();
    ui_.Icon(Rect(r.r - 30, r.t, r.r - 8, r.b), menu_.owner == id ? icon::ChevronUp : icon::ChevronDown, 10.f, TextDim);
    return picked;
}

// The open Dropdown's items, under its field (above it when there is no room). A click elsewhere or Esc closes it;
// with the keyboard, the arrows move through the items and Enter or Space picks one.
void App::DrawMenuPopover(Rect client) {
    const auto& in = ui_.In();
    const float itemH = 32, pad = 6, lineH = 9;
    float w = menu_.anchor.W(), h = pad * 2;
    for (const MenuItem& it : menu_.items) {
        w = std::max(w, ui_.Measure(it.text, 13.f, kSemibold) + 52);
        h += itemH + (it.line ? lineH : 0);
    }
    w = std::min(w, client.W() - 24);
    float top = menu_.anchor.b + 4;
    if (top + h > client.b - 8) top = std::max(client.t + 64, menu_.anchor.t - 4 - h);
    const Rect box = Rect::XYWH(std::clamp(menu_.anchor.l, client.l + 12, client.r - 12 - w), top, w, h);
    auto close = [&] {
        if (menu_.keys) ui_.SetKeyboardFocus(menu_.owner);  // back to its field
        menu_ = {};
        Invalidate();
    };
    if ((in.pressed && !box.Contains(in.mx, in.my)) || in.Key(VK_ESCAPE)) {
        close();
        return;
    }
    ui_.Fill(box.Offset(0, 6), WithAlpha(Shadow, 0.45f), 12);
    ui_.Fill(box, Surface2, 10);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 10);
    ui_.Declare(HashId("menu.box"), box, Role::Group, menu_.name);
    std::vector<UiId> keys;  // the items the arrows move through
    int at = -1, chosen = -1;
    Rect c = box.Inset(pad);
    for (size_t i = 0; i < menu_.items.size(); ++i) {
        const MenuItem& it = menu_.items[i];
        if (it.line) {
            c.CutTop(4);
            ui_.Line(c.l + 8, c.t + 0.5f, c.r - 8, c.t + 0.5f, Border);
            c.CutTop(lineH - 4);
        }
        const Rect row = c.CutTop(itemH);
        const UiId iid = HashId("menu.item", (uint32_t)i);
        const bool on = it.enabled && it.value == menu_.value;
        if (ui_.Click(iid, row, it.enabled)) {
            menuPicked_ = menu_.owner;
            menuPickedValue_ = it.value;
            close();
            return;
        }
        if (UiNode* n = ui_.Describe(iid, it.text, Role::Item)) n->on = on;
        if (it.enabled) {
            if (ui_.KeyboardFocus() == iid) at = (int)keys.size();
            if (on) chosen = (int)keys.size();
            keys.push_back(iid);
        }
        const bool lit = it.enabled && (ui_.Hover(row) || (ui_.FocusRing() && ui_.KeyboardFocus() == iid));
        if (lit) ui_.Fill(row, Surface3, 6);
        if (on) ui_.Icon(Rect(row.l + 6, row.t, row.l + 30, row.b), icon::Check, 11.f, AccentHi);
        ui_.Text(Rect(row.l + 34, row.t, row.r - 10, row.b), it.text, 13.f, it.enabled ? TextColor : TextFaint,
                 on ? Bold() : TextStyle{});
    }
    if (keys.empty()) return;
    // Opened from the keyboard: the focus goes to the chosen item.
    if (menu_.keys && at < 0) ui_.SetKeyboardFocus(keys[chosen >= 0 ? chosen : 0]);
    const int d = in.Key(VK_DOWN) ? 1 : in.Key(VK_UP) ? -1 : 0;
    if (d) {
        const int n = (int)keys.size();
        const int from = at >= 0 ? at : chosen >= 0 ? chosen : (d > 0 ? -1 : 0);
        ui_.SetKeyboardFocus(keys[(from + d + n) % n]);
        menu_.keys = true;
        Invalidate();
    }
}

// ---------------------------------------------------------------- 리플레이 (#20)

namespace {

bool SamePathI(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// A program without an icon (not found, or made up for the screenshots): its first letter on a color of its name.
D2D1_COLOR_F LetterColor(std::wstring_view name) {
    static constexpr uint32_t kColors[] = {0xD9573E, 0x3E7FD9, 0x2F9A66, 0x7A5AF8, 0xB45309, 0x0E7490, 0xBE185D, 0x5B6474};
    uint32_t h = 2166136261u;
    for (wchar_t c : name) h = (h ^ c) * 16777619u;
    return Hex(kColors[h % _countof(kColors)]);
}

std::wstring LenName(int seconds) {
    return seconds < 60 ? util::Format(tr(L"%d초"), seconds) : util::Format(tr(L"%d분"), seconds / 60);
}

SYSTEMTIME LocalTime(uint64_t fileTime) {
    FILETIME ft{(DWORD)fileTime, (DWORD)(fileTime >> 32)}, local;
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    return st;
}

uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

}  // namespace

// A program's icon from its exe, else its letter; a monitor's number.
void App::DrawTargetIcon(const Rect& ic, const std::wstring& exe, std::wstring_view name, int monitor) {
    const float rad = ic.W() / 4;
    if (monitor) {
        ui_.Fill(ic, Surface3, rad);
        ui_.Text(ic, std::to_wstring(monitor), ic.W() * 0.42f, TextDim, Bold(AlignCenter));
        return;
    }
    if (ID2D1Bitmap* bmp = exe.empty() ? nullptr : ExeIcon(exe)) {
        ui_.Painted(ic);
        gfx_.DC()->DrawBitmap(bmp, ic, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        return;
    }
    ui_.Fill(ic, Contrast() ? Surface3 : LetterColor(name), rad);
    if (Contrast()) ui_.Stroke(ic.Inset(0.5f), Border, rad);
    const std::wstring letter(1, name.empty() ? L'?' : (wchar_t)towupper(name[0]));
    TextStyle st = Bold(AlignCenter);
    st.weight = kBold;
    ui_.Text(ic, letter, ic.W() * 0.44f, Contrast() ? TextColor : White, st);
}

std::wstring App::TargetName(const ReplayTarget& t) const {
    if (!t.monitor) return t.name;
    for (const MonitorDesc& m : monitors_)
        if (_wcsicmp(m.device.c_str(), t.path.c_str()) == 0) return util::Format(tr(L"전체 화면 · 화면 %d"), m.number);
    return tr(L"전체 화면 · 연결되지 않은 모니터");
}

bool App::TargetRuns(const ReplayTarget& t) const {
    if (t.monitor)
        return std::any_of(monitors_.begin(), monitors_.end(),
                           [&](const MonitorDesc& m) { return _wcsicmp(m.device.c_str(), t.path.c_str()) == 0; });
    if (rpRunningForTest_ >= 0) return SamePathI(t.path, settings_.rpTargets[rpRunningForTest_].path);
    return std::any_of(windows_.begin(), windows_.end(), [&](const WinInfo& w) { return SamePathI(w.exePath, t.path); });
}

bool App::TargetHeld(const ReplayTarget& t) const {
    if (rpStateForTest_ >= 0)
        return rpRunningForTest_ >= 0 && &t == &settings_.rpTargets[rpRunningForTest_] &&
               (rpStateForTest_ == (int)RpState::Holding || rpStateForTest_ == (int)RpState::Paused);
    return !rpHeld_.path.empty() && rpHeld_.monitor == t.monitor && SamePathI(rpHeld_.path, t.path);
}

void App::DrawReplay(Rect r) {
    r.CutTop(20);
    DrawReplayStrip(r.CutTop(76));
    r.CutTop(16);
    Rect left = r.CutLeft(340);
    r.CutLeft(20);
    DrawReplayPrograms(left);
    DrawReplayFiles(r);
}

// What replay is doing, in one line, with what can be done about it.
void App::DrawReplayStrip(Rect r) {
    const RpState st = ReplayState();
    const double len = kReplayLenOptions[settings_.rpLenIdx];
    const std::wstring key = util::HotkeyToString(settings_.rpHkMods, settings_.rpHkVk);
    std::wstring names;
    for (const ReplayTarget& t : settings_.rpTargets)
        if (t.on) names += (names.empty() ? L"" : L" · ") + TargetName(t);
    const std::wstring held = rpStateForTest_ >= 0 && rpRunningForTest_ >= 0 ? TargetName(settings_.rpTargets[rpRunningForTest_])
                              : !rpHeld_.name.empty()                       ? rpHeld_.name
                                                                            : std::wstring();
    const bool closed = !rpClosed_.path.empty();
    std::wstring title, sub;
    enum { kNone, kAdd, kOn, kSave } primary = kNone;
    bool offBtn = false;
    switch (st) {
        case RpState::NoTarget:
            title = tr(L"리플레이를 켤 프로그램을 고르세요");
            sub = tr(L"게임을 켠 채로 추가하면, 다음부터 그 게임이 실행될 때마다 알아서 담습니다");
            primary = kAdd;
            break;
        case RpState::Off:
            title = tr(L"리플레이 꺼짐");
            sub = names.empty() ? tr(L"켜 둔 프로그램이 없습니다 · 왼쪽 목록에서 켜세요")
                                : tr(L"켜면 이 프로그램이 실행될 때 담기 시작합니다 · ") + names;
            primary = kOn;
            break;
        case RpState::Waiting:
            title = tr(L"기다리는 중");
            if (closed)
                sub = util::Format(tr(L"%s 닫힘 · 담아 둔 장면(%s)은 5분 동안 남겨 둡니다"), rpClosed_.name.c_str(),
                                   util::FormatClock(ReplayHeldSeconds(), false).c_str());
            else
                sub = names.empty() ? tr(L"켜 둔 프로그램이 없습니다 · 왼쪽 목록에서 켜세요")
                                    : tr(L"실행되면 바로 담기 시작합니다 · ") + names;
            primary = closed ? kSave : kNone;
            offBtn = true;
            break;
        case RpState::Paused:
            if (rec_ != RecState::Idle || (rpStateForTest_ == (int)RpState::Paused && rpPause_ == RpPause::None)) {
                title = tr(L"멈춤");
                sub = tr(L"녹화하는 동안은 쉽니다 · 녹화를 멈추면 처음부터 다시 담습니다");
            } else if (rpLocked_ || rpAsleep_) {
                title = tr(L"멈춤");
                sub = tr(L"PC가 잠겨 있습니다 · 잠금을 풀면 처음부터 다시 담습니다");
            } else if (rpPause_ == RpPause::Minimized) {
                title = tr(L"멈춤 · ") + held;
                sub = tr(L"창이 최소화되어 있습니다 · 창을 다시 열면 이어서 담습니다");
            } else {
                title = tr(L"담을 수 없음 · ") + held;
                sub = rpBlocked_.empty() ? tr(L"Windows가 이 창을 담지 못하게 막았습니다 · 관리자 권한으로 실행 중인 게임일 수 있습니다")
                                         : rpBlocked_;
            }
            offBtn = true;
            break;
        case RpState::Holding:
            title = tr(L"담는 중 · ") + held;
            sub = util::Format(tr(L"방금 지나간 %s까지 메모리에 담습니다 · 저장하면 오른쪽 목록에 쌓입니다"), LenName((int)len).c_str());
            primary = kSave;
            offBtn = true;
            break;
    }
    if ((st == RpState::Holding || st == RpState::Waiting) && settings_.rpOn && !replayHotkeyOk_)
        sub = tr(L"⚠ 저장 단축키를 다른 프로그램이 쓰고 있습니다 · 설정 › 리플레이에서 바꾸세요");

    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), st == RpState::Holding && !Contrast() ? WithAlpha(Rec, 0.45f) : Border, 14);
    ui_.Declare(HashId("rp.strip"), r, Role::Group, title + L". " + sub);
    Rect in = r.Inset(18, 0);
    // The state's mark: a red dot while it holds, a ring while it waits, ❚❚ while it rests.
    const float cx = in.l + 19, cy = r.CY();
    in.CutLeft(38 + 14);
    ui_.Circle(cx, cy, 19, Surface2);
    if (st == RpState::Holding) {
        ui_.Circle(cx, cy, 10, WithAlpha(Rec, 0.22f));
        ui_.Circle(cx, cy, 6, Rec);
    } else if (st == RpState::Waiting) {
        ui_.Circle(cx, cy, 6.5f, TextDim);
        ui_.Circle(cx, cy, 4.5f, Surface2);
    } else if (st == RpState::Paused) {
        ui_.Fill(Rect::XYWH(cx - 5, cy - 6, 3.5f, 12), Warning, 1);
        ui_.Fill(Rect::XYWH(cx + 1.5f, cy - 6, 3.5f, 12), Warning, 1);
    } else {
        ui_.Circle(cx, cy, 6, TextFaint);
    }

    // From the right: 끄기, the main button, the meter; the text takes what is left.
    const float btnH = 40;
    Rect right = in;
    Rect off, main, meter;
    if (offBtn) {
        off = right.CutRight(ui_.Measure(tr(L"끄기"), 13.f, kSemibold) + 32);
        right.CutRight(8);
    }
    const std::wstring mainLabel = primary == kAdd  ? tr(L"+ 프로그램 추가")
                                   : primary == kOn ? tr(L"리플레이 켜기")
                                   : rpSaving_      ? tr(L"저장하는 중…")
                                                    : tr(L"지금 저장");
    const float keyW = primary == kSave && replayHotkeyOk_ ? ui_.Measure(key, 11.5f, kSemibold) + 14 : 0;
    if (primary != kNone) {
        main = right.CutRight(ui_.Measure(mainLabel, 13.f, kSemibold) + 40 + (primary == kSave ? 22 : 0) + (keyW ? keyW + 8 : 0));
        right.CutRight(14);
    }
    if (st == RpState::Holding) {
        meter = right.CutRight(120 + 10 + ui_.Measure(L"0:00 / 0:00", 12.5f) + 4);
        right.CutRight(18);
    }
    const bool mic = st == RpState::Holding && settings_.rpMic;
    const std::wstring micTag = tr(L"마이크 포함");
    const float micW = mic ? ui_.Measure(micTag, 11.f, kBold) + 14 : 0;
    const float titleW = std::min(right.W() - micW - (mic ? 8 : 0), ui_.Measure(title, 15.f, kSemibold) + 2);
    ui_.PushClip(right);
    ui_.Text(Rect(right.l, r.t + 14, right.l + titleW, r.t + 40), title, 15.f, TextColor, Bold());
    if (mic) {
        Rect chip = Rect::XYWH(right.l + titleW + 8, r.t + 18, micW, 18);
        ui_.Fill(chip, WithAlpha(Warning, Contrast() ? 0.f : 0.14f), 6);
        if (Contrast()) ui_.Stroke(chip.Inset(0.5f), Warning, 6);
        TextStyle ts = Bold(AlignCenter);
        ts.weight = kBold;
        ui_.Text(chip, micTag, 11.f, Warning, ts);
    }
    ui_.Text(Rect(right.l, r.t + 40, right.r, r.b - 14), sub, 12.5f, TextDim);
    ui_.PopClip();
    if (st == RpState::Holding) {
        const double got = ReplayHeldSeconds();
        Rect bar = Rect::XYWH(meter.l, meter.CY() - 3, 120, 6);
        ui_.Fill(bar, Surface3, 3);
        if (Contrast()) ui_.Stroke(bar.Inset(-0.5f), Border, 3.5f);
        ui_.Fill(Rect(bar.l, bar.t, bar.l + bar.W() * (float)std::clamp(got / len, 0.0, 1.0), bar.b), Rec, 3);
        TextStyle ts;
        ts.tabular = true;
        ui_.Text(Rect(bar.r + 10, meter.t, meter.r, meter.b),
                 util::FormatClock(got, false) + L" / " + util::FormatClock(len, false), 12.5f, TextDim, ts);
        RequestWake(ui_.Now() + 0.5);
    }
    if (primary != kNone) {
        Rect b = Rect::Centered(main.CX(), r.CY(), main.W(), btnH);
        const UiId id = HashId("rp.main");
        const bool en = primary != kSave || !rpSaving_;
        if (primary == kSave) {
            // 지금 저장 with its key in a chip.
            if (ui_.Button(id, b, L"", BtnStyle::Primary, 0, en) && en) ReplaySave();
            ui_.Describe(id, keyW ? mainLabel + L", " + key : mainLabel);
            const D2D1_COLOR_F fg = en ? OnFill : P.Disabled;
            float x = std::round(b.CX() - (14 + 8 + ui_.Measure(mainLabel, 13.f, kSemibold) + (keyW ? 8 + keyW : 0)) / 2);
            ui_.Icon(Rect::XYWH(x, b.t, 14, b.H()), icon::Save, 13.f, fg);
            x += 22;
            const float lw = ui_.Measure(mainLabel, 13.f, kSemibold);
            ui_.Text(Rect(x, b.t, x + lw + 2, b.b), mainLabel, 13.f, fg, Bold());
            if (keyW) {
                Rect chip = Rect::XYWH(x + lw + 8, b.CY() - 10, keyW, 20);
                ui_.Fill(chip, Contrast() ? Bg : WithAlpha(Black, 0.22f), 6);
                if (Contrast()) ui_.Stroke(chip.Inset(0.5f), Border, 6);
                ui_.Text(chip, key, 11.5f, Contrast() ? TextColor : fg, Bold(AlignCenter));
            }
        } else if (ui_.Button(id, b, mainLabel, BtnStyle::Primary, primary == kOn ? icon::Replay : 0, true, 13.f)) {
            if (primary == kOn) {
                SetReplayOn(true);
            } else {
                rpAddOpen_ = true;
                RefreshWindows();
                ListRunningPrograms();
            }
        }
    }
    if (offBtn && ui_.Button(HashId("rp.off"), Rect::Centered(off.CX(), r.CY(), off.W(), btnH), tr(L"끄기"),
                             BtnStyle::Secondary, 0, true, 13.f))
        SetReplayOn(false);
}

void App::DrawReplayPrograms(Rect r) {
    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), Border, 14);
    Rect in = r.Inset(14);
    Rect head = in.CutTop(30);
    const auto& targets = settings_.rpTargets;
    ui_.Text(head, tr(L"리플레이를 켤 프로그램"), 14.f, TextColor, Bold());
    if (!targets.empty())
        ui_.Text(head, util::Format(tr(L"%zu개"), targets.size()), 12.f, TextFaint, Align(AlignEnd));
    in.CutTop(6);

    // 공통 설정 at the bottom: what every program is held with, and the way to it.
    Rect sum = in.CutBottom(56);
    in.CutBottom(10);
    {
        const UiId id = HashId("rp.sum");
        const bool clicked = ui_.Click(id, sum);
        const std::wstring line = LenName(kReplayLenOptions[settings_.rpLenIdx]) + L" · " +
                                  util::Format(L"%dfps", kVideoFpsOptions[settings_.rpFpsIdx]) + L" · " +
                                  (settings_.rpAudio ? tr(L"소리 켬") : tr(L"소리 끔")) + L" · " +
                                  (settings_.rpMic ? tr(L"마이크 켬") : tr(L"마이크 끔"));
        ui_.Describe(id, tr(L"공통 설정, ") + line);
        const float hv = ui_.Anim(id, ui_.Hover(sum) ? 1.f : 0.f);
        ui_.Fill(sum, Mix(Surface2, Surface3, hv), 10);
        if (Contrast()) ui_.Stroke(sum.Inset(0.5f), Border, 10);
        ui_.Text(Rect(sum.l + 14, sum.t + 9, sum.r - 34, sum.CY() + 1), tr(L"공통 설정"), 12.5f, TextColor, Bold());
        ui_.Text(Rect(sum.l + 14, sum.CY() + 1, sum.r - 34, sum.b - 9), line, 12.f, TextFaint);
        ui_.Icon(Rect(sum.r - 34, sum.t, sum.r - 12, sum.b), icon::Next, 11.f, TextDim);
        if (clicked) OpenSettings(kTabReplay);
    }

    Rect list = in;
    const float rowH = 56, step = 60, addH = 40;
    TextStyle wrap;
    wrap.wrap = true;
    wrap.v = AlignStart;
    const float content = targets.size() * step + addH + (targets.empty() ? 70 : 0);
    const float maxScroll = std::max(0.f, content - list.H());
    if (ui_.Hover(list) && ui_.In().wheel != 0) rpProgScroll_ -= ui_.In().wheel * step * 1.5f;
    ui_.KeepInView(list, rpProgScroll_);
    rpProgScroll_ = std::clamp(rpProgScroll_, 0.f, maxScroll);
    const float scroll = ui_.Anim(HashId("rp.progScroll"), rpProgScroll_, 22.f);
    ui_.PushClip(list);
    const float w = list.W() - (maxScroll > 0 ? 8 : 0);
    float y = list.t - scroll;
    for (size_t i = 0; i < targets.size(); ++i, y += step) {
        ReplayTarget& t = settings_.rpTargets[i];
        Rect row = Rect::XYWH(list.l, y, w, rowH);
        const std::wstring name = TargetName(t);
        const bool held = settings_.rpOn && TargetHeld(t);
        const bool runs = TargetRuns(t);
        const bool closedHere = !rpClosed_.path.empty() && !t.monitor && SamePathI(rpClosed_.path, t.path);
        std::wstring status;
        D2D1_COLOR_F tint = TextFaint;
        const RpState st = ReplayState();
        if (!settings_.rpOn) {
            status = runs ? tr(L"실행 중") : tr(L"실행 안 함");
        } else if (held && st == RpState::Paused) {
            status = rpPause_ == RpPause::Blocked ? tr(L"담을 수 없음") : tr(L"멈춤");
            if (rpPause_ == RpPause::Minimized) status = tr(L"최소화됨 · 멈춤");
            tint = Warning;
        } else if (held && st == RpState::Holding) {
            status = tr(L"담는 중");
            tint = RecHi;
        } else if (closedHere) {
            status = util::Format(tr(L"닫힘 · 담아 둔 %s 남겨 둠"), util::FormatClock(ReplayHeldSeconds(), false).c_str());
            tint = Warning;
        } else if (!t.on) {
            status = tr(L"꺼 둠");
        } else {
            status = runs ? tr(L"실행 중 · 대기") : tr(L"실행 안 함 · 기다리는 중");
        }
        const UiId rid = HashId("rp.prog", (uint32_t)i);
        ui_.Declare(rid, row, Role::Item, name + L", " + status);
        if (row.b < list.t || row.t > list.b) continue;
        const bool sel = held && st == RpState::Holding;
        if (sel) {
            ui_.Fill(row, Contrast() ? Surface2 : Mix(Surface2, Accent, 0.16f), 10);
            ui_.Stroke(row.Inset(0.75f), WithAlpha(Accent, 0.85f), 10, 1.5f);
        } else if (ui_.Hover(row)) {
            ui_.Fill(row, Surface2, 10);
        }
        int number = 0;
        if (t.monitor)
            for (const MonitorDesc& m : monitors_)
                if (_wcsicmp(m.device.c_str(), t.path.c_str()) == 0) number = m.number;
        DrawTargetIcon(Rect::XYWH(row.l + 10, row.CY() - 16, 32, 32), t.monitor ? L"" : t.path, name,
                       t.monitor ? std::max(1, number) : 0);
        Rect more = Rect::Centered(row.r - 22, row.CY(), 30, 30);
        Rect tog = Rect::Centered(more.l - 4 - 22, row.CY(), 44, 24);
        Rect text(row.l + 54, row.t, tog.l - 10, row.b);
        ui_.PushClip(text);
        ui_.Text(Rect(text.l, row.t + 8, text.r, row.CY() + 2), name, 13.5f, TextColor, Bold());
        TextStyle ss;
        if (tint.r != TextFaint.r || tint.g != TextFaint.g) ss.weight = kSemibold;
        ui_.Text(Rect(text.l, row.CY() + 2, text.r, row.b - 8), status, 12.f, tint, ss);
        ui_.PopClip();
        ui_.Name(name);
        if (ui_.Toggle(HashId("rp.progOn", (uint32_t)i), tog, t.on)) {
            settings_.Save();
            if (settings_.rpOn) ReplayTick();
        }
        ui_.Name({});
        if (ui_.IconButton(HashId("rp.progMore", (uint32_t)i), more, icon::More, tr(L"더 보기"), true, 13.f)) {
            rpProgMenu_ = (int)i;
            rpFileMenu_ = -1;
            rpMenuAnchor_ = more;
        }
        if (rpProgMenu_ == (int)i) rpMenuAnchor_ = more;  // it follows the row as it scrolls
    }
    Rect add = Rect::XYWH(list.l, y, w, addH);
    if (ui_.Button(HashId("rp.add"), add, tr(L"+ 프로그램 추가"), BtnStyle::Secondary, 0, true, 13.f)) {
        rpAddBtn_ = add;
        rpAddOpen_ = true;
        RefreshWindows();
        ListRunningPrograms();
    }
    rpAddBtn_ = add;  // the popover opens under it, from the strip's button too
    if (targets.empty())
        ui_.Text(Rect(list.l + 4, add.b + 12, list.r - 4, add.b + 70),
                 tr(L"게임이나 앱을 켠 채로 추가하세요. 실행하지 않은 게임은 exe 파일로 넣을 수 있습니다."), 12.f,
                 TextFaint, wrap);
    ui_.PopClip();
    if (maxScroll > 0) {
        float th = std::max(30.f, list.H() * list.H() / content);
        float ty = list.t + (list.H() - th) * (scroll / maxScroll);
        ui_.Fill(Rect::XYWH(list.r - 4, ty, 4, th), WithAlpha(TextFaint, 0.5f), 2);
    }
}

void App::DrawReplayFiles(Rect r) {
    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), Border, 14);
    Rect in = r.Inset(14);
    Rect head = in.CutTop(34);
    const float tw = ui_.Measure(tr(L"저장한 리플레이"), 14.f, kSemibold) + 4;
    ui_.Text(head.CutLeft(tw), tr(L"저장한 리플레이"), 14.f, TextColor, Bold());
    uint64_t total = 0;
    std::vector<std::wstring> programs;
    for (const ReplayFile& f : rpFiles_) {
        total += f.bytes;
        if (std::none_of(programs.begin(), programs.end(), [&](const std::wstring& p) { return SamePathI(p, f.program); }))
            programs.push_back(f.program);
    }
    if (!rpFiles_.empty()) {
        head.CutLeft(10);
        ui_.Text(head, util::Format(tr(L"%zu개 · %s"), rpFiles_.size(), util::FormatBytes(total).c_str()), 12.f, TextFaint);
    }
    // Which program's: a list, as there may be many (#53's dropdown).
    if (programs.size() > 1) {
        std::vector<MenuItem> items{{tr(L"모든 프로그램"), 0}};
        int value = 0;
        for (size_t i = 0; i < programs.size(); ++i) {
            items.push_back({programs[i], (int)i + 1, i == 0});
            if (SamePathI(programs[i], rpFilter_)) value = (int)i + 1;
        }
        Rect dd = Rect::Centered(head.r - 100, head.CY(), 200, 32);
        if (Dropdown(HashId("rp.filter"), dd, value ? programs[value - 1] : std::wstring(tr(L"모든 프로그램")),
                     tr(L"프로그램"), std::move(items), value, TextColor)) {
            rpFilter_ = value ? programs[value - 1] : L"";
            rpFileScroll_ = 0;
        }
    }
    in.CutTop(8);
    // The folder they are read from, along the bottom.
    Rect foot = in.CutBottom(40);
    in.CutBottom(6);
    {
        ui_.Line(foot.l, foot.t + 0.5f, foot.r, foot.t + 0.5f, Border);
        const std::wstring open = tr(L"폴더 열기");
        Rect b = foot.CutRight(ui_.Measure(open, 12.5f, kSemibold) + 44);
        if (ui_.Button(HashId("rp.openDir"), Rect::Centered(b.CX(), foot.CY() + 3, b.W(), 32), open, BtnStyle::Secondary,
                       icon::Folder, true, 12.5f)) {
            util::EnsureDir(ReplayDir());
            util::ShellOpen(ReplayDir());
        }
        foot.CutRight(10);
        ui_.PushClip(foot);
        ui_.Text(Rect(foot.l + 2, foot.t + 3, foot.r, foot.b + 3), ReplayDir(), 12.f, TextFaint);
        ui_.PopClip();
    }

    Rect list = in;
    if (rpFiles_.empty()) {
        Rect c = Rect::Centered(list.CX(), list.CY() - 10, std::min(list.W() - 40, 540.f), 110);
        ui_.Circle(c.CX(), c.t + 22, 22, Surface2);
        ui_.Icon(Rect::Centered(c.CX(), c.t + 22, 44, 44), icon::Replay, 18.f, TextDim);
        const bool any = !settings_.rpTargets.empty();
        ui_.Text(Rect(c.l, c.t + 54, c.r, c.t + 76), any ? tr(L"아직 저장한 리플레이가 없습니다") : tr(L"저장한 리플레이가 여기에 모입니다"),
                 14.f, TextColor, Bold(AlignCenter));
        TextStyle st = Align(AlignCenter, AlignStart);
        st.wrap = true;
        const std::wstring tip = any ? util::Format(tr(L"담는 중일 때 저장 단축키(%s)를 누르면 방금 지나간 장면(%s)이 여기에 저장됩니다."),
                                                    util::HotkeyToString(settings_.rpHkMods, settings_.rpHkVk).c_str(),
                                                    LenName(kReplayLenOptions[settings_.rpLenIdx]).c_str())
                                     : std::wstring(tr(L"왼쪽에서 리플레이를 켤 프로그램을 먼저 고르세요."));
        ui_.Text(Rect(c.l, c.t + 80, c.r, c.b + 20), tip, 12.5f, TextFaint, st);
        return;
    }

    // Grouped by the day they were saved, newest first.
    const SYSTEMTIME today = LocalTime(NowFileTime());
    const SYSTEMTIME yesterday = LocalTime(NowFileTime() - 864000000000ull);
    auto dayName = [&](const SYSTEMTIME& t) -> std::wstring {
        if (t.wYear == today.wYear && t.wMonth == today.wMonth && t.wDay == today.wDay) return tr(L"오늘");
        if (t.wYear == yesterday.wYear && t.wMonth == yesterday.wMonth && t.wDay == yesterday.wDay) {
            std::wstring d = tr(L"어제");  // "yesterday" in English, which starts a heading here
            if (!d.empty()) d[0] = (wchar_t)towupper(d[0]);
            return d;
        }
        static const wchar_t* const kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                                 L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        std::wstring d = Language() == Lang::English ? util::Format(L"%s %d", kMonths[(t.wMonth + 11) % 12], t.wDay)
                                                     : util::Format(L"%d월 %d일", t.wMonth, t.wDay);
        if (t.wYear != today.wYear) d = util::Format(L"%d. ", t.wYear) + d;
        return d;
    };
    const float rowH = 52, groupH = 30;
    struct Line {
        size_t file;  // SIZE_MAX: a day's heading
        std::wstring day;
    };
    std::vector<Line> lines;
    std::wstring day;
    for (size_t i = 0; i < rpFiles_.size(); ++i) {
        if (!rpFilter_.empty() && !SamePathI(rpFiles_[i].program, rpFilter_)) continue;
        std::wstring d = dayName(LocalTime(rpFiles_[i].written));
        if (d != day) lines.push_back({SIZE_MAX, day = d});
        lines.push_back({i, {}});
    }
    float content = 0;
    for (const Line& l : lines) content += l.file == SIZE_MAX ? groupH : rowH + 2;
    const float maxScroll = std::max(0.f, content - list.H());
    if (ui_.Hover(list) && ui_.In().wheel != 0) rpFileScroll_ -= ui_.In().wheel * rowH * 2;
    ui_.KeepInView(list, rpFileScroll_);
    rpFileScroll_ = std::clamp(rpFileScroll_, 0.f, maxScroll);
    const float scroll = ui_.Anim(HashId("rp.fileScroll"), rpFileScroll_, 22.f);
    ui_.PushClip(list);
    const float w = list.W() - (maxScroll > 0 ? 8 : 0);
    const uint64_t now = NowFileTime();
    float y = list.t - scroll;
    for (const Line& l : lines) {
        if (l.file == SIZE_MAX) {
            if (y + groupH >= list.t && y <= list.b)
                ui_.Text(Rect(list.l + 6, y + 8, list.l + w, y + groupH), l.day, 12.f, TextFaint, Bold());
            y += groupH;
            continue;
        }
        const size_t i = l.file;
        const ReplayFile& f = rpFiles_[i];
        Rect row = Rect::XYWH(list.l, y, w, rowH);
        y += rowH + 2;
        const SYSTEMTIME t = LocalTime(f.written);
        const bool fresh = SamePathI(f.path, rpNewPath_);
        const std::wstring title = f.program + util::Format(L" · %02d:%02d", t.wHour, t.wMinute);
        const std::wstring sub = util::FormatClock(f.seconds, false) + L" · " + util::FormatBytes(f.bytes) + L" · " +
                                 util::FormatAgo(now > f.written ? (now - f.written) / 1e7 : 0);
        const UiId rid = HashId("rp.file", (uint32_t)i);
        ui_.Declare(rid, row, Role::Item, title + L", " + sub);
        if (row.b < list.t || row.t > list.b) continue;
        if (fresh)
            ui_.Fill(row, Contrast() ? Surface2 : Mix(Surface2, Accent, 0.14f), 10);
        else if (ui_.Hover(row))
            ui_.Fill(row, Surface2, 10);
        // The program's icon, when one in the list has this name.
        std::wstring exe;
        for (const ReplayTarget& rt : settings_.rpTargets)
            if (!rt.monitor && SamePathI(util::SafeFileName(rt.name, 60), f.program)) exe = rt.path;
        DrawTargetIcon(Rect::XYWH(row.l + 10, row.CY() - 14, 28, 28), exe, f.program, 0);
        Rect more = Rect::Centered(row.r - 22, row.CY(), 30, 30);
        Rect folder = Rect::Centered(more.l - 4 - 15, row.CY(), 30, 30);
        const std::wstring edit = tr(L"편집");
        const float ew = ui_.Measure(edit, 12.5f, kSemibold) + 46;
        Rect editR = Rect::XYWH(folder.l - 8 - ew, row.CY() - 15, ew, 30);
        Rect text(row.l + 50, row.t, editR.l - 10, row.b);
        if (rpRenaming_ == (int)i) {
            const UiId fid = HashId("rp.rename");
            const bool esc = ui_.In().Key(VK_ESCAPE), enter = ui_.In().Key(VK_RETURN);
            ui_.Name(tr(L"새 이름"));
            ui_.TextField(fid, Rect(text.l, row.t + 6, text.r, row.t + 30), rpRenameText_, L"", 120);
            ui_.Name({});
            const bool focused = ui_.FocusedField() == fid;
            if (esc) {
                rpRenaming_ = -1;
            } else if (enter || (rpRenameFocused_ && !focused)) {
                rpRenaming_ = -1;
                RenameReplay(i, rpRenameText_);
                break;  // the list changed
            }
            rpRenameFocused_ = focused;
        } else {
            ui_.PushClip(text);
            const float titleW = ui_.Measure(title, 13.f, kSemibold) + 2;
            ui_.Text(Rect(text.l, row.t + 7, text.l + titleW, row.CY() + 2), title, 13.f, TextColor, Bold());
            if (fresh) {
                const std::wstring tag = tr(L"새로");
                Rect chip = Rect::XYWH(text.l + titleW + 6, row.t + 9, ui_.Measure(tag, 10.5f, kBold) + 12, 17);
                ui_.Fill(chip, WithAlpha(Accent, Contrast() ? 0.f : 0.18f), 6);
                if (Contrast()) ui_.Stroke(chip.Inset(0.5f), AccentHi, 6);
                TextStyle ts = Bold(AlignCenter);
                ts.weight = kBold;
                ui_.Text(chip, tag, 10.5f, AccentHi, ts);
            }
            ui_.PopClip();
        }
        ui_.PushClip(text);
        ui_.Text(Rect(text.l, row.CY() + 2, text.r, row.b - 7), sub, 12.f, TextFaint);
        ui_.PopClip();
        if (ui_.Button(HashId("rp.edit", (uint32_t)i), editR, edit, fresh ? BtnStyle::Primary : BtnStyle::Secondary,
                       icon::Cut, rec_ == RecState::Idle, 12.5f)) {
            EditReplay(f.path);
            break;
        }
        if (ui_.IconButton(HashId("rp.folder", (uint32_t)i), folder, icon::Folder, tr(L"폴더에서 보기"), true, 13.f))
            util::OpenFolderAndSelect(f.path);
        if (ui_.IconButton(HashId("rp.fileMore", (uint32_t)i), more, icon::More, tr(L"더 보기"), true, 13.f)) {
            rpFileMenu_ = (int)i;
            rpProgMenu_ = -1;
        }
        if (rpFileMenu_ == (int)i) rpMenuAnchor_ = more;
        if (ui_.Hover(row) && !ui_.Hover(editR) && !ui_.Hover(folder) && !ui_.Hover(more)) ui_.Tooltip(rid, row, f.path);
    }
    ui_.PopClip();
    if (maxScroll > 0) {
        float th = std::max(30.f, list.H() * list.H() / content);
        float ty = list.t + (list.H() - th) * (scroll / maxScroll);
        ui_.Fill(Rect::XYWH(list.r - 4, ty, 4, th), WithAlpha(TextFaint, 0.5f), 2);
    }
}

// Under 프로그램 추가: the programs that show a window now, an exe to pick, and the monitors.
void App::DrawReplayAddPopover(Rect client) {
    const auto& in = ui_.In();
    const float W = std::min(380.f, client.W() - 40), itemH = 50, labelH = 28;
    std::vector<const MonitorDesc*> mons;
    for (const MonitorDesc& m : monitors_)
        if (std::none_of(settings_.rpTargets.begin(), settings_.rpTargets.end(), [&](const ReplayTarget& t) {
                return t.monitor && _wcsicmp(t.path.c_str(), m.device.c_str()) == 0;
            }))
            mons.push_back(&m);
    const std::wstring note = tr(L"목록에는 창이 보이는 프로그램만 나옵니다. 고른 프로그램의 exe 경로와 이름 · 아이콘만 기억하고, 창 제목은 남기지 않습니다.");
    TextStyle wrap;
    wrap.wrap = true;
    wrap.v = AlignStart;
    DWRITE_TEXT_METRICS m{};
    float noteH = 34;
    if (auto lay = Fonts::Layout(note, 11.5f, wrap, W - 32, 200); lay && SUCCEEDED(lay->GetMetrics(&m)))
        noteH = std::ceil(m.height);
    const float listH = labelH + std::max<size_t>(rpRunning_.size(), 0) * itemH + (rpRunning_.empty() ? 34 : 0) + itemH +
                        (mons.empty() ? 0 : labelH + mons.size() * itemH);
    // Beside the list it adds to, over the saved replays, as high as it fits.
    const float headH = 44;
    const float want = headH + listH + 12 + noteH + 16;
    const float top = std::clamp(rpAddBtn_.t - 60, client.t + 72, std::max(client.t + 72, client.b - 12 - want));
    const float H = std::min(want, client.b - 12 - top);
    Rect box = Rect::XYWH(std::clamp(rpAddBtn_.r + 26, client.l + 12, client.r - 12 - W), top, W, H);
    const bool outside = !box.Contains(in.mx, in.my);
    if ((in.pressed && outside && !rpAddBtn_.Contains(in.mx, in.my)) || in.Key(VK_ESCAPE)) {
        rpAddOpen_ = false;
        return;
    }
    ui_.Fill(box.Offset(0, 6), WithAlpha(Shadow, 0.45f), 14);
    ui_.Fill(box, Surface2, 12);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 12);
    ui_.Declare(HashId("rp.addBox"), box, Role::Group, tr(L"리플레이를 켤 프로그램 추가"));
    Rect c = box.Inset(8);
    Rect head = c.CutTop(headH - 8);
    ui_.Text(Rect(head.l + 8, head.t, head.r - 36, head.b), tr(L"리플레이를 켤 프로그램 추가"), 13.5f, TextColor, Bold());
    if (ui_.IconButton(HashId("rp.addClose"), Rect::Centered(head.r - 16, head.CY(), 28, 28), icon::Close, tr(L"닫기"), true, 11.f)) {
        rpAddOpen_ = false;
        return;
    }
    Rect foot = c.CutBottom(noteH + 12);
    ui_.Line(foot.l + 4, foot.t + 0.5f, foot.r - 4, foot.t + 0.5f, Border);
    ui_.Text(Rect(foot.l + 8, foot.t + 9, foot.r - 8, foot.b), note, 11.5f, TextFaint, wrap);

    Rect list = c;
    const float maxScroll = std::max(0.f, listH - list.H());
    if (!outside && in.wheel != 0) rpAddScroll_ -= in.wheel * 60;
    ui_.KeepInView(list, rpAddScroll_);
    rpAddScroll_ = std::clamp(rpAddScroll_, 0.f, maxScroll);
    ui_.PushClip(list);
    Rect cur(list.l, list.t - rpAddScroll_, list.r, list.t - rpAddScroll_ + listH);
    auto label = [&](std::wstring_view s) { ui_.Text(cur.CutTop(labelH).Inset(8, 0), s, 12.f, TextFaint, Bold()); };
    auto item = [&](UiId id, std::wstring_view name, std::wstring_view sub, D2D1_COLOR_F subInk,
                    const std::function<void(Rect)>& drawIcon) {
        Rect r = cur.CutTop(itemH);
        const bool clicked = ui_.Click(id, r);
        ui_.Describe(id, std::wstring(name) + L", " + std::wstring(sub), Role::Item);
        if (ui_.Hover(r) || (ui_.FocusRing() && ui_.KeyboardFocus() == id)) ui_.Fill(r, Surface3, 8);
        drawIcon(Rect::XYWH(r.l + 8, r.CY() - 14, 28, 28));
        Rect t(r.l + 46, r.t, r.r - 8, r.b);
        ui_.PushClip(t);
        ui_.Text(Rect(t.l, r.t + 6, t.r, r.CY() + 2), name, 13.f, TextColor, Bold());
        ui_.Text(Rect(t.l, r.CY() + 1, t.r, r.b - 6), sub, 12.f, subInk);
        ui_.PopClip();
        return clicked;
    };
    label(tr(L"지금 실행 중"));
    if (rpRunning_.empty())
        ui_.Text(cur.CutTop(34).Inset(8, 0), tr(L"목록에 없는 실행 중인 프로그램이 없습니다"), 12.f, TextFaint);
    for (size_t i = 0; i < rpRunning_.size(); ++i) {
        const RunningProgram& p = rpRunning_[i];
        const std::wstring sub = util::FileNameOf(p.path) + (p.full ? tr(L" · 전체 화면으로 실행 중") : L"");
        if (item(HashId("rp.addRun", (uint32_t)i), p.name, sub, p.full ? AccentHi : TextFaint,
                 [&](Rect ic) { DrawTargetIcon(ic, p.path, p.name, 0); })) {
            ReplayTarget t{false, p.path, p.name, true};
            rpAddOpen_ = false;
            ui_.PopClip();
            AddReplayTarget(std::move(t));
            return;
        }
    }
    if (item(HashId("rp.addExe"), tr(L"exe 파일 찾아보기…"), tr(L"실행하지 않은 게임은 실행 파일로 넣습니다"), TextFaint,
             [&](Rect ic) {
                 ui_.Fill(ic, Surface3, 7);
                 ui_.Icon(ic, icon::Folder, 13.f, TextDim);
             })) {
        rpAddOpen_ = false;
        ui_.PopClip();
        BrowseReplayProgram();
        return;
    }
    if (!mons.empty()) label(tr(L"프로그램이 아닌 대상"));
    for (size_t i = 0; i < mons.size(); ++i) {
        const MonitorDesc& md = *mons[i];
        const std::wstring sub = util::Format(L"%d × %d", md.rect.right - md.rect.left, md.rect.bottom - md.rect.top) +
                                 (md.primary ? tr(L" · 주 모니터") : L"") + tr(L" · 프로그램이 하나도 실행되지 않을 때 담습니다");
        if (item(HashId("rp.addMon", (uint32_t)i), util::Format(tr(L"전체 화면 · 화면 %d"), md.number), sub, TextFaint,
                 [&](Rect ic) { DrawTargetIcon(ic, L"", L"", md.number); })) {
            ReplayTarget t{true, md.device, L"", true};
            rpAddOpen_ = false;
            ui_.PopClip();
            AddReplayTarget(std::move(t));
            return;
        }
    }
    ui_.PopClip();
}

// The ⋯ menus of the two lists.
void App::DrawReplayMenus(Rect client) {
    const auto& in = ui_.In();
    struct Item {
        std::wstring text;
        bool danger;
    };
    std::vector<Item> items;
    if (rpProgMenu_ >= 0) items = {{tr(L"목록에서 빼기"), false}};
    if (rpFileMenu_ >= 0) items = {{tr(L"이름 바꾸기"), false}, {tr(L"휴지통으로 버리기"), true}};
    float w = 0;
    for (const Item& it : items) w = std::max(w, ui_.Measure(it.text, 13.f) + 36);
    const float itemH = 34, pad = 5, h = pad * 2 + items.size() * itemH;
    float top = rpMenuAnchor_.b + 4;
    if (top + h > client.b - 8) top = rpMenuAnchor_.t - 4 - h;
    Rect box = Rect::XYWH(std::clamp(rpMenuAnchor_.r - w, client.l + 12, client.r - 12 - w), top, w, h);
    auto close = [&] {
        rpProgMenu_ = rpFileMenu_ = -1;
        Invalidate();
    };
    if ((in.pressed && !box.Contains(in.mx, in.my)) || in.Key(VK_ESCAPE)) {
        close();
        return;
    }
    ui_.Fill(box.Offset(0, 6), WithAlpha(Shadow, 0.45f), 12);
    ui_.Fill(box, Surface2, 10);
    ui_.Stroke(box.Inset(0.5f), BorderStrong, 10);
    ui_.Declare(HashId("rp.menu"), box, Role::Group, tr(L"더 보기"));
    Rect c = box.Inset(pad);
    for (size_t k = 0; k < items.size(); ++k) {
        Rect row = c.CutTop(itemH);
        const UiId id = HashId("rp.menuItem", (uint32_t)k);
        const bool clicked = ui_.Click(id, row);
        ui_.Describe(id, items[k].text, Role::Item);
        if (ui_.Hover(row) || (ui_.FocusRing() && ui_.KeyboardFocus() == id)) ui_.Fill(row, Surface3, 6);
        ui_.Text(Rect(row.l + 12, row.t, row.r - 8, row.b), items[k].text, 13.f, items[k].danger ? RecHi : TextColor);
        if (!clicked) continue;
        const int prog = rpProgMenu_, file = rpFileMenu_;
        close();
        if (prog >= 0) {
            RemoveReplayTarget((size_t)prog);
        } else if (file >= 0 && (size_t)file < rpFiles_.size() && k == 0) {
            rpRenaming_ = file;
            rpRenameFocused_ = false;
            std::wstring stem = util::FileNameOf(rpFiles_[file].path);
            if (stem.size() > 4) stem.resize(stem.size() - 4);
            rpRenameText_ = stem;
            ui_.Focus(HashId("rp.rename"));
        } else if (file >= 0 && (size_t)file < rpFiles_.size()) {
            TrashReplay((size_t)file);
        }
        return;
    }
}

// 저장 단축키's row is the same as the 녹화 tab's (HotkeyRow).
void App::SettingsReplay(Rect& area, bool& changed) {
    bool on = settings_.rpOn;
    SettingRow(area, tr(L"순간 리플레이"),
               on ? tr(L"켜져 있습니다 · 리플레이 탭의 프로그램이 실행되면 방금 지나간 장면을 메모리에 담습니다")
                  : tr(L"켜 두면 리플레이 탭에 고른 프로그램이 실행될 때마다 방금 지나간 장면을 메모리에 담습니다"),
               60, [&](Rect c) {
                   if (ui_.Toggle(HashId("set.rpOn"), Rect::Centered(c.r - 22, c.CY(), 44, 24), on)) SetReplayOn(on);
               });
    // What it holds costs memory: the bitrate times the length (and the keyframe interval it keeps over).
    int sw = 1920, sh = 1080;
    if (rpCap_.Running() && rpCap_.OutWidth() > 0) {
        sw = rpCap_.OutWidth();
        sh = rpCap_.OutHeight();
    } else {
        for (const MonitorDesc& m : monitors_)
            if (m.primary) sw = m.rect.right - m.rect.left, sh = m.rect.bottom - m.rect.top;
        float f = 1;
        if (int res = kVideoResOptions[settings_.rpResIdx]) {
            f = std::min(f, std::min((float)res / std::min(sw, sh), (float)res * 16 / 9 / std::max(sw, sh)));
        }
        f = std::min(f, 4096.f / std::max(sw, sh));
        sw = (int)std::lround(sw * f / 2) * 2;
        sh = (int)std::lround(sh * f / 2) * 2;
    }
    const int fps = kVideoFpsOptions[settings_.rpFpsIdx];
    const uint64_t bps = (settings_.VideoBitrate() ? settings_.VideoBitrate() : Mp4Writer::AutoBitrate(sw, sh, fps, false)) +
                         (settings_.rpAudio || settings_.rpMic ? kAudioBitrateOptions[settings_.aBitrateIdx] * 1000 : 0);
    const uint64_t bytes = bps / 8 * (kReplayLenOptions[settings_.rpLenIdx] + kKeyframeOptions[settings_.vKeyIdx] + 2);
    SettingRow(area, tr(L"리플레이 길이"),
               util::Format(tr(L"메모리 약 %s · %d×%d %dfps 기준"), util::FormatBytes(bytes).c_str(), sw, sh, fps), 330,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.rpLen"), c,
                                            {LenName(15), LenName(30), LenName(60), LenName(120), LenName(300)},
                                            settings_.rpLenIdx);
               });
    SettingRow(area, tr(L"프레임"), tr(L"게임은 60이면 충분합니다 · 켜 둔 채 바꾸면 처음부터 다시 담습니다"), 230, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.rpFps"), c, {L"24", L"30", L"60"}, settings_.rpFpsIdx);
    });
    SettingRow(area, tr(L"해상도"), tr(L"원본보다 크게 키우지는 않습니다 · 낮추면 메모리를 덜 씁니다"), 380, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.rpRes"), c, {tr(L"원본"), L"2160p", L"1440p", L"1080p", L"720p"},
                                 settings_.rpResIdx);
    });
    SettingRow(area, tr(L"소리"), tr(L"게임 · 앱 소리를 함께 담습니다"), 60, [&](Rect c) {
        changed |= ui_.Toggle(HashId("set.rpAudio"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.rpAudio);
    });
    SettingRow(area, tr(L"마이크"),
               settings_.rpMic ? tr(L"켜져 있습니다 · 방 소리가 늘 메모리에 담깁니다 · 상태 띠에 ‘마이크 포함’이 보입니다")
                               : tr(L"처음에는 꺼져 있습니다 · 녹화 탭의 마이크와 따로 정합니다"),
               60, [&](Rect c) {
                   changed |= ui_.Toggle(HashId("set.rpMic"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.rpMic);
               });
    SettingRow(area, tr(L"마우스 커서"), tr(L"커서를 함께 담습니다"), 60, [&](Rect c) {
        changed |= ui_.Toggle(HashId("set.rpCursor"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.rpCursor);
    });
    HotkeyRow(area, kHotkeyReplay, tr(L"저장 단축키"),
              settings_.rpOn && !replayHotkeyOk_
                  ? tr(L"⚠ 다른 프로그램이 사용 중이라 등록하지 못했습니다. 다른 조합을 지정하세요")
              : settings_.rpHkHook ? tr(L"다른 프로그램보다 먼저 받습니다 · 그 프로그램에서는 이 조합이 듣지 않습니다")
                                   : tr(L"리플레이가 켜져 있을 때만 씁니다 · 방금 지나간 장면을 MP4로 저장합니다"));
    SettingRow(area, tr(L"저장 폴더"),
               tr(L"저장 폴더 › 리플레이 › 프로그램 이름 폴더에 MP4로 저장합니다 · 리플레이 탭의 목록은 이 폴더를 읽습니다"), 110,
               [&](Rect c) {
                   if (ui_.Button(HashId("set.rpDir"), c, tr(L"열기"), BtnStyle::Secondary, icon::Folder, true, 13.f)) {
                       util::EnsureDir(ReplayDir());
                       util::ShellOpen(ReplayDir());
                   }
               });
    SettingRow(area, tr(L"화질"), tr(L"코덱 · 비트레이트 · 키프레임 간격은 설정 › 동영상을 따릅니다 (리플레이는 H.264)"), 150,
               [&](Rect c) {
                   if (ui_.Button(HashId("set.rpVideo"), c, tr(L"동영상 설정 ›"), BtnStyle::Secondary, 0, true, 13.f))
                       OpenSettings(kTabVideo);
               });
}

// ---------------------------------------------------------------- editor

void App::DrawEditor(Rect r) {
    r.CutTop(20);
    if (!clip_) {
        DrawEditHome(r);
        return;
    }
    Rect right = r.CutRight(316);
    r.CutRight(20);
    Rect left = r;
    DrawFileBar(left.CutTop(36));
    left.CutTop(10);
    Rect tl = left.CutBottom(84);
    left.CutBottom(8);
    Rect cb;
    if (captionOpen_) {
        cb = left.CutBottom(36);
        left.CutBottom(8);
    }
    Rect tr = left.CutBottom(44);
    left.CutBottom(10);

    // Keyboard shortcuts, unless the caption is being typed
    const UiInput& in = ui_.TextFocused() ? UiInput() : ui_.In();
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
    const bool plain = !in.ctrl && !in.alt;  // a letter on its own
    if (plain && in.Key('I')) {
        inF_ = std::min(playhead_, outF_);
        hasResult_ = false;
        Seek(inF_);
    }
    if (plain && in.Key('O')) {
        outF_ = std::max(playhead_, inF_);
        hasResult_ = false;
    }
    // J, K, L as in editing programs: backward, stop, forward, and again for 2× and 4×. R: the whole clip again.
    if (plain && in.Key('L')) Shuttle(1);
    if (plain && in.Key('J')) Shuttle(-1);
    if (plain && in.Key('K')) SetPlaying(false);
    if (plain && in.Key('R')) ResetRange();
    if (in.Key(VK_ESCAPE)) cropMode_ = false;
    if (in.Key(VK_DELETE) && CanDiscard()) ConfirmDiscard();

    if (playing_) playhead_ = PlayheadFromClock();
    playhead_ = std::clamp(playhead_, (size_t)0, count - 1);

    DrawPreview(left);
    DrawTransport(tr);
    if (captionOpen_) DrawCaptionBar(cb);  // after the 자막 button, which gives it the focus
    DrawTimeline(tl);
    DrawExportPanel(right);
}

void App::DrawCaptionBar(Rect r) {
    Rect close = r.CutRight(34);
    r.CutRight(6);
    Rect pos = r.CutRight(120);
    r.CutRight(8);
    if (ui_.TextField(HashId("cap.text"), r, caption_, tr(L"자막을 입력하세요 (한 줄)"), 60)) hasResult_ = false;
    ui_.Name(tr(L"자막 위치"));
    if (ui_.Segmented(HashId("cap.pos"), pos, {tr(L"아래"), tr(L"위")}, captionPos_)) hasResult_ = false;
    if (ui_.IconButton(HashId("cap.clear"), Rect::Centered(close.CX(), close.CY(), 34, 34), icon::Close, tr(L"자막 빼기"),
                       true, 12.f)) {
        caption_.clear();
        captionOpen_ = false;
        hasResult_ = false;
    }
}

std::wstring App::ClipTitle() const {
    if (!clip_) return {};
    return clip_->SourcePath().empty() ? clipName_ : util::FileNameOf(clip_->SourcePath());
}

// Under an opened 움짤's transparent parts (#41): a checkerboard while it is saved as a 움짤, which keeps them, and
// white for a video, which fills them so.
void App::DrawAlphaBacking(const Rect& dst) {
    if (IsVideoFormat(fmt_)) {
        ui_.Fill(dst, D2D1::ColorF(1, 1, 1));
        return;
    }
    if (!checker_ || checkerGen_ != gfx_.Generation()) {
        constexpr int kCell = 10;
        uint32_t px[kCell * 2 * kCell * 2];
        for (int y = 0; y < kCell * 2; ++y)
            for (int x = 0; x < kCell * 2; ++x) px[y * kCell * 2 + x] = (x < kCell) == (y < kCell) ? 0xFFFFFFFF : 0xFFD4D4D4;
        ComPtr<ID2D1Bitmap1> bmp = gfx_.CreateBitmap(kCell * 2, kCell * 2, px, kCell * 2 * 4);
        checker_.Reset();
        if (bmp)
            gfx_.DC()->CreateBitmapBrush(bmp.Get(),
                                         D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                      D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                         &checker_);
        checkerGen_ = gfx_.Generation();
    }
    if (!checker_) return;
    checker_->SetTransform(D2D1::Matrix3x2F::Translation(dst.l, dst.t));
    gfx_.DC()->FillRectangle(D2D1::RectF(dst.l, dst.t, dst.r, dst.b), checker_.Get());
}

void App::DrawFileBar(Rect r) {
    const bool video = clip_->IsVideo();
    auto* anim = dynamic_cast<const AnimClip*>(clip_.get());
    const bool held = std::any_of(held_.begin(), held_.end(), [&](const HeldClip& h) { return h.clip == clip_; });
    Rect close = r.CutRight(84);
    close = Rect::Centered(close.CX(), close.CY(), 84, 32);
    if (ui_.Button(HashId("edit.close"), close, tr(L"닫기"), BtnStyle::Ghost, icon::Close, !exporter_.Busy(), 12.5f))
        CloseClip();
    if (held) ui_.Tooltip(HashId("edit.close"), close, tr(L"닫아도 ‘저장하지 않은 녹화’에 남습니다"));
    r.CutRight(12);
    Rect ic = r.CutLeft(18);
    if (video)
        ui_.Icon(ic, icon::Video, 14.f, AccentHi);
    else if (anim)
        ui_.Icon(ic, icon::Photo, 14.f, Success);
    else
        ui_.Circle(ic.CX(), ic.CY(), 5.f, Rec);
    r.CutLeft(8);
    std::wstring kind = video  ? tr(L"동영상")
                        : held ? tr(L"움짤 녹화 · 저장하지 않음")
                               : tr(L"움짤 녹화");
    if (anim) {
        kind = util::Format(tr(L"%s 파일 · 프레임 %zu개"), FormatName(anim->FileFormat()), clip_->Count());
        if (anim->HasAlpha()) kind += tr(L" · 투명 있음");
    }
    std::wstring meta = kind + util::Format(L" · %d×%d · ", clip_->Width(), clip_->Height()) +
                        util::FormatClock(clip_->Seconds(), true);
    const std::wstring name = ClipTitle();
    const float mw = ui_.Measure(meta, 12.f) + 4;
    const float nw = std::min(ui_.Measure(name, 13.5f, kSemibold) + 4, std::max(80.f, r.W() - mw - 12));
    ui_.Text(r.CutLeft(nw), name, 13.5f, TextColor, Bold());
    r.CutLeft(12);
    ui_.Text(r, meta, 12.f, TextFaint);
}

void App::DrawEditHome(Rect r) {
    // Where to drop a video, with the button for those who would rather pick one.
    Rect dz = r.CutTop(std::clamp(r.H() * 0.42f, 200.f, 250.f));
    r.CutTop(16);
    ui_.Fill(dz, WithAlpha(Accent, 0.06f), 16);
    ui_.StrokeDashed(dz.Inset(1.f), P.Dashed, 16, 2.f, 7.f, 5.f);
    const float stackH = 56 + 12 + 26 + 6 + 42 + 14 + 36;
    float y = std::round(dz.CY() - stackH / 2);
    Rect ic = Rect::Centered(dz.CX(), y + 28, 56, 56);
    ui_.Fill(ic, WithAlpha(Accent, 0.16f), 16);
    ui_.Icon(ic, icon::Download, 24.f, AccentHi);
    y += 56 + 12;
    ui_.Text(Rect(dz.l, y, dz.r, y + 26), tr(L"동영상이나 움짤을 여기에 끌어다 놓으세요"), 18.f, TextColor,
             Bold(AlignCenter));
    y += 26 + 6;
    TextStyle lines = Align(AlignCenter, AlignStart);
    lines.wrap = true;
    ui_.Text(Rect(dz.l + 20, y, dz.r - 20, y + 42),
             tr(L"MP4, MOV, MKV, WebM 같은 동영상과\nGIF, WebP, AVIF 움짤을 열 수 있습니다."), 13.f, TextDim, lines);
    y += 42 + 14;
    const float bw = ui_.Measure(tr(L"파일 열기…"), 13.f, kSemibold) + 13 + 8 + 40;
    if (ui_.Button(HashId("edit.browse"), Rect::Centered(dz.CX(), y + 18, bw, 36), tr(L"파일 열기…"), BtnStyle::Primary,
                   icon::OpenFile, true, 13.f))
        BrowseForVideo();

    // What there is to open: this run's recordings not saved yet, and the files saved or opened lately.
    const float rowH = 52;
    enum class Act { None, OpenHeld, DropHeld, OpenRecent, Folder } act = Act::None;
    size_t actAt = 0;
    auto panel = [&](int k, Rect p, const std::wstring& title, size_t rows, std::wstring_view empty,
                     const std::function<void(size_t, Rect)>& row) {
        ui_.Fill(p, Surface, 14);
        ui_.Stroke(p.Inset(0.5f), Border, 14);
        Rect in = p.Inset(14, 12);
        ui_.Text(in.CutTop(24).Inset(8, 0), title, 12.f, TextFaint, Bold());
        in.CutTop(4);
        if (rows == 0) {
            TextStyle st = Align(AlignStart, AlignStart);
            st.wrap = true;
            ui_.Text(in.Inset(8, 6), empty, 12.5f, TextFaint, st);
            return;
        }
        const float content = rows * rowH, maxScroll = std::max(0.f, content - in.H());
        float& scroll = editListScroll_[k];
        if (ui_.Hover(in) && ui_.In().wheel != 0) scroll -= ui_.In().wheel * rowH;
        scroll = std::clamp(scroll, 0.f, maxScroll);
        ui_.PushClip(in);
        for (size_t i = 0; i < rows; ++i) {
            Rect rr = Rect::XYWH(in.l, in.t + i * rowH - scroll, in.W() - (maxScroll > 0 ? 8 : 0), rowH);
            if (rr.b < in.t || rr.t > in.b) continue;
            if (ui_.Hover(rr)) ui_.Fill(rr, Surface2, 10);
            row(i, rr);
        }
        ui_.PopClip();
        if (maxScroll > 0) {
            float th = std::max(24.f, in.H() * in.H() / content);
            float ty = in.t + (in.H() - th) * (scroll / maxScroll);
            ui_.Fill(Rect(in.r - 3, ty, in.r, ty + th), BorderStrong, 1.5f);
        }
    };
    auto item = [&](Rect rr, wchar_t glyph, D2D1_COLOR_F tint, const std::wstring& name, const std::wstring& sub) {
        Rect ic = Rect::XYWH(rr.l + 8, rr.CY() - 17, 34, 34);
        ui_.Fill(ic, WithAlpha(tint, 0.15f), 9);
        ui_.Icon(ic, glyph, 15.f, tint);
        Rect text(ic.r + 10, rr.t, rr.r - 8 - 30 - 6 - 74 - 10, rr.b);
        ui_.Text(Rect(text.l, rr.t + 7, text.r, rr.CY() + 2), name, 13.f, TextColor, Bold());
        ui_.Text(Rect(text.l, rr.CY() + 1, text.r, rr.b - 7), sub, 12.f, TextFaint);
    };
    auto edit = [&](Rect rr) { return Rect::XYWH(rr.r - 8 - 30 - 6 - 74, rr.CY() - 15, 74, 30); };
    auto side = [&](Rect rr) { return Rect::XYWH(rr.r - 8 - 30, rr.CY() - 15, 30, 30); };

    Rect left = r.CutLeft(std::floor((r.W() - 16) / 2));
    r.CutLeft(16);
    panel(0, left, util::Format(tr(L"저장하지 않은 녹화 %zu개"), held_.size()), held_.size(),
          tr(L"아직 없습니다. 저장하지 않은 움짤 녹화가 여기에 남습니다."), [&](size_t i, Rect rr) {
              const HeldClip& h = held_[i];
              item(rr, icon::Record, Accent2, h.name,
                   util::FormatClock(h.clip->Seconds(), true) + L" · " + util::FormatAgo(util::NowSeconds() - h.when));
              if (ui_.Button(HashId("edit.held", (uint32_t)i), edit(rr), tr(L"편집"), BtnStyle::Secondary, icon::Cut, true,
                             12.5f))
                  act = Act::OpenHeld, actAt = i;
              if (ui_.IconButton(HashId("edit.drop", (uint32_t)i), side(rr), icon::Delete, tr(L"버리기")))
                  act = Act::DropHeld, actAt = i;
          });
    panel(1, r, tr(L"최근 파일"), recentFiles_.size(), tr(L"저장하거나 연 파일이 여기에 표시됩니다."), [&](size_t i, Rect rr) {
        const RecentFile& f = recentFiles_[i];
        item(rr, f.video ? icon::Video : icon::Photo, f.video ? Accent : Success, util::FileNameOf(f.path),
             util::FormatBytes(f.bytes) + L" · " + util::FormatAgo(f.age));
        const UiId eid = HashId("edit.recent", (uint32_t)i);
        if (ui_.Button(eid, edit(rr), tr(L"편집"), BtnStyle::Secondary, icon::Cut, true, 12.5f))
            act = Act::OpenRecent, actAt = i;
        if (ui_.IconButton(HashId("edit.folder", (uint32_t)i), side(rr), icon::Folder, tr(L"폴더에서 보기")))
            act = Act::Folder, actAt = i;
        Rect name(rr.l, rr.t, edit(rr).l - 10, rr.b);
        if (ui_.Hover(name)) ui_.Tooltip(HashId("edit.tip", (uint32_t)i), name, f.path);
    });
    switch (act) {
        case Act::OpenHeld: OpenHeld(actAt); break;
        case Act::DropHeld: ConfirmDropHeld(actAt); break;
        case Act::OpenRecent: OpenMediaFile(std::wstring(recentFiles_[actAt].path)); break;
        case Act::Folder: util::OpenFolderAndSelect(recentFiles_[actAt].path); break;
        case Act::None: break;
    }
}

void App::DrawDropCue(Rect client) {
    const bool yes = DragOpens();
    const D2D1_COLOR_F tint = yes ? Accent : Warning;
    ui_.Fill(client, WithAlpha(P.DropScrim, 0.86f));
    Rect box = client.Inset(14);
    ui_.Fill(box, WithAlpha(tint, yes ? 0.10f : 0.07f), 18);
    ui_.StrokeDashed(box, tint, 18, 2.5f, 9.f, 6.f);
    const wchar_t *title = tr(L"이 파일은 열 수 없습니다"), *sub = tr(L"동영상이나 움짤 파일만 열 수 있습니다");
    switch (dragKind_) {
        case DragKind::Video: title = tr(L"놓으면 편집 탭에서 엽니다"), sub = tr(L"MP4, MOV, MKV, WebM 같은 동영상"); break;
        case DragKind::Anim: title = tr(L"놓으면 편집 탭에서 엽니다"), sub = tr(L"GIF, WebP, AVIF 움짤"); break;
        case DragKind::Busy:
            if (rec_ != RecState::Idle)
                title = tr(L"녹화하는 동안은 열 수 없습니다"), sub = tr(L"녹화를 멈춘 뒤에 놓으세요");
            else
                title = tr(L"저장하는 동안은 열 수 없습니다"), sub = tr(L"저장이 끝난 뒤에 놓으세요");
            break;
        default: break;
    }
    const float h = 72 + 16 + 30 + 6 + 22 + (dragName_.empty() ? 0.f : 14 + 30);
    float y = std::round(client.CY() - h / 2);
    Rect ic = Rect::Centered(client.CX(), y + 36, 72, 72);
    ui_.Fill(ic, WithAlpha(tint, yes ? 0.2f : 0.16f), 20);
    if (yes) {
        ui_.Icon(ic, icon::Download, 30.f, AccentHi);
    } else {  // a "no" sign: MDL2 has none that reads well at this size
        const float rr = 14, d = rr * 0.7071f;
        ui_.Ring(ic.CX(), ic.CY(), rr, Warning, 2.5f);
        ui_.Line(ic.CX() - d, ic.CY() + d, ic.CX() + d, ic.CY() - d, Warning, 2.5f);
    }
    y += 72 + 16;
    ui_.Text(Rect(box.l, y, box.r, y + 30), title, 22.f, TextColor, Bold(AlignCenter));
    y += 30 + 6;
    ui_.Text(Rect(box.l, y, box.r, y + 22), sub, 14.f, TextDim, Align(AlignCenter));
    y += 22 + 14;
    if (!dragName_.empty()) {
        const float w = std::min(ui_.Measure(dragName_, 13.f) + 28, box.W() - 60);
        Rect chip = Rect::Centered(client.CX(), y + 15, w, 30);
        ui_.Fill(chip, Surface3, 8);
        ui_.Text(chip.Inset(14, 0), dragName_, 13.f, TextColor, Align(AlignCenter));
    }
}

void App::DrawPreview(Rect r) {
    ui_.Fill(r, P.Preview, 14);
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
        if (clip_->HasAlpha()) DrawAlphaBacking(dst);
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
    ui_.Describe(id, playing_ ? tr(L"미리보기, 누르면 일시정지") : tr(L"미리보기, 누르면 재생"));

    // The caption over what gets saved, drawn the way the export draws it.
    if (!caption_.empty() && frameBmp_) {
        ui_.Painted(cr);
        caption::Draw(gfx_.DC(), gfx_.DWrite(), caption_, captionPos_, cr);
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
                       playing_ ? icon::Pause : icon::Play, playing_ ? tr(L"일시정지 (Space)") : tr(L"재생 (Space)"), true,
                       18.f))
        SetPlaying(!playing_);
    r.CutLeft(4);
    Rect prevR = r.CutLeft(34), nextR = r.CutLeft(34);
    if (ui_.IconButton(HashId("tr.prev"), Rect::Centered(prevR.CX(), prevR.CY(), 34, 34), icon::Prev,
                       tr(L"이전 프레임 (←)"), playhead_ > 0, 12.f)) {
        SetPlaying(false);
        Seek(playhead_ - 1);
    }
    if (ui_.IconButton(HashId("tr.next"), Rect::Centered(nextR.CX(), nextR.CY(), 34, 34), icon::Next,
                       tr(L"다음 프레임 (→)"), playhead_ + 1 < clip_->Count(), 12.f)) {
        SetPlaying(false);
        Seek(playhead_ + 1);
    }
    if (clip_->HasAudio()) {
        Rect volR = r.CutLeft(34);
        bool slow = kSpeedOptions[settings_.speedIdx] != 1.0;
        const int vol = previewMute_ ? 0 : settings_.previewVolume;
        const wchar_t* tip = slow ? tr(L"재생 속도가 1×일 때만 소리가 나옵니다")
                             : vol == 0 ? tr(L"미리보기 소리 켜기") : tr(L"미리보기 소리 끄기");
        if (ui_.IconButton(HashId("tr.mute"), Rect::Centered(volR.CX(), volR.CY(), 34, 34),
                           vol == 0 || slow ? icon::Mute : icon::Volume, tip, true, 13.f))
            TogglePreviewMute();
        // The volume (#36). Crop mode pauses the preview and needs the room.
        if (!cropMode_) {
            Rect volS = r.CutLeft(78);
            Rect track(volS.l + 8, volS.t + 8, volS.r - 8, volS.b - 8);
            UiId id = HashId("tr.vol");
            float v = (float)vol;
            ui_.Name(tr(L"미리보기 음량"));
            if (ui_.Slider(id, track, v, 0.f, 100.f)) SetPreviewVolume((int)std::lround(v / 5) * 5);
            if (ui_.In().wheel != 0 && ui_.Hover(Rect(volR.l, volS.t, volS.r, volS.b)))
                SetPreviewVolume(vol + (ui_.In().wheel > 0 ? 5 : -5));
            ui_.Tooltip(id, track, util::Format(tr(L"미리보기 음량 %d%% · 휠로도 조절"), vol));
        }
    }
    r.CutLeft(10);
    std::wstring time = util::FormatClock(clip_->Ts(playhead_) / 1e7, true) + L"  /  " +
                        util::FormatClock(clip_->Seconds(), true);
    TextStyle st;
    st.tabular = true;
    Rect timeR = r.CutLeft(150);
    ui_.Text(timeR, time, 13.f, TextDim, st);
    // J / L faster than 1× or backwards, right after the time.
    if (playing_ && shuttle_ && shuttle_ != 1) {
        std::wstring s = util::Format(L"%s %d×", shuttle_ < 0 ? L"◀" : L"▶", std::abs(shuttle_));
        float bw = ui_.Measure(s, 12.f, kSemibold) + 14;
        Rect badge = Rect::XYWH(std::round(timeR.l + ui_.Measure(time, 13.f) + 8), timeR.CY() - 11, bw, 22);
        ui_.Fill(badge, WithAlpha(Accent, 0.18f), 6);
        TextStyle bs = Bold(AlignCenter);
        bs.tabular = true;
        ui_.Text(badge, s, 12.f, AccentHi, bs);
    }

    // Crop controls on the right, 자막 next to them (out of the way while cropping).
    Rect cropBtn = r.CutRight(92);
    if (ui_.Button(HashId("tr.crop"), Rect::Centered(cropBtn.CX(), cropBtn.CY(), 92, 36), tr(L"자르기"),
                   cropMode_ ? BtnStyle::Primary : BtnStyle::Secondary, icon::Crop, true, 13.f)) {
        cropMode_ = !cropMode_;
        if (cropMode_) SetPlaying(false);
    }
    if (!cropMode_) {
        r.CutRight(8);
        Rect capBtn = r.CutRight(84);
        UiId id = HashId("tr.caption");
        if (ui_.Button(id, Rect::Centered(capBtn.CX(), capBtn.CY(), 84, 36), tr(L"자막"),
                       captionOpen_ ? BtnStyle::Primary : BtnStyle::Secondary, icon::Font, true, 13.f)) {
            captionOpen_ = !captionOpen_;
            if (captionOpen_) ui_.Focus(HashId("cap.text"));
        }
        if (!captionOpen_ && !caption_.empty()) ui_.Tooltip(id, capBtn, tr(L"자막: ") + caption_);
    }
    if (cropMode_) {
        r.CutRight(8);
        Rect reset = r.CutRight(68);
        if (ui_.Button(HashId("tr.cropreset"), Rect::Centered(reset.CX(), reset.CY(), 68, 36), tr(L"초기화"),
                       BtnStyle::Ghost, 0, true, 13.f)) {
            cropAspect_ = 0;
            ResetCrop();
            hasResult_ = false;
        }
        r.CutRight(6);
        Rect seg = r.CutRight(std::min(270.f, r.W()));
        ui_.Name(tr(L"비율"));
        if (ui_.Segmented(HashId("tr.aspect"), Rect::Centered(seg.CX(), seg.CY(), seg.W(), 36),
                          {tr(L"자유"), L"1:1", L"4:3", L"16:9", L"9:16"}, cropAspect_)) {
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
    // A video cut without encoding again starts at the keyframe before the range.
    const wchar_t* copyBlocker = CopyBlocker();
    const size_t key = copyBlocker && !*copyBlocker ? cutSource_->KeyBefore(inF_) : inF_;
    TextStyle st;
    st.tabular = true;
    std::wstring range = tr(L"구간  ") + util::FormatClock(selStart, true) + L" – " + util::FormatClock(selEnd, true);
    std::wstring note = key < inF_ ? util::Format(tr(L" · 시작은 키프레임 %s에 맞춰집니다"),
                                                  util::FormatClock(clip_->Ts(key) / 1e7, true).c_str())
                                   : std::wstring();
    std::wstring keys = util::Format(tr(L"%.1f초 · I / O 구간 · J / K / L 재생 · R 처음으로"), selEnd - selStart);
    const float rangeW = ui_.Measure(range, 12.f) + 2, noteW = note.empty() ? 0 : ui_.Measure(note, 12.f) + 2;
    if (rangeW + noteW + ui_.Measure(keys, 12.f) + 16 > labels.W()) keys = util::Format(tr(L"%.1f초"), selEnd - selStart);
    ui_.Text(labels, range, 12.f, TextDim, st);
    if (!note.empty()) ui_.Text(Rect(labels.l + rangeW, labels.t, labels.r, labels.b), note, 12.f, Warning, st);
    st.h = AlignEnd;
    ui_.Text(labels, keys, 12.f, TextFaint, st);

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
    // A reader hears where they are, in seconds; I, O and the arrow keys move them.
    auto at = [&](UiId tid, const wchar_t* name, size_t f, const wchar_t* keys) {
        if (UiNode* n = ui_.Describe(tid, name, Role::Slider))
            n->value = (float)(clip_->Ts(f) / 1e7), n->hi = (float)clip_->Seconds(), n->lo = 0, n->keys = keys;
    };
    at(idIn, tr(L"시작 지점"), inF_, L"I");
    at(idOut, tr(L"끝 지점"), outF_, L"O");
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
    at(idScrub, tr(L"재생 위치"), playhead_, L"");
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
    if (key < inF_) {
        float kx = X(clip_->Ts(key));
        ui_.Line(kx, track.t, kx, track.b, Warning, 2.f);
    }
    for (Rect h : {hIn, hOut}) {
        ui_.Fill(h, Accent, 4);
        ui_.Line(h.CX(), h.CY() - 8, h.CX(), h.CY() + 8, WithAlpha(OnFill, 0.85f), 1.5f);
    }
    float px = X(clip_->Ts(playhead_));
    const D2D1_COLOR_F head = P.light ? TextColor : White;  // its knob is over the panel, not the pictures
    ui_.Line(px, track.t - 4, px, track.b + 2, head, 2.f);
    ui_.Circle(px, track.t - 4, 4.5f, head);
}

void App::DrawExportPanel(Rect r) {
    ui_.Fill(r, Surface, 14);
    ui_.Stroke(r.Inset(0.5f), Border, 14);
    Rect in = r.Inset(18);
    bool busy = exporter_.Busy();

    ui_.Text(in.CutTop(28), tr(L"내보내기"), 16.f, TextColor, Bold());
    in.CutTop(8);

    uint32_t disabled = 0;
    for (int i = 0; i < (int)Format::Count; ++i)
        if (!FormatAvailable((Format)i)) disabled |= 1u << i;
    Format fmt = (Format)fmt_;
    const bool mp4 = fmt == Format::Mp4, webm = fmt == Format::WebM;
    const bool isVideo = IsVideoFormat(fmt);  // MP4 or WebM: a constant rate, sound, always forward
    const double speed = kSpeedOptions[settings_.speedIdx];
    int ow = 0, oh = 0;
    OutputSize(ow, oh);
    double fpsLimit = EffectiveOutFps(), outSec = 0, outFps = 0;
    const int direction = isVideo ? 0 : settings_.directionIdx;
    PlanSummary& pc = planCache_;
    if (pc.clip != clip_.get() || pc.count != clip_->Count() || pc.first != inF_ || pc.last != outF_ ||
        pc.fps != fpsLimit || pc.speed != speed || pc.video != isVideo || pc.direction != direction) {
        pc = {clip_.get(), clip_->Count(), inF_, outF_, fpsLimit, speed, isVideo, direction};
        if (isVideo) {
            pc.frames = PlanVideoFrames(*clip_, inF_, outF_, fpsLimit, speed).size();
        } else {
            auto plan = PlanFrames(*clip_, inF_, outF_, fpsLimit, speed, direction);
            for (auto& p : plan) pc.ms += p.durationMs;
            pc.frames = plan.size();
        }
    }
    const size_t outFrames = pc.frames;
    if (isVideo) {
        outFps = fpsLimit;
        outSec = outFrames / fpsLimit;
    } else {
        outSec = pc.ms / 1000.0;
        outFps = outSec > 0 ? outFrames / outSec : 0;
        if (settings_.holdLast) outSec += kHoldLastMs / 1000.0;
    }
    // A video cut as it is: its size is what it holds, no estimate needed.
    const wchar_t* copyBlocker = CopyBlocker();
    const bool copying = copyBlocker && !*copyBlocker;
    if (!busy && !copying) UpdateEstimate();
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
    ui_.KeepInView(view, exportScroll_);
    exportScroll_ = std::clamp(exportScroll_, 0.f, maxScroll);
    ui_.PushClip(view);
    in = Rect(view.l, view.t - exportScroll_, view.r - (maxScroll > 0 ? 8 : 0), view.t - exportScroll_ + 10000);
    const float top = in.t;

    auto label = [&](std::wstring_view name, std::wstring_view value) {
        Rect l = in.CutTop(20);
        ui_.Text(l, name, 12.5f, TextDim);
        ui_.Name(name);
        if (!value.empty()) ui_.Text(l, value, 12.5f, TextColor, Align(AlignEnd));
        in.CutTop(4);
    };
    bool changed = false;

    // A hint under a row, on two lines when one is too short.
    auto hintText = [&](const wchar_t* hint, D2D1_COLOR_F c) {
        TextStyle st;
        st.wrap = true;
        ui_.Text(in.CutTop(ui_.Measure(hint, 11.5f) > in.W() ? 34.f : 20.f), hint, 11.5f, c, st);
    };

    // 움짤 on one row and the videos on the next (#40), so it shows which kind sound, 반복 and 재생 방향 go with.
    label(tr(L"형식"), {});
    {
        // The kinds in a column, 40 wide unless a translation needs more.
        const float col = std::max({40.f, ui_.Measure(tr(L"움짤"), 12.f), ui_.Measure(tr(L"동영상"), 12.f)});
        auto row = [&](UiId id, const wchar_t* kind, const wchar_t* name, int first,
                       std::initializer_list<std::wstring_view> items) {
            Rect line = in.CutTop(32);
            ui_.Text(line.CutLeft(col), tr(kind), 12.f, TextFaint);
            line.CutLeft(8);
            int sel = fmt_ >= first && fmt_ < first + (int)items.size() ? fmt_ - first : -1;
            ui_.Name(tr(name));
            if (!ui_.Segmented(id, line, items, sel, disabled >> first)) return false;
            fmt_ = first + sel;
            return true;
        };
        bool picked = row(HashId("ex.fmt"), L"움짤", L"움짤 형식", 0, {L"GIF", L"WebP", L"AVIF"});
        in.CutTop(6);
        picked |= row(HashId("ex.fmt.video"), L"동영상", L"동영상 형식", (int)Format::Mp4, {L"MP4", L"WebM"});
        if (picked) {
            changed = true;
            hasResult_ = false;
            if (!IsVideoFormat(fmt_)) settings_.format = fmt_;  // the 움짤 format the next clip starts with
            // 움짤 and the videos each keep their own 화질.
            settings_.UseExportKind(IsVideoFormat(fmt_));
            if (settings_.exportPreset >= 0) settings_.ApplyPreset(settings_.exportPreset, IsVideoFormat(fmt_));
            FollowDirection();  // a video always plays forward
        }
    }
    in.CutTop(4);
    if (fmt == Format::Mp4) ProbeEncoders();
    std::wstring blurb = (disabled & (1u << fmt_)) ? tr(L"이 빌드에는 포함되어 있지 않습니다")
                         : fmt == Format::WebM
                             ? tr(L"브라우저 · GitHub에서 바로 재생 · 소리 포함 가능 · CPU로 인코딩해서 MP4보다 느림")
                         : fmt != Format::Mp4 ? std::wstring(FormatBlurb(fmt))
                         : ExportAudio()      ? tr(L"어디서나 재생되는 동영상 · 소리 포함")
                                              : tr(L"어디서나 재생되는 동영상");
    hintText(blurb.c_str(), TextFaint);
    in.CutTop(8);

    // 화질 sets the size, frame rate and GIF lossy level at once. 용량 맞추기 picks those itself while it is on.
    {
        const bool fit = settings_.targetIdx != 0;
        int preset = settings_.exportPreset;
        Rect l = in.CutTop(20);
        ui_.Text(l, tr(L"화질"), 12.5f, TextDim);
        ui_.Name(tr(L"화질"));
        // Not a warning: yellow is kept for things to watch out for (paused, the keyframe the cut starts at).
        if (preset < 0 && !fit) ui_.Text(l, tr(L"직접 설정함"), 12.5f, TextDim, Align(AlignEnd));
        in.CutTop(4);
        if (ui_.Segmented(HashId("ex.preset"), in.CutTop(32), {tr(L"고화질"), tr(L"보통"), tr(L"저용량")}, preset,
                          fit ? 7u : 0u)) {
            settings_.exportPreset = preset;
            settings_.ApplyPreset(preset, isVideo);
            changed = true;
        }
        static const wchar_t* const kPresetHint[] = {L"원본 크기 그대로입니다. 용량이 큽니다.",
                                                     L"메신저로 보내기 좋은 크기입니다.",
                                                     L"용량이 가장 작습니다. 화면이 조금 거칠 수 있습니다."};
        std::wstring hint = fit          ? tr(L"용량 맞추기가 크기와 프레임 레이트를 고릅니다.")
                            : preset < 0 ? tr(L"‘자세히 설정’에서 고른 값으로 저장합니다.")
                                         : tr(kPresetHint[preset]);
        // A video saved as MP4: whether it is cut as it is, and if not, why.
        if (copying)
            hint = tr(L"인코딩 없이 자릅니다. 빠르고 화질이 그대로입니다.");
        else if (copyBlocker)
            hint = util::Format(tr(L"%s 다시 인코딩합니다."), copyBlocker);
        hintText(hint.c_str(), copying ? Success : TextFaint);
        in.CutTop(8);
    }

    // 용량 맞추기: size, frame rate and quality (MP4, WebM: the bitrate) are picked to land under the size.
    {
        const bool missed = target && fitMissed_ && !fitting_;
        label(tr(L"용량 맞추기"), !target ? L"" : fitting_ ? tr(L"맞추는 중…") : missed ? L"" : tr(L"맞춤"));
        if (ui_.Segmented(HashId("ex.target"), in.CutTop(32), {tr(L"끔"), L"5MB", L"10MB", L"15MB", L"20MB"},
                          settings_.targetIdx)) {
            changed = true;
            fitContent_.clear();  // fit again even when nothing else changed
            fitCorrection_ = 1;
            if (!settings_.targetIdx) {
                fitBitrate_ = 0;
                // Back to what 화질 asks for.
                if (settings_.exportPreset >= 0) settings_.ApplyPreset(settings_.exportPreset, isVideo);
            }
        }
        if (int mb = kTargetMB[settings_.targetIdx]) {
            const wchar_t* hint = missed     ? tr(L"가장 작게 해도 넘습니다 · 구간을 줄여 보세요")
                                  : isVideo  ? tr(L"비트레이트를 이 용량에 맞춥니다")
                                  : mb == 20 ? tr(L"크기·프레임·품질을 맞춤 · 디스코드 무료 한도")
                                  : mb == 15 ? tr(L"크기·프레임·품질을 맞춤 · X(트위터) GIF 한도")
                                             : tr(L"크기·프레임 레이트·품질을 이 용량에 맞춥니다");
            ui_.Text(in.CutTop(20), hint, 11.5f, missed ? Warning : TextFaint);
        }
        in.CutTop(8);
    }

    if (isVideo && clip_->HasAudio()) {
        auto* video = dynamic_cast<const VideoClip*>(clip_.get());
        if (video && !video->AudioExportable()) {
            bool off = false;
            ui_.ToggleRow(HashId("ex.audio"), in.CutTop(46).Inset(-10, 0), tr(L"소리 포함"), off,
                          util::Format(tr(L"%.1fkHz 소리는 넣을 수 없습니다"), video->AudioRate() / 1000.0), false);
        } else {
            bool slow = speed != 1.0;
            changed |= ui_.ToggleRow(HashId("ex.audio"), in.CutTop(46).Inset(-10, 0), tr(L"소리 포함"), exportAudio_,
                                     slow ? tr(L"속도를 바꾸면 소리는 빠집니다") : L"", !slow);
        }
        in.CutTop(8);
    }

    // 자세히 설정: everything else, folded away until it is asked for. Stays open once opened.
    static const wchar_t* const kLossyNames[] = {L"끔", L"약하게", L"강하게"};
    {
        Rect row = in.CutTop(50);
        bool hov = ui_.Hover(row);
        if (ui_.Click(HashId("ex.details"), row)) {
            settings_.exportDetails = !settings_.exportDetails;
            settingsDirty_ = true;
        }
        ui_.Describe(HashId("ex.details"), settings_.exportDetails ? tr(L"자세히 설정 접기") : tr(L"자세히 설정 펼치기"));
        ui_.Fill(row, Surface2, 10);
        ui_.Stroke(row.Inset(0.5f), hov ? BorderStrong : Border, 10);
        Rect t = row.Inset(12, 7);
        ui_.Icon(t.CutRight(16), settings_.exportDetails ? icon::ChevronUp : icon::ChevronDown, 10.f, TextDim);
        t.CutRight(6);
        ui_.Text(t.CutTop(19), tr(L"자세히 설정"), 13.f, TextColor, Bold());
        static const wchar_t* const kLoopNames[] = {L"계속 반복", L"한 번 재생", L"3번 반복"};
        std::wstring sum;
        auto part = [&](const std::wstring& p) { sum += (sum.empty() ? L"" : L" · ") + p; };
        // fmt, not fmt_: a format picked in this frame shows from the next one.
        if (webm) {
            part(settings_.targetIdx ? std::wstring(L"VP9") : tr(L"VP9 · 품질 ") + std::to_wstring(settings_.webmQuality));
        } else if (!isVideo) {
            int q = settings_.quality[(int)fmt];
            part(q >= 100 && fmt == Format::WebP ? tr(L"무손실") : tr(L"품질 ") + std::to_wstring(q));
            if (fmt == Format::Gif) part(util::Format(tr(L"손실 압축 %s"), tr(kLossyNames[settings_.gifLossy])));
        }
        part(util::Format(L"%d×%d", ow, oh));
        part(isVideo && settings_.vFrameMode == 1 ? util::Format(tr(L"%.0ffps 가변"), outFps)
                                                  : util::Format(L"%.0ffps", outFps));
        part(util::Format(L"%g×", speed));
        if (direction) part(direction == 1 ? tr(L"거꾸로") : tr(L"부메랑"));
        if (!isVideo) part(tr(kLoopNames[settings_.loopIdx]));
        if (!isVideo && settings_.holdLast) part(tr(L"끝에서 멈춤"));
        ui_.Text(t, sum, 11.5f, TextFaint);
        in.CutTop(8);
    }

    if (settings_.exportDetails) {
        // Size, frame rate and lossy level picked by hand: the preset they match now, if any.
        auto byHand = [&](bool c) {
            if (!c) return;
            changed = true;
            settings_.exportPreset = settings_.MatchPreset(isVideo, fmt == Format::Gif);
        };
        if (mp4) {
            // Codec and bitrate follow the video recording settings.
            Rect head = in.CutTop(20);
            ui_.Text(head, tr(L"인코딩"), 12.5f, TextDim);
            Rect link = head.CutRight(90);
            bool lh = ui_.Hover(link);
            if (ui_.Click(HashId("ex.vsettings"), link)) OpenSettings(kTabVideo);
            ui_.Describe(HashId("ex.vsettings"), tr(L"동영상 세부 설정"));
            ui_.Text(link, tr(L"세부 설정 ›"), 12.f, lh ? LinkHi : Link, Align(AlignEnd));
            in.CutTop(4);
            int ow = 0, oh = 0;
            OutputSize(ow, oh);
            const bool hevc = UseHevc(std::max(ow, oh));
            ui_.Text(in.CutTop(22), VideoSummary(hevc, hevc && settings_.vCodec == kCodecAuto), 12.5f, TextColor);
        } else if (webm) {
            // VP9 by quality (#40). Under 용량 맞추기 a bitrate takes its place.
            std::wstring how = ExportAudio() ? tr(L"VP9 · 화면 내용에 맞춤 · CPU로 인코딩 · 소리는 Opus")
                                             : tr(L"VP9 · 화면 내용에 맞춤 · CPU로 인코딩");
            if (settings_.vFrameMode == 1) how += tr(L" · 가변 프레임");
            if (settings_.targetIdx) {
                label(tr(L"인코딩"), {});
                std::wstring text = how;
                if (fitBitrate_) text += util::Format(tr(L" · 약 %.1f Mbps"), fitBitrate_ / 1e6);
                hintText(text.c_str(), TextColor);
            } else {
                int& q = settings_.webmQuality;
                label(tr(L"품질"), std::to_wstring(q));
                float qv = (float)q;
                if (ui_.Slider(HashId("ex.q"), in.CutTop(22).Inset(7, 0), qv, 1.f, 100.f)) {
                    q = (int)std::lround(qv);
                    changed = true;
                }
                in.CutTop(4);
                hintText(how.c_str(), TextFaint);
            }
        } else {
            int& q = settings_.quality[(int)fmt];
            label(tr(L"품질"), q >= 100 && fmt == Format::WebP ? tr(L"무손실") : std::to_wstring(q));
            float qv = (float)q;
            if (ui_.Slider(HashId("ex.q"), in.CutTop(22).Inset(7, 0), qv, 1.f, 100.f)) {
                q = (int)std::lround(qv);
                changed = true;
            }
            if (fmt == Format::Gif) {
                // 손실 압축 (#23): lossy LZW, as gifsicle --lossy.
                in.CutTop(8);
                label(tr(L"손실 압축"), {});
                byHand(ui_.Segmented(HashId("ex.lossy"), in.CutTop(32), {tr(L"끔"), tr(L"약하게"), tr(L"강하게")},
                                     settings_.gifLossy));
                static const wchar_t* const kLossyHint[] = {L"지금과 같은 화질과 용량",
                                                            L"눈으로는 차이가 거의 없습니다",
                                                            L"용량이 가장 작습니다. 점 무늬가 보일 수 있습니다."};
                hintText(tr(kLossyHint[settings_.gifLossy]), TextFaint);
            }
        }
        in.CutTop(8);

        label(tr(L"크기"), util::Format(L"%d × %d", ow, oh));
        byHand(ui_.Segmented(HashId("ex.scale"), in.CutTop(32),
                             {L"100%", L"75%", L"50%", L"33%", L"640px", L"480px"}, settings_.scaleIdx));
        in.CutTop(8);

        label(tr(L"프레임 레이트"), util::Format(L"%.0f fps", outFps));
        byHand(ui_.Segmented(HashId("ex.fps"), in.CutTop(32), {tr(L"원본"), L"30", L"25", L"20", L"15", L"10"},
                             settings_.outFpsIdx));
        in.CutTop(8);

        label(tr(L"재생 속도"), {});
        int prevSpeed = settings_.speedIdx;
        if (ui_.Segmented(HashId("ex.speed"), in.CutTop(32), {L"0.5×", L"1×", L"1.5×", L"2×"},
                          settings_.speedIdx)) {
            changed = true;
            // Re-base the preview clock so the playhead doesn't jump.
            std::swap(prevSpeed, settings_.speedIdx);
            size_t ph = playing_ ? PlayheadFromClock() : playhead_;
            std::swap(prevSpeed, settings_.speedIdx);
            playhead_ = ph;
            RestartClock();
        }
        in.CutTop(8);
        if (!isVideo) {
            // The preview plays the same way.
            label(tr(L"재생 방향"), {});
            if (ui_.Segmented(HashId("ex.dir"), in.CutTop(32), {tr(L"그대로"), tr(L"거꾸로"), tr(L"부메랑")},
                              settings_.directionIdx)) {
                changed = true;
                FollowDirection();
            }
            static const wchar_t* const kDirectionHint[] = {
                L"", L"끝에서 처음으로 재생합니다.", L"앞으로 갔다가 되돌아옵니다. 길이가 2배가 됩니다."};
            if (settings_.directionIdx) hintText(tr(kDirectionHint[settings_.directionIdx]), TextFaint);
            in.CutTop(8);
            Rect row = in.CutTop(32);
            ui_.Text(row, tr(L"반복"), 12.5f, TextDim);
            ui_.Name(tr(L"반복"));
            changed |= ui_.Segmented(HashId("ex.loop"), row.CutRight(std::min(186.f, row.W() - 60)),
                                     {tr(L"계속"), tr(L"1번"), tr(L"3번")}, settings_.loopIdx);
            in.CutTop(6);
            changed |= ui_.ToggleRow(HashId("ex.hold"), in.CutTop(46).Inset(-10, 0), tr(L"끝에서 잠깐 멈춤"),
                                     settings_.holdLast, tr(L"마지막 장면을 1초 더 보여 줍니다"));
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

    if (!showResult && copying) {
        // From the keyframe the cut starts at, the size the samples take.
        double sec = (RangeEndTs() - clip_->Ts(cutSource_->KeyBefore(inF_))) / 1e7;
        uint64_t bytes = cutSource_->SizeOf(inF_, outF_, ExportAudio());
        std::wstring sum = util::Format(tr(L"%d×%d · %.0ffps · %.1f초"), clip_->Width(), clip_->Height(), outFps, sec) +
                           (ExportAudio() ? tr(L" · 소리 포함") : tr(L" · 소리 없음"));
        if (bytes) sum += tr(L" · 약 ") + util::FormatBytes(bytes);
        ui_.Text(summary, sum, 12.f, TextDim, Align(AlignCenter));
    } else if (!showResult) {
        std::wstring sum = util::Format(tr(L"%d×%d · %.0ffps · %.1f초"), ow, oh, outFps, outSec);
        if (isVideo) sum += ExportAudio() ? tr(L" · 소리 포함") : tr(L" · 소리 없음");
        // The estimate, once it matches what is on screen.
        bool current = estValid_ && estKey_ == EstimateKey();
        bool over = current && target && estBytes_ > target;
        if (!busy && FormatAvailable(fmt))
            sum += current ? tr(L" · 약 ") + util::FormatBytes(estBytes_) : std::wstring(tr(L" · 크기 계산 중…"));
        ui_.Text(summary, sum, 12.f, over ? Warning : current ? TextDim : TextFaint, Align(AlignCenter));
        if (!current && !busy) RequestWake(ui_.Now() + 0.25);
    }

    if (busy) {
        float p = exporter_.Progress();
        ui_.Fill(save, Surface2, 12);
        Rect bar = Rect(save.l + 14, save.CY() + 8, save.r - 14, save.CY() + 12);
        ui_.ProgressBar(bar, p, Accent);
        std::wstring text = util::Format(tr(L"%s 인코딩 중…  %d%%"), FormatName(fmt), (int)(p * 100));
        // WebM is encoded on the CPU and takes a while (#40): how long is left, once there is enough to tell.
        const double took = util::NowSeconds() - exportStarted_;
        if (fmt == Format::WebM && p >= 0.03f && took >= 2) {
            const double left = took * (1 - p) / p;
            text += left < 90 ? util::Format(tr(L" · 약 %d초 남음"), std::max(1, (int)std::lround(left)))
                              : util::Format(tr(L" · 약 %d분 남음"), (int)std::lround(left / 60));
        }
        ui_.Text(Rect(save.l + 14, save.t + 4, save.r - 14, save.CY() + 4), text, 13.f, TextColor, Bold());
        if (ui_.Button(HashId("ex.cancel"), saveAs, tr(L"취소"), BtnStyle::Ghost, icon::Close, true, 13.f))
            exporter_.Cancel();
        RequestWake(ui_.Now() + 0.1);
    } else {
        bool avail = !(disabled & (1u << fmt_));
        std::wstring label = util::Format(tr(L"%s로 저장"), FormatName(fmt));
        if (ui_.Button(HashId("ex.save"), save, label, BtnStyle::Primary, icon::Save, avail, 15.f))
            StartExport(false);
        // 버리기 / 삭제 (#37), for what this run recorded.
        if (CanDiscard()) {
            const bool video = !clip_->SourcePath().empty();
            Rect discard = saveAs.CutRight(92);
            saveAs.CutRight(8);
            UiId id = HashId("ex.discard");
            if (ui_.Button(id, discard, video ? tr(L"삭제") : tr(L"버리기"), BtnStyle::Secondary, icon::Delete, true, 13.f))
                ConfirmDiscard();
            ui_.Tooltip(id, discard, video ? tr(L"이 동영상을 휴지통으로 옮깁니다 (Delete)")
                                           : tr(L"이 녹화를 버립니다 (Delete)"));
        }
        if (ui_.Button(HashId("ex.saveas"), saveAs, tr(L"다른 이름으로 저장…"), BtnStyle::Secondary, 0, avail, 13.f))
            StartExport(true);
    }

    if (showResult) {
        ui_.Fill(card, WithAlpha(Success, 0.08f), 12);
        ui_.Stroke(card.Inset(0.5f), WithAlpha(Success, 0.35f), 12);
        Rect top = Rect(card.l + 12, card.t + 4, card.r - 12, card.t + 32);
        ui_.Icon(Rect::XYWH(top.l, top.t, 18, top.H()), icon::Check, 13.f, Success);
        // The recording itself (just saved) or the last export.
        std::wstring size = (resultOriginal_ ? tr(L"원본 · ") : L"") + util::FormatBytes(lastResult_.bytes);
        float sw = ui_.Measure(size, 12.f) + 4;
        ui_.Text(Rect(top.l + 24, top.t, top.r - sw - 8, top.b), util::FileNameOf(lastResult_.path), 12.5f,
                 TextColor, Bold());
        ui_.Text(top, size, 12.f, TextDim, Align(AlignEnd));
        Rect btns = Rect(card.l + 10, card.b - 40, card.r - 10, card.b - 8);
        float bw = (btns.W() - 12) / 3;
        if (ui_.Button(HashId("res.folder"), Rect::XYWH(btns.l, btns.t, bw, btns.H()), tr(L"폴더"), BtnStyle::Secondary,
                       icon::Folder, true, 12.f))
            util::OpenFolderAndSelect(lastResult_.path);
        if (ui_.Button(HashId("res.copy"), Rect::XYWH(btns.l + bw + 6, btns.t, bw, btns.H()), tr(L"복사"),
                       BtnStyle::Secondary, icon::Copy, true, 12.f)) {
            if (util::CopyFileToClipboard(hwnd_, lastResult_.path))
                Toast(tr(L"파일을 클립보드에 복사했습니다 · 메신저나 탐색기에 붙여넣으세요"));
            else
                Toast(tr(L"클립보드에 복사할 수 없습니다."), true);
        }
        if (ui_.Button(HashId("res.open"), Rect::XYWH(btns.l + 2 * (bw + 6), btns.t, bw, btns.H()), tr(L"열기"),
                       BtnStyle::Secondary, icon::OpenFile, true, 12.f))
            util::ShellOpen(lastResult_.path);
    }
}

// ---------------------------------------------------------------- settings

Rect App::SettingRow(Rect& area, std::wstring_view title, std::wstring_view desc, float controlW,
                     const std::function<void(Rect)>& control, float below) {
    Rect row = area.CutTop(68);
    Rect more = area.CutTop(below);
    Rect ctl = row.CutRight(controlW);
    row.CutRight(16);
    ui_.Text(Rect(row.l, row.t + 12, row.r, row.t + 34), title, 14.f, TextColor, Bold());
    ui_.Text(Rect(row.l, row.t + 34, row.r, row.b - 10), desc, 12.f, TextFaint);
    ui_.Name(title);  // for the toggle, slider or row of choices in it
    control(Rect::Centered(ctl.CX(), row.CY(), ctl.W(), 36));
    ui_.Name({});
    ui_.Line(area.l, area.t - 0.5f, area.r, area.t - 0.5f, Border);
    return more;
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
    hkConflict_ = {};
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
    ui_.Text(in.CutTop(36), tr(L"설정"), 20.f, TextColor, Bold());
    in.CutTop(10);

    // Tabs down the left; the one viewed last comes back next time. Ctrl+Tab steps through them.
    static constexpr struct {
        const wchar_t* name;
        wchar_t glyph;
    } kTabs[kTabCount] = {{L"일반", icon::Settings},
                          {L"녹화", icon::Record},
                          {L"동영상", icon::Video},
                          {L"소리", icon::Volume},
                          {L"정보", icon::Info},
                          {L"리플레이", icon::Replay}};
    // The order they are shown in; the numbers stay as they were saved.
    static constexpr int kOrder[kTabCount] = {kTabGeneral, kTabRecord, kTabReplay, kTabVideo, kTabAudio, kTabAbout};
    int tab = settings_.settingsTab;
    Rect list = in.CutLeft(150);
    ui_.Declare(HashId("set.tabs"), list, Role::Group, tr(L"설정 탭"));
    ui_.Line(list.r + 12.5f, list.t, list.r + 12.5f, list.b, Border);
    in.CutLeft(28);
    int shownAt = 0;
    for (int k = 0; k < kTabCount; ++k) {
        const int i = kOrder[k];
        if (i == tab) shownAt = k;
        Rect item = list.CutTop(40);
        list.CutTop(4);
        bool on = i == tab;
        if (ui_.Click(HashId("set.tab", i), item)) tab = i;
        if (UiNode* n = ui_.Describe(HashId("set.tab", i), tr(kTabs[i].name), Role::Tab))
            n->on = on, n->pos = k, n->count = kTabCount, n->group = HashId("set.tabs");
        float hl = ui_.Anim(HashId("set.tabHl", i), on ? 1.f : ui_.Hover(item) ? 0.5f : 0.f);
        const bool onHighlight = on && Contrast();
        if (onHighlight)
            ui_.Fill(item, Accent, 8);
        else if (hl > 0.01f)
            ui_.Fill(item, WithAlpha(Surface3, hl), 8);
        if (on && !Contrast()) ui_.Fill(Rect::XYWH(item.l, item.CY() - 9, 3, 18), Accent, 1.5f);
        ui_.Icon(Rect::XYWH(item.l + 12, item.t, 20, item.H()), kTabs[i].glyph, 14.f,
                 onHighlight ? OnFill : on ? AccentHi : TextDim);
        ui_.Text(Rect(item.l + 42, item.t, item.r - 8, item.b), tr(kTabs[i].name), 14.f,
                 onHighlight ? OnFill : on ? TextColor : TextDim, on ? Bold() : TextStyle{});
    }
    if (ui_.In().ctrl && ui_.In().Key(VK_TAB)) tab = kOrder[(shownAt + (ui_.In().shift ? kTabCount - 1 : 1)) % kTabCount];
    if (tab != settings_.settingsTab) {
        settings_.settingsTab = tab;
        settingsScroll_ = 0;
        settingsDirty_ = true;
        SetCapturingHotkey(0);
        hkConflict_ = {};
    }

    // Scroll view, one per tab
    Rect view = in;
    float maxScroll = std::max(0.f, settingsContentH_ - view.H());
    if (ui_.Hover(view) && ui_.In().wheel != 0) settingsScroll_ -= ui_.In().wheel * 90.f;
    ui_.KeepInView(view, settingsScroll_);
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
        case kTabReplay: SettingsReplay(area, changed); break;
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
    SettingRow(area, tr(L"저장 폴더"), settings_.outDir, 196, [&](Rect c) {
        Rect a = c.CutLeft(94);
        c.CutLeft(8);
        if (ui_.Button(HashId("set.openDir"), a, tr(L"열기"), BtnStyle::Secondary, icon::Folder, true, 13.f)) {
            util::EnsureDir(settings_.outDir);
            util::ShellOpen(settings_.outDir);
        }
        if (ui_.Button(HashId("set.changeDir"), c, tr(L"변경"), BtnStyle::Secondary, 0, true, 13.f)) {
            std::wstring dir;
            if (util::PickFolder(hwnd_, settings_.outDir, dir)) {
                settings_.outDir = dir;
                changed = true;
            }
        }
    });

    // 파일 이름: three patterns, or 직접 입력 with buttons that put the tokens in ("{연} 2026"), wrapped to the row.
    int nameIdx = 3;
    for (int i = 0; i < (int)_countof(kNameTemplates) && !nameCustom_; ++i)
        if (settings_.nameTemplate == kNameTemplates[i]) nameIdx = i;
    const bool custom = nameIdx == 3;
    const UiId field = HashId("set.nameText");
    const std::wstring insert = tr(L"넣기"), example = tr(L"리그 오브 레전드");
    const float tokH = 26, gap = 6, insertW = ui_.Measure(insert, 12.f) + 10;
    struct Tok {
        std::wstring name, value;
        float x, w;
        int row;
    };
    std::vector<Tok> toks;
    int rows = 1;
    if (custom) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        float x = insertW;
        for (int k = 0; k < kTokCount; ++k) {
            Tok t{kNameTokens[k][Language() == Lang::English ? 1 : 0], FillName(kNameTokens[k][0], example, now, 1)};
            t.w = ui_.Measure(t.name, 12.f, kSemibold) + 6 + ui_.Measure(t.value, 12.f) + 18;
            if (x + t.w > area.W() && x > insertW) x = insertW, ++rows;
            t.x = x, t.row = rows - 1;
            x += t.w + gap;
            toks.push_back(std::move(t));
        }
    }
    const std::wstring nameDesc = tr(L"예: ") + FileStem(example, false) + L".gif";
    Rect more = SettingRow(
        area, tr(L"파일 이름"), nameDesc, 400,
        [&](Rect c) {
            if (ui_.Segmented(HashId("set.name"), c,
                              {tr(L"OzMaker_날짜"), tr(L"창 이름_날짜"), tr(L"날짜"), tr(L"직접 입력")}, nameIdx)) {
                nameCustom_ = nameIdx == 3;
                if (nameCustom_) {  // the pattern so far, to change
                    settings_.nameTemplate = NameInLanguage(settings_.nameTemplate);
                    ui_.Focus(field);
                } else {
                    settings_.nameTemplate = kNameTemplates[nameIdx];
                }
                changed = true;
            }
        },
        custom ? 36 + 8 + rows * tokH + (rows - 1) * gap + 8 + 16 + 14 : 0);
    if (custom) {
        if (ui_.TextField(field, more.CutTop(36), settings_.nameTemplate, tr(L"예: {창 이름}_{날짜}"), 120))
            changed = true;
        ui_.Describe(field, tr(L"파일 이름 형식"), Role::Edit);
        more.CutTop(8);
        const Rect band = more.CutTop(rows * tokH + (rows - 1) * gap);
        ui_.Text(Rect(band.l, band.t, band.l + insertW, band.t + tokH), insert, 12.f, TextFaint);
        for (int k = 0; k < kTokCount; ++k) {
            const Tok& t = toks[k];
            const Rect r = Rect::XYWH(band.l + t.x, band.t + t.row * (tokH + gap), t.w, tokH);
            const UiId id = HashId("set.nameTok", k);
            if (ui_.Button(id, r, L"", BtnStyle::Secondary, 0, true, 12.f) &&
                ui_.InsertText(field, settings_.nameTemplate, t.name, 120))
                changed = true;
            ui_.Describe(id, t.name + L", " + t.value);
            const float nw = ui_.Measure(t.name, 12.f, kSemibold);
            ui_.Text(Rect(r.l + 9, r.t, r.l + 9 + nw + 1, r.b), t.name, 12.f, TextColor, Bold());
            ui_.Text(Rect(r.l + 15 + nw, r.t, r.r - 4, r.b), t.value, 12.f, TextFaint);
            if (k == kTokDate) ui_.Tooltip(id, r, tr(L"날짜와 시각"));
            if (k == kTokWindow) ui_.Tooltip(id, r, tr(L"녹화한 창의 이름 (화면이면 ‘화면 1’)"));
            if (k == kTokNumber) ui_.Tooltip(id, r, tr(L"001부터, 그 이름으로 아직 없는 번호"));
        }
        more.CutTop(8);
        const auto [hint, warn] = NameHint(settings_.nameTemplate);
        ui_.Text(more.CutTop(16), hint, 12.f, warn ? Warning : TextFaint);
    }

    // A Windows contrast theme wins over the choice here.
    SettingRow(area, tr(L"테마"),
               Contrast() ? tr(L"Windows의 대비 테마가 켜져 있어서 그 색으로 그립니다.")
                          : tr(L"‘시스템’은 Windows의 밝은 모드와 어두운 모드를 따릅니다."),
               300, [&](Rect c) {
                   if (ui_.Segmented(HashId("set.theme"), c, {tr(L"시스템"), tr(L"다크"), tr(L"라이트")}, settings_.theme,
                                     Contrast() ? 0b111 : 0)) {
                       changed = true;
                       ApplyTheme();
                   }
               });

    // A list rather than buttons, so more languages fit later.
    SettingRow(area, tr(L"언어"), tr(L"‘시스템’은 Windows 표시 언어가 한국어면 한국어, 아니면 영어입니다."), 300, [&](Rect c) {
        std::vector<MenuItem> items{{LanguageName(kLangSystem), kLangSystem},
                                    {LanguageName(kLangKorean), kLangKorean, true},
                                    {LanguageName(kLangEnglish), kLangEnglish}};
        if (Dropdown(HashId("set.language"), c, LanguageName(settings_.language), tr(L"언어"), std::move(items),
                     settings_.language, TextColor)) {
            changed = true;
            ApplyLanguage();
        }
    });

    const bool portable = Settings::Portable() || portableForTest_;
    SettingRow(area, tr(L"Windows 시작 시 실행"),
               portable ? tr(L"포터블 모드에서는 이 PC의 시작 프로그램에 등록하지 않습니다")
                        : tr(L"로그인하면 창 없이 트레이에서 시작해 단축키를 바로 쓸 수 있습니다"),
               60, [&](Rect c) {
                   if (portable) {
                       ui_.Text(c, tr(L"쓰지 않음"), 13.f, TextFaint, Align(AlignEnd));
                       return;
                   }
                   bool on = autostart_;
                   if (ui_.Toggle(HashId("set.autostart"), Rect::Centered(c.r - 22, c.CY(), 44, 24), on)) {
                       if (util::SetAutostart(on))
                           autostart_ = on;
                       else
                           Toast(tr(L"시작 프로그램에 등록하지 못했습니다."), true);
                   }
               });

    int closeIdx = settings_.closeToTray ? 0 : 1;
    SettingRow(area, tr(L"닫기 버튼"), tr(L"트레이로 숨기면 창을 닫아도 단축키로 녹화할 수 있습니다"), 260, [&](Rect c) {
        if (ui_.Segmented(HashId("set.close"), c, {tr(L"트레이로 숨기기"), tr(L"종료")}, closeIdx)) {
            settings_.closeToTray = closeIdx == 0;
            changed = true;
        }
    });

    SettingRow(area, tr(L"효과음"), tr(L"녹화 시작 · 중지 · 저장 시 짧은 알림음"), 60, [&](Rect c) {
        changed |= ui_.Toggle(HashId("set.sound"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.sounds);
    });

    SettingRow(area, tr(L"새 버전 자동 확인"), tr(L"시작할 때와 하루 한 번 GitHub에서 확인하고 새 버전을 미리 받아 둡니다"), 60,
               [&](Rect c) {
                   if (ui_.Toggle(HashId("set.autoUpdate"), Rect::Centered(c.r - 22, c.CY(), 44, 24),
                                  settings_.autoUpdate)) {
                       changed = true;
                       CheckForUpdate();
                   }
               });

    // #49: asks first, with what goes back and what stays.
    SettingRow(area, tr(L"설정 초기화"), tr(L"설정을 처음 값으로 되돌립니다 · 녹화한 파일과 최근 파일 목록은 그대로입니다"), 120,
               [&](Rect c) {
                   if (ui_.Button(HashId("set.reset"), c, tr(L"초기화…"), BtnStyle::Secondary, icon::Refresh, true, 13.f))
                       AskReset();
               });
}

void App::AskReset() {
    const Settings d;
    Question q;
    q.title = tr(L"설정을 처음으로 되돌릴까요?");
    q.yes = tr(L"되돌리기");
    q.danger = true;
    q.glyph = icon::Refresh;
    std::vector<std::wstring> back{tr(L"녹화 · 동영상 · 소리 · 내보내기 설정"),
                                   tr(L"파일 이름 · 테마 · 언어 · 닫기 버튼 · 효과음 · 새 버전 확인"),
                                   tr(L"단축키") + std::wstring(L"  → ") + util::HotkeyToString(d.hkMods, d.hkVk) + L" · " +
                                       util::HotkeyToString(d.hkPauseMods, d.hkPauseVk)};
    std::vector<std::wstring> keep{tr(L"녹화한 파일 · 최근 파일 · 저장하지 않은 녹화"),
                                   tr(L"저장 폴더") + std::wstring(L"  (") + settings_.outDir + L")"};
    if (!Settings::Portable()) keep.push_back(tr(L"Windows 시작 시 실행") + std::wstring(L"  (") + (autostart_ ? tr(L"켬") : tr(L"끔")) + L")");
    keep.push_back(tr(L"창 위치와 크기 · 녹화 탭에서 고른 창 · 모니터 · 영역"));
    q.lists = {{tr(L"되돌리는 것"), std::move(back)}, {tr(L"그대로 두는 것"), std::move(keep)}};
    q.onYes = [this] { ResetSettings(); };
    Ask(std::move(q));
}

// A hotkey row, and under it, when the combination pressed is another program's, the offer to take it over (#50).
void App::HotkeyRow(Rect& area, int which, std::wstring_view title, std::wstring_view desc) {
    const std::wstring caveat = tr(L"OzMaker가 쓰면 그 프로그램에서는 이 조합이 듣지 않습니다. 관리자 권한으로 실행한 게임 안에서는 OzMaker도 받지 못할 수 있습니다.");
    TextStyle wrap;
    wrap.wrap = true;
    wrap.v = AlignStart;
    DWRITE_TEXT_METRICS m{};
    float caveatH = 34;
    if (auto lay = Fonts::Layout(caveat, 12.f, wrap, area.W() - 32 - 28, 200); lay && SUCCEEDED(lay->GetMetrics(&m)))
        caveatH = std::ceil(m.height);
    const float panelH = 14 + 22 + 10 + 32 + 10 + caveatH + 14;
    auto [mods, vk, taken] = Hotkey(which);
    const float top = area.t;
    const bool conflict = hkConflict_.which == which;
    Rect more = SettingRow(
        area, title, desc, 230,
        [&](Rect c) {
            bool on = capturingHotkey_ == which;
            std::wstring label = on ? tr(L"키 조합을 누르세요…") : util::HotkeyToString(mods, vk);
            if (ui_.Button(HashId("set.hotkey", which), c, label, on ? BtnStyle::Primary : BtnStyle::Secondary,
                           icon::Keyboard, true, 13.f))
                SetCapturingHotkey(on ? 0 : which);
        },
        conflict ? panelH + 14 : 0);
    if (taken) {  // 가져옴: received ahead of the program that has it
        const std::wstring tag = tr(L"가져옴");
        const float x = area.l + ui_.Measure(title, 14.f, kSemibold) + 10, tw = ui_.Measure(tag, 11.f, kSemibold) + 14;
        Rect chip = Rect::XYWH(x, top + 14, tw, 18);
        ui_.Fill(chip, WithAlpha(Accent, Contrast() ? 0.f : 0.18f), 9);
        if (Contrast()) ui_.Stroke(chip.Inset(0.5f), AccentHi, 9);
        ui_.Text(chip, tag, 11.f, AccentHi, Bold(AlignCenter));
    }
    if (!conflict) return;
    Rect box = more.CutTop(panelH);
    ui_.Fill(box, WithAlpha(Warning, Contrast() ? 0.f : 0.08f), 10);
    ui_.Stroke(box.Inset(0.5f), WithAlpha(Warning, 0.5f), 10);
    const std::wstring combo = util::HotkeyToString(hkConflict_.mods, hkConflict_.vk);
    const std::wstring head = tr(L"다른 프로그램이 이미 쓰고 있는 조합입니다:");
    ui_.Declare(HashId("set.hkConflict"), box, Role::Group, head + L" " + combo);
    Rect c = box.Inset(16, 14);
    ui_.Icon(Rect::XYWH(c.l, c.t, 18, 22), icon::Warning, 14.f, Warning);
    c.CutLeft(28);
    Rect line = c.CutTop(22);
    const float hw = ui_.Measure(head, 13.f, kSemibold) + 4;
    ui_.Text(line.CutLeft(hw), head, 13.f, TextColor, Bold());
    line.CutLeft(8);
    Rect key = line.CutLeft(ui_.Measure(combo, 12.f, kSemibold) + 16);
    ui_.Fill(key.Inset(0, 1), Surface3, 6);
    ui_.Stroke(key.Inset(0.5f, 1.5f), BorderStrong, 6);
    ui_.Text(key, combo, 12.f, TextColor, Bold(AlignCenter));
    c.CutTop(10);
    Rect btns = c.CutTop(32);
    const std::wstring take = tr(L"그래도 OzMaker가 쓰기"), again = tr(L"다른 조합 누르기"), cancel = tr(L"취소");
    if (ui_.Button(HashId("set.hkTake"), btns.CutLeft(ui_.Measure(take, 13.f, kSemibold) + 32), take, BtnStyle::Primary,
                   0, true, 13.f))
        TakeHotkey();
    btns.CutLeft(8);
    if (ui_.Button(HashId("set.hkAgain"), btns.CutLeft(ui_.Measure(again, 13.f, kSemibold) + 32), again,
                   BtnStyle::Secondary, 0, true, 13.f))
        SetCapturingHotkey(which);
    btns.CutLeft(8);
    if (ui_.Button(HashId("set.hkCancel"), btns.CutLeft(ui_.Measure(cancel, 13.f, kSemibold) + 32), cancel,
                   BtnStyle::Ghost, 0, true, 13.f))
        hkConflict_ = {};
    c.CutTop(10);
    ui_.Text(c.CutTop(caveatH), caveat, 12.f, TextFaint, wrap);
}

void App::SettingsRecord(Rect& area, bool& changed) {
    const wchar_t* takenDesc = tr(L"다른 프로그램보다 먼저 받습니다 · 그 프로그램에서는 이 조합이 듣지 않습니다");
    HotkeyRow(area, kHotkeyRecord, tr(L"녹화 단축키"),
              !hotkeyOk_          ? tr(L"⚠ 다른 프로그램이 사용 중이라 등록하지 못했습니다. 다른 조합을 지정하세요")
              : settings_.hkHook ? takenDesc
                                 : tr(L"게임 중에도 이 키로 녹화를 시작/중지합니다 (창 탭: 지금 보고 있는 창)"));
    HotkeyRow(area, kHotkeyPause, tr(L"일시정지 단축키"),
              !pauseHotkeyOk_         ? tr(L"⚠ 지난 녹화 때 다른 프로그램이 쓰고 있어 쓰지 못했습니다. 다른 조합을 지정하세요")
              : settings_.hkPauseHook ? takenDesc
                                      : tr(L"녹화 중에만 쓰입니다 · 녹화 중 표시의 ❚❚ 버튼으로도 멈추고 이어 갑니다"));

    // 녹화 타이머 (#47): the same one as the 녹화 tab's button.
    const bool custom = settings_.timerIdx == kTimerCustom;
    Rect more = SettingRow(
        area, tr(L"녹화 타이머"), tr(L"정한 길이가 되면 알리거나 멈춥니다 · 녹화 탭과 같은 값"), 380,
        [&](Rect c) {
            if (ui_.Segmented(HashId("set.timer"), c, {tr(L"끔"), tr(L"30분"), tr(L"1시간"), tr(L"2시간"), tr(L"직접 입력")},
                              settings_.timerIdx)) {
                changed = true;
                if (settings_.timerIdx == kTimerCustom) ui_.Focus(HashId("timer.field", 0));
            }
        },
        custom ? 32 + 8 + 16 + 14.f : 0.f);
    if (custom) {
        TimerFields(more.CutTop(32));
        more.CutTop(8);
        ui_.Text(more.CutTop(16), tr(L"최대 24시간 · 녹화된 길이로 셉니다 (일시정지한 시간은 빼고)"), 12.f, TextFaint);
    }
    const int minutes = settings_.TimerMinutes();
    int end = settings_.timerStop ? 1 : 0;
    SettingRow(area, tr(L"시간이 되면"),
               !minutes             ? tr(L"타이머를 켜면 고를 수 있습니다")
               : settings_.timerStop ? tr(L"녹화를 멈추고 저장합니다 (지금까지의 최대 녹화 길이와 같음)")
                                     : tr(L"알림과 효과음으로 알리고 녹화는 계속합니다"),
               260, [&](Rect c) {
                   if (ui_.Segmented(HashId("set.timerEnd"), c, {tr(L"알림만"), tr(L"멈추고 저장")}, end, minutes ? 0 : 0b11)) {
                       settings_.timerStop = end == 1;
                       changed = true;
                   }
               });

    SettingRow(area, tr(L"시작 카운트다운"), tr(L"녹화를 누른 뒤 시작까지의 시간 · 영역 지정 막대에서도 바꿈 (단축키는 즉시 시작)"), 230,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.cd"), c, {tr(L"없음"), tr(L"3초"), tr(L"5초")}, settings_.countdown);
               });

    // #46: hidden in the tray, or a taskbar button that opens the window while recording.
    SettingRow(area, tr(L"녹화를 시작하면"),
               settings_.startHide == kHideMinimize ? tr(L"작업 표시줄 단추로 녹화 중에도 엽니다 · 연 창은 녹화에 안 찍힘")
                                                    : tr(L"창을 트레이로 숨깁니다 · 트레이 아이콘을 눌러 다시 엽니다"),
               330, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.startHide"), c, {tr(L"트레이로 숨기기"), tr(L"작업 표시줄로 최소화")},
                                            settings_.startHide);
               });

    SettingRow(area, tr(L"창 캡처 방식"), tr(L"자동: 전체화면 게임만 화면 캡처 · 검은 화면이 녹화되면 ‘화면’으로 바꾸세요"),
               230, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.mode"), c, {tr(L"자동"), tr(L"창"), tr(L"화면")}, settings_.captureMode);
               });

    SettingRow(area, tr(L"녹화 중 표시"), tr(L"녹화 시간 · 일시정지 · 중지 버튼 · 영역 테두리 (녹화 결과에는 찍히지 않습니다)"), 320,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.overlay"), c, {tr(L"항상"), tr(L"전체화면 제외"), tr(L"숨김")},
                                            settings_.overlayMode);
               });

    SettingRow(area, tr(L"클릭 강조"), tr(L"마우스를 누른 곳에 퍼지는 원을 함께 녹화합니다 · 왼쪽 버튼 노랑, 오른쪽 파랑"), 60,
               [&](Rect c) {
                   changed |= ui_.Toggle(HashId("set.clickfx"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.clickFx);
               });
}

// Mbps as typed in 설정 › 동영상 › 비트레이트, in kbps; -1 when it isn't a number.
static int ParseMbps(std::wstring_view t) {
    const size_t dot = t.find(L'.');
    const std::wstring_view whole = t.substr(0, dot), tenth = dot == t.npos ? std::wstring_view{} : t.substr(dot + 1);
    if ((whole.empty() && tenth.empty()) || whole.size() > 5 || tenth.size() > 1) return -1;
    int kbps = 0;
    for (wchar_t c : whole) {
        if (c < L'0' || c > L'9') return -1;
        kbps = kbps * 10 + (c - L'0');
    }
    kbps *= 1000;
    if (!tenth.empty()) {
        if (tenth[0] < L'0' || tenth[0] > L'9') return -1;
        kbps += (tenth[0] - L'0') * 100;
    }
    return kbps;
}

void App::SettingsVideo(Rect& area, bool& changed) {
    ProbeEncoders();
    const bool hevcOk = HevcEncoder();
    // HEVC is written as a plain MP4 (Windows' fragmented MP4 sink refuses it), so a crash leaves no recording. 자동
    // takes it only where H.264 would have to shrink the picture (#42).
    SettingRow(area, tr(L"코덱"),
               settings_.vCodec == kCodecH264   ? tr(L"H.264: 어디서나 재생 · 긴 변 4096까지 (큰 화면은 줄여서 녹화)")
               : settings_.vCodec == kCodecHevc ? tr(L"HEVC: 용량 약 절반 · 일부 기기 미지원 · 비정상 종료되면 녹화가 남지 않음")
               : hevcOk && HevcPlayable()       ? tr(L"자동: 보통은 H.264 · 긴 변이 4096보다 큰 화면만 HEVC로 원본 크기")
                                                : tr(L"자동: H.264 · HEVC 비디오 확장이 없어 큰 화면은 줄여서 녹화"),
               300, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.vcodec"), c, {tr(L"자동"), L"H.264", L"HEVC"}, settings_.vCodec,
                                            hevcOk ? 0 : 0b100);
               });
    if (!hevcOk && settings_.vCodec == kCodecHevc) settings_.vCodec = kCodecAuto;

    const std::wstring& hwName = encName_[settings_.vCodec == kCodecHevc][1];
    std::wstring hwDesc = hwName.empty() ? tr(L"그래픽카드 인코더를 찾지 못해 CPU로 인코딩합니다")
                                         : tr(L"그래픽카드 인코더 사용: ") + hwName + tr(L" (게임 성능에 영향이 거의 없음)");
    SettingRow(area, tr(L"하드웨어 인코딩"), hwDesc, 60, [&](Rect c) {
        changed |= ui_.Toggle(HashId("set.vhw"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.vHardware);
    });

    const bool vfr = settings_.vFrameMode == 1;
    SettingRow(area, tr(L"프레임 레이트"),
               vfr ? tr(L"가변일 때는 가장 높은 값입니다 · 게임은 60, 강의는 30이면 충분합니다")
                   : tr(L"게임은 60, 화면 설명 · 강의는 30이면 충분합니다"),
               230, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.vfps"), c, {L"24", L"30", L"60"}, settings_.vFpsIdx);
               });

    SettingRow(area, tr(L"해상도"), tr(L"원본보다 크게 키우지는 않습니다 · 세로 화면은 짧은 쪽 기준"), 330, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.vres"), c, {tr(L"원본"), L"2160p", L"1440p", L"1080p", L"720p"},
                                 settings_.vResIdx);
    });

    std::wstring brDesc = tr(L"자동: 해상도 · 프레임에 맞춰 정합니다 (Mbps)");
    {
        RecTarget t;
        if (CurrentTarget(t, nullptr)) {
            int sw, sh, ow, oh;
            bool hevc = false;
            SourceSize(t, sw, sh);
            PlannedSize(sw, sh, ow, oh, &hevc);
            UINT32 auto_ = Mp4Writer::AutoBitrate(ow, oh, kVideoFpsOptions[settings_.vFpsIdx], hevc);
            brDesc = util::Format(tr(L"자동: 지금 대상(%d×%d)이면 약 %.1f Mbps · 1분에 %.0f MB"), ow, oh, auto_ / 1e6,
                                  auto_ / 8.0 * 60 / 1e6);
        }
    }
    // #53: the steps in a list, and a field for any number of Mbps (a tenth at most). Typing in it makes it 직접 입력;
    // a number out of range becomes the nearest one when the field lets go, and the line under the row says so.
    const UiId brField = HashId("set.vbrField");
    const bool brTyping = ui_.FocusedField() == brField;
    const int brKbps = ParseMbps(brText_);
    if (brTypingWas_ && !brTyping) {
        if (brKbps < 0) {
            brNote_.clear();
        } else if (brKbps < kVideoBitrateMinKbps || brKbps > kVideoBitrateMaxKbps) {
            settings_.vBitrateKbps = std::clamp(brKbps, kVideoBitrateMinKbps, kVideoBitrateMaxKbps);
            brNote_ = util::Format(brKbps < kVideoBitrateMinKbps ? tr(L"%s은(는) 너무 작아서 %s Mbps로 맞췄습니다")
                                                                 : tr(L"%s은(는) 너무 커서 %s Mbps로 맞췄습니다"),
                                   brText_.c_str(), MbpsText(settings_.vBitrateKbps).c_str());
            changed = true;
        }
    }
    brTypingWas_ = brTyping;
    if (!brTyping) brText_ = MbpsText(settings_.vBitrateKbps);  // as typed only while typing
    const bool brCustom = settings_.vBitrateIdx == kVideoBitrateCustom;
    if (brCustom) brDesc = tr(L"직접 정한 값으로 녹화합니다 · 편집 화면의 MP4 저장도 이 값을 씁니다");
    const std::wstring range = MbpsText(kVideoBitrateMinKbps) + L" – " + MbpsText(kVideoBitrateMaxKbps) + L" Mbps";
    std::wstring hint = tr(L"숫자를 넣으면 ‘직접 입력’이 됩니다 · ") + range;
    bool warn = false;
    if (brCustom) {
        const int k = ParseMbps(brText_);
        warn = true;
        if (k < 0)
            hint = tr(L"숫자를 넣어 주세요 (소수는 한 자리까지) · ") + range;
        else if (k < kVideoBitrateMinKbps || k > kVideoBitrateMaxKbps)
            hint = util::Format(tr(L"%s 사이로 넣어 주세요 · 칸을 벗어나면 %s Mbps로 맞춥니다"), range.c_str(),
                                MbpsText(std::clamp(k, kVideoBitrateMinKbps, kVideoBitrateMaxKbps)).c_str());
        else {
            hint = util::Format(tr(L"1분에 약 %d MB · "), (int)std::lround(k * 0.0075)) + range;
            if (!brNote_.empty()) hint = brNote_ + L" · " + hint;
            warn = !brNote_.empty();
        }
    }
    const float unitW = ui_.Measure(L"Mbps", 13.f) + 2;
    Rect brMore = SettingRow(
        area, tr(L"비트레이트"), brDesc, 150 + 10 + 78 + 6 + unitW,
        [&](Rect c) {
            std::vector<MenuItem> items{{tr(L"자동"), 0}};
            for (int i = 1; i < kVideoBitrateCustom; ++i)
                items.push_back({std::to_wstring(kVideoBitrateOptions[i]) + L" Mbps", i, i == 1});
            items.push_back({tr(L"직접 입력"), kVideoBitrateCustom, true});
            const std::wstring label = items[settings_.vBitrateIdx].text;  // before the list moves into Dropdown
            if (Dropdown(HashId("set.vbr"), c.CutLeft(150), label, tr(L"비트레이트"), std::move(items),
                         settings_.vBitrateIdx, TextColor)) {
                changed = true;
                brNote_.clear();
                if (settings_.vBitrateIdx == kVideoBitrateCustom) ui_.Focus(brField);
            }
            c.CutLeft(10);
            if (ui_.TextField(brField, Rect::Centered(c.l + 39, c.CY(), 78, 32), brText_, L"", 5)) {
                std::wstring t;  // digits, one '.', and one digit after it
                for (wchar_t ch : brText_) {
                    const size_t dot = t.find(L'.');
                    if (ch == L'.' && dot == t.npos) t += ch;
                    if (ch >= L'0' && ch <= L'9' && (dot == t.npos || t.size() - dot < 2)) t += ch;
                }
                brText_ = t;
                settings_.vBitrateIdx = kVideoBitrateCustom;
                brNote_.clear();
                const int k = ParseMbps(brText_);
                if (k >= kVideoBitrateMinKbps && k <= kVideoBitrateMaxKbps) settings_.vBitrateKbps = k;
                changed = true;
            }
            ui_.Describe(brField, tr(L"비트레이트 (Mbps)"), Role::Edit);
            c.CutLeft(78 + 6);
            ui_.Text(c, L"Mbps", 13.f, TextDim);
        },
        16 + 14.f);
    ui_.Text(brMore.CutTop(16), hint, 12.f, warn ? Warning : TextFaint);

    SettingRow(area, tr(L"비트레이트 방식"), tr(L"VBR: 움직임에 따라 조절해 화질 우선 · CBR: 일정한 용량 (스트리밍 · 업로드 규격)"),
               230, [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.vrc"), c, {L"VBR", L"CBR"}, settings_.vRateCtrl);
               });

    // #55: 가변 puts a frame in only when the screen changes, and one per keyframe interval while it doesn't.
    const wchar_t* keyNames[] = {tr(L"1초"), tr(L"2초"), tr(L"5초")};
    Rect vfrMore = SettingRow(
        area, tr(L"프레임 레이트 방식"), tr(L"고정: 1초에 늘 같은 수 · 편집 프로그램과 잘 맞음 · 가변: 화면이 바뀔 때만 넣어 용량이 작음"),
        230,
        [&](Rect c) {
            changed |= ui_.Segmented(HashId("set.vfr"), c, {tr(L"고정 (CFR)"), tr(L"가변 (VFR)")}, settings_.vFrameMode);
        },
        vfr ? 16 + 14.f : 0);
    if (vfr)
        ui_.Text(vfrMore.CutTop(16),
                 util::Format(tr(L"화면이 그대로여도 %s(키프레임 간격)마다 한 장은 넣습니다 · 편집 화면의 MP4와 WebM 저장도 가변입니다"),
                              keyNames[settings_.vKeyIdx]),
                 12.f, TextFaint);

    SettingRow(area, tr(L"키프레임 간격"), tr(L"짧을수록 편집 프로그램에서 탐색이 빠르고, 용량은 조금 늘어납니다"), 230,
               [&](Rect c) {
                   changed |= ui_.Segmented(HashId("set.vkey"), c, {tr(L"1초"), tr(L"2초"), tr(L"5초")}, settings_.vKeyIdx);
               });

    SettingRow(area, tr(L"인코딩 품질"), tr(L"고품질은 같은 용량에서 더 선명하지만 GPU/CPU를 더 씁니다"), 260, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.vpreset"), c, {tr(L"빠르게"), tr(L"균형"), tr(L"고품질")}, settings_.vPresetIdx);
    });
}

void App::SettingsAudio(Rect& area, bool& changed) {
    SettingRow(area, tr(L"소리 녹음"),
               AudioCapture::ProcessLoopbackSupported()
                   ? tr(L"창 캡처: 그 창(프로그램)의 소리만 · 전체 화면 · 영역: 시스템 전체 소리(OzMaker 알림음은 빼고)")
                   : tr(L"전체 화면 · 영역: 시스템 전체 소리 · 창 캡처는 Windows 10 2004 이상에서만 녹음됩니다"),
               60, [&](Rect c) {
                   changed |= ui_.Toggle(HashId("set.audio"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.audio);
               });

    SettingRow(area, tr(L"마이크 녹음"), tr(L"내 목소리를 동영상(MP4)에 함께 녹음합니다 · 위의 소리와 한 트랙으로 섞입니다"), 60,
               [&](Rect c) {
                   changed |= ui_.Toggle(HashId("set.mic"), Rect::Centered(c.r - 22, c.CY(), 44, 24), settings_.mic);
               });
    if (!micListed_) {
        // Tests show a made-up device, not this PC's.
        micDevices_ = testWindowsOnly_ ? std::vector<AudioCapture::Device>{{L"test", tr(L"마이크(USB 테스트 장치)"), true}}
                                       : AudioCapture::InputDevices();
        micListed_ = true;
    }
    bool micKnown = settings_.micDevice.empty();
    for (const auto& d : micDevices_) micKnown |= d.id == settings_.micDevice;
    std::wstring micDesc = micDevices_.empty() ? tr(L"연결된 마이크가 없습니다")
                           : !micKnown         ? tr(L"고른 장치가 연결되어 있지 않습니다 · 연결하거나 다른 장치를 고르세요")
                                               : tr(L"기본 장치로 두면 Windows에서 기본 장치를 바꿀 때 함께 바뀝니다");
    SettingRow(area, tr(L"마이크 장치"), micDesc, 300, [&](Rect c) {
        const UiId id = HashId("set.micDevice");
        int pick = settings_.micDevice.empty() ? 0 : -1;  // -1: a device that isn't connected
        for (size_t i = 0; i < micDevices_.size(); ++i)
            if (micDevices_[i].id == settings_.micDevice) pick = (int)i + 1;
        const bool wasOpen = menu_.owner == id;
        if (Dropdown(id, c, MicDeviceName(), tr(L"마이크 장치"), MicItems(), pick, micKnown ? TextColor : Warning)) {
            settings_.micDevice = pick > 0 && pick <= (int)micDevices_.size() ? micDevices_[pick - 1].id : L"";
            changed = true;
            if (micTest_.Running()) StartMicTest();  // now on the new device
        }
        if (!wasOpen && menu_.owner == id && !testWindowsOnly_) {
            micDevices_ = AudioCapture::InputDevices();  // what is plugged in now
            menu_.items = MicItems();
        }
    });
    SettingRow(area, tr(L"마이크 음량"), tr(L"목소리가 작거나 크게 들리면 조절하세요 · 100%는 장치 그대로"), 300, [&](Rect c) {
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
    SettingRow(area, tr(L"마이크 확인"),
               testing ? (settings_.micMonitor ? tr(L"말해 보세요 · 내 목소리가 들립니다 · 녹음하지 않습니다 · 10초 뒤 멈춥니다")
                                               : tr(L"말해 보세요 · 녹음하지 않습니다 · 10초 뒤 저절로 멈춥니다"))
                       : tr(L"누르는 동안 마이크를 잠시 열어 소리 크기를 보여 줍니다 · 녹음하지 않습니다"),
               300, [&](Rect c) {
                   Rect btn = c.CutRight(84);
                   if (ui_.Button(HashId("set.micTest"), btn.Inset(0, 4), testing ? tr(L"그만") : tr(L"테스트"),
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
    SettingRow(area, tr(L"확인할 때 내 목소리 듣기"),
               tr(L"테스트하는 동안 마이크 소리를 바로 들려줍니다 · 스피커로 들으면 소리가 되울릴 수 있으니 이어폰을 쓰세요"),
               60, [&](Rect c) {
                   if (ui_.Toggle(HashId("set.micMon"), Rect::Centered(c.r - 22, c.CY(), 44, 24),
                                  settings_.micMonitor)) {
                       changed = true;
                       SyncMicMonitor();
                   }
               });

    SettingRow(area, tr(L"오디오 비트레이트"), tr(L"AAC · 160 kbps면 게임 · 음악 모두 충분합니다"), 260, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.abr"), c, {L"96", L"128", L"160", L"192"}, settings_.aBitrateIdx);
    });

    SettingRow(area, tr(L"샘플레이트"), tr(L"영상 편집에는 48 kHz가 표준입니다"), 230, [&](Rect c) {
        changed |= ui_.Segmented(HashId("set.arate"), c, {L"44.1 kHz", L"48 kHz"}, settings_.aRateIdx);
    });
}

void App::SettingsAbout(Rect& area) {
    Rect head = area.CutTop(84);
    ui_.Logo(Rect::XYWH(head.l, head.CY() - 24, 48, 48));
    ui_.Text(Rect(head.l + 64, head.CY() - 24, head.r, head.CY() + 2), L"OzMaker", 20.f, TextColor, Bold());
    ui_.Text(Rect(head.l + 64, head.CY() + 2, head.r, head.CY() + 24), util::Format(tr(L"움짤 메이커 · 버전 %s"), OZ_VERSION_WSTR), 13.f,
             TextDim);
    ui_.Line(area.l, area.t - 0.5f, area.r, area.t - 0.5f, Border);

    std::wstring formats, videos;
    for (int i = 0; i < (int)Format::Count; ++i) {
        std::wstring& list = IsVideoFormat(i) ? videos : formats;
        if (FormatAvailable((Format)i)) list += (list.empty() ? L"" : L", ") + std::wstring(FormatName((Format)i));
    }
    SettingRow(area, tr(L"지원 형식"), tr(L"움짤: ") + formats + tr(L" · 동영상: ") + videos, 0, [](Rect) {});

    const bool portable = Settings::Portable() || portableForTest_;
    SettingRow(area, tr(L"설정 파일"),
               portable ? tr(L"포터블 모드 · OzMaker.exe 옆의 settings.ini에 저장합니다")
                        : tr(L"%APPDATA%\\OzMaker\\settings.ini · OzMaker.exe 옆에 settings.ini를 만들면 포터블로 씁니다"),
               94, [&](Rect c) {
                   if (ui_.Button(HashId("set.iniDir"), c, tr(L"열기"), BtnStyle::Secondary, icon::Folder, true, 13.f))
                       util::OpenFolderAndSelect(Settings::FilePath());
               });

    // Crash reports (#24): only on this PC, for the user to attach to an issue.
    auto reports = crash::List();
    std::wstring crashDesc = tr(L"없음 · 비정상 종료되면 덤프와 로그를 이 PC에만 남깁니다");
    if (!reports.empty()) {
        const std::wstring& n = reports[0].name;  // crash_yyyymmdd_hhmmss_<version>
        std::wstring when = n.size() >= 21 ? n.substr(6, 4) + L"-" + n.substr(10, 2) + L"-" + n.substr(12, 2) + L" " +
                                                 n.substr(15, 2) + L":" + n.substr(17, 2)
                                           : n;
        crashDesc = util::Format(tr(L"%zu개 · 최근 %s · 이슈에 첨부해 알려 주시면 고치는 데 도움이 됩니다"), reports.size(),
                                 when.c_str());
    }
    SettingRow(area, tr(L"충돌 기록"), crashDesc, 94, [&](Rect c) {
        if (ui_.Button(HashId("set.crash"), c, tr(L"열기"), BtnStyle::Secondary, icon::Folder, !reports.empty(), 13.f))
            util::OpenFolderAndSelect(reports[0].dump);
    });

    Updater::Status st = updater_.Get();
    std::wstring desc;
    switch (st.state) {
        case Updater::State::Idle: desc = tr(L"아직 확인하지 않았습니다"); break;
        case Updater::State::Checking: desc = tr(L"확인 중…"); break;
        case Updater::State::UpToDate: desc = tr(L"최신 버전입니다"); break;
        case Updater::State::Downloading:
            desc = util::Format(tr(L"새 버전 %s 받는 중… %d%%"), st.version.c_str(), (int)(st.progress * 100));
            break;
        case Updater::State::Ready:
            desc = util::Format(tr(L"새 버전 %s 준비 완료 · 다시 시작하면 바뀝니다"), st.version.c_str());
            break;
        case Updater::State::Available:
            desc = util::Format(tr(L"새 버전 %s · 이 폴더에는 직접 바꿀 수 없어 내려받기 페이지에서 받아 주세요"),
                                st.version.c_str());
            break;
        case Updater::State::Failed: desc = tr(L"확인하지 못했습니다: ") + st.error; break;
    }
    if (st.state == Updater::State::Checking || st.state == Updater::State::Downloading) RequestWake(ui_.Now() + 0.2);
    SettingRow(area, tr(L"업데이트"), desc, 200, [&](Rect c) {
        if (st.state == Updater::State::Ready) {
            bool en = rec_ == RecState::Idle && !exporter_.Busy();
            if (ui_.Button(HashId("set.install"), c, tr(L"다시 시작해서 업데이트"), BtnStyle::Primary, 0, en, 13.f))
                InstallUpdate();
        } else if (st.state == Updater::State::Available) {
            if (ui_.Button(HashId("set.updPage"), c, tr(L"내려받기 페이지"), BtnStyle::Secondary, icon::OpenFile, true, 13.f))
                util::ShellOpen(Updater::ReleasePage(st.version));
        } else if (ui_.Button(HashId("set.check"), c, tr(L"지금 확인"), BtnStyle::Secondary, icon::Refresh,
                              !updater_.Busy(), 13.f)) {
            updater_.Check();
        }
    });

    SettingRow(area, tr(L"프로젝트"), tr(L"소스 코드 · 바뀐 점 · 오픈소스 라이선스"), 260, [&](Rect c) {
        Rect a = c.CutLeft((c.W() - 8) / 2);
        c.CutLeft(8);
        if (ui_.Button(HashId("set.github"), a, L"GitHub", BtnStyle::Secondary, icon::OpenFile, true, 13.f))
            util::ShellOpen(kRepoUrl);
        if (ui_.Button(HashId("set.licenses"), c, tr(L"라이선스"), BtnStyle::Secondary, 0, true, 13.f))
            util::ShellOpen(std::wstring(kRepoUrl) + L"/blob/main/THIRD-PARTY-NOTICES.md");
    });
}
