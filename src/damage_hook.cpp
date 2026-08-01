#include "damage_hook.hpp"
#include "config.hpp"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
struct BreakpointHook {
    uintptr_t address{};
    unsigned char original{};
};

volatile LONG g_enemy_damage_events{};
std::vector<BreakpointHook> g_hooks;
thread_local BreakpointHook* g_rearm_hook{};

bool displacement_is_health(const unsigned char* instruction, size_t available) {
    size_t opcode{};
    if (available && instruction[0] >= 0x40 && instruction[0] <= 0x4F) opcode = 1;
    if (available < opcode + 7) return false;

    const unsigned char operation = instruction[opcode];
    if (operation != 0x29 && operation != 0x81 && operation != 0x83) return false;
    const unsigned char modrm = instruction[opcode + 1];
    if ((modrm & 0xC0) != 0x80) return false;
    if ((operation == 0x81 || operation == 0x83) && (modrm & 0x38) != 0x28) return false;

    size_t displacement = opcode + 2;
    if ((modrm & 7) == 4) ++displacement;
    if (available < displacement + sizeof(uint32_t)) return false;
    uint32_t value{};
    memcpy(&value, instruction + displacement, sizeof(value));
    return value == 0x858;
}

std::vector<uintptr_t> find_damage_instructions() {
    std::vector<uintptr_t> results;
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        for (size_t i = 0; i + 9 <= size; ++i) {
            if (displacement_is_health(start + i, size - i)) {
                results.push_back(reinterpret_cast<uintptr_t>(start + i));
                i += 6;
            }
        }
    }
    return results;
}

BreakpointHook* hook_at(uintptr_t address) {
    for (auto& hook : g_hooks)
        if (hook.address == address) return &hook;
    return nullptr;
}

LONG CALLBACK damage_exception_handler(EXCEPTION_POINTERS* exception) {
    auto* record = exception->ExceptionRecord;
    auto* context = exception->ContextRecord;
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT) {
        auto* hook = hook_at(reinterpret_cast<uintptr_t>(record->ExceptionAddress));
        if (!hook) return EXCEPTION_CONTINUE_SEARCH;
        *reinterpret_cast<volatile unsigned char*>(hook->address) = hook->original;
        context->Rip = hook->address;
        context->EFlags |= 0x100;
        g_rearm_hook = hook;
        InterlockedIncrement(&g_enemy_damage_events);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && g_rearm_hook) {
        *reinterpret_cast<volatile unsigned char*>(g_rearm_hook->address) = 0xCC;
        context->EFlags &= ~0x100UL;
        g_rearm_hook = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

bool install_enemy_damage_hook() {
    const auto candidates = find_damage_instructions();
    if (candidates.empty()) {
        log_line("Outgoing hits: no subtract-from-health instructions were found");
        return false;
    }
    if (!AddVectoredExceptionHandler(1, &damage_exception_handler)) {
        log_line("Outgoing hits: failed to register damage exception handler");
        return false;
    }

    g_hooks.reserve(candidates.size());
    for (const uintptr_t candidate : candidates)
        g_hooks.push_back({candidate, *reinterpret_cast<unsigned char*>(candidate)});
    for (auto& hook : g_hooks) {
        DWORD old_protection{};
        if (!VirtualProtect(reinterpret_cast<void*>(hook.address), 1,
                            PAGE_EXECUTE_READWRITE, &old_protection)) continue;
        *reinterpret_cast<volatile unsigned char*>(hook.address) = 0xCC;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    log_line("Outgoing hits: armed %llu health-subtraction instruction%s",
             static_cast<unsigned long long>(g_hooks.size()),
             g_hooks.size() == 1 ? "" : "s");
    for (const auto& hook : g_hooks) {
        log_line("Outgoing hits: breakpoint at NieRAutomata.exe+0x%llX",
                 static_cast<unsigned long long>(hook.address -
                 reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    }
    return !g_hooks.empty();
}

bool consume_enemy_damage_event() {
    return InterlockedExchange(&g_enemy_damage_events, 0) > 0;
}
