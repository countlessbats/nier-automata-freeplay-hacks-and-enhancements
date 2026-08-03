#include "quick_load.hpp"

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "config.hpp"

namespace {

// Located by disassembling the decrypted image; see docs/NIER-INTERNALS.md.
constexpr unsigned kRequestRva = 0x1421EF0;  // pending operation; non-zero means busy
constexpr unsigned kStateRva = 0x1421EF4;    // the step both drivers switch on
constexpr unsigned kSlotRva = 0x1421EF8;     // which slot the operation applies to
// The save system takes numbered requests. Two of its entry points take a slot
// and were tested against a real save:
//
//   +0x9C9330  request 11  reads a slot into the staging buffer. Harmless, but
//                          it stops there; the live block is never touched.
//   +0x9C93A0  request  6  WRITES the live block to the slot. This is save, not
//                          load, and it overwrote a real save file when tried.
//                          Never call it looking for a load.
//
// Which request loads a slot into the running game is still unknown, so nothing
// is issued here yet. It has to be read off a real Start Game rather than
// guessed at, because half the codes in this table destroy data.
constexpr unsigned kRequestSlotReadRva = 0x9C9330;
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
    auto* staging = reinterpret_cast<const unsigned char*>(base + kStagingRva);
    auto* live = reinterpret_cast<volatile unsigned short*>(base + kLiveRva);

    // The title screen reads every slot before it draws the list, which leaves
    // the header in the staging buffer. That is the signal that the save system
    // is up and idle, and it costs nothing to wait for.
    if (*reinterpret_cast<const volatile unsigned short*>(staging) != kMagic) return false;
    if (*state != 0) return false;
    if (*live == kMagic) { done = true; return true; }  // already in a game

    std::wstring path;
    int slot = 0;
    if (!newest_slot(path, slot)) {
        log_line("Quick load: no save files found; leaving the title alone");
        done = true;
        return true;
    }

    // The file is read only to confirm the slot is one this build understands
    // before asking the game to load it; the game does its own read.
    std::vector<unsigned char> data;
    if (!read_slot(path, data) ||
        *reinterpret_cast<const unsigned short*>(data.data() + kFileHeader) != kMagic) {
        log_line("Quick load: slot %d is not a save this build recognises; skipping", slot);
        done = true;
        return true;
    }

    (void)kRequestSlotReadRva;
    done = true;
    log_line("Quick load: slot %d is ready, but the load request is not identified "
             "yet; doing nothing", slot);
    return true;
}
