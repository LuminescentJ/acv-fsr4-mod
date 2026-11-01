// ACV FSR 4 mod: one-file installer and uninstaller for players (the same job as install_fsr4.ps1 -NoBuild / -Uninstall).
// The mod's three DLLs are embedded as resources. Install and uninstall use the same manifest as install_fsr4.ps1
// (acv_fsr4_install.txt in the game folder), so either tool can remove what the other installed.
// Command line (used for the elevated step and by tests): /op=install|uninstall "/dir=<game folder>" ["/result=<file>"]
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "resource.h"

namespace {

const wchar_t kGameExe[] = L"ACValhalla.exe";
const wchar_t kManifest[] = L"acv_fsr4_install.txt";
const wchar_t kTitle[] = L"ACV FSR 4 Mod Setup";

struct ModFile { const wchar_t* name; int resource; };
const ModFile kFiles[] = {
    {L"d3d12.dll", IDR_D3D12},
    {L"amd_fidelityfx_loader_dx12.dll", IDR_LOADER},
    {L"amd_fidelityfx_upscaler_dx12.dll", IDR_UPSCALER},
};

struct Result { bool ok; std::wstring message; };

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    return (!dir.empty() && dir.back() == L'\\') ? dir + name : dir + L"\\" + name;
}

bool fileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool isGameFolder(const std::wstring& dir) { return !dir.empty() && fileExists(joinPath(dir, kGameExe)); }

std::wstring systemMessage(DWORD error) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"unknown error";
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r' || result.back() == L' ')) result.pop_back();
    return result;
}

bool gameRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        found = _wcsicmp(entry.szExeFile, kGameExe) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

// ---------- SHA-256 (upper-case hex, same as PowerShell's Get-FileHash) ----------

class Sha256 {
public:
    Sha256() { BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0); BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0); }
    ~Sha256() { BCryptDestroyHash(hash_); BCryptCloseAlgorithmProvider(alg_, 0); }
    void add(const void* data, DWORD size) { BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), size, 0); }
    std::string hex() {
        UCHAR digest[32];
        BCryptFinishHash(hash_, digest, sizeof(digest), 0);
        std::string out;
        char pair[3];
        for (UCHAR b : digest) { sprintf_s(pair, "%02X", b); out += pair; }
        return out;
    }
private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::string hashFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    Sha256 sha;
    std::vector<char> buffer(1 << 20);
    DWORD got = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got) sha.add(buffer.data(), got);
    CloseHandle(file);
    return sha.hex();
}

struct Payload { const void* data; DWORD size; };

Payload payload(int resource) {
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(resource), RT_RCDATA);
    HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    return {loaded ? LockResource(loaded) : nullptr, found ? SizeofResource(nullptr, found) : 0};
}

std::string payloadHash(int resource) {
    const Payload p = payload(resource);
    Sha256 sha;
    sha.add(p.data, p.size);
    return sha.hex();
}

// name -> hash from the "file=<name>|sha256=<hash>" lines of the manifest.
std::map<std::wstring, std::string> readManifest(const std::wstring& dir) {
    std::map<std::wstring, std::string> entries;
    HANDLE file = CreateFileW(joinPath(dir, kManifest).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return entries;
    char buffer[4096];
    DWORD got = 0;
    ReadFile(file, buffer, sizeof(buffer) - 1, &got, nullptr);
    CloseHandle(file);
    std::string text(buffer, got);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t bar = line.find("|sha256=");
        if (line.rfind("file=", 0) != 0 || bar == std::string::npos) continue;
        entries[std::wstring(line.begin() + 5, line.begin() + bar)] = line.substr(bar + 8);
    }
    return entries;
}

bool writeBytes(const std::wstring& path, const void* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) { DeleteFileW(path.c_str()); SetLastError(error ? error : ERROR_WRITE_FAULT); }
    return ok != FALSE;
}

// ---------- install / uninstall ----------

Result install(const std::wstring& dir) {
    if (!isGameFolder(dir)) return {false, L"ACValhalla.exe was not found in that folder."};
    if (gameRunning()) return {false, L"Assassin's Creed Valhalla is running. Close it and try again."};

    // Never overwrite another mod's file. Ours = listed in our manifest, or identical to what this installer carries.
    const auto previous = readManifest(dir);
    for (const ModFile& f : kFiles) {
        const std::wstring path = joinPath(dir, f.name);
        if (!fileExists(path)) continue;
        const std::string hash = hashFile(path);
        const auto known = previous.find(f.name);
        const bool ours = (known != previous.end() && known->second == hash) || hash == payloadHash(f.resource);
        if (!ours) {
            return {false, std::wstring(f.name) + L" is already in the game folder and does not belong to this mod "
                L"(probably another mod). Nothing was changed. Remove that mod first, then try again."};
        }
    }

    // All new files land next to their target first, so a full disk or a permissions problem changes nothing.
    std::vector<std::wstring> temporary;
    const auto removeTemporary = [&] { for (const auto& path : temporary) DeleteFileW(path.c_str()); };
    for (const ModFile& f : kFiles) {
        const Payload p = payload(f.resource);
        const std::wstring path = joinPath(dir, f.name) + L".acvnew";
        if (!p.data || !writeBytes(path, p.data, p.size)) {
            const DWORD error = GetLastError();
            removeTemporary();
            return {false, L"Could not write to the game folder: " + systemMessage(error) + L" Nothing was changed."};
        }
        temporary.push_back(path);
    }

    std::string manifest = "# ACV FSR 4 mod install manifest (used by the mod's uninstaller)\r\n";
    for (const ModFile& f : kFiles) {
        std::string name;
        for (const wchar_t* c = f.name; *c; ++c) name += static_cast<char>(*c);
        manifest += "file=" + name + "|sha256=" + payloadHash(f.resource) + "\r\n";
    }
    if (!writeBytes(joinPath(dir, kManifest), manifest.data(), static_cast<DWORD>(manifest.size()))) {
        const DWORD error = GetLastError();
        removeTemporary();
        return {false, L"Could not write to the game folder: " + systemMessage(error) + L" Nothing was changed."};
    }
    for (size_t i = 0; i < temporary.size(); ++i) {
        if (!MoveFileExW(temporary[i].c_str(), joinPath(dir, kFiles[i].name).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            const std::wstring reason = systemMessage(GetLastError());
            removeTemporary();
            return {false, L"Could not replace " + std::wstring(kFiles[i].name) + L": " + reason + L" Close anything using the game folder and click Install again."};
        }
    }

    std::wstring message = L"FSR 4 is installed.\n\n"
        L"Start the game from your launcher as usual.\n\n"
        L"FSR ON in Valhalla's graphics settings (e.g. Quality): FSR 4 upscaling.\n"
        L"FSR OFF: FSR 4 as anti-aliasing at full resolution.\n\n"
        L"Press F2 in the game for the settings menu. A green or cyan square at the top left after start-up means FSR 4 is active.";
    if (fileExists(joinPath(dir, L"dxgi.dll"))) {
        message += L"\n\nNote: a dxgi.dll from another mod is in the game folder (for example ReShade). It was left alone, "
            L"but using it together with this mod is untested. If the game misbehaves, try without it.";
    }
    return {true, message};
}

Result uninstall(const std::wstring& dir) {
    if (!isGameFolder(dir)) return {false, L"ACValhalla.exe was not found in that folder."};
    if (gameRunning()) return {false, L"Assassin's Creed Valhalla is running. Close it and try again."};
    if (!fileExists(joinPath(dir, kManifest))) return {false, L"The mod is not installed in that folder."};

    const auto entries = readManifest(dir);
    std::wstring kept;
    for (const ModFile& f : kFiles) {
        const std::wstring path = joinPath(dir, f.name);
        const auto known = entries.find(f.name);
        if (known == entries.end() || !fileExists(path)) continue;
        const std::string hash = hashFile(path);
        if (hash != known->second && hash != payloadHash(f.resource)) { kept += L"\n" + std::wstring(f.name); continue; }
        if (!DeleteFileW(path.c_str())) return {false, L"Could not remove " + std::wstring(f.name) + L": " + systemMessage(GetLastError())};
    }
    // install_fsr4.ps1 parked an older proxy under this name.
    const std::wstring dxgi = joinPath(dir, L"dxgi.dll");
    const std::wstring parked = dxgi + L".acvbak";
    if (fileExists(parked) && !fileExists(dxgi)) MoveFileW(parked.c_str(), dxgi.c_str());
    DeleteFileW(joinPath(dir, kManifest).c_str());

    std::wstring message = L"The mod has been removed. Your game is back to normal.";
    if (!kept.empty()) message += L"\n\nThese files changed after installing, so they were left in place:" + kept;
    return {true, message};
}

Result runOperation(const std::wstring& op, const std::wstring& dir) { return op == L"uninstall" ? uninstall(dir) : install(dir); }

// ---------- finding the game ----------

void addSteamLibrary(const std::wstring& steam, std::vector<std::wstring>& libraries) {
    libraries.push_back(steam);
    HANDLE file = CreateFileW(joinPath(steam, L"steamapps\\libraryfolders.vdf").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string text(GetFileSize(file, nullptr), '\0');
    DWORD got = 0;
    ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(file);
    // Lines like:  "path"    "D:\\SteamLibrary"
    for (size_t at = text.find("\"path\""); at != std::string::npos; at = text.find("\"path\"", at + 1)) {
        const size_t open = text.find('"', at + 6);
        const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
        if (close == std::string::npos) break;
        std::string path;
        for (size_t i = open + 1; i < close; ++i) {
            if (text[i] == '\\' && text[i + 1] == '\\') ++i;
            path += text[i];
        }
        wchar_t wide[MAX_PATH];
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, MAX_PATH);
        if (n > 0) libraries.emplace_back(wide);
    }
}

std::wstring findGameFolder() {
    std::vector<std::wstring> libraries;
    wchar_t value[MAX_PATH];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        std::wstring steam = value;
        for (wchar_t& c : steam) if (c == L'/') c = L'\\';
        addSteamLibrary(steam, libraries);
    }
    addSteamLibrary(L"C:\\Program Files (x86)\\Steam", libraries);
    for (const auto& library : libraries) {
        const std::wstring candidate = joinPath(library, L"steamapps\\common\\Assassin's Creed Valhalla");
        if (isGameFolder(candidate)) return candidate;
    }
    HKEY installs = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs", 0, KEY_READ, &installs) == ERROR_SUCCESS) {
        wchar_t id[64];
        DWORD idLength = ARRAYSIZE(id);
        std::wstring found;
        for (DWORD i = 0; found.empty() && RegEnumKeyExW(installs, i, id, &idLength, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; ++i, idLength = ARRAYSIZE(id)) {
            size = sizeof(value);
            if (RegGetValueW(installs, id, L"InstallDir", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) continue;
            std::wstring dir = value;
            for (wchar_t& c : dir) if (c == L'/') c = L'\\';
            if (isGameFolder(dir)) found = dir;
        }
        RegCloseKey(installs);
        if (!found.empty()) return found;
    }
    return {};
}

bool supportedCardName(const std::wstring& name) {
    if (name.find(L"Radeon") == std::wstring::npos) return false;
    for (const wchar_t* series : {L"RX 6", L"RX 7", L"RX 8", L"RX 9"}) {
        if (name.find(series) != std::wstring::npos) return true;
    }
    return false;
}

// The display name of the first AMD card, preferring an RX 6000 series or newer one; otherwise the first adapter.
std::wstring graphicsCard(bool* supported) {
    std::wstring first, amd;
    *supported = false;
    DISPLAY_DEVICEW device{sizeof(device)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i, device.cb = sizeof(device)) {
        const std::wstring name = device.DeviceString;
        if (first.empty()) first = name;
        if (name.find(L"Radeon") != std::wstring::npos && amd.empty()) amd = name;
        if (supportedCardName(name)) { *supported = true; return name; }
    }
    return !amd.empty() ? amd : first;
}

// ---------- elevation ----------

bool folderWritable(const std::wstring& dir) {
    const std::wstring probe = joinPath(dir, L"acv_fsr4_write_test.tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

Result runElevated(HWND owner, const std::wstring& op, const std::wstring& dir) {
    wchar_t self[MAX_PATH * 2], tempDir[MAX_PATH], resultPath[MAX_PATH];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    GetTempPathW(ARRAYSIZE(tempDir), tempDir);
    GetTempFileNameW(tempDir, L"acv", 0, resultPath);
    const std::wstring params = L"/op=" + op + L" \"/dir=" + dir + L"\" \"/result=" + resultPath + L"\"";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = self;
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        DeleteFileW(resultPath);
        return {false, L"Windows permission is needed to change the game folder, so nothing was changed."};
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);

    std::wstring message;
    HANDLE file = CreateFileW(resultPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        message.resize(GetFileSize(file, nullptr) / sizeof(wchar_t));
        DWORD got = 0;
        ReadFile(file, message.data(), static_cast<DWORD>(message.size() * sizeof(wchar_t)), &got, nullptr);
        CloseHandle(file);
    }
    DeleteFileW(resultPath);
    return {exitCode == 0, message.empty() ? L"The installer stopped unexpectedly." : message};
}

// ---------- window ----------

std::wstring dialogText(HWND dialog, int id) {
    wchar_t text[MAX_PATH * 2] = {};
    GetDlgItemTextW(dialog, id, text, ARRAYSIZE(text));
    std::wstring dir = text;
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    return dir;
}

void refresh(HWND dialog) {
    const std::wstring dir = dialogText(dialog, IDC_FOLDER);
    const bool valid = isGameFolder(dir);
    const bool installed = valid && fileExists(joinPath(dir, kManifest));
    const bool current = installed && hashFile(joinPath(dir, L"d3d12.dll")) == payloadHash(IDR_D3D12);
    const wchar_t* status = !valid ? L"Choose the folder that contains ACValhalla.exe."
        : current ? L"Installed (this version)" : installed ? L"Installed (a different version)" : L"Not installed";
    SetDlgItemTextW(dialog, IDC_STATUS, status);
    SetDlgItemTextW(dialog, IDOK, current ? L"Reinstall" : installed ? L"Update" : L"Install");
    EnableWindow(GetDlgItem(dialog, IDOK), valid);
    EnableWindow(GetDlgItem(dialog, IDC_UNINSTALL), installed);
}

void browse(HWND dialog) {
    IFileOpenDialog* picker = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) return;
    DWORD options = 0;
    picker->GetOptions(&options);
    picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    picker->SetTitle(L"Select the Assassin's Creed Valhalla folder");
    IShellItem* item = nullptr;
    if (SUCCEEDED(picker->Show(dialog)) && SUCCEEDED(picker->GetResult(&item))) {
        wchar_t* path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { SetDlgItemTextW(dialog, IDC_FOLDER, path); CoTaskMemFree(path); }
        item->Release();
    }
    picker->Release();
}

void perform(HWND dialog, const std::wstring& op) {
    const std::wstring dir = dialogText(dialog, IDC_FOLDER);
    if (op == L"uninstall" && MessageBoxW(dialog, L"Remove the FSR 4 mod from this game folder?", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    HCURSOR cursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const Result result = folderWritable(dir) ? runOperation(op, dir) : runElevated(dialog, op, dir);
    SetCursor(cursor);
    MessageBoxW(dialog, result.message.c_str(), kTitle, MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    refresh(dialog);
}

HFONT g_headingFont = nullptr;

INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM) {
    switch (message) {
    case WM_INITDIALOG: {
        HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
        SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        LOGFONTW font{};
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0)), sizeof(font), &font);
        font.lfHeight = font.lfHeight * 3 / 2;
        font.lfWeight = FW_SEMIBOLD;
        g_headingFont = CreateFontIndirectW(&font);
        SendDlgItemMessageW(dialog, IDC_HEADING, WM_SETFONT, reinterpret_cast<WPARAM>(g_headingFont), TRUE);

        bool supported = false;
        const std::wstring card = graphicsCard(&supported);
        SetDlgItemTextW(dialog, IDC_GPU, (card + (supported ? L"" : L" (older than RX 6000 series, or not AMD)")).c_str());
        ShowWindow(GetDlgItem(dialog, IDC_GPU_WARNING), supported ? SW_HIDE : SW_SHOW);
        SetDlgItemTextW(dialog, IDC_FOLDER, findGameFolder().c_str());
        refresh(dialog);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_FOLDER: if (HIWORD(wParam) == EN_CHANGE) refresh(dialog); return TRUE;
        case IDC_BROWSE: browse(dialog); return TRUE;
        case IDOK: perform(dialog, L"install"); return TRUE;
        case IDC_UNINSTALL: perform(dialog, L"uninstall"); return TRUE;
        case IDCANCEL: EndDialog(dialog, 0); return TRUE;
        }
        break;
    }
    return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring op, dir, resultPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"/op=", 0) == 0) op = arg.substr(4);
        else if (arg.rfind(L"/dir=", 0) == 0) dir = arg.substr(5);
        else if (arg.rfind(L"/result=", 0) == 0) resultPath = arg.substr(8);
    }
    if (!op.empty()) {
        const Result result = runOperation(op, dir);
        if (!resultPath.empty()) writeBytes(resultPath, result.message.data(), static_cast<DWORD>(result.message.size() * sizeof(wchar_t)));
        return result.ok ? 0 : 1;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    DialogBoxW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialogProc);
    CoUninitialize();
    return 0;
}
