#include "quick_load.hpp"

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "config.hpp"

namespace {

// Located by disassembling the decrypted image; see docs/NIER-INTERNALS.md.
constexpr unsigned kStateRva = 0x1421EF4;    // shared by the save and load drivers
constexpr unsigned kStagingRva = 0x14220C0;  // the buffer a slot is read into
constexpr unsigned kLiveRva = 0x145BF60;     // the live game data block
constexpr unsigned kBlockSize = 0x399C0;
constexpr unsigned short kMagic = 0x5954;
// On disk the block carries a twelve byte wrapper ahead of it, so a slot file
// is twelve bytes longer than the block the game keeps in memory and its header
// starts at that offset.
constexpr unsigned kFileHeader = 0xC;
constexpr int kApplyState = 1;

// Resolved at run time so the proxy DLL gains no new import. The Documents
// folder is often redirected off the system drive, so building the path from
// the profile environment variable would find the wrong place.
std::wstring save_directory() {
    using GetFolderFn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, wchar_t*);
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    if (!shell) return {};
    auto get_folder = reinterpret_cast<GetFolderFn>(GetProcAddress(shell, "SHGetFolderPathW"));
    wchar_t documents[MAX_PATH]{};
    const bool ok = get_folder &&
                    SUCCEEDED(get_folder(nullptr, CSIDL_PERSONAL, nullptr, 0, documents));
    FreeLibrary(shell);
    if (!ok) return {};
    return std::wstring(documents) + L"\\My Games\\NieR_Automata\\";
}

// Newest by write time, which is what "the save I was last playing" means.
bool newest_slot(std::wstring& path_out, int& slot_out) {
    const std::wstring directory = save_directory();
    if (directory.empty()) return false;
    FILETIME best{};
    bool found = false;
    for (int slot = 0; slot < 4; ++slot) {
        const std::wstring path = directory + L"SlotData_" + std::to_wstring(slot) + L".dat";
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) continue;
        if (found && CompareFileTime(&info.ftLastWriteTime, &best) <= 0) continue;
        best = info.ftLastWriteTime;
        path_out = path;
        slot_out = slot;
        found = true;
    }
    return found;
}

bool read_slot(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    out.assign(kFileHeader + kBlockSize, 0);
    DWORD read = 0;
    const bool ok = ReadFile(file, out.data(), kFileHeader + kBlockSize, &read, nullptr) != 0;
    CloseHandle(file);
    // A short read means the file is not the block this build expects, and
    // copying it over the live data would be worse than doing nothing.
    return ok && read == kFileHeader + kBlockSize;
}

}  // namespace

bool try_quick_load() {
    static bool done = false;
    if (done) return true;

    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* state = reinterpret_cast<volatile int*>(base + kStateRva);
    auto* staging = reinterpret_cast<unsigned char*>(base + kStagingRva);
    auto* live = reinterpret_cast<volatile unsigned short*>(base + kLiveRva);

    // The title screen reads every slot before it draws the list, which leaves
    // the header in the staging buffer. That is the signal that the save system
    // is up and idle, and it costs nothing to wait for.
    if (*reinterpret_cast<volatile unsigned short*>(staging) != kMagic) return false;
    if (*state != 0) return false;
    if (*live == kMagic) { done = true; return true; }  // already in a game

    std::wstring path;
    int slot = 0;
    if (!newest_slot(path, slot)) {
        log_line("Quick load: no save files found; leaving the title alone");
        done = true;
        return true;
    }

    std::vector<unsigned char> data;
    if (!read_slot(path, data)) {
        log_line("Quick load: slot %d is not a %u byte block; skipping", slot, kBlockSize);
        done = true;
        return true;
    }
    const unsigned char* block = data.data() + kFileHeader;
    if (*reinterpret_cast<const unsigned short*>(block) != kMagic) {
        log_line("Quick load: slot %d has header %04X, expected %04X; skipping",
                 slot, *reinterpret_cast<const unsigned short*>(block), kMagic);
        done = true;
        return true;
    }

    memcpy(staging, block, kBlockSize);
    *state = kApplyState;
    done = true;
    log_line("Quick load: staged slot %d and set the save system to state %d",
             slot, kApplyState);
    return true;
}
