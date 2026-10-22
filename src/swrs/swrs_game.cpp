// Rogue Squadron specific glue: RSP microcode lookup, thread names, and replacements
// for the functions N64Recomp can't translate (see recomp/swrs.toml in the SWRS repo).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/rsp.hpp"
#include "recomp.h"

// rsp/aspF5.cpp (global namespace).
RspExitReason aspF5(uint8_t* rdram, uint32_t ucode_addr);

namespace swrs {

// Factor 5's audio microcode (text at ROM 0x8EBF0, vram 0x8008DFF0), translated by RSPRecomp
// into rsp/aspF5.cpp (recomp/aspF5.toml in the SWRS repo).
std::atomic<uint32_t> audio_task_count{0};

// Bring-up: with SWRS_AUDIO_TASK_DUMP set, write each audio task's input (the 0x10-byte header
// plus voice records, data_size blocks of 0xA10 bytes, big-endian as on the N64) to
// swrs-audio-tasks.raw: per task u32 data_ptr, u32 block count, then the bytes.
static void dump_audio_task(uint8_t* rdram) {
    static FILE* f = nullptr;
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (std::getenv("SWRS_AUDIO_TASK_DUMP")) f = std::fopen("swrs-audio-tasks.raw", "wb");
    }
    if (!f) return;
    const uint32_t data_ptr = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x30);
    const uint32_t blocks = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x34);
    if (blocks == 0 || blocks > 8) return;
    const uint32_t header[2] = { data_ptr, blocks };
    std::fwrite(header, sizeof(header), 1, f);
    std::vector<uint8_t> bytes(blocks * 0xA10);
    for (uint32_t i = 0; i < bytes.size(); i++) {
        bytes[i] = MEM_BU(0, (int32_t)((data_ptr | 0x80000000) + i));
    }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
}

// Output buffers are copied for playback when the AI would start them (runtime audio.cpp), but
// the game sometimes hands over a buffer whose audio task runs a moment later: the first block
// of a 4-block catch-up task, about every 1.2 s in the 32 kHz opening crawl, 0.1-1 ms late.
// The output ring has 8 slots; each task block writes the slot named in its last voice record
// (index = voice count - 1, field 0x44). wait_audio_buffer_ready (main.cpp, at play time)
// waits briefly when a slot hasn't been rewritten since its previous playback.
static std::mutex audio_slot_mutex;
static std::condition_variable audio_slot_written;
static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> audio_slot_write_time;
static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> audio_slot_play_time;
static uint8_t* audio_rdram_base = nullptr;
static std::chrono::steady_clock::time_point last_audio_task_time;
static std::unordered_map<uint32_t, bool> audio_slot_gave_up; // timed out; skip until written again
std::atomic<uint32_t> audio_buffer_waits{0};
std::atomic<uint32_t> audio_buffer_wait_timeouts{0};

void wait_audio_buffer_ready(const int16_t* samples) {
    using namespace std::chrono_literals;
    std::unique_lock lock{ audio_slot_mutex };
    if (audio_rdram_base == nullptr) return;
    const uint32_t addr = 0x80000000u + uint32_t(reinterpret_cast<const uint8_t*>(samples) - audio_rdram_base);
    const auto now = std::chrono::steady_clock::now();
    auto played = audio_slot_play_time.find(addr);
    // Only while audio tasks are running: in silent stretches the game hands over buffers
    // without running any, and waiting would only stall playback.
    const bool tasks_running = (now - last_audio_task_time) < 50ms;
    if (tasks_running && !audio_slot_gave_up[addr] && played != audio_slot_play_time.end() && (now - played->second) < 500ms) {
        const auto last_play = played->second;
        auto written = [&] {
            auto w = audio_slot_write_time.find(addr);
            return w != audio_slot_write_time.end() && w->second > last_play;
        };
        if (!written()) {
            audio_buffer_waits++;
            if (!audio_slot_written.wait_for(lock, 6ms, written)) {
                audio_buffer_wait_timeouts++;
                audio_slot_gave_up[addr] = true;
            }
        }
    }
    audio_slot_play_time[addr] = std::chrono::steady_clock::now();
}

static void audio_task_outputs(uint8_t* rdram, std::vector<uint32_t>& outs) {
    const uint32_t data_ptr = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x30);
    const uint32_t blocks = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x34);
    if (blocks == 0 || blocks > 8) return;
    for (uint32_t b = 0; b < blocks; b++) {
        const int32_t block = (int32_t)((data_ptr | 0x80000000) + b * 0xA10);
        const uint32_t count = MEM_HU(0, block);
        if (count == 0 || count > 32) continue;
        outs.push_back(MEM_W((int32_t)(0x10 + (count - 1) * 0x50 + 0x44), block));
    }
}

static RspExitReason audio_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    audio_task_count++;
    dump_audio_task(rdram);
    std::vector<uint32_t> outs;
    audio_task_outputs(rdram, outs);
    RspExitReason result = ::aspF5(rdram, ucode_addr);
    {
        std::lock_guard lock{ audio_slot_mutex };
        audio_rdram_base = rdram;
        const auto now = std::chrono::steady_clock::now();
        last_audio_task_time = now;
        for (uint32_t out : outs) {
            audio_slot_write_time[out] = now;
            audio_slot_gave_up[out] = false;
        }
    }
    audio_slot_written.notify_all();
    return result;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return audio_ucode;

    default:
        fprintf(stderr, "[swrs] unknown RSP task type: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

std::string get_game_thread_name(const OSThread* t) {
    // Thread entry points are known (CLAUDE.md, "Threads") but not which id each gets yet.
    return "[Game] " + std::to_string(t->id) + " pri " + std::to_string(t->priority);
}

} // namespace swrs

// Busy-wait loops. On the N64 a spinning thread gets preempted by interrupts and
// higher-priority threads. The runtime only switches threads (and delivers external
// messages such as VI/SP/AI events) when a game thread blocks, so a spin loop stalls the
// whole game. Hooks in recomp/swrs.toml call this on every pass of such a loop.
void dequeue_external_messages(uint8_t* rdram); // ultramodern mesgqueue.cpp

// Last RDRAM pointer seen by a hook (for the write watch below).
static std::atomic<uint8_t*> swrs_rdram_seen{nullptr};

extern "C" void swrs_spin_wait_yield(uint8_t* rdram, recomp_context*) {
    swrs_rdram_seen = rdram;
    dequeue_external_messages(rdram);           // wake threads whose events arrived
    ultramodern::check_running_queue(rdram);    // let a higher-priority thread run
    ultramodern::wait_for_external_message_timed(rdram, 1); // don't burn a core
    ultramodern::check_running_queue(rdram);
}

// Function call trace (bring-up aid, see include/trace.h). Creating C:\projects\swrs-trace-now
// records call counts per game function for 2 seconds into C:\projects\swrs-trace.txt.
extern "C" volatile int swrs_trace_on = 0;
static std::mutex swrs_trace_mutex;
static std::unordered_map<const char*, uint64_t> swrs_trace_counts;

#ifdef _WIN32
// Crash report (bring-up aid): on an access violation, print the faulting thread's host
// stack as module + offset (symbolize exe offsets with llvm-symbolizer 0x140000000+off).
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>

static void swrs_print_frame(const char* tag, DWORD64 pc) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)pc, &mod) && mod != nullptr) {
        GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        fprintf(stderr, "[crash]   %s %s+0x%llX\n", tag, base ? base + 1 : name, (unsigned long long)(pc - (DWORD64)mod));
    } else {
        fprintf(stderr, "[crash]   %s 0x%llX\n", tag, (unsigned long long)pc);
    }
}

static LONG CALLBACK swrs_crash_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fprintf(stderr, "[crash] access violation (%s 0x%llX) on thread %lu\n",
        info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
        (unsigned long long)info->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    CONTEXT ctx = *info->ContextRecord;
    for (int i = 0; i < 32 && ctx.Rip != 0; i++) {
        swrs_print_frame(i == 0 ? "at" : "by", ctx.Rip);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn == nullptr) {
            // Leaf function: return address is at [rsp].
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static const bool swrs_crash_handler_installed = []() {
    AddVectoredExceptionHandler(1, swrs_crash_handler);
    return true;
}();

// Write watch on one RDRAM word (bring-up aid): SWRS_GUARD_ADDR=<hex N64 address>. A guard page on the host
// page holding the word traps every access; writes to the word record the host instruction address (symbolize
// with llvm-symbolizer to find the recompiled function), then a single step re-arms the guard. After
// SWRS_GUARD_DELAY seconds (default 85) it arms; 10 s later the distinct writers go to stderr.
static std::atomic<uint32_t> swrs_last_terrain_obj{0}; // set by swrs_log_terrain
static std::atomic<uintptr_t> swrs_guard_target{0};
static std::atomic<void*> swrs_guard_page{nullptr};
static thread_local bool swrs_guard_rearm = false;
static std::mutex swrs_guard_mutex;
static std::unordered_map<DWORD64, uint32_t> swrs_guard_writers;
static std::unordered_map<ULONG, std::vector<DWORD64>> swrs_guard_stacks;
static std::atomic<bool> swrs_guard_active{false};

static LONG CALLBACK swrs_guard_handler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if ((code == STATUS_GUARD_PAGE_VIOLATION) && swrs_guard_active.load()) {
        const uintptr_t target = swrs_guard_target.load();
        const uintptr_t accessed = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
        const bool write = info->ExceptionRecord->ExceptionInformation[0] == 1;
        if (write && (accessed >= target) && (accessed < target + 4)) {
            std::lock_guard lock{ swrs_guard_mutex };
            swrs_guard_writers[info->ContextRecord->Rip]++;
            // One stack per distinct caller chain (frames past the exception dispatch).
            void* frames[31];
            ULONG hash = 0;
            const USHORT n = RtlCaptureStackBackTrace(0, 31, frames, &hash);
            if (swrs_guard_stacks.size() < 8 && swrs_guard_stacks.find(hash) == swrs_guard_stacks.end()) {
                std::vector<DWORD64> stack{ info->ContextRecord->Rip };
                for (USHORT i = 0; i < n; i++) {
                    stack.push_back((DWORD64)frames[i]);
                }
                swrs_guard_stacks[hash] = stack;
            }
        }
        swrs_guard_rearm = true;
        info->ContextRecord->EFlags |= 0x100; // single step, then re-arm
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((code == EXCEPTION_SINGLE_STEP) && swrs_guard_rearm) {
        swrs_guard_rearm = false;
        if (swrs_guard_active.load()) {
            DWORD old;
            VirtualProtect(swrs_guard_page.load(), 1, PAGE_READWRITE | PAGE_GUARD, &old);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void swrs_guard_thread() {
    const char* env = getenv("SWRS_GUARD_ADDR");
    if (env == nullptr) return;
    uint32_t addr = (uint32_t)strtoul(env, nullptr, 16);
    const char* delay_env = getenv("SWRS_GUARD_DELAY");
    std::this_thread::sleep_for(std::chrono::seconds(delay_env ? atoi(delay_env) : 85));
    // SWRS_GUARD_ADDR=terrain: the patch list head of the terrain object the emitter saw last.
    if (strcmp(env, "terrain") == 0) {
        addr = swrs_last_terrain_obj.load() + 0x20;
    }
    uint8_t* rdram = swrs_rdram_seen.load();
    if (rdram == nullptr) { fprintf(stderr, "[guard] no rdram\n"); return; }
    const uintptr_t host = (uintptr_t)(rdram + ((addr & ~3u) - 0x80000000u));
    swrs_guard_target = host;
    swrs_guard_page = (void*)(host & ~uintptr_t(0xFFF));
    AddVectoredExceptionHandler(0, swrs_guard_handler);
    swrs_guard_active = true;
    DWORD old;
    VirtualProtect(swrs_guard_page.load(), 1, PAGE_READWRITE | PAGE_GUARD, &old);
    fprintf(stderr, "[guard] armed on 0x%08X\n", addr);
    const char* seconds_env = getenv("SWRS_GUARD_SECONDS");
    std::this_thread::sleep_for(std::chrono::seconds(seconds_env ? atoi(seconds_env) : 10));
    swrs_guard_active = false;
    VirtualProtect(swrs_guard_page.load(), 1, PAGE_READWRITE, &old);
    std::lock_guard lock{ swrs_guard_mutex };
    for (const auto& [rip, count] : swrs_guard_writers) {
        fprintf(stderr, "[guard] write x%u from:\n", count);
        swrs_print_frame("writer", rip);
    }
    for (const auto& [hash, stack] : swrs_guard_stacks) {
        fprintf(stderr, "[guard] write stack %08lX:\n", hash);
        for (DWORD64 pc : stack) {
            swrs_print_frame("frame", pc);
        }
    }
    fprintf(stderr, "[guard] done\n");
}

static const bool swrs_guard_started = []() {
    std::thread(swrs_guard_thread).detach();
    return true;
}();

// Bring-up: with SWRS_FBDUMP_DELAY=<s>, save the two gameplay framebuffers (400x448 RGBA16 at 0x80651000 and
// 0x806A8800) from RDRAM every 50 ms for SWRS_FBDUMP_SECONDS (default 15) to C:\projects\swrs-fbdump\.
static void swrs_fbdump_thread() {
    const char* delay_env = getenv("SWRS_FBDUMP_DELAY");
    if (delay_env == nullptr) return;
    std::this_thread::sleep_for(std::chrono::seconds(atoi(delay_env)));
    const char* seconds_env = getenv("SWRS_FBDUMP_SECONDS");
    const int count = (seconds_env ? atoi(seconds_env) : 15) * 20;
    uint8_t* rdram = swrs_rdram_seen.load();
    if (rdram == nullptr) return;
    CreateDirectoryA("C:/projects/swrs-fbdump", nullptr);
    const uint32_t fbs[2] = { 0x00651000, 0x006A8800 };
    const uint32_t bytes = 400 * 448 * 2;
    for (int i = 0; i < count; i++) {
        char path[128];
        snprintf(path, sizeof(path), "C:/projects/swrs-fbdump/%04d.raw", i);
        if (FILE* f = fopen(path, "wb")) {
            for (uint32_t fb : fbs) {
                fwrite(rdram + fb, 1, bytes, f);
            }
            fclose(f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    fprintf(stderr, "[fbdump] done\n");
}

static const bool swrs_fbdump_started = []() {
    std::thread(swrs_fbdump_thread).detach();
    return true;
}();

// Bring-up: with SWRS_RAMDUMP_DELAY=<s>, write all of RDRAM (8 MB, word-swapped) to C:\projects\swrs-ram.bin once.
static const bool swrs_ramdump_started = []() {
    std::thread([]() {
        const char* delay_env = getenv("SWRS_RAMDUMP_DELAY");
        if (delay_env == nullptr) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(int(atof(delay_env) * 1000)));
        uint8_t* rdram = swrs_rdram_seen.load();
        if (rdram == nullptr) return;
        if (FILE* f = fopen("C:/projects/swrs-ram.bin", "wb")) {
            fwrite(rdram, 1, 0x800000, f);
            fclose(f);
            fprintf(stderr, "[swrs] RAM dumped (timed)\n");
        }
    }).detach();
    return true;
}();
#endif

// Ring mode (swrs_trace_on == 2, env SWRS_TRACE_RING=1): remember the most recent calls.
static constexpr uint32_t swrs_ring_size = 1 << 16;
static const char* swrs_ring[swrs_ring_size];
static std::atomic<uint32_t> swrs_ring_pos{0};

extern "C" void swrs_trace_hit(const char* name) {
    if (swrs_trace_on == 2) {
        swrs_ring[swrs_ring_pos.fetch_add(1, std::memory_order_relaxed) & (swrs_ring_size - 1)] = name;
        return;
    }
    std::lock_guard lock{ swrs_trace_mutex };
    swrs_trace_counts[name]++;
}

// Writes the call ring to C:\projects\swrs-ring.txt (most recent call last) and stops recording.
static void swrs_dump_ring() {
    swrs_trace_on = 0;
    uint32_t end = swrs_ring_pos.load();
    if (FILE* f = fopen("C:/projects/swrs-ring.txt", "w")) {
        const char* prev = nullptr;
        uint32_t run = 0;
        for (uint32_t i = end - swrs_ring_size; i != end; i++) {
            const char* n = swrs_ring[i & (swrs_ring_size - 1)];
            if (n == prev) { run++; continue; }
            if (prev) fprintf(f, "%s x%u\n", prev, run);
            prev = n; run = 1;
        }
        if (prev) fprintf(f, "%s x%u\n", prev, run);
        fclose(f);
    }
}

// Write watch (bring-up aid): when the 16 bytes at `addr` (given big-endian, as in the ROM)
// appear in RDRAM, dump the call ring to C:\projects\swrs-ring.txt.
static void swrs_ring_watcher() {
    const char* env = getenv("SWRS_TRACE_RING");
    if (env == nullptr) {
        return;
    }
    swrs_trace_on = 2;
    // SWRS_WATCH_WORD=<hex addr>: fire when that 32-bit word becomes nonzero. Otherwise the
    // last tile of save slot 1's mission picture (QC019 of the first set), as seen in a dump.
    const char* word_env = getenv("SWRS_WATCH_WORD");
    const bool watch_word = word_env != nullptr;
    const uint32_t addr = watch_word ? (uint32_t)strtoul(word_env, nullptr, 16) : 0x80642E10;
    const uint8_t ref_be[16] = { 0x62,0x4b,0x62,0xcd,0x7b,0x53,0x8b,0xd7,0x94,0x17,0xa4,0x9d,0xbd,0x21,0xac,0x9d };
    uint8_t ref[16];
    for (int i = 0; i < 16; i += 4) {
        for (int k = 0; k < 4; k++) ref[i + k] = ref_be[i + 3 - k];
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        uint8_t* rdram = swrs_rdram_seen.load();
        if (rdram == nullptr) {
            continue;
        }
        if (watch_word) {
            const uint32_t value = *reinterpret_cast<uint32_t*>(rdram + (addr - 0x80000000));
            if (value == 0) continue;
            fprintf(stderr, "[swrs] watched word 0x%08X = 0x%08X\n", addr, value);
        } else if (memcmp(rdram + (addr - 0x80000000), ref, 16) != 0) {
            continue;
        }
        swrs_dump_ring();
        fprintf(stderr, "[swrs] write watch hit at 0x%08X, call ring written\n", addr);
        return;
    }
}

static const bool swrs_ring_watcher_started = []() {
    std::thread(swrs_ring_watcher).detach();
    return true;
}();

static void swrs_trace_watcher() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::error_code ec;
        if (!std::filesystem::remove("C:/projects/swrs-trace-now", ec)) {
            continue;
        }
        {
            std::lock_guard lock{ swrs_trace_mutex };
            swrs_trace_counts.clear();
        }
        swrs_trace_on = 1;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        swrs_trace_on = 0;
        std::vector<std::pair<const char*, uint64_t>> sorted;
        {
            std::lock_guard lock{ swrs_trace_mutex };
            sorted.assign(swrs_trace_counts.begin(), swrs_trace_counts.end());
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (FILE* f = fopen("C:/projects/swrs-trace.txt", "w")) {
            for (const auto& [name, count] : sorted) {
                fprintf(f, "%10llu %s\n", (unsigned long long)count, name);
            }
            fclose(f);
        }
        fprintf(stderr, "[swrs] trace written: %zu functions\n", sorted.size());
    }
}

static const bool swrs_trace_watcher_started = []() {
    std::thread(swrs_trace_watcher).detach();
    return true;
}();

// Bring-up logging hooks (recomp/swrs.toml). Throttled to a few lines per second per site.
static bool swrs_throttle(int site) {
    static std::chrono::steady_clock::time_point last[8];
    auto now = std::chrono::steady_clock::now();
    if (now - last[site] < std::chrono::milliseconds(500)) {
        return false;
    }
    last[site] = now;
    return true;
}

// RAM dump (bring-up aid): creating C:\projects\swrs-ramdump-now writes RDRAM (8 MB, in the
// runtime's word-swapped layout) to C:\projects\swrs-ram.bin. Checked from the front end's job poll hook and
// from every graphics task (rt64_render_context.cpp send_dl), so it also works in missions and cutscenes.
void swrs_check_ram_dump(uint8_t* rdram) {
    static std::chrono::steady_clock::time_point last;
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(250)) {
        return;
    }
    last = now;
    std::error_code ec;
    if (!std::filesystem::remove("C:/projects/swrs-ramdump-now", ec)) {
        return;
    }
    if (FILE* f = fopen("C:/projects/swrs-ram.bin", "wb")) {
        fwrite(rdram, 1, 0x800000, f);
        fclose(f);
        fprintf(stderr, "[swrs] RAM dumped\n");
    }
}

// func_800075F4(id): poll job `id` for a message. At 0x80007624 $s0 is the job record found
// (0 if none) and $a0 still the id.
extern "C" void swrs_log_job_poll(uint8_t* rdram, recomp_context* ctx) {
    swrs_rdram_seen = rdram;
    swrs_check_ram_dump(rdram);
    if (swrs_throttle(0)) {
        uint32_t rec = (uint32_t)ctx->r16;
        fprintf(stderr, "[swrs] job poll: id 0x%08X record 0x%08X queue 0x%08X\n",
            (uint32_t)ctx->r4, rec, rec ? rec + 0x1D0 : 0);
    }
}

// func_80005DCC(a0, a1, a2): add DMA requests for the worker thread 80005CA0.
extern "C" void swrs_log_dma_enqueue(uint8_t* rdram, recomp_context* ctx) {
    if (swrs_throttle(1)) {
        fprintf(stderr, "[swrs] dma enqueue: a0 0x%08X a1 0x%08X a2 0x%08X\n",
            (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6);
    }
}

// ovl1_func_800AF450 (front-end 3-slot manager), after its mailbox poll at 0x800AF4C8:
// $v0 = got a message, byte at $sp+0x10 = the message's id (+0x17). Slots: 0x800C0970 + i*0x48,
// state +0x41, id +0x20, timer (float) +0x18.
extern "C" void swrs_log_fe_slots(uint8_t* rdram, recomp_context* ctx) {
    const bool got = (ctx->r2 & 0xFF) != 0;
    if (!got && !swrs_throttle(2)) {
        return;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "[swrs] fe slots: msg %s", got ? "yes" : "no");
    if (got) {
        n += snprintf(line + n, sizeof(line) - n, " id %u", (unsigned)MEM_BU(0x10, ctx->r29));
    }
    for (int i = 0; i < 3; i++) {
        const gpr slot = (gpr)(int32_t)(0x800C0970 + i * 0x48);
        const uint32_t timer = (uint32_t)MEM_W(0x18, slot);
        float t;
        memcpy(&t, &timer, sizeof(t));
        n += snprintf(line + n, sizeof(line) - n, " | %d: state %u id %u t %.2f", i,
            (unsigned)MEM_BU(0x41, slot), (unsigned)MEM_BU(0x20, slot), t);
    }
    fprintf(stderr, "%s\n", line);
}

// Front-end picture worker (ovl1 thread 0x800A4780) logging, first 400 lines.
static std::atomic<int> swrs_pic_lines{0};
static bool swrs_pic_ok() { return swrs_pic_lines.fetch_add(1) < 400; }

static void swrs_dump_words(const char* tag, uint8_t* rdram, uint32_t addr, int n) {
    char line[256];
    int k = snprintf(line, sizeof(line), "[pic] %s 0x%08X:", tag, addr);
    if (addr < 0x80000000 || addr + n * 4 > 0x80800000 || (addr & 3) != 0) {
        fprintf(stderr, "%s (not a pointer)\n", line);
        return;
    }
    for (int i = 0; i < n && k < 230; i++) {
        k += snprintf(line + k, sizeof(line) - k, " %08X", (uint32_t)MEM_W(i * 4, (gpr)(int32_t)addr));
    }
    fprintf(stderr, "%s\n", line);
}

// At 0x800A47C4: $s3 = the job message just received.
extern "C" void swrs_log_pic_job(uint8_t* rdram, recomp_context* ctx) {
    if (!swrs_pic_ok()) return;
    uint32_t job = (uint32_t)ctx->r19;
    swrs_dump_words("job", rdram, job, 24);
    uint32_t desc = (uint32_t)MEM_W(0x14, (gpr)(int32_t)job);
    if (desc >= 0x80000000 && desc < 0x80800000) swrs_dump_words("desc", rdram, desc, 16);
}

// func_8001D284(a0, a1 = texture info, a2): create a texture.
static bool swrs_on_worker(uint8_t* rdram) {
    PTR(OSThread) t = ultramodern::this_thread();
    return t != NULLPTR && TO_PTR(OSThread, t)->id == 7;
}

extern "C" void swrs_log_tex_create(uint8_t* rdram, recomp_context* ctx) {
    if (!swrs_on_worker(rdram) || !swrs_pic_ok()) return;
    fprintf(stderr, "[pic] create a0 0x%08X a2 0x%08X\n", (uint32_t)ctx->r4, (uint32_t)ctx->r6);
    swrs_dump_words("  info", rdram, (uint32_t)ctx->r5, 8);
}

// func_8001CC38(handle, a1, a2 = texture info, a3): update a texture.
extern "C" void swrs_log_tex_update(uint8_t* rdram, recomp_context* ctx) {
    if (!swrs_on_worker(rdram) || !swrs_pic_ok()) return;
    fprintf(stderr, "[pic] update handle %u a1 0x%08X a3 0x%08X\n", (uint32_t)(ctx->r4 & 0xFFFF), (uint32_t)ctx->r5, (uint32_t)ctx->r7);
    swrs_dump_words("  info", rdram, (uint32_t)ctx->r6, 8);
}

// Front-end picture worker (ovl1 thread 0x800A4780), after it posts a job's reply at
// 0x800A493C (hook before 0x800A4944). The reply is a message struct on the worker's own
// stack ($sp + 0x10), reused for every job; the receiver reads the job id from it later,
// when it polls its mailbox (once per frame). On the N64 decoding a picture takes long
// enough that the front end reads each reply before the next job overwrites it. Here the
// worker finishes all three save-slot pictures within one frame, so every queued reply
// shows the last job's id and two slots never get their picture (black box on SELECT
// GAME). Keep the hardware's order: wait until the reply has left the receiver's mailbox.
extern "C" void swrs_wait_reply_consumed(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t msg = (uint32_t)ctx->r29 + 0x10;
    // Receiver's mailbox: job record with id *(job + 0x58), mailbox at record + 0x1D0
    // (the lookup func_800076A8 does; record list at 0x801044C0, next at +0x18).
    const uint32_t target = (uint32_t)MEM_W(0x58, ctx->r19);
    uint32_t rec = (uint32_t)MEM_W(0, (gpr)(int32_t)0x801044C0);
    while (rec != 0 && (uint32_t)MEM_W(0, (gpr)(int32_t)rec) != target) {
        rec = (uint32_t)MEM_W(0x18, (gpr)(int32_t)rec);
    }
    if (rec == 0) {
        return;
    }
    const gpr mq = (gpr)(int32_t)(rec + 0x1D0);
    auto queued = [&]() {
        const int32_t count = MEM_W(0x8, mq), first = MEM_W(0xC, mq), size = MEM_W(0x10, mq);
        const gpr buf = (gpr)MEM_W(0x14, mq);
        for (int32_t i = 0; i < count && size > 0; i++) {
            if ((uint32_t)MEM_W(((first + i) % size) * 4, buf) == msg) {
                return true;
            }
        }
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (queued() && std::chrono::steady_clock::now() < deadline) {
        swrs_spin_wait_yield(rdram, ctx);
    }
}

// After each call to the front end's mailbox poll (ovl1 800A512C): $v0 = got a message,
// the message id byte was stored at $sp + off.
extern "C" void swrs_log_fe_poll(uint8_t* rdram, recomp_context* ctx, uint32_t site, int off) {
    if ((ctx->r2 & 0xFF) != 0) {
        fprintf(stderr, "[pic] poll at 0x%08X got id %u\n", site, (unsigned)MEM_BU(off, ctx->r29));
    }
}

// load_overlay(n) at 0x800012D0 (hook at its start, $a0 = n). The three overlays share vram
// 0x800960E0. Calls into them are resolved when recompiling, but function pointers (thread
// entries, callbacks) go through the runtime's function map, which only holds what was
// loaded at boot (main + overlay 0). Swap the map's entries to the overlay being loaded.
extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);   // librecomp overlays.cpp
extern "C" void unload_overlays(int32_t ram_addr, uint32_t size);

void swrs_set_force_original_aspect(bool force); // rt64_render_context.cpp
extern std::atomic<float> swrs_cull_aspect_scale; // rt64_render_context.cpp

// Hooked right after func_8001780C (object visibility test) loads the 4:3 aspect from 0x8010C438 into $f2:
// widen the horizontal test by the Expand factor. The projection itself stays 4:3 (RT64 widens it).
// Hooked in func_8005A8D4 (box-vs-view test): divide a projected x by the Expand factor, which widens the test
// horizontally. reg is the float register holding it ($f6 or $f2).
extern "C" void swrs_cull_narrow_x(uint8_t* rdram, recomp_context* ctx, int reg) {
    const float scale = swrs_cull_aspect_scale.load();
    if (scale > 1.0f) {
        if (reg == 6) {
            ctx->f6.fl /= scale;
        }
        else {
            ctx->f2.fl /= scale;
        }
    }
}

// Bring-up: terrain emitter (func_8000D58C(?, ?, obj)): log the object, its flags and the length of its patch
// list (obj+0x20, linked through the first word) with SWRS_TERRAIN_LOG set.
extern "C" void swrs_log_terrain(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = std::getenv("SWRS_TERRAIN_LOG") != nullptr;
    static uint32_t logs = 0;
    if (!enabled) return;
    if (logs >= 3000) {
        const int32_t obj = (int32_t)ctx->r6;
        if (MEM_W(0xC, obj) == (int32_t)0x80040000) swrs_last_terrain_obj = (uint32_t)obj;
        return;
    }
    logs++;
    const int32_t obj = (int32_t)ctx->r6;
    if (MEM_W(0xC, obj) == (int32_t)0x80040000) {
        swrs_last_terrain_obj = (uint32_t)obj; // the main terrain object
    }
    uint32_t count = 0;
    int32_t patch = MEM_W(0x20, obj);
    uint32_t first = (uint32_t)patch;
    while (patch != 0 && count < 2000) {
        count++;
        patch = MEM_W(0, patch);
    }
    if ((logs == 1200) && (swrs_trace_on == 2)) {
        swrs_dump_ring(); // the calls that built this terrain list (with SWRS_TRACE_RING)
        printf("[terrain] call ring written\n");
    }
    printf("[terrain] obj 0x%08X flags 0x%08X 0x2C 0x%08X patches %u first 0x%08X frame %d\n",
        (uint32_t)obj, (uint32_t)MEM_W(0xC, obj), (uint32_t)MEM_W(0x2C, obj), count, first, MEM_W(0, (int32_t)0x8010C48C));
}

// Hooked in func_8003AB80 (terrain patch selection) after the view pyramid's corner ray at sp+0x40 (x, y, z
// floats) has been normalized and scaled to the draw distance: scale its horizontal component by the Expand
// factor so terrain in the extended side area is selected. Doing it before the normalize shortened the forward
// reach, which cut the terrain off before the fog band (pop-in instead of fading in).
extern "C" void swrs_terrain_widen(uint8_t* rdram, recomp_context* ctx) {
    // Bring-up switch (pyramid blinking study): SWRS_NO_TERRAIN_WIDEN keeps the 4:3 terrain selection.
    static const bool disabled = std::getenv("SWRS_NO_TERRAIN_WIDEN") != nullptr;
    const float scale = swrs_cull_aspect_scale.load();
    if (!disabled && (scale > 1.0f)) {
        uint32_t bits = (uint32_t)MEM_W(0x40, ctx->r29);
        float x;
        memcpy(&x, &bits, sizeof(x));
        x *= scale;
        memcpy(&bits, &x, sizeof(x));
        MEM_W(0x40, ctx->r29) = (int32_t)bits;
    }
}

extern "C" void swrs_cull_widen(uint8_t* rdram, recomp_context* ctx) {
    // Bring-up switch (pyramid blinking study): SWRS_NO_CULL_WIDEN keeps the 4:3 object test.
    static const bool disabled = std::getenv("SWRS_NO_CULL_WIDEN") != nullptr;
    const float scale = swrs_cull_aspect_scale.load();
    if (!disabled && (scale > 1.0f)) {
        ctx->f2.fl *= scale;
    }
}

// Input recording and replay (bring-up aid for repeatable in-mission tests).
// Each load_overlay(0) (a mission or the attract demo starting) begins a new segment. Positions in a segment are
// keyed on the game clock at 0x8010C48C (about 120 ticks a second in flight), relative to its value at the segment's
// first controller poll, plus the number of polls since the clock last changed: the clock stands still while the
// mission loads and during the opening flyby, and there the poll order decides.
// SWRS_INPUT_RECORD=<path>: controller 1 is written to <path>_seg<N>.bin, one 12-byte entry per poll
//   (s32 relative tick, u16 polls since the tick changed, u16 buttons, s8 stick x, s8 stick y, u16 zero; little-endian).
// SWRS_INPUT_REPLAY=<file>: from the first segment of this run (SWRS_INPUT_REPLAY_SEG=<N> picks another), controller 1
//   reads the recording instead: the last entry at or before the current (tick, poll) key. Neutral after the end.
namespace {
    struct InputEntry {
        int32_t tick;
        uint16_t sub;
        uint16_t buttons;
        int8_t stickX;
        int8_t stickY;
        uint16_t reserved;
    };
    static_assert(sizeof(InputEntry) == 12, "InputEntry layout");

    bool input_key_le(const InputEntry& e, int32_t tick, uint32_t sub) {
        return (e.tick < tick) || ((e.tick == tick) && (e.sub <= sub));
    }

    std::mutex input_mutex;
    int input_segment = -1;             // segments seen so far - 1
    bool input_segment_started = false; // first poll of the segment seen (base tick known)
    int32_t input_base_tick = 0;
    int32_t input_last_tick = 0;
    uint32_t input_sub = 0;
    FILE* input_record_file = nullptr;
    std::vector<InputEntry> input_replay;
    size_t input_replay_pos = 0;
    bool input_replay_active = false;
    bool input_replay_done_logged = false;
}

static void swrs_input_new_segment() {
    std::lock_guard lock{ input_mutex };
    input_segment++;
    input_segment_started = false;
    static const char* record_path = std::getenv("SWRS_INPUT_RECORD");
    static const char* replay_path = std::getenv("SWRS_INPUT_REPLAY");
    static const int replay_seg = std::getenv("SWRS_INPUT_REPLAY_SEG") ? std::atoi(std::getenv("SWRS_INPUT_REPLAY_SEG")) : 0;
    if (record_path != nullptr) {
        if (input_record_file != nullptr) {
            fclose(input_record_file);
        }
        const std::string path = std::string(record_path) + "_seg" + std::to_string(input_segment) + ".bin";
        input_record_file = fopen(path.c_str(), "wb");
        fprintf(stderr, "[input] recording segment %d to %s\n", input_segment, path.c_str());
    }
    input_replay_active = false;
    if ((replay_path != nullptr) && (input_segment == replay_seg)) {
        input_replay.clear();
        input_replay_pos = 0;
        input_replay_done_logged = false;
        if (FILE* f = fopen(replay_path, "rb")) {
            InputEntry e;
            while (fread(&e, sizeof(e), 1, f) == 1) {
                input_replay.push_back(e);
            }
            fclose(f);
        }
        input_replay_active = !input_replay.empty();
        fprintf(stderr, "[input] replaying %zu entries from %s in segment %d\n", input_replay.size(), replay_path, input_segment);
    }
}

extern "C" void swrs_input_filter(uint8_t* rdram, int32_t data) {
    std::lock_guard lock{ input_mutex };
    if (input_segment < 0) {
        return;
    }

    const int32_t now = MEM_W(0, (int32_t)0x8010C48C);
    if (!input_segment_started) {
        input_segment_started = true;
        input_base_tick = now;
        input_last_tick = 0;
        input_sub = 0;
    }

    // The game resets its clock shortly after the overlay loads: start counting from there.
    if (now - input_base_tick < input_last_tick) {
        input_base_tick = now;
        input_last_tick = -1;
        fprintf(stderr, "[input] game clock reset, rebased\n");
    }

    const int32_t tick = now - input_base_tick;
    if (tick != input_last_tick) {
        input_last_tick = tick;
        input_sub = 0;
    }
    else {
        input_sub = std::min<uint32_t>(input_sub + 1, 0xFFFF);
    }

    // Bring-up: map wall-clock time (the HHmmss_fff format of the session tools' frame names) to game ticks, a
    // line about every 100 ms; and SWRS_RDP_LOG_AT_TICKS=<t1,t2,...> starts the RDP/F5 log at those ticks.
    {
        static int32_t last_printed = -1000;
        if ((input_sub == 0) && (tick - last_printed >= 12)) {
            last_printed = tick;
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(stderr, "[tick] %02u%02u%02u_%03u %d\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tick);
        }

        static std::vector<int32_t> log_ticks = [] {
            std::vector<int32_t> v;
            if (const char* s = std::getenv("SWRS_RDP_LOG_AT_TICKS")) {
                std::string str(s);
                size_t pos = 0;
                while (pos < str.size()) {
                    size_t next = str.find(',', pos);
                    v.push_back(std::atoi(str.substr(pos, next - pos).c_str()));
                    pos = (next == std::string::npos) ? str.size() : next + 1;
                }
            }
            return v;
        }();
        static size_t next_log = 0;
        if (input_replay_active && (next_log < log_ticks.size()) && (tick >= log_ticks[next_log])) {
            next_log++;
            if (FILE* f = fopen("C:/projects/swrs-rdp-log-now", "wb")) {
                fclose(f);
            }
            fprintf(stderr, "[input] RDP log requested at tick %d\n", tick);
        }
    }

    if (input_replay_active) {
        while ((input_replay_pos + 1 < input_replay.size()) && input_key_le(input_replay[input_replay_pos + 1], tick, input_sub)) {
            input_replay_pos++;
        }

        InputEntry e = input_replay[input_replay_pos];
        if (!input_key_le(e, tick, input_sub)) {
            e = {};
        }
        else if (tick > input_replay.back().tick + 120) {
            // A second past the last entry: neutral.
            e = {};
            if (!input_replay_done_logged) {
                input_replay_done_logged = true;
                fprintf(stderr, "[input] replay finished at tick %d\n", tick);
            }
        }

        MEM_H(0, data) = e.buttons;
        MEM_B(2, data) = e.stickX;
        MEM_B(3, data) = e.stickY;
    }

    if (input_record_file != nullptr) {
        const InputEntry e = { tick, (uint16_t)input_sub, (uint16_t)MEM_HU(0, data), (int8_t)MEM_B(2, data), (int8_t)MEM_B(3, data), 0 };
        fwrite(&e, sizeof(e), 1, input_record_file);
        fflush(input_record_file);
    }
}

extern "C" void swrs_register_overlay(uint8_t* rdram, recomp_context* ctx) {
    struct Overlay { uint32_t rom, size; };
    static const Overlay overlays[3] = {
        { 0x096CE0, 0x671E0 },
        { 0x0FDEC0, 0x287A0 },
        { 0x126660, 0x0B840 },
    };
    static int current = 0; // overlay 0 is part of the boot image
    const int n = (int)(int32_t)ctx->r4;
    // Overlay 0 is in-flight gameplay (missions and the attract demo); only that is widened with Expand.
    if (n >= 0 && n <= 2) {
        swrs_set_force_original_aspect(n != 0);
    }
    if (n == 0) {
        swrs_input_new_segment();
    }
    if (n < 0 || n > 2 || n == current) {
        return;
    }
    unload_overlays((int32_t)0x800960E0, overlays[0].size); // largest of the three
    load_overlays(overlays[n].rom, (int32_t)0x800960E0, overlays[n].size);
    current = n;
    fprintf(stderr, "[swrs] overlay %d registered\n", n);
}

// Test switch (SWRS_TEST_AUDIO_IDLE=1): report all audio voices as idle, to check whether
// the game waits on audio that the placeholder microcode never finishes.
extern "C" int swrs_test_audio_idle(void) {
    static const int enabled = getenv("SWRS_TEST_AUDIO_IDLE") != nullptr;
    return enabled;
}

// Functions N64Recomp leaves out ([patches] ignored). Until they're reimplemented, stop
// with a clear message if the game ever reaches one.
static void not_implemented(const char* name) {
    fprintf(stderr, "[swrs] reached unimplemented function %s\n", name);
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, name);
}

// The hand-written assembly module at 0x800918A0 (GSM speech decoder) is translated by
// hand in swrs_gsm.cpp.


// libultra functions N64Recomp lists as ignored (internal hardware access) that the game
// calls directly, so the runtime has to provide them.

// u32 osViGetCurrentField(void): which interlace field is being shown. Called by the
// thread at 0x80014260. No interlacing here, so always field 0.
extern "C" void osViGetCurrentField_recomp(uint8_t*, recomp_context* ctx) {
    ctx->r2 = 0;
}

// void osDpGetCounters(u32* array): RDP performance counters (clock, buffer busy, pipe
// busy, TMEM). Called by func_8000A3A8; only statistics, so report zeros.
extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    for (int i = 0; i < 4; i++) {
        MEM_W(i * 4, ctx->r4) = 0;
    }
}

// Majora's Mask debug warp list, still referenced by the shared debug menu code. The menu
// reads game_warps[0].scenes[0] while static objects are constructed, so it can't be empty.
#include "zelda_debug.h"
namespace zelda64 {
    std::vector<AreaWarps> game_warps{
        { "None", { { 0, "None", { "None" } } } },
    };
}
