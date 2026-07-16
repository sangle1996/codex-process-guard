#include "core.h"
#include "native.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace {

using namespace guard;

constexpr wchar_t class_name[] = L"CodexProcessGuardNativeWindow";
constexpr wchar_t mutex_name[] = L"Local\\CodexProcessGuardNative";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT timer_id = 1;
constexpr UINT tray_id = 1;
constexpr int id_interval = 100;
constexpr int id_startup = 101;
constexpr int id_save = 102;
constexpr int id_scan = 103;
constexpr int id_kill = 104;
constexpr int id_hide = 105;
constexpr int id_list = 106;
constexpr int menu_open = 200;
constexpr int menu_scan = 201;
constexpr int menu_exit = 202;

struct Settings { int interval_minutes = 2; bool start_with_windows = false; };

struct CohortRow {
    Cohort cohort;
    std::vector<std::wstring> cells;
};

struct ScanResult {
    int memory_percent{};
    std::uint64_t physical_used{};
    std::uint64_t physical_total{};
    bool physical_memory_known{};
    int roots{};
    int live_attached{};
    int pending{};
    int killed{};
    std::uint64_t estimated_released{};
    int estimated_unknown{};
    EvidenceSnapshot evidence;
    std::uint64_t guard_working_set{};
    bool guard_working_set_known{};
    long long elapsed_ms{};
    std::vector<CohortRow> rows;
};

struct EvidenceTotals {
    std::uint64_t scans{};
    std::uint64_t auto_confirmed{};
    std::uint64_t manual_confirmed{};
    std::uint64_t estimated_released{};
    std::uint64_t unknown_released{};
};

struct SystemMemory {
    int percent{};
    std::uint64_t used{};
    std::uint64_t total{};
    bool known{};
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring join_counts(const std::map<std::wstring, int>& values) {
    std::wostringstream output;
    bool first = true;
    for (const auto& [name, count] : values) {
        if (!first) output << L", ";
        output << name << L' ' << count;
        first = false;
    }
    return output.str();
}

std::wstring process_type(std::wstring name) {
    name = lower(std::move(name));
    if (name == L"python.exe" || name == L"pythonw.exe") return L"python";
    if (name == L"uv.exe" || name == L"uvx.exe") return L"uv";
    if (name.ends_with(L".exe")) name.resize(name.size() - 4);
    return name;
}

std::wstring started_text(std::uint64_t ticks) {
    ULARGE_INTEGER value{};
    value.QuadPart = ticks;
    FILETIME utc{value.LowPart, value.HighPart}, local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &time)) return L"?";
    wchar_t text[16]{};
    swprintf_s(text, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    return text;
}

std::wstring age_text(std::uint64_t started) {
    FILETIME now_file{};
    GetSystemTimeAsFileTime(&now_file);
    ULARGE_INTEGER now{};
    now.LowPart = now_file.dwLowDateTime;
    now.HighPart = now_file.dwHighDateTime;
    const auto minutes = now.QuadPart > started ? (now.QuadPart - started) / 600'000'000ULL : 0;
    std::wostringstream output;
    if (minutes >= 1440) output << minutes / 1440 << L"d " << minutes % 1440 / 60 << L"h";
    else if (minutes >= 60) output << minutes / 60 << L"h " << minutes % 60 << L"m";
    else output << minutes << L"m";
    return output.str();
}

std::wstring decision_text(CohortDecision decision) {
    if (decision == CohortDecision::newest_live) return L"Newest live cohort - preserved";
    if (decision == CohortDecision::older_live) return L"Older live cohort - preserved";
    return L"Owner gone - automatic two-scan policy";
}

SystemMemory system_memory() {
    MEMORYSTATUSEX status{sizeof(status)};
    if (!GlobalMemoryStatusEx(&status)) return {};
    return {static_cast<int>(status.dwMemoryLoad), status.ullTotalPhys - status.ullAvailPhys, status.ullTotalPhys, true};
}

std::wstring megabytes(std::uint64_t bytes) {
    std::wostringstream output;
    output << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / 1024.0 / 1024.0;
    return output.str();
}

class App {
public:
    int run(HINSTANCE instance, const wchar_t* command_line) {
        mutex_ = CreateMutexW(nullptr, TRUE, mutex_name);
        if (!mutex_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (const HWND existing = FindWindowW(class_name, nullptr)) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            return 0;
        }

        instance_ = instance;
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        data_directory_ = local_data_directory();
        std::filesystem::create_directories(data_directory_);
        settings_ = load_settings();
        tracked_ = load_tracking();
        totals_ = load_evidence_totals();
        cleanup_records_ = load_cleanup_records();

        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = class_name;
        RegisterClassExW(&window_class);

        window_ = CreateWindowExW(0, class_name, L"Codex Process Guard Native",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 1160, 920, nullptr, nullptr, instance, this);
        if (!window_) return 1;
        create_controls();
        add_tray_icon();
        apply_startup();
        apply_timer();
        load_recent_log();
        append_log(L"Native guard started. Automatic cleanup preserves live Codex owners.");
        if (evidence_history_new_) append_log(L"Local evidence history started this run; no earlier totals were available.");
        if (!evidence_history_valid_) append_log(L"Evidence history was missing or invalid and was reset; current measurements are unaffected.");
        scan(true);
        append_log(show_balloon(L"Codex Process Guard Native", L"Running in the notification area. Automatic scans are active.")
            ? L"Startup notification sent." : L"Startup notification unavailable; keeping the window visible.");

        const bool background = command_line && wcsstr(command_line, L"/background");
        ShowWindow(window_, background && tray_added_ ? SW_HIDE : SW_SHOW);
        UpdateWindow(window_);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (mutex_) CloseHandle(mutex_);
        return static_cast<int>(message.wParam);
    }

private:
    HINSTANCE instance_{};
    HWND window_{}, interval_{}, startup_{}, status_{}, next_scan_{}, evidence_{}, list_{}, log_{}, kill_{};
    HANDLE mutex_{};
    NOTIFYICONDATAW tray_{};
    Settings settings_;
    EvidenceTotals totals_;
    std::filesystem::path data_directory_;
    std::vector<TrackedProcess> tracked_;
    std::vector<CohortRow> rows_;
    std::vector<CleanupRecord> cleanup_records_;
    bool exiting_{}, tray_added_{}, evidence_history_valid_{true}, evidence_history_new_{}, cleanup_history_valid_{true};
    UINT taskbar_created_{};

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->handle_message(window, message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbar_created_ && message == taskbar_created_) {
            add_tray_icon();
            if (!tray_added_ && !IsWindowVisible(window_)) show_window();
            return 0;
        }
        if (message == tray_message) {
            if (LOWORD(lparam) == WM_LBUTTONDBLCLK) show_window();
            if (LOWORD(lparam) == WM_RBUTTONUP) show_tray_menu();
            return 0;
        }
        switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case id_save: save_from_controls(); break;
            case id_scan: scan(true); break;
            case id_kill: kill_selected(); break;
            case id_hide: hide_to_tray(); break;
            case menu_open: show_window(); break;
            case menu_scan: scan(true); break;
            case menu_exit: exiting_ = true; DestroyWindow(window_); break;
            }
            return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == id_list && reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED)
                update_kill_button();
            return 0;
        case WM_TIMER:
            if (wparam == timer_id) scan(true);
            return 0;
        case WM_CLOSE:
            if (!exiting_) { hide_to_tray(); return 0; }
            break;
        case WM_DESTROY:
            if (tray_added_) Shell_NotifyIconW(NIM_DELETE, &tray_);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static std::filesystem::path local_data_directory() {
        wchar_t value[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value, static_cast<DWORD>(std::size(value)));
        return std::filesystem::path(length ? value : L".") / L"CodexProcessGuardNative";
    }

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id = 0) {
        HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return result;
    }

    void create_controls() {
        control(L"STATIC", L"Codex Process Guard Native", SS_LEFT, 18, 14, 300, 22);
        control(L"STATIC", L"Automatic cleanup only removes exact helpers after their Codex owner is missing for two scans. Manual removal requires fresh exact-cohort validation.", SS_LEFT, 18, 42, 1090, 36);
        control(L"STATIC", L"Check every", SS_LEFT, 18, 91, 80, 22);
        interval_ = control(L"EDIT", std::to_wstring(settings_.interval_minutes).c_str(), WS_BORDER | ES_NUMBER, 100, 88, 60, 24, id_interval);
        control(L"STATIC", L"minute(s)", SS_LEFT, 170, 91, 75, 22);
        startup_ = control(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX, 270, 89, 150, 24, id_startup);
        SendMessageW(startup_, BM_SETCHECK, settings_.start_with_windows ? BST_CHECKED : BST_UNCHECKED, 0);
        control(L"BUTTON", L"Save", BS_PUSHBUTTON, 18, 126, 90, 28, id_save);
        control(L"BUTTON", L"Scan now", BS_PUSHBUTTON, 116, 126, 105, 28, id_scan);
        kill_ = control(L"BUTTON", L"Kill selected older", BS_PUSHBUTTON, 229, 126, 150, 28, id_kill);
        EnableWindow(kill_, FALSE);
        control(L"BUTTON", L"Hide to tray", BS_PUSHBUTTON, 387, 126, 110, 28, id_hide);
        status_ = control(L"STATIC", L"Status: starting...", SS_LEFT, 18, 172, 1090, 22);
        next_scan_ = control(L"STATIC", L"Next scan: -", SS_LEFT, 18, 196, 600, 22);
        control(L"STATIC", L"Cleanup results", SS_LEFT, 18, 224, 420, 22);
        evidence_ = control(L"EDIT", L"Collecting first measurement...", WS_BORDER | ES_MULTILINE | ES_READONLY,
                            18, 246, 1100, 150);
        control(L"STATIC", L"Managed process cohorts", SS_LEFT, 18, 406, 300, 22);

        list_ = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, 18, 428, 1100, 250, id_list);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* headers[] = {L"Started", L"Age", L"Owner PID", L"Proc", L"RAM MB", L"TCP", L"Process types", L"MCP groups", L"Safety decision"};
        const int widths[] = {75, 65, 72, 48, 66, 45, 170, 290, 250};
        for (int index = 0; index < static_cast<int>(std::size(headers)); ++index) {
            LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH, 0, widths[index], const_cast<LPWSTR>(headers[index])};
            ListView_InsertColumn(list_, index, &column);
        }
        control(L"STATIC", L"Recent native-scan log", SS_LEFT, 18, 688, 300, 22);
        log_ = control(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 18, 710, 1100, 150);
    }

    void add_tray_icon() {
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = tray_id;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = tray_message;
        tray_.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        wcscpy_s(tray_.szTip, L"Codex Process Guard Native");
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
        if (tray_added_) {
            tray_.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &tray_);
        }
    }

    void hide_to_tray() {
        if (tray_added_) ShowWindow(window_, SW_HIDE);
        else MessageBoxW(window_, L"The notification icon is unavailable, so the window will stay open.",
                         L"Codex Process Guard Native", MB_OK | MB_ICONWARNING);
    }

    void show_tray_menu() {
        POINT point{};
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, menu_open, L"Open");
        AppendMenuW(menu, MF_STRING, menu_scan, L"Scan now");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, menu_exit, L"Exit");
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void show_window() {
        ShowWindow(window_, SW_SHOW);
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
    }

    Settings load_settings() const {
        Settings value;
        std::wifstream input(data_directory_ / L"settings.ini");
        std::wstring line;
        while (std::getline(input, line)) {
            if (line.starts_with(L"IntervalMinutes=")) value.interval_minutes = std::clamp(_wtoi(line.c_str() + 16), 1, 60);
            if (line.starts_with(L"StartWithWindows=")) value.start_with_windows = line.substr(17) == L"true";
        }
        return value;
    }

    void save_settings() const {
        std::wofstream output(data_directory_ / L"settings.ini", std::ios::trunc);
        output << L"IntervalMinutes=" << settings_.interval_minutes << L"\nStartWithWindows="
               << (settings_.start_with_windows ? L"true" : L"false") << L'\n';
    }

    EvidenceTotals load_evidence_totals() {
        EvidenceTotals result;
        const auto path = data_directory_ / L"evidence.ini";
        if (!std::filesystem::exists(path)) { evidence_history_new_ = true; return result; }
        std::wifstream input(path);
        std::wstring key;
        std::uint64_t value = 0;
        bool version_seen = false, scans_seen = false, auto_seen = false, manual_seen = false;
        bool estimate_seen = false, unknown_seen = false;
        while (input >> key >> value) {
            if (key == L"version" && !version_seen) {
                if (value != 1) { evidence_history_valid_ = false; return {}; }
                version_seen = true;
            }
            else if (key == L"scans" && !scans_seen) { result.scans = value; scans_seen = true; }
            else if (key == L"auto_confirmed" && !auto_seen) { result.auto_confirmed = value; auto_seen = true; }
            else if (key == L"manual_confirmed" && !manual_seen) { result.manual_confirmed = value; manual_seen = true; }
            else if (key == L"observed_pre_kill_ws" && !estimate_seen) { result.estimated_released = value; estimate_seen = true; }
            else if (key == L"unknown_pre_kill_ws" && !unknown_seen) { result.unknown_released = value; unknown_seen = true; }
            else { evidence_history_valid_ = false; return {}; }
        }
        evidence_history_valid_ = input.eof() && version_seen && scans_seen && auto_seen && manual_seen && estimate_seen && unknown_seen;
        if (!evidence_history_valid_) return {};
        return result;
    }

    bool save_evidence_totals() const {
        try {
            const auto path = data_directory_ / L"evidence.ini";
            const auto temporary = data_directory_ / L"evidence.tmp";
            std::filesystem::remove(temporary);
            std::wofstream output(temporary, std::ios::trunc);
            output << L"version 1\nscans " << totals_.scans << L"\nauto_confirmed " << totals_.auto_confirmed
                   << L"\nmanual_confirmed " << totals_.manual_confirmed << L"\nobserved_pre_kill_ws "
                   << totals_.estimated_released << L"\nunknown_pre_kill_ws " << totals_.unknown_released << L'\n';
            output.flush();
            if (!output) return false;
            output.close();
            if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            std::filesystem::remove(temporary);
            return false;
        } catch (...) { return false; }
    }

    std::vector<CleanupRecord> load_cleanup_records() {
        std::vector<CleanupRecord> result;
        const auto path = data_directory_ / L"cleanup.tsv";
        if (!std::filesystem::exists(path)) return result;
        std::wifstream input(path);
        if (!input) { cleanup_history_valid_ = false; return result; }
        const auto raw_now = std::time(nullptr);
        if (raw_now < 0) { cleanup_history_valid_ = false; return result; }
        const auto now = static_cast<std::uint64_t>(raw_now);
        std::wstring line;
        while (std::getline(input, line)) {
            CleanupRecord record;
            int known = 0, automatic = 0;
            std::wstring extra;
            std::wistringstream row(line);
            if (row >> record.timestamp >> record.working_set >> known >> automatic &&
                !(row >> extra) && record.timestamp && record.timestamp <= now &&
                (known == 0 || known == 1) && (automatic == 0 || automatic == 1)) {
                record.working_set_known = known != 0;
                record.automatic = automatic != 0;
                if (now - record.timestamp <= 60 * 60) result.push_back(record);
            } else cleanup_history_valid_ = false;
        }
        return result;
    }

    bool save_cleanup_records() const {
        try {
            const auto path = data_directory_ / L"cleanup.tsv";
            const auto temporary = data_directory_ / L"cleanup.tmp";
            std::filesystem::remove(temporary);
            std::wofstream output(temporary, std::ios::trunc);
            for (const auto& record : cleanup_records_)
                output << record.timestamp << L'\t' << record.working_set << L'\t'
                       << record.working_set_known << L'\t' << record.automatic << L'\n';
            output.flush();
            if (!output) return false;
            output.close();
            if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            std::filesystem::remove(temporary);
            return false;
        } catch (...) { return false; }
    }

    bool record_cleanup(std::uint64_t working_set, bool known, bool automatic) {
        const auto raw_now = std::time(nullptr);
        if (raw_now < 0) { cleanup_history_valid_ = false; return false; }
        const auto now = static_cast<std::uint64_t>(raw_now);
        std::erase_if(cleanup_records_, [&](const CleanupRecord& item) {
            return !item.timestamp || item.timestamp > now || now - item.timestamp > 60 * 60;
        });
        const CleanupRecord record{now, working_set, known, automatic};
        cleanup_records_.push_back(record);
        if (save_cleanup_records()) return true;
        cleanup_history_valid_ = false;
        return false;
    }

    std::vector<TrackedProcess> load_tracking() const {
        std::vector<TrackedProcess> result;
        IdentitySet seen;
        std::wifstream input(data_directory_ / L"tracked.tsv");
        std::wstring line;
        while (std::getline(input, line)) {
            TrackedProcess item;
            std::wistringstream row(line);
            if (!(row >> item.helper.pid >> item.helper.started >> item.owner.pid >> item.owner.started >> item.orphan_observations >> item.name) ||
                !item.helper.pid || !item.helper.started || !item.owner.pid || !item.owner.started || !seen.insert(item.helper).second) continue;
            item.orphan_observations = std::clamp(item.orphan_observations, 0, 1);
            result.push_back(std::move(item));
        }
        return result;
    }

    bool save_tracking() const {
        try {
            const auto path = data_directory_ / L"tracked.tsv";
            const auto temporary = data_directory_ / L"tracked.tmp";
            std::filesystem::remove(temporary);
            std::wofstream output(temporary, std::ios::trunc);
            if (!output) return false;
            for (const auto& item : tracked_)
                output << item.helper.pid << L'\t' << item.helper.started << L'\t' << item.owner.pid << L'\t'
                       << item.owner.started << L'\t' << item.orphan_observations << L'\t' << item.name << L'\n';
            output.flush();
            if (!output) return false;
            output.close();
            if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            std::filesystem::remove(temporary);
            return false;
        } catch (...) { return false; }
    }

    void apply_startup() const {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
        if (settings_.start_with_windows) {
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
            const std::wstring command = L"\"" + std::wstring(executable) + L"\" /background";
            RegSetValueExW(key, L"CodexProcessGuardNative", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        } else RegDeleteValueW(key, L"CodexProcessGuardNative");
        RegCloseKey(key);
    }

    void apply_timer() {
        KillTimer(window_, timer_id);
        if (!SetTimer(window_, timer_id, static_cast<UINT>(settings_.interval_minutes * 60 * 1000), nullptr)) {
            SetWindowTextW(next_scan_, L"Next scan: timer failed");
            append_log(L"Automatic scan timer failed; use Scan now or Save to retry.");
            return;
        }
        SYSTEMTIME next{};
        GetLocalTime(&next);
        FILETIME file{};
        SystemTimeToFileTime(&next, &file);
        ULARGE_INTEGER value{};
        value.LowPart = file.dwLowDateTime;
        value.HighPart = file.dwHighDateTime;
        value.QuadPart += static_cast<ULONGLONG>(settings_.interval_minutes) * 60ULL * 10'000'000ULL;
        file.dwLowDateTime = value.LowPart;
        file.dwHighDateTime = value.HighPart;
        FileTimeToSystemTime(&file, &next);
        wchar_t text[64]{};
        swprintf_s(text, L"Next scan: %04u-%02u-%02u %02u:%02u:%02u", next.wYear, next.wMonth, next.wDay, next.wHour, next.wMinute, next.wSecond);
        SetWindowTextW(next_scan_, text);
    }

    void save_from_controls() {
        wchar_t text[16]{};
        GetWindowTextW(interval_, text, static_cast<int>(std::size(text)));
        settings_.interval_minutes = std::clamp(_wtoi(text), 1, 60);
        settings_.start_with_windows = SendMessageW(startup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SetWindowTextW(interval_, std::to_wstring(settings_.interval_minutes).c_str());
        save_settings();
        apply_startup();
        apply_timer();
        append_log(L"Settings saved.");
    }

    void load_recent_log() {
        std::wifstream input(data_directory_ / L"guard.log");
        std::deque<std::wstring> lines;
        std::wstring line;
        while (std::getline(input, line)) {
            lines.push_back(line);
            if (lines.size() > 100) lines.pop_front();
        }
        std::wstring text;
        for (const auto& value : lines) text += value + L"\r\n";
        SetWindowTextW(log_, text.c_str());
    }

    void append_log(const std::wstring& message) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t prefix[32]{};
        swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u] ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        const std::wstring line = prefix + message;
        const auto path = data_directory_ / L"guard.log";
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (!error && size > 1024 * 1024) {
            std::wofstream truncated(path, std::ios::trunc);
            truncated << L"[log truncated at 1 MiB]\n";
        }
        std::wofstream output(path, std::ios::app);
        output << line << L'\n';
        const int length = GetWindowTextLengthW(log_);
        SendMessageW(log_, EM_SETSEL, length, length);
        const std::wstring visible = line + L"\r\n";
        SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(visible.c_str()));
    }

    IdentitySet live_codex(const Snapshot& snapshot) const {
        IdentitySet result;
        for (const auto& [pid, process] : snapshot) {
            static_cast<void>(pid);
            if (lower(process.name) == L"codex.exe") result.insert(process.identity);
        }
        return result;
    }

    std::vector<CohortRow> make_rows(const std::vector<Cohort>& cohorts, const Snapshot& snapshot,
                                     const std::unordered_set<std::uint32_t>& tcp_owners) const {
        std::vector<CohortRow> result;
        for (const auto& cohort : cohorts) {
            std::map<std::wstring, int> types, services;
            std::uint64_t ram = 0;
            int tcp = 0, unknown_ram = 0;
            for (const auto& identity : cohort.members) {
                const auto found = snapshot.find(identity.pid);
                if (found == snapshot.end() || found->second.identity != identity) continue;
                ++types[process_type(found->second.name)];
                ++services[service_name(found->second).value_or(L"Unknown")];
                if (found->second.working_set_known) ram = saturating_add(ram, found->second.working_set);
                else ++unknown_ram;
                if (tcp_owners.contains(identity.pid)) ++tcp;
            }
            const auto ram_text = megabytes(ram) + (unknown_ram ? L" + " + std::to_wstring(unknown_ram) + L" unknown" : L"");
            result.push_back({cohort, {started_text(cohort.started), age_text(cohort.started), std::to_wstring(cohort.owner.pid),
                std::to_wstring(cohort.members.size()), ram_text, std::to_wstring(tcp),
                join_counts(types), join_counts(services), decision_text(cohort.decision)}});
        }
        return result;
    }

    ScanResult perform_scan(bool observe_orphans) {
        const auto started = std::chrono::steady_clock::now();
        const auto snapshot = read_process_snapshot();
        const auto owners = live_codex(snapshot);
        auto update = update_tracking(tracked_, snapshot, owners, observe_orphans);
        const auto removed = terminate_candidates(tracked_, snapshot, update.termination_candidates, check_identity, terminate_exact);
        const bool state_saved = save_tracking();
        const auto cohorts = build_cohorts(tracked_, snapshot, owners);
        const auto evidence = build_evidence(tracked_, snapshot, owners);
        const auto rows = make_rows(cohorts, snapshot, read_tcp_owner_pids());
        const auto self = snapshot.find(GetCurrentProcessId());
        const auto guard_working_set = self == snapshot.end() ? 0 : self->second.working_set;
        const bool guard_working_set_known = self != snapshot.end() && self->second.working_set_known;
        const auto memory = system_memory();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        totals_.scans = saturating_add(totals_.scans, 1);
        totals_.auto_confirmed = saturating_add(totals_.auto_confirmed, static_cast<std::uint64_t>(removed.confirmed));
        totals_.estimated_released = saturating_add(totals_.estimated_released, removed.estimated_working_set);
        totals_.unknown_released = saturating_add(totals_.unknown_released, static_cast<std::uint64_t>(removed.unknown_working_sets));
        for (const auto& event : removed.events) {
            if (!record_cleanup(event.working_set, event.working_set_known, true))
                append_log(L"Recent cleanup history save failed; the 1-hour summary may be incomplete after restart.");
            append_log(L"AUTO_CONFIRMED helper PID=" + std::to_wstring(event.helper.pid) + L", created=" +
                       std::to_wstring(event.helper.started) + L", owner PID=" + std::to_wstring(event.owner.pid) +
                       L", pre-kill WS=" + (event.working_set_known ? megabytes(event.working_set) + L" MB" : L"unavailable") +
                       L"; process exit handle signaled.");
        }
        if (!state_saved) append_log(L"State save failed; process safety remained fail-closed.");
        if (!save_evidence_totals()) append_log(L"Evidence totals save failed; current measurements remain valid.");
        return {memory.percent, memory.used, memory.total, memory.known, static_cast<int>(owners.size()), update.live_attached,
                (std::max)(0, update.pending - removed.confirmed), removed.confirmed,
                removed.estimated_working_set, removed.unknown_working_sets, evidence, guard_working_set,
                guard_working_set_known, elapsed, rows};
    }

    void scan(bool observe_orphans) {
        EnableWindow(kill_, FALSE);
        SetWindowTextW(status_, L"Status: native scan running...");
        const auto result = perform_scan(observe_orphans);
        rows_ = result.rows;
        populate_rows();
        std::wostringstream status;
        status << L"Status: RAM " << (result.physical_memory_known ? std::to_wstring(result.memory_percent) + L"%" : L"unavailable")
               << L", " << result.roots << L" Codex root(s), " << result.live_attached << L" live-attached, "
               << result.pending << L" pending, " << result.killed << L" killed. Native scan " << result.elapsed_ms << L" ms.";
        SetWindowTextW(status_, status.str().c_str());
        update_evidence(result);
        apply_timer();
        append_log(status.str() + L" Managed set " + std::to_wstring(result.evidence.managed) + L"/" +
                   megabytes(result.evidence.managed_working_set) + L" MB observed; confirmed pre-kill WS " +
                   megabytes(result.estimated_released) + L" MB (" + std::to_wstring(result.estimated_unknown) + L" unavailable)." );
        if (result.killed > 0) show_balloon(L"Native cleanup complete", std::to_wstring(result.killed) + L" orphaned helper process(es) removed.");
    }

    void update_evidence(const ScanResult& result) {
        const auto raw_now = std::time(nullptr);
        const bool recent_time_known = raw_now >= 0;
        if (!recent_time_known) cleanup_history_valid_ = false;
        const auto recent = recent_time_known
            ? summarize_cleanup(cleanup_records_, static_cast<std::uint64_t>(raw_now), 60 * 60)
            : CleanupSummary{};
        const auto suspicious = result.evidence.waiting_confirmation + result.evidence.eligible_preserved;
        std::wostringstream text;
        text << L"LAST 60 MINUTES: ";
        if (!recent_time_known) text << L"Clock unavailable; recent cleanup cannot be calculated.";
        else text << recent.confirmed << L" confirmed cleanup(s) (" << recent.automatic
                  << L" automatic); pre-exit process WS " << megabytes(recent.observed_working_set) << L" MB"
                  << (recent.unknown_working_sets ? L" + " + std::to_wstring(recent.unknown_working_sets) + L" unknown WS" : L"")
                  << L" (not measured system RAM saved)"
                  << (cleanup_history_valid_ ? L"." : L"; history may be incomplete.");
        text << L"\r\nCURRENT DECISION: " << result.evidence.protected_live << L" protected by an exact live Codex owner; "
             << suspicious << L" waiting/eligible because the exact owner was not observed in this scan."
             << L"\r\nSAFETY RULE: No age or idle timeout. Automatic cleanup requires the exact owner identity to be missing in two consecutive observing scans."
             << L"\r\nGUARD COST: " << (result.guard_working_set_known ? megabytes(result.guard_working_set) + L" MB RAM" : L"RAM unavailable")
             << L"; scan " << result.elapsed_ms << L" ms every " << settings_.interval_minutes << L" minute(s)."
             << L"\r\nALL RECORDED" << (evidence_history_new_ || !evidence_history_valid_ ? L" (this run only)" : L"")
             << L": " << saturating_add(totals_.auto_confirmed, totals_.manual_confirmed)
             << L" confirmed cleanup(s); pre-exit process WS " << megabytes(totals_.estimated_released) << L" MB"
             << (totals_.unknown_released ? L" + " + std::to_wstring(totals_.unknown_released) + L" unknown WS" : L"")
             << L" (not measured system RAM saved).";
        SetWindowTextW(evidence_, text.str().c_str());
    }

    void populate_rows() {
        ListView_DeleteAllItems(list_);
        for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = const_cast<LPWSTR>(rows_[row].cells[0].c_str());
            item.lParam = row;
            ListView_InsertItem(list_, &item);
            for (int column = 1; column < static_cast<int>(rows_[row].cells.size()); ++column)
                ListView_SetItemText(list_, row, column, const_cast<LPWSTR>(rows_[row].cells[column].c_str()));
        }
        update_kill_button();
    }

    void update_kill_button() {
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        const bool allowed = selected >= 0 && selected < static_cast<int>(rows_.size()) &&
            rows_[selected].cohort.decision == CohortDecision::older_live;
        EnableWindow(kill_, allowed ? TRUE : FALSE);
    }

    void kill_selected() {
        const int selected_index = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected_index < 0 || selected_index >= static_cast<int>(rows_.size())) return;
        const auto selected = rows_[selected_index].cohort;
        if (selected.decision != CohortDecision::older_live) return;
        std::wostringstream warning;
        warning << L"Remove this older live cohort?\n\nOwner PID: " << selected.owner.pid << L"\nProcesses: " << selected.members.size()
                << L"\n\nThis can stop another Codex task. Every identity and ancestry link will be checked again.";
        if (MessageBoxW(window_, warning.str().c_str(), L"Confirm manual cohort removal", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) return;

        const auto snapshot = read_process_snapshot();
        const auto owners = live_codex(snapshot);
        update_tracking(tracked_, snapshot, owners, false);
        const auto cohorts = build_cohorts(tracked_, snapshot, owners);
        const auto exact = find_exact_older(cohorts, selected);
        if (!exact || !owners.contains(exact->owner) || check_identity(exact->owner) != IdentityStatus::match) {
            refuse_manual(L"Selected cohort or owner changed; nothing was killed.");
            return;
        }
        for (const auto identity : exact->members) {
            const auto helper = snapshot.find(identity.pid);
            if (helper == snapshot.end() || helper->second.identity != identity || !is_managed_helper(helper->second) ||
                check_identity(identity) != IdentityStatus::match || find_codex_owner(helper->second, snapshot) != exact->owner) {
                refuse_manual(L"A cohort process changed during revalidation; nothing was killed.");
                return;
            }
        }
        if (check_identity(exact->owner) != IdentityStatus::match) {
            refuse_manual(L"Codex owner changed during revalidation; nothing was killed.");
            return;
        }

        auto members = exact->members;
        std::ranges::sort(members, [](const Identity& left, const Identity& right) { return left.started > right.started; });
        int killed = 0;
        std::uint64_t estimated_released = 0;
        int unknown_working_sets = 0;
        for (const auto identity : members) {
            if (!terminate_exact(identity)) continue;
            ++killed;
            const auto& process = snapshot.at(identity.pid);
            if (!record_cleanup(process.working_set, process.working_set_known, false))
                append_log(L"Recent cleanup history save failed; the 1-hour summary may be incomplete after restart.");
            if (process.working_set_known) estimated_released = saturating_add(estimated_released, process.working_set);
            else ++unknown_working_sets;
            append_log(L"MANUAL_CONFIRMED helper PID=" + std::to_wstring(identity.pid) + L", created=" +
                       std::to_wstring(identity.started) + L", owner PID=" + std::to_wstring(exact->owner.pid) +
                       L", pre-kill WS=" + (process.working_set_known ? megabytes(process.working_set) + L" MB" : L"unavailable") +
                       L"; process exit handle signaled.");
            std::erase_if(tracked_, [&](const TrackedProcess& item) { return item.helper == identity; });
        }
        totals_.manual_confirmed = saturating_add(totals_.manual_confirmed, static_cast<std::uint64_t>(killed));
        totals_.estimated_released = saturating_add(totals_.estimated_released, estimated_released);
        totals_.unknown_released = saturating_add(totals_.unknown_released, static_cast<std::uint64_t>(unknown_working_sets));
        save_tracking();
        if (!save_evidence_totals()) append_log(L"Evidence totals save failed after manual removal.");
        append_log(L"Manual removal confirmed " + std::to_wstring(killed) + L"/" + std::to_wstring(members.size()) +
                   L" exact process exit(s); observed " + megabytes(estimated_released) + L" MB pre-kill WS (" +
                   std::to_wstring(unknown_working_sets) + L" unavailable)." );
        scan(false);
    }

    void refuse_manual(const wchar_t* message) {
        append_log(std::wstring(L"Manual removal refused: ") + message);
        MessageBoxW(window_, message, L"Manual removal refused", MB_OK | MB_ICONINFORMATION);
        scan(false);
    }

    bool show_balloon(const std::wstring& title, const std::wstring& message) {
        if (!tray_added_) return false;
        tray_.uFlags = NIF_INFO;
        wcsncpy_s(tray_.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(tray_.szInfo, message.c_str(), _TRUNCATE);
        tray_.dwInfoFlags = NIIF_INFO;
        const bool sent = Shell_NotifyIconW(NIM_MODIFY, &tray_) != FALSE;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        return sent;
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    App app;
    return app.run(instance, command_line);
}
