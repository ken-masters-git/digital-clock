#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

struct CityInfo {
    std::wstring name;
    int offsetMinutes; // minutes offset from UTC
};

constexpr UINT_PTR kTimerId = 1;
constexpr UINT WM_APP_NTP_COMPLETE = WM_APP + 1;
constexpr wchar_t kWindowClassName[] = L"FloatingClockWindow";

enum MenuId {
    IDM_ADD_CITY = 101,
    IDM_SAVE_CITIES = 102,
    IDM_RELOAD_CITIES = 103,
    IDM_REFRESH_NTP = 106,
    IDM_SET_NTP_SERVER = 107,
    IDM_OPEN_CITY_CONFIG = 108,
    IDM_EXIT_APP = 199,
    IDM_EDIT_CITY_BASE = 1000,
    IDM_DELETE_CITY_BASE = 2000
};

static HFONT g_font = nullptr;
static std::vector<CityInfo> g_cities;
static std::wstring g_ntpServer = L"pool.ntp.org";
static const std::filesystem::path kConfigDir = std::filesystem::path(L"config");
static const std::filesystem::path kCitiesPath = kConfigDir / "cities.txt";
static const std::filesystem::path kNtpPath = kConfigDir / "ntp.txt";
constexpr int kInnerPadding = 12;
constexpr int kFrameThickness = 4;
constexpr COLORREF kFrameColor = RGB(170, 170, 170);

static ULONGLONG g_ntpFileTime = 0;   // 100-ns intervals since 1601 UTC
static ULONGLONG g_ntpTickAtFetch = 0;
static bool g_hasNtpTime = false;
static bool g_ntpInFlight = false;
static std::mutex g_ntpMutex;
static bool g_lastNtpSuccess = false;

static void DebugTrace(const std::wstring& msg) {
    OutputDebugStringW(msg.c_str());
    OutputDebugStringW(L"\r\n");
}

static void DebugTraceLastError(const std::wstring& prefix) {
    DWORD err = GetLastError();
    wchar_t buf[256];
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, 256, nullptr);
    std::wstring full = prefix + L" failed. GetLastError=" + std::to_wstring(err) + L" (" + buf + L")";
    DebugTrace(full);
}

static void EnsureConfigDir() {
    std::error_code ec;
    std::filesystem::create_directories(kConfigDir, ec);
}

static std::wstring Trim(const std::wstring& input) {
    size_t start = input.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) {
        return L"";
    }
    size_t end = input.find_last_not_of(L" \t\r\n");
    return input.substr(start, end - start + 1);
}

static std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

static void LoadDefaultCities() {
    g_cities = {
        {L"Auckland", 720},
        {L"Shanghai", 480}
    };
}

static void LoadCitiesFromFile() {
    EnsureConfigDir();
    g_cities.clear();
    std::ifstream in(kCitiesPath);
    if (!in.is_open()) {
        LoadDefaultCities();
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        size_t pipePos = line.find('|');
        if (pipePos == std::string::npos) {
            continue;
        }
        std::wstring name;
        name.assign(line.begin(), line.begin() + static_cast<long long>(pipePos));
        name = Trim(name);
        try {
            int offset = std::stoi(line.substr(pipePos + 1));
            if (!name.empty()) {
                g_cities.push_back({name, offset});
            }
        } catch (...) {
            // Skip invalid lines
        }
    }

    if (g_cities.empty()) {
        LoadDefaultCities();
    }
}

static void SaveCitiesToFile() {
    EnsureConfigDir();
    std::ofstream out(kCitiesPath, std::ios::trunc);
    for (const auto& city : g_cities) {
        std::string name = ToUtf8(city.name);
        out << name << "|" << city.offsetMinutes << "\n";
    }
}

static void LoadNtpServer() {
    EnsureConfigDir();
    std::wifstream in(kNtpPath);
    if (in.is_open()) {
        std::wstring line;
        if (std::getline(in, line)) {
            line = Trim(line);
            if (!line.empty()) {
                g_ntpServer = line;
            }
        }
    }
}

static void SaveNtpServer() {
    EnsureConfigDir();
    std::wofstream out(kNtpPath, std::ios::trunc);
    out << g_ntpServer;
}

static std::optional<ULONGLONG> QueryNtpFileTime(const std::string& server) {
    const uint32_t kNtpUnixDelta = 2208988800u;
    const uint64_t kUnixToFiletime = 11644473600ull;

    addrinfo hints = {};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    int err = getaddrinfo(server.c_str(), "123", &hints, &result);
    if (err != 0 || !result) {
        return std::nullopt;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock != INVALID_SOCKET) {
            DWORD timeoutMs = 2000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
            int sent;
            unsigned char packet[48] = {0};
            packet[0] = 0x1B; // LI=0, VN=3, Mode=3
            sent = sendto(sock, reinterpret_cast<const char*>(packet), sizeof(packet), 0, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
            if (sent == sizeof(packet)) {
                sockaddr_storage from = {};
                int fromLen = sizeof(from);
                int received = recvfrom(sock, reinterpret_cast<char*>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (received >= 48) {
                    closesocket(sock);
                    freeaddrinfo(result);
                    uint32_t seconds = (packet[40] << 24) | (packet[41] << 16) | (packet[42] << 8) | packet[43];
                    uint32_t fraction = (packet[44] << 24) | (packet[45] << 16) | (packet[46] << 8) | packet[47];
                    if (seconds <= kNtpUnixDelta) {
                        return std::nullopt;
                    }
                    uint64_t unixSeconds = seconds - kNtpUnixDelta;
                    uint64_t filetime = (unixSeconds + kUnixToFiletime) * 10000000ull;
                    filetime += (fraction * 10000000ull) >> 32;
                    return filetime;
                }
            }
            closesocket(sock);
        }
    }
    freeaddrinfo(result);
    return std::nullopt;
}

static ULONGLONG CurrentUtcFileTime() {
    std::lock_guard<std::mutex> lock(g_ntpMutex);
    if (g_hasNtpTime) {
        ULONGLONG elapsedMs = GetTickCount64() - g_ntpTickAtFetch;
        return g_ntpFileTime + (elapsedMs * 10000ull);
    }
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

static void UpdateFont(HWND hwnd) {
    if (g_font) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    int height = -MulDiv(24, dpi, 72);
    g_font = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void ResizeToContent(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    HFONT old = (HFONT)SelectObject(hdc, g_font);
    SIZE sz = {0, 0};
    SIZE lineSize = {0, 0};
    for (const auto& city : g_cities) {
        std::wostringstream oss;
        oss << city.name << L": 00:00:00";
        std::wstring sample = oss.str();
        GetTextExtentPoint32W(hdc, sample.c_str(), static_cast<int>(sample.size()), &lineSize);
        sz.cx = std::max(sz.cx, lineSize.cx);
        sz.cy = lineSize.cy;
    }
    if (sz.cx == 0) {
        GetTextExtentPoint32W(hdc, L"00:00:00", 8, &sz);
    }
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);

    int padding = kInnerPadding + kFrameThickness;
    int width = sz.cx + padding * 2;
    int height = static_cast<int>(g_cities.size()) * sz.cy + padding * 2;
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    SetWindowPos(hwnd, HWND_TOPMOST, rc.left, rc.top, width, height, SWP_NOMOVE | SWP_NOACTIVATE);
}

static void StartNtpSyncAsync(HWND hwnd, bool showResult) {
    std::lock_guard<std::mutex> lock(g_ntpMutex);
    if (g_ntpInFlight) {
        return;
    }
    g_ntpInFlight = true;
    std::wstring server = g_ntpServer;
    std::thread([hwnd, server, showResult]() {
        auto utf8Server = ToUtf8(server);
        auto result = QueryNtpFileTime(utf8Server);
        {
            std::lock_guard<std::mutex> guard(g_ntpMutex);
            if (result) {
                g_ntpFileTime = *result;
                g_ntpTickAtFetch = GetTickCount64();
                g_hasNtpTime = true;
                g_lastNtpSuccess = true;
            }
            g_lastNtpSuccess = result.has_value();
            g_ntpInFlight = false;
        }
        PostMessage(hwnd, WM_APP_NTP_COMPLETE, static_cast<WPARAM>(showResult ? 1 : 0), result ? 1 : 0);
    }).detach();
}

static int GetDstAdjustmentMinutes(const CityInfo& city, ULONGLONG utcFileTime);

static std::wstring FormatCityTime(const CityInfo& city) {
    ULONGLONG utcFileTime = CurrentUtcFileTime();
    int dstAdjustMinutes = GetDstAdjustmentMinutes(city, utcFileTime);
    LONGLONG adjusted = static_cast<LONGLONG>(utcFileTime) + static_cast<LONGLONG>(city.offsetMinutes + dstAdjustMinutes) * 60 * 10000000ll;
    FILETIME ft = {};
    ft.dwLowDateTime = static_cast<DWORD>(adjusted & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>((adjusted >> 32) & 0xFFFFFFFF);
    SYSTEMTIME st = {};
    FileTimeToSystemTime(&ft, &st);
    std::wostringstream oss;
    oss << city.name << L": "
        << std::setfill<wchar_t>(L'0') << std::setw(2) << st.wHour << L":"
        << std::setw(2) << st.wMinute << L":"
        << std::setw(2) << st.wSecond;
    return oss.str();
}

// Simple modal dialog for city editing (name + offset)
struct CityDialogState {
    CityInfo initial;
    CityInfo result;
    bool confirmed = false;
};

static void WriteWord(std::vector<WORD>& buf, WORD v) { buf.push_back(v); }
static void WriteDWord(std::vector<WORD>& buf, DWORD v) { buf.push_back(static_cast<WORD>(v & 0xFFFF)); buf.push_back(static_cast<WORD>((v >> 16) & 0xFFFF)); }
static void AlignDword(std::vector<WORD>& buf) { if (buf.size() % 2 != 0) buf.push_back(0); }
static void WriteString(std::vector<WORD>& buf, const wchar_t* s) { while (*s) { buf.push_back(static_cast<WORD>(*s++)); } buf.push_back(0); }

static constexpr WORD kNameEditId = 2001;
static constexpr WORD kOffsetEditId = 2002;
static constexpr WORD kSearchButtonId = 2003;

static int GetDstAdjustmentMinutes(const CityInfo&, ULONGLONG) {
    // DST adjustments are currently disabled for simplicity.
    return 0;
}

struct CitySuggestion {
    const wchar_t* city;
    const wchar_t* country;
    int offsetMinutes;
};

static const CitySuggestion kCitySuggestions[] = {
    {L"UTC", L"", 0},
    {L"New York", L"USA", -300},
    {L"Los Angeles", L"USA", -480},
    {L"London", L"UK", 0},
    {L"Berlin", L"Germany", 60},
    {L"Tokyo", L"Japan", 540},
    {L"Sydney", L"Australia", 600},
    {L"Auckland", L"New Zealand", 720},
    {L"Singapore", L"Singapore", 480},
    {L"Hong Kong", L"China", 480},
    {L"Shanghai", L"China", 480},
    {L"Dubai", L"UAE", 240},
    {L"Chicago", L"USA", -360},
    {L"Mexico City", L"Mexico", -360},
    {L"Mumbai", L"India", 330},
    {L"Johannesburg", L"South Africa", 120},
    {L"Paris", L"France", 60},
    {L"Toronto", L"Canada", -300},
    {L"San Francisco", L"USA", -480},
    {L"Beijing", L"China", 480},
    {L"Seoul", L"South Korea", 540}
};

static std::vector<WORD> BuildCityDialogTemplate() {
    std::vector<WORD> dlg;
    WriteDWord(dlg, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME);
    WriteDWord(dlg, 0);
    WriteWord(dlg, 7);
    WriteWord(dlg, 0); WriteWord(dlg, 0);
    WriteWord(dlg, 280); WriteWord(dlg, 170);
    WriteWord(dlg, 0);
    WriteWord(dlg, 0);
    WriteString(dlg, L"City");
    WriteWord(dlg, 9);
    WriteString(dlg, L"Segoe UI");
    AlignDword(dlg);

    auto addItem = [&](DWORD style, DWORD exStyle, short x, short y, short cx, short cy, WORD id, WORD classAtom, const wchar_t* text) {
        AlignDword(dlg);
        WriteDWord(dlg, style);
        WriteDWord(dlg, exStyle);
        WriteWord(dlg, static_cast<WORD>(x));
        WriteWord(dlg, static_cast<WORD>(y));
        WriteWord(dlg, static_cast<WORD>(cx));
        WriteWord(dlg, static_cast<WORD>(cy));
        WriteWord(dlg, id);
        WriteWord(dlg, 0xFFFF); WriteWord(dlg, classAtom);
        WriteString(dlg, text);
        WriteWord(dlg, 0);
    };

    addItem(WS_CHILD | WS_VISIBLE, 0, 8, 8, 80, 12, 1001, 0x0082, L"City name:");
    addItem(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, WS_EX_CLIENTEDGE, 8, 22, 180, 90, kNameEditId, 0x0085, L"");
    addItem(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 196, 22, 60, 14, kSearchButtonId, 0x0080, L"Search");
    addItem(WS_CHILD | WS_VISIBLE, 0, 8, 62, 220, 12, 1002, 0x0082, L"UTC offset (minutes, e.g. -300):");
    addItem(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 8, 76, 120, 14, kOffsetEditId, 0x0081, L"");
    addItem(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 60, 120, 60, 14, IDOK, 0x0080, L"OK");
    addItem(WS_CHILD | WS_VISIBLE, 0, 170, 120, 60, 14, IDCANCEL, 0x0080, L"Cancel");
    return dlg;
}

static INT_PTR CALLBACK CityDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CityDialogState* state = reinterpret_cast<CityDialogState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<CityDialogState*>(lParam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        HWND nameEdit = GetDlgItem(hwnd, kNameEditId);
        HWND offsetEdit = GetDlgItem(hwnd, kOffsetEditId);

        // Populate suggestions
        for (const auto& entry : kCitySuggestions) {
            SendMessageW(nameEdit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.city));
        }
        // Preselect if match
        LRESULT idx = SendMessageW(nameEdit, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(state->initial.name.c_str()));
        if (idx != CB_ERR) {
            SendMessageW(nameEdit, CB_SETCURSEL, static_cast<WPARAM>(idx), 0);
        } else {
            SetWindowTextW(nameEdit, state->initial.name.c_str());
        }

        if (nameEdit) {
            SendMessage(nameEdit, EM_SETSEL, 0, -1);
        }
        if (offsetEdit) {
            std::wstringstream ss;
            ss << state->initial.offsetMinutes;
            auto txt = ss.str();
            SetWindowTextW(offsetEdit, txt.c_str());
        }
        if (nameEdit) {
            SetFocus(nameEdit);
            return FALSE;
        }
        return TRUE;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD notif = HIWORD(wParam);

        auto lookupAndFill = [&](const std::wstring& name) {
            for (const auto& entry : kCitySuggestions) {
                if (_wcsicmp(entry.city, name.c_str()) == 0) {
                    HWND offsetEdit = GetDlgItem(hwnd, kOffsetEditId);
                    if (offsetEdit) {
                        std::wstringstream ss;
                        ss << entry.offsetMinutes;
                        auto txt = ss.str();
                        SetWindowTextW(offsetEdit, txt.c_str());
                    }
                    HWND nameEdit = GetDlgItem(hwnd, kNameEditId);
                    if (nameEdit) {
                        SetWindowTextW(nameEdit, entry.city);
                    }
                    return true;
                }
            }
            return false;
        };

        if (id == kSearchButtonId && notif == BN_CLICKED) {
            wchar_t buffer[256] = {};
            GetDlgItemTextW(hwnd, kNameEditId, buffer, 255);
            std::wstring name = Trim(buffer);
            if (name.empty() || !lookupAndFill(name)) {
                MessageBoxW(hwnd, L"City not found. Please try another name.", L"City Search", MB_ICONWARNING | MB_OK);
            }
            return TRUE;
        }

        if (id == kNameEditId && (notif == CBN_SELCHANGE || notif == CBN_EDITUPDATE)) {
            // When selection changes, auto-fill offset if known
            wchar_t buffer[256] = {};
            if (notif == CBN_SELCHANGE) {
                LRESULT sel = SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buffer));
                }
            } else {
                GetWindowTextW(reinterpret_cast<HWND>(lParam), buffer, 255);
            }
            std::wstring name = Trim(buffer);
            lookupAndFill(name);
        }

        if (id == IDOK) {
            if (!state) return TRUE;
            wchar_t nameBuf[256] = {};
            wchar_t offsetBuf[64] = {};
            GetDlgItemTextW(hwnd, kNameEditId, nameBuf, 255);
            GetDlgItemTextW(hwnd, kOffsetEditId, offsetBuf, 63);
            std::wstring name = Trim(nameBuf);
            if (name.empty()) {
                MessageBoxW(hwnd, L"City name cannot be empty.", L"Validation", MB_ICONWARNING | MB_OK);
                return TRUE;
            }
            wchar_t* endPtr = nullptr;
            long offset = wcstol(offsetBuf, &endPtr, 10);
            if (endPtr == offsetBuf) {
                MessageBoxW(hwnd, L"Enter a numeric UTC offset in minutes (e.g. -300 for UTC-5) or click Search to auto-fill.", L"Validation", MB_ICONWARNING | MB_OK);
                return TRUE;
            }
            state->result = {name, static_cast<int>(offset)};
            state->confirmed = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static bool ShowCityDialog(HWND parent, const CityInfo* existing, CityInfo& outCity) {
    CityDialogState state{};
    if (existing) {
        state.initial = *existing;
        state.result = *existing;
    } else {
        state.initial = {L"", 0};
        state.result = state.initial;
    }
    auto tmpl = BuildCityDialogTemplate();
    HINSTANCE hInst = GetModuleHandle(nullptr);
    SIZE_T byteSize = tmpl.size() * sizeof(WORD);
    HGLOBAL hMem = GlobalAlloc(GPTR, byteSize);
    if (!hMem) {
        MessageBoxW(parent, L"Failed to allocate dialog template.", L"City", MB_ICONERROR | MB_OK);
        return false;
    }
    WORD* dlgTemplate = static_cast<WORD*>(GlobalLock(hMem));
    memcpy(dlgTemplate, tmpl.data(), byteSize);
    GlobalUnlock(hMem);

    INT_PTR ret = DialogBoxIndirectParamW(hInst, reinterpret_cast<DLGTEMPLATE*>(dlgTemplate), parent, CityDialogProc, reinterpret_cast<LPARAM>(&state));
    GlobalFree(hMem);

    if (ret == IDOK && state.confirmed) {
        outCity = state.result;
        return true;
    }
    if (ret == -1) {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf(buf, 256, L"Failed to show dialog. Error: %lu", err);
        MessageBoxW(parent ? parent : nullptr, buf, L"City", MB_ICONERROR | MB_OK);
    } else {
        wchar_t buf[256];
        swprintf(buf, 256, L"Dialog closed without confirmation. Return: %lld", static_cast<long long>(ret));
        MessageBoxW(parent ? parent : nullptr, buf, L"City", MB_ICONINFORMATION | MB_OK);
    }
    return false;
}

// Simple modal dialog for a single text input (NTP server)
struct TextDialogState {
    std::wstring title;
    std::wstring prompt;
    std::wstring initial;
    std::wstring value;
    bool confirmed = false;
};

constexpr WORD kNtpPromptId = 4001;
constexpr WORD kNtpEditId = 4002;
constexpr WORD kNtpResetId = 4003;

static std::vector<WORD> BuildTextDialogTemplate() {
    std::vector<WORD> dlg;
    WriteDWord(dlg, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME);
    WriteDWord(dlg, 0);
    WriteWord(dlg, 5);
    WriteWord(dlg, 0); WriteWord(dlg, 0);
    WriteWord(dlg, 260); WriteWord(dlg, 130);
    WriteWord(dlg, 0);
    WriteWord(dlg, 0);
    WriteString(dlg, L"NTP Server");
    WriteWord(dlg, 9);
    WriteString(dlg, L"Segoe UI");
    AlignDword(dlg);

    auto addItem = [&](DWORD style, DWORD exStyle, short x, short y, short cx, short cy, WORD id, WORD classAtom, const wchar_t* text) {
        AlignDword(dlg);
        WriteDWord(dlg, style);
        WriteDWord(dlg, exStyle);
        WriteWord(dlg, static_cast<WORD>(x));
        WriteWord(dlg, static_cast<WORD>(y));
        WriteWord(dlg, static_cast<WORD>(cx));
        WriteWord(dlg, static_cast<WORD>(cy));
        WriteWord(dlg, id);
        WriteWord(dlg, 0xFFFF); WriteWord(dlg, classAtom);
        WriteString(dlg, text);
        WriteWord(dlg, 0);
    };

    addItem(WS_CHILD | WS_VISIBLE, 0, 10, 10, 220, 12, kNtpPromptId, 0x0082, L"Server host or IP:");
    addItem(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 10, 28, 240, 16, kNtpEditId, 0x0081, L"");
    // Buttons laid out evenly across the bottom
    addItem(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 12, 74, 70, 16, kNtpResetId, 0x0080, L"Reset");
    addItem(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 94, 74, 70, 16, IDOK, 0x0080, L"OK");
    addItem(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 176, 74, 70, 16, IDCANCEL, 0x0080, L"Cancel");
    return dlg;
}

static INT_PTR CALLBACK TextDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TextDialogState* state = reinterpret_cast<TextDialogState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<TextDialogState*>(lParam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        SetDlgItemTextW(hwnd, kNtpPromptId, state->prompt.c_str());
        SetDlgItemTextW(hwnd, kNtpEditId, state->initial.c_str());
        SetFocus(GetDlgItem(hwnd, kNtpEditId));
        return FALSE;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == kNtpResetId) { // Reset to default
            SetDlgItemTextW(hwnd, kNtpEditId, L"pool.ntp.org");
            return TRUE;
        }
        if (id == IDOK) {
            wchar_t buf[256] = {};
            GetDlgItemTextW(hwnd, kNtpEditId, buf, 255);
            std::wstring value = Trim(buf);
            if (value.empty()) {
                MessageBoxW(hwnd, L"Please enter a server host or IP.", L"NTP", MB_ICONWARNING | MB_OK);
                return TRUE;
            }
            state->value = value;
            state->confirmed = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static bool ShowTextDialog(HWND parent, const std::wstring& title, const std::wstring& prompt, const std::wstring& initial, std::wstring& outValue) {
    TextDialogState state;
    state.title = title;
    state.prompt = prompt;
    state.initial = initial;

    auto tmpl = BuildTextDialogTemplate();
    HINSTANCE hInst = GetModuleHandle(nullptr);
    SIZE_T byteSize = tmpl.size() * sizeof(WORD);
    HGLOBAL hMem = GlobalAlloc(GPTR, byteSize);
    if (!hMem) {
        MessageBoxW(parent, L"Failed to allocate dialog template.", L"NTP", MB_ICONERROR | MB_OK);
        return false;
    }
    WORD* dlgTemplate = static_cast<WORD*>(GlobalLock(hMem));
    memcpy(dlgTemplate, tmpl.data(), byteSize);
    GlobalUnlock(hMem);

    INT_PTR ret = DialogBoxIndirectParamW(hInst, reinterpret_cast<DLGTEMPLATE*>(dlgTemplate), parent, TextDialogProc, reinterpret_cast<LPARAM>(&state));
    GlobalFree(hMem);

    if (ret == IDOK && state.confirmed) {
        outValue = state.value;
        return true;
    }
    if (ret == -1) {
        DebugTraceLastError(L"[NTP dialog] DialogBoxIndirectParamW");
        MessageBoxW(parent ? parent : nullptr, L"Failed to show NTP server dialog.", L"NTP", MB_ICONERROR | MB_OK);
    }
    return false;
}

static void DrawContent(HWND hwnd, HDC hdc) {
    RECT client;
    GetClientRect(hwnd, &client);
    HBRUSH backBrush = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(hdc, &client, backBrush);
    DeleteObject(backBrush);

    RECT frameRect = client;
    HBRUSH frameBrush = CreateSolidBrush(kFrameColor);
    for (int i = 0; i < kFrameThickness; ++i) {
        FrameRect(hdc, &frameRect, frameBrush);
        InflateRect(&frameRect, -1, -1);
    }
    DeleteObject(frameBrush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 255, 128));
    HFONT oldFont = (HFONT)SelectObject(hdc, g_font);

    int padding = kInnerPadding + kFrameThickness;
    int y = padding;
    for (const auto& city : g_cities) {
        std::wstring line = FormatCityTime(city);
        TextOutW(hdc, padding, y, line.c_str(), static_cast<int>(line.size()));
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, line.c_str(), static_cast<int>(line.size()), &sz);
        y += sz.cy;
    }
    SelectObject(hdc, oldFont);
}

static void BuildContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_ADD_CITY, L"Add city...");

    HMENU editMenu = CreatePopupMenu();
    for (size_t i = 0; i < g_cities.size(); ++i) {
        AppendMenuW(editMenu, MF_STRING, IDM_EDIT_CITY_BASE + static_cast<UINT>(i), g_cities[i].name.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(editMenu), L"Edit city");

    HMENU deleteMenu = CreatePopupMenu();
    for (size_t i = 0; i < g_cities.size(); ++i) {
        AppendMenuW(deleteMenu, MF_STRING, IDM_DELETE_CITY_BASE + static_cast<UINT>(i), g_cities[i].name.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(deleteMenu), L"Delete city");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH_NTP, L"Sync time (NTP)");
    AppendMenuW(menu, MF_STRING, IDM_SET_NTP_SERVER, L"Set NTP server...");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SAVE_CITIES, L"Save cities to config");
    AppendMenuW(menu, MF_STRING, IDM_RELOAD_CITIES, L"Reload cities from config");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CITY_CONFIG, L"Open city config in Notepad");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT_APP, L"Exit");

    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        UpdateFont(hwnd);
        SetTimer(hwnd, kTimerId, 1000, nullptr);
        SetLayeredWindowAttributes(hwnd, 0, 230, LWA_ALPHA);
        StartNtpSyncAsync(hwnd, false);
        ResizeToContent(hwnd);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    case WM_RBUTTONUP:
        BuildContextMenu(hwnd);
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        if (id == IDM_ADD_CITY) {
            CityInfo newCity{L"", 0};
            if (ShowCityDialog(hwnd, nullptr, newCity)) {
                g_cities.push_back(newCity);
                ResizeToContent(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        if (id >= IDM_EDIT_CITY_BASE && id < IDM_EDIT_CITY_BASE + 1000) {
            size_t idx = id - IDM_EDIT_CITY_BASE;
            if (idx < g_cities.size()) {
                CityInfo updated = g_cities[idx];
                if (ShowCityDialog(hwnd, &g_cities[idx], updated)) {
                    g_cities[idx] = updated;
                    ResizeToContent(hwnd);
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        }
        if (id >= IDM_DELETE_CITY_BASE && id < IDM_DELETE_CITY_BASE + 1000) {
            size_t idx = id - IDM_DELETE_CITY_BASE;
            if (idx < g_cities.size()) {
                g_cities.erase(g_cities.begin() + static_cast<long long>(idx));
                ResizeToContent(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        switch (id) {
        case IDM_SAVE_CITIES:
            SaveCitiesToFile();
            return 0;
        case IDM_RELOAD_CITIES:
            LoadCitiesFromFile();
            ResizeToContent(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case IDM_OPEN_CITY_CONFIG:
            SaveCitiesToFile(); // ensure file exists
            ShellExecuteW(hwnd, L"open", L"notepad.exe", kCitiesPath.wstring().c_str(), nullptr, SW_SHOWNORMAL);
            return 0;
        case IDM_REFRESH_NTP:
            StartNtpSyncAsync(hwnd, true);
            return 0;
        case IDM_SET_NTP_SERVER: {
            DebugTrace(L"[NTP dialog] menu clicked");
            std::wstring newServer;
            if (ShowTextDialog(hwnd, L"NTP Server", L"Server host or IP:", g_ntpServer, newServer)) {
                g_ntpServer = newServer;
                SaveNtpServer();
                DebugTrace(L"[NTP dialog] new server saved, triggering sync");
    StartNtpSyncAsync(hwnd, false);
            } else {
                DebugTrace(L"[NTP dialog] canceled or failed");
            }
            return 0;
        }
        case IDM_EXIT_APP:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return 0;
    }
    case WM_APP_NTP_COMPLETE:
        if (wParam) { // showResult flag
            if (g_lastNtpSuccess) {
                MessageBoxW(hwnd, L"NTP sync succeeded.", L"NTP", MB_ICONINFORMATION | MB_OK);
            } else {
                MessageBoxW(hwnd, L"NTP sync failed. Using local system time.", L"NTP", MB_ICONWARNING | MB_OK);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawContent(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    LoadCitiesFromFile();
    LoadNtpServer();

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                kWindowClassName, L"Floating Clock",
                                WS_POPUP,
                                CW_USEDEFAULT, CW_USEDEFAULT, 360, 220,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        return 0;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, 100, 100, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_font) {
        DeleteObject(g_font);
    }
    WSACleanup();
    return 0;
}
