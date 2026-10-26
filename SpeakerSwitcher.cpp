// SpeakerSwitcher.cpp - choose a playback device's channels / sample rate / bit depth,
// save them to config.ini, apply them, reset to the previous format, and optionally
// start with Windows in silent mode.
//
// Build (x64 Native Tools Command Prompt for VS):
//   cl /EHsc /std:c++17 /O2 /MT SpeakerSwitcher.cpp
//
// Command line:
//   SpeakerSwitcher.exe            -> opens the window
//   SpeakerSwitcher.exe -silent    -> applies the saved config.ini settings, no window
//     silent exit codes: 0 ok, 2 driver rejected format, 3 device not found,
//                        4 IPolicyConfig unavailable, 5 no saved settings
//
// config.ini is stored next to the exe (the folder must be writable).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <string>
#include <vector>
#include <cwctype>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- Undocumented IPolicyConfig (Windows 10/11 layout) ---------------------
static const GUID IID_IPolicyConfig =
    {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
static const GUID CLSID_PolicyConfigClient =
    {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

struct DeviceShareMode;

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY*, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY*, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};

// PKEY_AudioEndpoint_PhysicalSpeakers - what the "Configure" wizard writes
static const PROPERTYKEY PKEY_PhysicalSpeakers =
    {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 3};

// ---- Globals ----------------------------------------------------------------
static const wchar_t* kRunKey  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"SpeakerFormatSwitcher";

enum { ID_DEV = 101, ID_CH, ID_RATE, ID_BITS, ID_ALL, ID_SAVE, ID_APPLY, ID_RESET, ID_STARTUP };

struct Dev { std::wstring id, name; };
struct Fmt { int ch; DWORD rate; int bits; };
struct CurFmt { int ch = 0; DWORD rate = 0; int container = 0, valid = 0; DWORD mask = 0, speakers = 0; };

static std::vector<Dev> g_devs;
static std::vector<Fmt> g_fmts;
static std::wstring g_ini;
static HWND hDev, hCh, hRate, hBits, hAll, hStatus, hStartup;

// ---- Audio helpers ----------------------------------------------------------
static DWORD MaskFor(int ch) {
    switch (ch) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;        // 0x3F
    case 8: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                   SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;                               // 0x63F
    default: return 0;
    }
}

static WAVEFORMATEXTENSIBLE MakeFmt(int ch, int container, int valid, DWORD rate, DWORD mask) {
    WAVEFORMATEXTENSIBLE f = {};
    f.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels       = (WORD)ch;
    f.Format.nSamplesPerSec  = rate;
    f.Format.wBitsPerSample  = (WORD)container;
    f.Format.nBlockAlign     = (WORD)(ch * (container / 8));
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = (WORD)valid;
    f.dwChannelMask          = mask;
    f.SubFormat              = KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

static std::wstring FriendlyName(IMMDevice* dev) {
    IPropertyStore* ps = nullptr;
    std::wstring name;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            name = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return name;
}

static void EnumDevices() {
    g_devs.clear();
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
        return;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                Dev dv;
                dv.id = id;
                dv.name = FriendlyName(d);
                g_devs.push_back(dv);
                CoTaskMemFree(id);
            }
            d->Release();
        }
        col->Release();
    }
    en->Release();
}

// Ask the driver (exclusive-mode query, the same way the Sound panel's Advanced tab does)
// which combinations it supports. all=true skips the query and lists every option.
static void Detect(const std::wstring& id, bool all) {
    g_fmts.clear();
    const int   chs[]    = {2, 4, 6, 8};
    const DWORD rates[]  = {44100, 48000, 88200, 96000, 176400, 192000};
    const int   depths[] = {16, 24, 32};

    IAudioClient* ac = nullptr;
    if (!all) {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDevice(id.c_str(), &dev)))
            dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        if (dev) dev->Release();
        if (en) en->Release();
    }

    auto test = [&](int ch, int container, int valid, DWORD rate) -> bool {
        if (!ac) return true;
        WAVEFORMATEXTENSIBLE f = MakeFmt(ch, container, valid, rate, MaskFor(ch));
        return ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) == S_OK;
    };

    for (int ch : chs)
        for (DWORD rate : rates)
            for (int bits : depths) {
                bool ok;
                if (bits == 16)      ok = test(ch, 16, 16, rate);
                else if (bits == 24) ok = test(ch, 24, 24, rate) || test(ch, 32, 24, rate);
                else                 ok = test(ch, 32, 32, rate);
                if (ok) g_fmts.push_back({ch, rate, bits});
            }
    if (ac) ac->Release();
}

static IPolicyConfig* NewPC() {
    IPolicyConfig* pc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void**)&pc)))
        return nullptr;
    return pc;
}

static bool GetCur(IPolicyConfig* pc, const std::wstring& id, CurFmt& c) {
    WAVEFORMATEX* wf = nullptr;
    HRESULT hr = pc->GetDeviceFormat(id.c_str(), FALSE, &wf);
    if (FAILED(hr) || !wf) return false;
    c.ch = wf->nChannels;
    c.rate = wf->nSamplesPerSec;
    c.container = wf->wBitsPerSample;
    c.valid = c.container;
    c.mask = MaskFor(c.ch);
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* e = (const WAVEFORMATEXTENSIBLE*)wf;
        if (e->Samples.wValidBitsPerSample) c.valid = e->Samples.wValidBitsPerSample;
        if (e->dwChannelMask) c.mask = e->dwChannelMask;
    }
    CoTaskMemFree(wf);

    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(pc->GetPropertyValue(id.c_str(), FALSE, &PKEY_PhysicalSpeakers, &pv)) && pv.vt == VT_UI4)
        c.speakers = pv.ulVal;
    else
        c.speakers = c.mask;
    PropVariantClear(&pv);
    return true;
}

static void SetSpeakers(IPolicyConfig* pc, const std::wstring& id, DWORD mask) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_UI4;
    pv.ulVal = mask;
    pc->SetPropertyValue(id.c_str(), FALSE, &PKEY_PhysicalSpeakers, &pv);
}

// 24-bit: try a packed 24-bit container first, then 24 valid bits in a 32-bit container
// (some drivers, e.g. Creative's, only accept the latter).
static bool SetFormatTry(IPolicyConfig* pc, const std::wstring& id, int ch, DWORD rate, int bits, HRESULT& hr) {
    struct C { int container, valid; };
    std::vector<C> cands;
    if (bits == 16)      cands = {{16, 16}};
    else if (bits == 24) cands = {{24, 24}, {32, 24}};
    else                 cands = {{32, 32}};
    hr = E_FAIL;
    for (const C& c : cands) {
        WAVEFORMATEXTENSIBLE f = MakeFmt(ch, c.container, c.valid, rate, MaskFor(ch));
        hr = pc->SetDeviceFormat(id.c_str(), &f.Format, &f.Format);
        if (SUCCEEDED(hr)) return true;
    }
    return false;
}

// ---- config.ini -------------------------------------------------------------
static int IniInt(const wchar_t* s, const wchar_t* k, int d) {
    return (int)GetPrivateProfileIntW(s, k, d, g_ini.c_str());
}
static std::wstring IniStr(const wchar_t* s, const wchar_t* k) {
    wchar_t b[1024];
    GetPrivateProfileStringW(s, k, L"", b, 1024, g_ini.c_str());
    return b;
}
static bool IniPut(const wchar_t* s, const wchar_t* k, const std::wstring& v) {
    return WritePrivateProfileStringW(s, k, v.c_str(), g_ini.c_str()) != 0;
}
static bool IniPutInt(const wchar_t* s, const wchar_t* k, long long v) {
    wchar_t b[32];
    swprintf_s(b, L"%lld", v);
    return IniPut(s, k, b);
}

static bool HasPrev() { return IniInt(L"Previous", L"Channels", 0) > 0; }

static void SavePrev(const std::wstring& id, const CurFmt& c) {
    IniPut(L"Previous", L"DeviceId", id);
    IniPutInt(L"Previous", L"Channels", c.ch);
    IniPutInt(L"Previous", L"Rate", c.rate);
    IniPutInt(L"Previous", L"Container", c.container);
    IniPutInt(L"Previous", L"Valid", c.valid);
    IniPutInt(L"Previous", L"Mask", c.mask);
    IniPutInt(L"Previous", L"Speakers", c.speakers);
}

// ---- Apply / reset ----------------------------------------------------------
static std::wstring HrText(HRESULT hr) {
    wchar_t b[64];
    swprintf_s(b, L"0x%08X", (unsigned)hr);
    return b;
}

static int ApplyCore(const std::wstring& id, int ch, DWORD rate, int bits, std::wstring& msg) {
    IPolicyConfig* pc = NewPC();
    if (!pc) { msg = L"IPolicyConfig is not available on this Windows build."; return 4; }

    CurFmt cur;
    if (GetCur(pc, id, cur) && !(cur.ch == ch && cur.rate == rate && cur.valid == bits) && !HasPrev())
        SavePrev(id, cur);   // remember the original format once, so Reset can go back to it

    SetSpeakers(pc, id, MaskFor(ch));
    HRESULT hr = S_OK;
    int rc;
    wchar_t b[256];
    if (SetFormatTry(pc, id, ch, rate, bits, hr)) {
        swprintf_s(b, L"Applied: %d channels, %lu Hz, %d-bit.", ch, (unsigned long)rate, bits);
        msg = b;
        rc = 0;
    } else {
        msg = L"The driver rejected this format (" + HrText(hr) + L").";
        rc = 2;
    }
    pc->Release();
    return rc;
}

static int ResetCore(std::wstring& msg) {
    if (!HasPrev()) { msg = L"Nothing to reset: no previous format has been recorded yet."; return 6; }
    std::wstring id = IniStr(L"Previous", L"DeviceId");
    int ch = IniInt(L"Previous", L"Channels", 2);
    DWORD rate = (DWORD)IniInt(L"Previous", L"Rate", 44100);
    int container = IniInt(L"Previous", L"Container", 16);
    int valid = IniInt(L"Previous", L"Valid", container);
    DWORD mask = (DWORD)IniInt(L"Previous", L"Mask", (int)MaskFor(ch));
    DWORD speakers = (DWORD)IniInt(L"Previous", L"Speakers", (int)mask);

    IPolicyConfig* pc = NewPC();
    if (!pc) { msg = L"IPolicyConfig is not available on this Windows build."; return 4; }
    SetSpeakers(pc, id, speakers);
    WAVEFORMATEXTENSIBLE f = MakeFmt(ch, container, valid, rate, mask);
    HRESULT hr = pc->SetDeviceFormat(id.c_str(), &f.Format, &f.Format);
    pc->Release();
    if (FAILED(hr)) { msg = L"Could not restore the previous format (" + HrText(hr) + L")."; return 2; }

    WritePrivateProfileStringW(L"Previous", nullptr, nullptr, g_ini.c_str());   // drop the backup
    wchar_t b[256];
    swprintf_s(b, L"Restored previous format: %d channels, %lu Hz, %d-bit.", ch, (unsigned long)rate, valid);
    msg = b;
    return 0;
}

static int RunSilent() {
    int ch = IniInt(L"Settings", L"Channels", 0);
    int rate = IniInt(L"Settings", L"Rate", 0);
    int bits = IniInt(L"Settings", L"Bits", 0);
    if (ch <= 0 || rate <= 0 || bits <= 0) return 5;

    EnumDevices();
    std::wstring wantId = IniStr(L"Settings", L"DeviceId");
    std::wstring wantName = IniStr(L"Settings", L"DeviceName");
    std::wstring id;
    for (const Dev& d : g_devs) if (d.id == wantId) { id = d.id; break; }
    if (id.empty() && !wantName.empty())
        for (const Dev& d : g_devs) if (d.name == wantName) { id = d.id; break; }
    if (id.empty()) return 3;

    std::wstring msg;
    return ApplyCore(id, ch, (DWORD)rate, bits, msg);
}

// ---- Startup (HKCU Run key, no admin needed) ----------------------------------
static bool StartupEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool SetStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok;
    if (on) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(p) + L"\" -silent";
        ok = RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueW(k, kRunName);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(k);
    return ok;
}

// ---- UI helpers -------------------------------------------------------------
static void SetStatus(const std::wstring& t) { SetWindowTextW(hStatus, t.c_str()); }

static int Sel(HWND h) {
    int i = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    return i < 0 ? -1 : (int)SendMessageW(h, CB_GETITEMDATA, i, 0);
}
static void AddItem(HWND h, const std::wstring& t, int data) {
    int i = (int)SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)t.c_str());
    SendMessageW(h, CB_SETITEMDATA, i, data);
}
// Select the item whose data == want; else the one == pref; else the last item.
static void Pick(HWND h, int want, int pref) {
    int n = (int)SendMessageW(h, CB_GETCOUNT, 0, 0);
    if (n <= 0) return;
    int sel = -1, prefIdx = -1;
    for (int i = 0; i < n; ++i) {
        int d = (int)SendMessageW(h, CB_GETITEMDATA, i, 0);
        if (d == want) sel = i;
        if (d == pref) prefIdx = i;
    }
    if (sel < 0) sel = prefIdx;
    if (sel < 0) sel = n - 1;
    SendMessageW(h, CB_SETCURSEL, sel, 0);
}

static const wchar_t* ChName(int ch) {
    switch (ch) {
    case 2: return L"2 channels (Stereo)";
    case 4: return L"4 channels (Quadraphonic)";
    case 6: return L"6 channels (5.1 Surround)";
    case 8: return L"8 channels (7.1 Surround)";
    default: return L"?";
    }
}

static void RefreshCh(int want) {
    SendMessageW(hCh, CB_RESETCONTENT, 0, 0);
    for (int ch : {2, 4, 6, 8}) {
        bool any = false;
        for (const Fmt& f : g_fmts) if (f.ch == ch) { any = true; break; }
        if (any) AddItem(hCh, ChName(ch), ch);
    }
    Pick(hCh, want, 6);
}
static void RefreshRate(int want) {
    SendMessageW(hRate, CB_RESETCONTENT, 0, 0);
    int ch = Sel(hCh);
    DWORD last = 0;
    for (const Fmt& f : g_fmts) {
        if (f.ch != ch || f.rate == last) continue;
        last = f.rate;
        wchar_t b[32];
        swprintf_s(b, L"%lu Hz", (unsigned long)f.rate);
        AddItem(hRate, b, (int)f.rate);
    }
    Pick(hRate, want, 192000);
}
static void RefreshBits(int want) {
    SendMessageW(hBits, CB_RESETCONTENT, 0, 0);
    int ch = Sel(hCh), rate = Sel(hRate);
    for (const Fmt& f : g_fmts) {
        if (f.ch != ch || (int)f.rate != rate) continue;
        wchar_t b[32];
        swprintf_s(b, L"%d-bit", f.bits);
        AddItem(hBits, b, f.bits);
    }
    Pick(hBits, want, 24);
}

static std::wstring DevId() {
    int d = Sel(hDev);
    return (d >= 0 && d < (int)g_devs.size()) ? g_devs[d].id : L"";
}
static std::wstring DevName() {
    int d = Sel(hDev);
    return (d >= 0 && d < (int)g_devs.size()) ? g_devs[d].name : L"";
}

static std::wstring CurrentText(const std::wstring& id) {
    IPolicyConfig* pc = NewPC();
    if (!pc) return L"";
    CurFmt c;
    std::wstring t;
    if (GetCur(pc, id, c)) {
        wchar_t b[128];
        swprintf_s(b, L"Current format: %d channels, %lu Hz, %d-bit.", c.ch, (unsigned long)c.rate, c.valid);
        t = b;
    }
    pc->Release();
    return t;
}

static void RefreshDevice(int wantCh, int wantRate, int wantBits) {
    std::wstring id = DevId();
    if (id.empty()) { SetStatus(L"No active playback devices found."); return; }
    bool all = SendMessageW(hAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
    Detect(id, all);
    bool fell = false;
    if (g_fmts.empty() && !all) { Detect(id, true); fell = true; }
    RefreshCh(wantCh);
    RefreshRate(wantRate);
    RefreshBits(wantBits);
    std::wstring t = CurrentText(id);
    if (fell) t += L"\r\nThe driver did not report any formats, so all options are shown.";
    SetStatus(t);
}

static void FillDevices(const std::wstring& wantId, const std::wstring& wantName) {
    SendMessageW(hDev, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g_devs.size(); ++i) {
        AddItem(hDev, g_devs[i].name, (int)i);
        if (!wantId.empty() && g_devs[i].id == wantId) sel = (int)i;
    }
    if (sel < 0 && !wantName.empty())
        for (size_t i = 0; i < g_devs.size(); ++i) if (g_devs[i].name == wantName) { sel = (int)i; break; }
    if (sel < 0)
        for (size_t i = 0; i < g_devs.size(); ++i)
            if (g_devs[i].name.find(L"Audigy") != std::wstring::npos) { sel = (int)i; break; }
    if (sel < 0 && !g_devs.empty()) sel = 0;
    if (sel >= 0) SendMessageW(hDev, CB_SETCURSEL, sel, 0);
}

static void UpdateStartupBtn() {
    SetWindowTextW(hStartup, StartupEnabled() ? L"Remove from Windows startup"
                                              : L"Add to Windows startup (runs with -silent)");
}

static HWND Mk(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static BOOL CALLBACK SetFontProc(HWND c, LPARAM l) {
    SendMessageW(c, WM_SETFONT, (WPARAM)l, TRUE);
    return TRUE;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        const DWORD cb = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
        Mk(hwnd, L"STATIC", L"Playback device", SS_LEFT, 15, 18, 130, 20, 0);
        hDev = Mk(hwnd, L"COMBOBOX", L"", cb, 150, 14, 295, 220, ID_DEV);
        Mk(hwnd, L"STATIC", L"Channels", SS_LEFT, 15, 53, 130, 20, 0);
        hCh = Mk(hwnd, L"COMBOBOX", L"", cb, 150, 49, 295, 220, ID_CH);
        Mk(hwnd, L"STATIC", L"Sample rate", SS_LEFT, 15, 88, 130, 20, 0);
        hRate = Mk(hwnd, L"COMBOBOX", L"", cb, 150, 84, 295, 220, ID_RATE);
        Mk(hwnd, L"STATIC", L"Bit depth", SS_LEFT, 15, 123, 130, 20, 0);
        hBits = Mk(hwnd, L"COMBOBOX", L"", cb, 150, 119, 295, 220, ID_BITS);
        hAll = Mk(hwnd, L"BUTTON", L"Show all options (ignore what the driver reports)",
                  BS_AUTOCHECKBOX | WS_TABSTOP, 15, 155, 430, 22, ID_ALL);
        Mk(hwnd, L"BUTTON", L"Save settings", BS_PUSHBUTTON | WS_TABSTOP, 15, 190, 135, 30, ID_SAVE);
        Mk(hwnd, L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 160, 190, 135, 30, ID_APPLY);
        Mk(hwnd, L"BUTTON", L"Reset to previous", BS_PUSHBUTTON | WS_TABSTOP, 305, 190, 140, 30, ID_RESET);
        hStartup = Mk(hwnd, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 15, 235, 430, 30, ID_STARTUP);
        Mk(hwnd, L"STATIC",
           L"Startup runs this app with -silent: it applies the saved settings (config.ini) without opening a window.",
           SS_LEFT, 15, 275, 430, 36, 0);
        hStatus = Mk(hwnd, L"STATIC", L"", SS_LEFT, 15, 320, 430, 60, 0);

        NONCLIENTMETRICSW ncm = {};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        EnumChildWindows(hwnd, SetFontProc, (LPARAM)font);

        EnumDevices();
        FillDevices(IniStr(L"Settings", L"DeviceId"), IniStr(L"Settings", L"DeviceName"));
        RefreshDevice(IniInt(L"Settings", L"Channels", -1), IniInt(L"Settings", L"Rate", -1),
                      IniInt(L"Settings", L"Bits", -1));
        UpdateStartupBtn();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == CBN_SELCHANGE) {
            if (id == ID_DEV)       RefreshDevice(-1, -1, -1);
            else if (id == ID_CH)   { RefreshRate(-1); RefreshBits(-1); }
            else if (id == ID_RATE) RefreshBits(-1);
            return 0;
        }
        if (code != BN_CLICKED) break;
        std::wstring m;
        switch (id) {
        case ID_ALL:
            RefreshDevice(Sel(hCh), Sel(hRate), Sel(hBits));
            break;
        case ID_SAVE: {
            int ch = Sel(hCh), rate = Sel(hRate), bits = Sel(hBits);
            if (DevId().empty() || ch < 0 || rate < 0 || bits < 0) { SetStatus(L"Choose all four options first."); break; }
            bool ok = true;
            ok &= IniPut(L"Settings", L"DeviceId", DevId());
            ok &= IniPut(L"Settings", L"DeviceName", DevName());
            ok &= IniPutInt(L"Settings", L"Channels", ch);
            ok &= IniPutInt(L"Settings", L"Rate", rate);
            ok &= IniPutInt(L"Settings", L"Bits", bits);
            SetStatus(ok ? L"Settings saved to config.ini."
                         : L"Could not write config.ini. Is the app's folder writable?");
            break;
        }
        case ID_APPLY: {
            int ch = Sel(hCh), rate = Sel(hRate), bits = Sel(hBits);
            if (DevId().empty() || ch < 0 || rate < 0 || bits < 0) { SetStatus(L"Choose all four options first."); break; }
            ApplyCore(DevId(), ch, (DWORD)rate, bits, m);
            SetStatus(m);
            break;
        }
        case ID_RESET:
            ResetCore(m);
            SetStatus(m);
            break;
        case ID_STARTUP: {
            bool want = !StartupEnabled();
            if (!SetStartup(want)) SetStatus(L"Could not change the startup entry.");
            else SetStatus(want ? L"Added to Windows startup. Remember to Save settings so there is something to apply."
                                : L"Removed from Windows startup.");
            UpdateStartupBtn();
            break;
        }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int show) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L'\\');
    g_ini = s.substr(0, k + 1) + L"config.ini";

    bool silent = false;
    if (cmdLine) {
        std::wstring c = cmdLine;
        for (wchar_t& x : c) x = (wchar_t)towlower(x);
        silent = c.find(L"-silent") != std::wstring::npos || c.find(L"/silent") != std::wstring::npos;
    }
    if (silent) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int rc = RunSilent();
        CoUninitialize();
        return rc;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));     // icon resource 1 from app.rc (NULL = default if absent)
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SpeakerSwitcherWnd";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, 460, 390};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Speaker Format Switcher", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return (int)m.wParam;
}
