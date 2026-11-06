#include "audio.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <thread>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

constexpr UINT mediaReady = WM_APP + 1;

enum ControlId { Open = 101, PlayA, PlayB, Stop, PickA, PickB, Restart };
enum class State { Empty, Loading, Testing, Finished };
enum class Work { Reference, Excerpt };

void setText(HWND control, const std::wstring& value) {
    SetWindowTextW(control, value.c_str());
}

class App {
public:
    ~App() {
        shutdown();
        if (cancel_)
            CloseHandle(cancel_);
        if (font_)
            DeleteObject(font_);
        if (boldFont_)
            DeleteObject(boldFont_);
    }

    bool create(HINSTANCE instance, int show) {
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = dispatch;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"OpusBlindTest";
        if (!RegisterClassW(&wc) || !cancel_)
            return false;

        const HDC screen = GetDC(nullptr);
        dpi_ = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(nullptr, screen);
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT bounds{0, 0, scale(516), scale(316)};
        AdjustWindowRect(&bounds, style, FALSE);
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Opus 盲测",
                                  style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  bounds.right - bounds.left, bounds.bottom - bounds.top,
                                  nullptr, nullptr, instance, this);
        if (!window_)
            return false;
        ShowWindow(window_, show);
        return true;
    }

    HWND window() const { return window_; }

private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto self = static_cast<App*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        const auto self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->handle(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }

    LRESULT handle(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE:
            createControls();
            DragAcceptFiles(window_, TRUE);
            refresh();
            return 0;
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wp);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::wstring path;
            if (count == 1) {
                path.resize(DragQueryFileW(drop, 0, nullptr, 0) + 1);
                path.resize(DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size())));
            }
            DragFinish(drop);
            if (!path.empty())
                load(path);
            else if (count > 1)
                MessageBoxW(window_, L"请一次拖入一个媒体文件。", L"Opus 盲测", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED)
                command(LOWORD(wp));
            return 0;
        case mediaReady:
            if (wp != generation_ || !worker_.joinable())
                return 0;
            worker_.join();
            if (state_ == State::Loading)
                completePreparation();
            return 0;
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wp);
            const auto control = reinterpret_cast<HWND>(lp);
            SetTextColor(dc, control == failureLabel_ ? RGB(180, 0, 0) : GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_DESTROY:
            shutdown();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wp, lp);
    }

    int scale(int value) const { return MulDiv(value, dpi_, 96); }

    void applyFont(HWND control) const {
        auto font = font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }

    HWND label(const wchar_t* value, int x, int y, int w, int h, DWORD style = SS_LEFT) {
        const auto control = CreateWindowExW(0, L"STATIC", value, WS_CHILD | WS_VISIBLE | style,
            scale(x), scale(y), scale(w), scale(h), window_, nullptr, nullptr, nullptr);
        applyFont(control);
        return control;
    }

    void group(const wchar_t* value, int x, int y, int w, int h) {
        const auto control = CreateWindowExW(WS_EX_TRANSPARENT, L"BUTTON", value,
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            scale(x), scale(y), scale(w), scale(h), window_, nullptr, nullptr, nullptr);
        applyFont(control);
    }

    void button(ControlId id, const wchar_t* value, int x, int y, int w, int h) {
        const auto control = CreateWindowExW(0, L"BUTTON", value,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            scale(x), scale(y), scale(w), scale(h), window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        applyFont(control);
    }

    void createControls() {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);
            metrics.lfMessageFont.lfWeight = FW_BOLD;
            boldFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
        }

        group(L"媒体文件", 12, 10, 492, 74);
        file_ = label(L"拖入一个音频或视频文件", 24, 32, 348, 18,
                      SS_PATHELLIPSIS | SS_NOPREFIX);
        button(Open, L"打开媒体…", 392, 26, 98, 27);
        label(L"从第一条音轨的前 90 秒选取试听片段。", 24, 58, 460, 18);

        group(L"音质比较", 12, 94, 492, 172);
        stage_ = label(L"", 24, 115, 466, 18);
        prompt_ = label(L"", 24, 138, 466, 18);
        button(PlayA, L"播放 A", 24, 163, 110, 28);
        button(PlayB, L"播放 B", 144, 163, 110, 28);
        button(Stop, L"停止播放", 264, 163, 110, 28);
        button(PickA, L"A 的音质更好", 24, 199, 110, 28);
        button(PickB, L"B 的音质更好", 144, 199, 110, 28);
        detail_ = label(L"", 24, 235, 466, 25);
        failureLabel_ = label(L"挑战失败", 24, 235, 76, 25);
        failureRate_ = label(L"", 102, 235, 84, 25);
        failureHint_ = label(L"连对次数已重置，请继续挑战。", 188, 235, 302, 25);
        if (boldFont_)
            SendMessageW(failureRate_, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), FALSE);
        showFailure(false);
        button(Restart, L"重新测试", 394, 277, 110, 27);
    }

    void showFailure(bool visible) {
        ShowWindow(detail_, visible ? SW_HIDE : SW_SHOWNA);
        for (auto control : {failureLabel_, failureRate_, failureHint_})
            ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    }

    void setDetail(const std::wstring& value) {
        setText(detail_, value);
        showFailure(false);
    }

    void refresh() {
        const bool testing = state_ == State::Testing;
        const bool loading = state_ == State::Loading;
        const auto enable = [this](ControlId id, bool allowed) {
            const auto control = GetDlgItem(window_, id);
            if (!allowed && GetFocus() == control)
                SetFocus(window_);
            EnableWindow(control, allowed);
        };
        enable(Open, !loading);
        for (auto id : {PlayA, PlayB, Stop})
            enable(id, testing);
        enable(PickA, testing && trial_.listened == 3);
        enable(PickB, testing && trial_.listened == 3);
        enable(Restart, testing || state_ == State::Finished);

        if (testing) {
            setText(stage_, L"第 " + std::to_wstring(answers_ + 1) + L" 题");
            setText(prompt_, L"试听 A 和 B，选择你认为音质更好的一段。");
        } else if (loading) {
            setText(stage_, L"正在准备音频");
            setText(prompt_, L"正在生成试听片段，请稍候。");
        } else if (state_ == State::Empty) {
            setText(stage_, L"等待媒体文件");
            setText(prompt_, L"比较同一段音频的两个版本，选择音质更好的一段。");
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void chooseFile() {
        wchar_t path[32768]{};
        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker);
        picker.hwndOwner = window_;
        picker.lpstrFilter = L"所有文件\0*.*\0";
        picker.lpstrFile = path;
        picker.nMaxFile = ARRAYSIZE(path);
        picker.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&picker))
            load(path);
    }

    void load(const std::filesystem::path& path) {
        if (state_ == State::Loading)
            return;
        player_.stop();
        cancelWork();
        source_.clear();
        reference_.clear();
        encoded_.clear();
        setText(file_, path.filename().native());
        setDetail(L"");
        state_ = State::Loading;
        refresh();
        launch(Work::Reference, [this, path] { source_ = readReference(path, cancel_); });
        if (!worker_.joinable())
            completePreparation();
    }

    template<typename Task>
    void launch(Work work, Task task) {
        work_ = work;
        error_.clear();
        const auto generation = ++generation_;
        try {
            worker_ = std::thread([this, generation, task = std::move(task)] {
                try {
                    task();
                } catch (const std::wstring& message) {
                    error_ = message;
                } catch (const std::exception&) {
                    error_ = L"音频处理失败。请检查媒体文件、临时目录和剩余磁盘空间。";
                }
                PostMessageW(window_, mediaReady, generation, 0);
            });
        } catch (const std::system_error&) {
            error_ = L"无法启动音频处理线程。";
        }
    }

    void begin() {
        player_.stop();
        cancelWork();
        stageChoices_ = 0;
        previousMiss_ = false;
        answers_ = 0;
        referenceChoices_ = 0;
        stageIndex_ = initialStage;
        const size_t frames = source_.size() / channels;
        constexpr size_t maxExcerptFrames = sampleRate * excerptSeconds;
        const size_t count = (frames + maxExcerptFrames - 1) / maxExcerptFrames;
        excerptCount_ = frames >= 10 * sampleRate ? std::max<size_t>(2, count) : 1;
        const auto first = std::uniform_int_distribution<size_t>(0, excerptCount_ - 1)(random_);
        prepareExcerpt(first, stageIndex_, stageIndex_);
        awaitExcerpt();
    }

    size_t excerptStart(size_t index) const {
        return index * (source_.size() / channels) / excerptCount_;
    }

    void prepareExcerpt(size_t excerpt, size_t successStage, size_t failureStage) {
        queuedExcerpt_ = excerpt;
        queuedStage_ = successStage;
        const auto start = excerptStart(excerpt);
        const auto length = excerptStart(excerpt + 1) - start;
        launch(Work::Excerpt, [this, start, length, successStage, failureStage] {
            preparedReference_ = encodeExcerpt(source_, start, length, referenceBitrate, cancel_);
            preparedComparison_ = encodeExcerpt(source_, start, length, bitrates[successStage], cancel_);
            if (failureStage != successStage)
                preparedOnFailure_ = encodeExcerpt(source_, start, length, bitrates[failureStage], cancel_);
        });
    }

    void prefetchNext() {
        const bool hasHigherRate = stageIndex_ + 1 < bitrates.size();
        const bool higherRate = hasHigherRate && stageChoices_ + 1 == advanceStreak;
        const size_t step = excerptCount_ > 1
            ? std::uniform_int_distribution<size_t>(1, excerptCount_ - 1)(random_) : 0;
        const size_t failureStage = stageIndex_ > 0 ? stageIndex_ - 1 : 0;
        prepareExcerpt((excerptIndex_ + step) % excerptCount_, stageIndex_ + higherRate, failureStage);
    }

    void awaitExcerpt() {
        state_ = State::Loading;
        if (worker_.joinable())
            refresh();
        else
            completePreparation();
    }

    void completePreparation() {
        if (!error_.empty()) {
            state_ = State::Empty;
            setDetail(error_);
            refresh();
            return;
        }
        if (work_ == Work::Reference) {
            begin();
            return;
        }
        excerptIndex_ = queuedExcerpt_;
        reference_ = std::move(preparedReference_);
        encoded_ = stageIndex_ == queuedStage_
            ? std::move(preparedComparison_) : std::move(preparedOnFailure_);
        preparedComparison_.clear();
        preparedOnFailure_.clear();
        state_ = State::Testing;
        nextTrial();
    }

    void nextTrial() {
        trial_.randomize(random_);
        if (!previousMiss_)
            setDetail(L"同一题中切换 A 和 B 会沿用当前播放位置。");
        refresh();
        SetFocus(GetDlgItem(window_, PlayA));
    }

    void answer(bool choseB) {
        if (trial_.listened != 3)
            return;
        player_.stop();
        const bool choseReference = !trial_.usesComparison(choseB ? 1 : 0);
        ++answers_;
        previousMiss_ = !choseReference;
        if (!choseReference) {
            stageChoices_ = 0;
            setText(failureRate_, std::to_wstring(bitrates[stageIndex_]) + L" kbps");
            setText(failureHint_, stageIndex_ > 0 ? L"已降一档，连对次数已重置。"
                                                  : L"已在最低档，连对次数已重置。");
            if (stageIndex_ > 0)
                --stageIndex_;
            showFailure(true);
            awaitExcerpt();
            return;
        }
        ++referenceChoices_;
        ++stageChoices_;
        const bool hasHigherRate = stageIndex_ + 1 < bitrates.size();
        if (!hasHigherRate && stageChoices_ == finalStreak) {
            finish(L"测试结束 · 已完成最高难度");
            return;
        }
        if (hasHigherRate && stageChoices_ == advanceStreak) {
            ++stageIndex_;
            stageChoices_ = 0;
        }
        awaitExcerpt();
    }

    void finish(const wchar_t* result) {
        state_ = State::Finished;
        setText(stage_, result);
        const int comparison = bitrates[stageIndex_];
        const int rateA = trial_.usesComparison(0) ? comparison : referenceBitrate;
        const int rateB = trial_.usesComparison(1) ? comparison : referenceBitrate;
        setText(prompt_, L"本题码率    A："
            + std::to_wstring(rateA) + L" kbps    B：" + std::to_wstring(rateB) + L" kbps");
        setDetail(L"选中参照版本 " + std::to_wstring(referenceChoices_)
            + L" / " + std::to_wstring(answers_) + L" 题");
        refresh();
        SetFocus(GetDlgItem(window_, Restart));
    }

    void command(int id) {
        if (id == Open && state_ != State::Loading)
            chooseFile();
        else if (id == Restart && (state_ == State::Testing || state_ == State::Finished))
            begin();
        else if (state_ == State::Testing) {
            if (id == PlayA || id == PlayB) {
                const int sample = id - PlayA;
                if (!player_.play(reference_, encoded_, trial_.usesComparison(sample))) {
                    MessageBoxW(window_, L"无法播放音频。请检查 Windows 默认音频输出设备。",
                                L"Opus 盲测", MB_OK | MB_ICONERROR);
                    return;
                }
                if (trial_.listened == 0)
                    prefetchNext();
                trial_.listened |= 1u << sample;
                setDetail(std::wstring(L"播放片段 ") + L"AB"[sample] + L"  ·  可随时切换或停止");
                refresh();
            } else if (id == Stop) {
                player_.stop();
                setDetail(L"已停止播放。再次播放时从片段开头开始。");
                refresh();
            } else if (id == PickA || id == PickB) {
                answer(id == PickB);
            }
        }
    }

    void shutdown() {
        player_.stop();
        cancelWork();
    }

    void cancelWork() {
        if (worker_.joinable()) {
            SetEvent(cancel_);
            worker_.join();
            ResetEvent(cancel_);
        }
        ++generation_;
    }

    HWND window_ = nullptr;
    HWND file_ = nullptr;
    HWND stage_ = nullptr;
    HWND prompt_ = nullptr;
    HWND detail_ = nullptr;
    HWND failureLabel_ = nullptr;
    HWND failureRate_ = nullptr;
    HWND failureHint_ = nullptr;
    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;
    int dpi_ = 96;
    HANDLE cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker_;
    std::wstring error_;
    WPARAM generation_ = 0;
    Work work_ = Work::Reference;
    Pcm source_;
    Pcm reference_;
    Pcm encoded_;
    Pcm preparedReference_;
    Pcm preparedComparison_;
    Pcm preparedOnFailure_;
    size_t queuedExcerpt_ = 0;
    size_t queuedStage_ = 0;
    Player player_;
    State state_ = State::Empty;
    size_t stageIndex_ = 0;
    size_t excerptCount_ = 1;
    size_t excerptIndex_ = 0;
    int stageChoices_ = 0;
    bool previousMiss_ = false;
    int answers_ = 0;
    int referenceChoices_ = 0;
    std::mt19937 random_{std::random_device{}()};
    BlindTrial trial_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    App app;
    if (!app.create(instance, show)) {
        MessageBoxW(nullptr, L"无法创建应用窗口。", L"Opus 盲测", MB_OK | MB_ICONERROR);
        return 1;
    }
    MSG message{};
    int result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(app.window(), &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
