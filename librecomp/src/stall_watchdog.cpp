// [wcw] Stall watchdog (aki-roadmap#187). WCW_STALL_DUMP=<seconds>: when the game hasn't swapped a frame for that long,
// write every game thread's state and every native thread's stack to stderr, as "[stall]" lines, and keep running. Off
// (no thread at all) without the variable. A freeze with no crash has nothing else to go on.
//
// The dump suspends every other thread, so nothing that can take a lock runs between suspend and resume: no allocation,
// no CRT, no printing. Frames go into fixed buffers and are printed once everything is running again.
//
// Recompiled functions are matched to their game address through the section tables, marked "~" because the match is the
// nearest function below. Other exe frames print as exe+offset. A live version has no unwind data, so the unwinder stops
// there; the stack scan lists the return addresses past it.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "librecomp/stall_rules.hpp"

#if defined(_WIN32) && defined(_M_X64)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>
#include <TlHelp32.h>

#include "librecomp/live_code_rules.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"
#include "ultramodern/ultramodern.hpp"

extern std::atomic_bool exited;
extern std::atomic_int wcw_viswaps;
extern uint64_t total_vis;

namespace {
    constexpr size_t MaxThreads = 256;
    constexpr int MaxFrames = 64;
    constexpr int MaxScan = 96;
    constexpr size_t MaxScanBytes = 512 * 1024;

    struct Capture {
        DWORD tid = 0;
        bool ok = false;
        uint64_t rsp = 0;
        uint64_t frames[MaxFrames];
        int num_frames = 0;
        uint64_t scan[MaxScan];
        int num_scan = 0;
    };

    struct NativeFunc {
        uintptr_t native;
        uint32_t vram;
    };

    uintptr_t text_start = 0, text_end = 0;

    void find_exe_text() {
        const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                text_start = reinterpret_cast<uintptr_t>(base) + section->VirtualAddress;
                text_end = text_start + section->Misc.VirtualSize;
                return;
            }
        }
    }

    // Does a call end just before this exe address? Drops data and stale code pointers from the stack scan.
    bool follows_call(uintptr_t addr) {
        if ((addr < text_start + 8) || (addr >= text_end)) {
            return false;
        }
        const auto p = reinterpret_cast<const uint8_t*>(addr);
        return (p[-5] == 0xE8) || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)
            || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
    }

    // Runs with every other thread suspended: no allocation, CRT or locks.
    void capture_stack(const CONTEXT& start, uintptr_t live_low, uintptr_t live_high, Capture& out) {
        CONTEXT ctx = start;
        out.rsp = ctx.Rsp;
        while (out.num_frames < MaxFrames && ctx.Rip != 0) {
            out.frames[out.num_frames++] = ctx.Rip;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (function == nullptr) {
                break; // a live version, or a leaf: the scan covers the rest
            }
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, function, &ctx, &handler_data, &establisher, nullptr);
        }

        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(start.Rsp), &info, sizeof(info)) == 0) {
            return;
        }
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        const uintptr_t scan_end = std::min<uintptr_t>(region_end, start.Rsp + MaxScanBytes);
        for (uintptr_t at = start.Rsp & ~uintptr_t(7); (at + 8) <= scan_end && out.num_scan < MaxScan; at += 8) {
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(at);
            const bool in_live = (live_low != 0) && (value >= live_low) && (value < live_high);
            if (in_live || follows_call(value)) {
                out.scan[out.num_scan++] = value;
            }
        }
    }

    std::string thread_name(HANDLE thread) {
        wchar_t* wide = nullptr;
        std::string name{};
        if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide != nullptr) {
            for (const wchar_t* c = wide; *c != 0; c++) {
                name.push_back(*c < 128 ? char(*c) : '?');
            }
            LocalFree(wide);
        }
        return name;
    }

    std::vector<NativeFunc> native_funcs() {
        std::vector<NativeFunc> funcs{};
        for (size_t s = 0; s < recomp::overlays::num_code_sections(); s++) {
            const SectionTableEntry& section = recomp::overlays::get_code_section(static_cast<uint16_t>(s));
            for (size_t f = 0; f < section.num_funcs; f++) {
                funcs.push_back(NativeFunc{ reinterpret_cast<uintptr_t>(section.funcs[f].func), section.ram_addr + section.funcs[f].offset });
            }
        }
        std::sort(funcs.begin(), funcs.end(), [](const NativeFunc& a, const NativeFunc& b) { return a.native < b.native; });
        return funcs;
    }

    void print_address(const char* tag, int index, uint64_t addr, const std::vector<NativeFunc>& funcs) {
        char module_name[MAX_PATH] = "?";
        HMODULE module = nullptr;
        uint64_t base = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &module)) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(module, path, MAX_PATH) > 0) {
                const char* slash = strrchr(path, '\\');
                snprintf(module_name, sizeof(module_name), "%s", slash != nullptr ? slash + 1 : path);
            }
            base = reinterpret_cast<uint64_t>(module);
        }

        uint32_t live_vram = 0;
        if (module == nullptr && recomp::mods::live_codes::version_at(addr, live_vram)) {
            fprintf(stderr, "[stall]   %s%02d %016llX live version of func_%08X\n", tag, index, (unsigned long long)addr, live_vram);
            return;
        }

        std::string game{};
        if ((addr >= text_start) && (addr < text_end) && !funcs.empty()) {
            auto it = std::upper_bound(funcs.begin(), funcs.end(), uintptr_t(addr), [](uintptr_t a, const NativeFunc& f) { return a < f.native; });
            if (it != funcs.begin()) {
                --it;
                char text[64];
                snprintf(text, sizeof(text), " ~func_%08X+0x%llX", it->vram, (unsigned long long)(addr - it->native));
                game = text;
            }
        }
        fprintf(stderr, "[stall]   %s%02d %s+0x%llX%s\n", tag, index, module_name, (unsigned long long)(addr - base), game.c_str());
    }

    void dump_threads(uint8_t* rdram, const std::vector<NativeFunc>& funcs) {
        ultramodern::print_game_threads(rdram);
        recomp::mods::live_codes::print_stall_state();

        // Everything allocated before the first suspend. Toolhelp allocates, so the list comes first too.
        std::vector<DWORD> tids{};
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
                if ((entry.th32OwnerProcessID == GetCurrentProcessId()) && (entry.th32ThreadID != GetCurrentThreadId()) && (tids.size() < MaxThreads)) {
                    tids.push_back(entry.th32ThreadID);
                }
            }
            CloseHandle(snapshot);
        }
        auto captures = std::make_unique<Capture[]>(tids.size());
        std::vector<HANDLE> handles(tids.size(), nullptr);
        for (size_t i = 0; i < tids.size(); i++) {
            captures[i].tid = tids[i];
            handles[i] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tids[i]);
        }
        uintptr_t live_low = 0, live_high = 0;
        recomp::mods::live_codes::version_bounds(live_low, live_high);

        // SuspendThread returns before the thread has stopped; GetThreadContext is what waits for it.
        std::vector<CONTEXT> contexts(tids.size());
        for (size_t i = 0; i < tids.size(); i++) {
            if (handles[i] != nullptr && SuspendThread(handles[i]) != DWORD(-1)) {
                contexts[i].ContextFlags = CONTEXT_FULL;
                captures[i].ok = GetThreadContext(handles[i], &contexts[i]) != 0;
            }
        }
        for (size_t i = 0; i < tids.size(); i++) {
            if (captures[i].ok) {
                capture_stack(contexts[i], live_low, live_high, captures[i]);
            }
        }
        for (size_t i = 0; i < tids.size(); i++) {
            if (handles[i] != nullptr) {
                ResumeThread(handles[i]);
            }
        }

        for (size_t i = 0; i < tids.size(); i++) {
            const Capture& c = captures[i];
            const std::string name = handles[i] != nullptr ? thread_name(handles[i]) : std::string{};
            fprintf(stderr, "[stall] native thread %lu '%s'%s rsp=%016llX\n", c.tid, name.c_str(), c.ok ? "" : " (no context)", (unsigned long long)c.rsp);
            for (int f = 0; f < c.num_frames; f++) {
                print_address("#", f, c.frames[f], funcs);
            }
            for (int s = 0; s < c.num_scan; s++) {
                print_address("s", s, c.scan[s], funcs);
            }
            if (handles[i] != nullptr) {
                CloseHandle(handles[i]);
            }
        }
    }

    void watchdog_thread(uint8_t* rdram, double threshold) {
        ultramodern::set_native_thread_name("Stall Watchdog");
        find_exe_text();
        const std::vector<NativeFunc> funcs = native_funcs();
        recomp::stall::Detector detector{ threshold };
        const auto start = std::chrono::steady_clock::now();
        const auto poll = std::chrono::duration<double>(std::clamp(threshold / 4.0, 0.02, 0.25));
        while (!exited.load()) {
            std::this_thread::sleep_for(poll);
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const uint64_t swaps = uint64_t(wcw_viswaps.load());
            const recomp::stall::Event event = detector.update(now, swaps);
            // Parked threads after a quit would read as a freeze, and rdram goes away.
            if (exited.load()) {
                return;
            }
            if (event == recomp::stall::Event::Dump) {
                fprintf(stderr, "[stall] === no game frame for %.1f s at %.1f s: swaps %llu, VIs %llu, dump %d of this stall\n",
                    detector.stalled_for(), now, (unsigned long long)swaps, (unsigned long long)total_vis, detector.dumps_this_stall());
                dump_threads(rdram, funcs);
                fprintf(stderr, "[stall] === end of dump\n");
                fflush(stderr);
            }
            else if (event == recomp::stall::Event::Resumed) {
                fprintf(stderr, "[stall] frames resumed at %.1f s after %.1f s\n", now, detector.stalled_for());
            }
        }
    }
}

void recomp::stall::start_watchdog(uint8_t* rdram) {
    const double threshold = parse_threshold(getenv("WCW_STALL_DUMP"));
    if (threshold <= 0) {
        return;
    }
    fprintf(stderr, "[stall] watchdog on: dump after %.2f s without a game frame\n", threshold);
    std::thread{ watchdog_thread, rdram, threshold }.detach();
}

#else

void recomp::stall::start_watchdog(uint8_t*) {
    if (parse_threshold(getenv("WCW_STALL_DUMP")) > 0) {
        fprintf(stderr, "[stall] watchdog is Windows x64 only\n");
    }
}

#endif
