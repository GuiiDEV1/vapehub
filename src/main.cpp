// VapeHub — one launcher for RikeoVape (OpenVape 4.21) and Slinky (Minecraft 1.8 javaw.exe)
// build: llvm-mingw clang++ -std=c++17 -O2 -mwindows -static, see build.bat
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace fs = std::filesystem;

// ---------- palette ----------
static const COLORREF C_BG          = RGB(9, 10, 13);
static const COLORREF C_PANEL       = RGB(20, 22, 28);
static const COLORREF C_PANEL_HOVER = RGB(25, 28, 36);
static const COLORREF C_BORDER      = RGB(40, 44, 54);
static const COLORREF C_BORDER_HOT  = RGB(62, 68, 84);
static const COLORREF C_TEXT        = RGB(232, 234, 240);
static const COLORREF C_DIM         = RGB(138, 144, 160);
static const COLORREF C_DIM2        = RGB(100, 106, 120);
static const COLORREF C_ACCENT      = RGB(155, 123, 255);
static const COLORREF C_ACCENT_HOT  = RGB(178, 154, 255);
static const COLORREF C_MINT        = RGB(76, 224, 179);
static const COLORREF C_MINT_HOT    = RGB(112, 236, 200);
static const COLORREF C_BTN_TEXT    = RGB(10, 11, 14);
static const COLORREF C_STATUS      = RGB(12, 14, 18);
static const COLORREF C_OK          = RGB(74, 222, 128);
static const COLORREF C_WARN        = RGB(251, 191, 36);
static const COLORREF C_ERR         = RGB(248, 113, 113);

static HINSTANCE g_inst{};
static HWND      g_hwnd{};
static int       g_dpi = 96;
static bool      g_closing = false;
static std::atomic<bool> g_busy{false};

static fs::path g_exe_dir, g_openvape, g_slinky, g_map_file;

static std::mutex g_mu;
static std::wstring g_status = L"Ready.";
static bool  g_svc_up = false;
static DWORD g_svc_pid = 0;
static int   g_svc_http = 0, g_svc_zeus = 0;

static HFONT f_h1{}, f_card{}, f_tag{}, f_body{}, f_btn{}, f_small{};

static int g_hover = 0;

struct ui_layout {
    RECT card1{}, btn1{}, link1{}, card2{}, btn2{}, link2{}, docs{}, status{}, svc{};
};
static ui_layout g_lay;

enum hit_id { H_NONE = 0, H_BTN1, H_LINK1, H_BTN2, H_LINK2, H_SVC, H_DOCS };

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------- logging ----------
static void log_line(const std::wstring& line) {
    std::wstring l = line + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, u8.data(), n, nullptr, nullptr);

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(out, u8.data(), (DWORD)u8.size(), &wr, nullptr);
    }
    if (!g_exe_dir.empty()) {
        HANDLE f = CreateFileW((g_exe_dir / L"vapehub.log").c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(f, u8.data(), (DWORD)u8.size(), &wr, nullptr);
            CloseHandle(f);
        }
    }
}

static void logf_(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 2048, fmt, ap);
    va_end(ap);
    log_line(buf);
}

static void set_status(const std::wstring& s) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = s;
    }
    log_line(s);
    if (g_hwnd && !g_closing) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static std::wstring win_err(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"unknown error";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    return s + L" (" + std::to_wstring(e) + L")";
}

// ---------- helpers ----------
static bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

static fs::path pick_dir(std::vector<fs::path> cands, std::vector<const wchar_t*> required) {
    for (auto& c : cands) {
        bool ok = !c.empty();
        for (auto r : required) if (!ok || !file_exists(c / r)) { ok = false; break; }
        if (ok) return c;
    }
    return {};
}

static fs::path env_dir(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::path();
}

static std::wstring lower_w(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static bool ends_with_i(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return lower_w(s.substr(s.size() - suffix.size())) == lower_w(suffix);
}

static bool is_alive(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return alive;
}

static bool is_java_pid(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    bool ok = QueryFullProcessImageNameW(p, 0, buf, &n) != 0;
    CloseHandle(p);
    return ok && ends_with_i(buf, L"java.exe");
}

static int free_port() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = 0;
    int port = 0;
    if (bind(s, (sockaddr*)&a, sizeof(a)) == 0) {
        int len = sizeof(a);
        if (getsockname(s, (sockaddr*)&a, &len) == 0) port = ntohs(a.sin_port);
    }
    closesocket(s);
    return port;
}

static bool port_open(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons((u_short)port);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr*)&a, sizeof(a));
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv{0, 250000};
    bool up = false;
    if (select(0, nullptr, &wf, nullptr, &tv) > 0) {
        int err = 0, len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        up = (err == 0);
    }
    closesocket(s);
    return up;
}

static fs::path state_path() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    fs::path d = (n > 0 && n < MAX_PATH) ? fs::path(buf) / L"VapeHub" : g_exe_dir;
    std::error_code ec;
    fs::create_directories(d, ec);
    return d / L"service.txt";
}

static bool read_state(DWORD& pid, int& http, int& zeus) {
    std::ifstream in(state_path().string());
    unsigned long p = 0;
    int h = 0, z = 0;
    if (!(in >> p >> h >> z)) return false;
    pid = (DWORD)p;
    http = h;
    zeus = z;
    return pid && http && zeus;
}

static void write_state(DWORD pid, int http, int zeus) {
    std::ofstream out(state_path().string(), std::ios::trunc);
    out << pid << " " << http << " " << zeus << "\n";
}

static void clear_state() {
    std::error_code ec;
    fs::remove(state_path(), ec);
}

static std::wstring find_java() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"JAVA_HOME", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        fs::path p = fs::path(buf) / L"bin" / L"java.exe";
        if (file_exists(p)) return p.wstring();
    }
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"java.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

static std::wstring read_log_tail(const fs::path& p, size_t max_bytes) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec || sz == 0) return {};
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    size_t take = (size_t)sz > max_bytes ? max_bytes : (size_t)sz;
    in.seekg((std::streamoff)((size_t)sz - take));
    std::string data(take, '\0');
    in.read(data.data(), (std::streamsize)take);
    data = data.substr(0, (size_t)in.gcount());
    size_t pos = data.find_last_not_of("\r\n\t ");
    if (pos == std::string::npos) return {};
    size_t start = data.rfind('\n', pos);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::string line = data.substr(start, pos - start + 1);
    if (line.size() > 240) line = line.substr(line.size() - 240);
    int wl = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0);
    std::wstring w((size_t)(wl > 0 ? wl : 0), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), w.data(), wl);
    return w;
}

static void set_service(bool up, DWORD pid, int http, int zeus) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_svc_up = up;
    g_svc_pid = pid;
    g_svc_http = http;
    g_svc_zeus = zeus;
}

// ---------- service control ----------
static bool reuse_service() {
    DWORD pid = 0;
    int http = 0, zeus = 0;
    if (!read_state(pid, http, zeus)) return false;
    if (!is_alive(pid) || !is_java_pid(pid)) return false;
    if (!port_open(http) || !port_open(zeus)) return false;
    set_service(true, pid, http, zeus);
    return true;
}

static bool spawn_service(int http, int zeus, PROCESS_INFORMATION& pi) {
    std::wstring java = find_java();
    if (java.empty()) {
        set_status(L"java.exe not found — install a JDK or set JAVA_HOME.");
        return false;
    }
    fs::path data = g_openvape / L"data";
    std::error_code ec;
    fs::create_directories(data, ec);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW((data / L"service.log").c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        set_status(L"Cannot open service log in " + data.wstring());
        return false;
    }
    SetFilePointer(log, 0, nullptr, FILE_END);

    std::wstring cmd = L"\"" + java + L"\" -jar \"" + (g_openvape / L"vape421-experimental-service-0.1.0.jar").wstring() +
                       L"\" --bind-address 127.0.0.1 --http-port " + std::to_wstring(http) +
                       L" --zeus-port " + std::to_wstring(zeus) + L" --data-file \"" +
                       (data / L"vape-service.json").wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = INVALID_HANDLE_VALUE;

    BOOL ok = CreateProcessW(java.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, g_openvape.c_str(), &si, &pi);
    CloseHandle(log);
    if (!ok) {
        set_status(L"Cannot start service: " + win_err(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    return true;
}

static bool ensure_service() {
    if (reuse_service()) {
        int port = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            port = g_svc_http;
        }
        set_status(L"Service already running on 127.0.0.1:" + std::to_wstring(port) + L".");
        return true;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        int http = 0, zeus = 0;
        if (!port_open(8080) && !port_open(8091)) {
            http = 8080;
            zeus = 8091;
        } else {
            do {
                http = free_port();
                zeus = free_port();
            } while (http == 0 || zeus == 0 || http == zeus);
        }

        set_status(L"Starting local service on 127.0.0.1:" + std::to_wstring(http) + L"\u2026");
        PROCESS_INFORMATION pi{};
        if (!spawn_service(http, zeus, pi)) return false;

        bool up = false;
        for (int i = 0; i < 140; ++i) {
            if (WaitForSingleObject(pi.hProcess, 0) != WAIT_TIMEOUT) break;
            if (port_open(http) && port_open(zeus)) { up = true; break; }
            Sleep(120);
        }

        if (up) {
            write_state(pi.dwProcessId, http, zeus);
            set_service(true, pi.dwProcessId, http, zeus);
            CloseHandle(pi.hProcess);
            set_status(L"Service ready on 127.0.0.1:" + std::to_wstring(http) + L".");
            return true;
        }

        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        clear_state();

        std::wstring tail = read_log_tail(g_openvape / L"data" / L"service.log", 4096);
        if (!tail.empty()) set_status(L"Service failed: " + tail);
        else set_status(L"Service did not answer on its ports. See openvape\\data\\service.log.");
    }
    return false;
}

static void stop_service(bool announce) {
    DWORD pid = 0;
    int http = 0, zeus = 0;
    if (!read_state(pid, http, zeus)) {
        if (announce) set_status(L"No tracked service instance.");
        set_service(false, 0, 0, 0);
        return;
    }
    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    bool killed = false;
    if (p) {
        if (is_java_pid(pid)) killed = TerminateProcess(p, 0) != 0;
        CloseHandle(p);
    }
    clear_state();
    set_service(false, 0, 0, 0);
    if (announce) set_status(killed ? L"Local service stopped." : L"Service was not running; state cleared.");
}

// ---------- payload launch ----------
static bool launch_loader() {
    fs::path exe = g_openvape / L"vape_v4_rewrite.exe";
    if (!file_exists(exe)) {
        set_status(L"Missing vape_v4_rewrite.exe in " + g_openvape.wstring());
        return false;
    }

    int http = 0, zeus = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        http = g_svc_http;
        zeus = g_svc_zeus;
    }
    SetEnvironmentVariableW(L"VAPE_ONLINE_BASE_URL", (L"http://127.0.0.1:" + std::to_wstring(http)).c_str());
    SetEnvironmentVariableW(L"VAPE_ZEUS_ADDRESS", (L"127.0.0.1:" + std::to_wstring(zeus)).c_str());

    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             g_openvape.c_str(), &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        set_status(L"RikeoVape loader opened — log in, pick javaw.exe, load.");
        return true;
    }

    DWORD e = GetLastError();
    if (e == ERROR_ELEVATION_REQUIRED || e == ERROR_ACCESS_DENIED) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpDirectory = g_openvape.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) CloseHandle(sei.hProcess);
            set_status(L"RikeoVape loader opened with admin rights.");
            return true;
        }
        DWORD se = GetLastError();
        set_status(se == ERROR_CANCELLED ? L"Elevation cancelled — loader not started."
                                         : L"Cannot start loader: " + win_err(se));
        return false;
    }

    set_status(L"Cannot start loader: " + win_err(e));
    return false;
}

static bool launch_injector() {
    fs::path exe = g_openvape / L"Vape421Injector.exe";
    if (!file_exists(exe)) {
        set_status(L"Missing Vape421Injector.exe in " + g_openvape.wstring());
        return false;
    }
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                             nullptr, g_openvape.c_str(), &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        set_status(L"Direct launcher opened — select the game window in its console.");
        return true;
    }
    set_status(L"Cannot start direct launcher: " + win_err(GetLastError()));
    return false;
}

static bool launch_slinky(bool admin) {
    fs::path exe = g_slinky / L"slinkyloader.exe";
    if (!file_exists(exe)) {
        set_status(L"Slinky payload missing (slinky\\slinkyloader.exe).");
        return false;
    }
    if (admin) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpDirectory = g_slinky.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) CloseHandle(sei.hProcess);
            set_status(L"Slinky started elevated — waiting for javaw.exe\u2026");
            return true;
        }
        set_status(GetLastError() == ERROR_CANCELLED ? L"Elevation cancelled — Slinky not started."
                                                     : L"Cannot start Slinky: " + win_err(GetLastError()));
        return false;
    }
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                             nullptr, g_slinky.c_str(), &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        set_status(L"Slinky started — waiting for javaw.exe\u2026");
        return true;
    }
    set_status(L"Cannot start Slinky: " + win_err(GetLastError()));
    return false;
}

static void start_openvape_flow() {
    if (g_openvape.empty()) {
        set_status(L"RikeoVape payload not found next to VapeHub.exe.");
        return;
    }
    if (g_busy.exchange(true)) {
        set_status(L"RikeoVape is already starting\u2026");
        return;
    }
    std::thread([] {
        bool ok = ensure_service();
        if (ok) launch_loader();
        g_busy = false;
        if (g_hwnd && !g_closing) InvalidateRect(g_hwnd, nullptr, FALSE);
    }).detach();
}

static void open_map() {
    if (file_exists(g_map_file)) {
        ShellExecuteW(nullptr, L"open", g_map_file.c_str(), nullptr, g_exe_dir.c_str(), SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", g_exe_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---------- CLI ----------
static int run_cli(bool selftest, bool stop, const std::wstring& launch, bool no_loader) {
    if (selftest) {
        auto mark = [](bool ok) { return ok ? L"OK" : L"MISSING"; };
        logf_(L"VapeHub selftest");
        logf_(L"  exe dir : %ls", g_exe_dir.wstring().c_str());
        logf_(L"  openvape: %ls  [%ls]", g_openvape.wstring().c_str(), mark(!g_openvape.empty()));
        logf_(L"  slinky  : %ls  [%ls]", g_slinky.wstring().c_str(), mark(!g_slinky.empty()));
        for (auto& f : {L"vape_v4_rewrite.exe", L"Vape421Native.dll", L"vape421-experimental-service-0.1.0.jar", L"Vape421Injector.exe"})
            logf_(L"    %-45ls %ls", f, mark(!g_openvape.empty() && file_exists(g_openvape / f)));
        for (auto& f : {L"slinkyloader.exe", L"slinkyhook.dll", L"slinky_library.dll"})
            logf_(L"    %-45ls %ls", f, mark(!g_slinky.empty() && file_exists(g_slinky / f)));
        std::wstring java = find_java();
        logf_(L"  java    : %ls", java.empty() ? L"NOT FOUND" : java.c_str());
        logf_(L"  ports   : 8080 %ls, 8091 %ls", port_open(8080) ? L"in use" : L"free", port_open(8091) ? L"in use" : L"free");
        DWORD pid = 0; int h = 0, z = 0;
        if (read_state(pid, h, z) && is_alive(pid))
            logf_(L"  service : running pid=%lu http=%d zeus=%d", pid, h, z);
        else
            logf_(L"  service : offline");
        bool ok = !g_openvape.empty() && !g_slinky.empty() && !java.empty();
        logf_(L"  result  : %ls", ok ? L"OK" : L"FAIL");
        return ok ? 0 : 1;
    }

    if (stop) {
        stop_service(true);
        return 0;
    }

    if (launch.empty()) {
        logf_(L"usage: VapeHub.exe [--selftest] [--stop-service] [--launch openvape|direct|slinky|slinky-admin] [--no-loader]");
        return 2;
    }

    if (launch == L"openvape") {
        if (g_openvape.empty()) { logf_(L"openvape payload missing"); return 1; }
        if (!ensure_service()) return 1;
        if (no_loader) { logf_(L"service only; loader skipped (--no-loader)"); return 0; }
        return launch_loader() ? 0 : 1;
    }
    if (launch == L"direct") return launch_injector() ? 0 : 1;
    if (launch == L"slinky") return launch_slinky(false) ? 0 : 1;
    if (launch == L"slinky-admin") return launch_slinky(true) ? 0 : 1;
    logf_(L"unknown launch target: %ls", launch.c_str());
    return 2;
}

// ---------- drawing ----------
static void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void round_fill(HDC dc, RECT r, int rad, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

static void round_stroke(HDC dc, RECT r, int rad, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

static void dot(HDC dc, int cx, int cy, int radius, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

static void text(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    COLORREF oc = SetTextColor(dc, c);
    int ob = SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt);
    SetBkMode(dc, ob);
    SetTextColor(dc, oc);
    SelectObject(dc, of);
}

static int text_w(HDC dc, HFONT f, const wchar_t* s) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

static void paint_card(HDC dc, const RECT& c, const RECT& btn, const RECT& link,
                       int id_btn, int id_link, COLORREF accent, COLORREF accent_hot,
                       const wchar_t* tag, const wchar_t* name, const wchar_t* desc,
                       const wchar_t* key, const wchar_t* btn_label, const wchar_t* link_label) {
    bool hot = (g_hover == id_btn || g_hover == id_link);
    round_fill(dc, c, S(16), hot ? C_PANEL_HOVER : C_PANEL);
    round_stroke(dc, c, S(16), hot ? C_BORDER_HOT : C_BORDER);

    int l = c.left + S(24), r = c.right - S(24);
    dot(dc, l + S(3), c.top + S(30), S(3), accent);
    RECT tr{l + S(14), c.top + S(21), r - S(38), c.top + S(41)};
    text(dc, tag, tr, f_tag, accent, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT kc{r - S(32), c.top + S(18), r, c.top + S(46)};
    round_stroke(dc, kc, S(7), C_BORDER);
    text(dc, key, kc, f_small, C_DIM2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    RECT nr{l, c.top + S(46), r, c.top + S(82)};
    text(dc, name, nr, f_card, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT dr{l, c.top + S(86), r, c.top + S(146)};
    text(dc, desc, dr, f_body, C_DIM, DT_LEFT | DT_WORDBREAK);

    round_fill(dc, btn, S(11), g_hover == id_btn ? accent_hot : accent);
    text(dc, btn_label, btn, f_btn, C_BTN_TEXT, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    text(dc, link_label, link, f_small, g_hover == id_link ? accent : C_DIM2,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

static void compute_layout() {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    int W = rc.right;
    int pad = S(36), gap = S(24);
    int cw = (W - 2 * pad - gap) / 2;
    int top = S(140), bottom = S(440);

    g_lay.card1 = {pad, top, pad + cw, bottom};
    g_lay.card2 = {pad + cw + gap, top, W - pad, bottom};
    auto mk = [](RECT c, RECT& b, RECT& lk) {
        b = {c.left + S(22), c.bottom - S(96), c.right - S(22), c.bottom - S(50)};
        lk = {b.left, c.bottom - S(42), b.right, c.bottom - S(16)};
    };
    mk(g_lay.card1, g_lay.btn1, g_lay.link1);
    mk(g_lay.card2, g_lay.btn2, g_lay.link2);
    g_lay.docs = {W - pad - S(44), S(24), W - pad, S(44)};
    g_lay.status = {0, S(488), W, rc.bottom};
}

static void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldbmp = SelectObject(mem, bmp);

    int W = rc.right;
    int pad = S(36);

    fill(mem, rc, C_BG);

    dot(mem, pad + S(3), S(32), S(3), C_ACCENT);
    text(mem, L"VAPEHUB", {pad + S(13), S(23), W - pad, S(43)}, f_tag, C_ACCENT,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    int tw = text_w(mem, f_tag, L"VAPEHUB");
    text(mem, L"MINECRAFT 1.8  \u00b7  LOCAL LAUNCHER", {pad + S(13) + tw + S(12), S(24), W - pad, S(43)},
         f_small, C_DIM2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    text(mem, L"Pick your client", {pad, S(44), W - pad, S(88)}, f_h1, C_TEXT,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    text(mem, L"Start Minecraft first. Both loaders attach to a running javaw.exe.",
         {pad, S(90), W - pad, S(114)}, f_body, C_DIM, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    text(mem, L"MAP", g_lay.docs, f_small, g_hover == H_DOCS ? C_ACCENT : C_DIM2,
         DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    paint_card(mem, g_lay.card1, g_lay.btn1, g_lay.link1, H_BTN1, H_LINK1, C_ACCENT, C_ACCENT_HOT,
               L"RIKEOVAPE 4.21", L"RikeoVape",
               L"Starts its local service, then the GUI loader.\nLog in, pick javaw.exe, load.",
               L"1", L"Launch RikeoVape", L"Direct launch (no service)");

    paint_card(mem, g_lay.card2, g_lay.btn2, g_lay.link2, H_BTN2, H_LINK2, C_MINT, C_MINT_HOT,
               L"SLINKY", L"Slinky",
               L"Console loader. Waits for javaw.exe.\nLoads slinkyhook into the game.",
               L"2", L"Launch Slinky", L"Run as administrator");

    RECT st = g_lay.status;
    fill(mem, st, C_STATUS);
    fill(mem, {0, st.top, W, st.top + S(1)}, C_BORDER);

    bool up = false;
    int http = 0;
    std::wstring status;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        up = g_svc_up;
        http = g_svc_http;
        status = g_status;
    }
    COLORREF dc_dot = g_busy ? C_WARN : (up ? C_OK : C_DIM2);
    dot(mem, pad + S(4), (st.top + st.bottom) / 2, S(4), dc_dot);

    std::wstring svc = up ? (L"Service 127.0.0.1:" + std::to_wstring(http) + L"  \u00b7  stop")
                          : L"Service offline";
    int sw = text_w(mem, f_small, svc.c_str());
    int sx = W - pad - sw;
    g_lay.svc = {sx - S(6), st.top + S(6), W - pad, st.bottom - S(6)};
    text(mem, svc.c_str(), {sx, st.top + S(8), W - pad, st.bottom - S(8)}, f_small,
         g_hover == H_SVC && up ? C_ERR : (up ? C_DIM : C_DIM2),
         DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    RECT sr{pad + S(16), st.top + S(8), sx - S(20), st.bottom - S(8)};
    text(mem, status.c_str(), sr, f_small, C_DIM, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------- window ----------
static void make_fonts() {
    auto mk = [](int px, int weight) {
        return CreateFontW(-MulDiv(px, g_dpi, 96), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    if (f_h1) DeleteObject(f_h1);
    if (f_card) DeleteObject(f_card);
    if (f_tag) DeleteObject(f_tag);
    if (f_body) DeleteObject(f_body);
    if (f_btn) DeleteObject(f_btn);
    if (f_small) DeleteObject(f_small);
    f_h1 = mk(28, 600);
    f_card = mk(23, 600);
    f_tag = mk(11, 700);
    f_body = mk(13, 400);
    f_btn = mk(15, 600);
    f_small = mk(12, 400);
}

static int hit_test(int x, int y) {
    POINT p{x, y};
    if (g_svc_up && PtInRect(&g_lay.svc, p)) return H_SVC;
    if (PtInRect(&g_lay.docs, p)) return H_DOCS;
    if (PtInRect(&g_lay.btn1, p)) return H_BTN1;
    if (PtInRect(&g_lay.link1, p)) return H_LINK1;
    if (PtInRect(&g_lay.btn2, p)) return H_BTN2;
    if (PtInRect(&g_lay.link2, p)) return H_LINK2;
    if (PtInRect(&g_lay.card1, p)) return H_BTN1;
    if (PtInRect(&g_lay.card2, p)) return H_BTN2;
    return H_NONE;
}

static bool is_clickable(int h) { return h != H_NONE; }

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        make_fonts();
        compute_layout();
        if (g_openvape.empty()) set_status(L"RikeoVape payload not found next to VapeHub.exe.");
        else if (g_slinky.empty()) set_status(L"Slinky payload missing (slinky folder).");
        else set_status(L"Ready. Start Minecraft 1.8, then launch a client.");
        std::thread([] {
            if (reuse_service()) {
                std::lock_guard<std::mutex> lk(g_mu);
                g_status = L"Local service already running on 127.0.0.1:" + std::to_wstring(g_svc_http) + L".";
            }
            if (g_hwnd && !g_closing) InvalidateRect(g_hwnd, nullptr, FALSE);
        }).detach();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        int h = hit_test(x, y);
        if (h != g_hover) {
            g_hover = h;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        SetCursor(LoadCursorW(nullptr, is_clickable(hit_test(p.x, p.y)) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_LBUTTONUP: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        int h = hit_test(x, y);
        switch (h) {
        case H_BTN1: start_openvape_flow(); break;
        case H_LINK1: launch_injector(); break;
        case H_BTN2: launch_slinky(false); break;
        case H_LINK2: launch_slinky(true); break;
        case H_SVC: stop_service(true); break;
        case H_DOCS: open_map(); break;
        default: break;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        switch (wp) {
        case '1': case VK_NUMPAD1: start_openvape_flow(); return 0;
        case '2': case VK_NUMPAD2: launch_slinky(false); return 0;
        case '3': case VK_NUMPAD3: launch_injector(); return 0;
        case VK_ESCAPE: DestroyWindow(hwnd); return 0;
        }
        break;
    }
    case WM_DPICHANGED: {
        g_dpi = (int)HIWORD(wp);
        make_fonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        compute_layout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        g_closing = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_inst = hInst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t exe_buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    g_exe_dir = fs::path(exe_buf).parent_path();
    g_map_file = g_exe_dir / L"MAP.md";

    g_openvape = pick_dir({env_dir(L"VAPEHUB_OPENVAPE_DIR"), g_exe_dir, g_exe_dir / L"openvape"},
                          {L"vape_v4_rewrite.exe", L"Vape421Native.dll"});
    g_slinky = pick_dir({env_dir(L"VAPEHUB_SLINKY_DIR"), g_exe_dir / L"slinky", g_exe_dir / L"payloads" / L"slinky"},
                        {L"slinkyloader.exe", L"slinkyhook.dll", L"slinky_library.dll"});

    UINT dpi = 96;
    {
        auto fn = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
        if (fn) dpi = fn();
    }
    g_dpi = (int)dpi;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool selftest = false, stop = false, no_loader = false;
    std::wstring launch;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        std::wstring al = lower_w(a);
        if (al == L"--selftest") selftest = true;
        else if (al == L"--stop-service") stop = true;
        else if (al == L"--no-loader") no_loader = true;
        else if (al == L"--launch" && i + 1 < argc) launch = lower_w(argv[++i]);
        else if (al == L"--help" || al == L"/?") {
            logf_(L"usage: VapeHub.exe [--selftest] [--stop-service] [--launch openvape|direct|slinky|slinky-admin] [--no-loader]");
            LocalFree(argv);
            return 0;
        }
    }
    if (argv) LocalFree(argv);

    if (selftest || stop || !launch.empty()) {
        int rc = run_cli(selftest, stop, launch, no_loader);
        WSACleanup();
        CoUninitialize();
        return rc;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VapeHubWnd";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{0, 0, S(980), S(540)};
    auto awr = (BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT))GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (awr) awr(&wr, style, FALSE, 0, dpi);
    else AdjustWindowRectEx(&wr, style, FALSE, 0);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - ww) / 2;
    int y = work.top + ((work.bottom - work.top) - wh) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VapeHub", style, x, y, ww, wh,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        auto set_attr = (HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD))GetProcAddress(dwm, "DwmSetWindowAttribute");
        BOOL dark = TRUE;
        if (set_attr) set_attr(hwnd, 20, &dark, sizeof(dark));
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    CoUninitialize();
    return 0;
}
