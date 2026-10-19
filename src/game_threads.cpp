#include "game_threads.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>

namespace tongs {
namespace {

// Reads one pool's worker vector; false on a fault or an implausible vector.
bool ReadPool(std::uintptr_t vector, std::int64_t& workers, std::int64_t& capacity) {
    __try {
        std::uint64_t begin = 0, end = 0, cap = 0;  // MSVC std::vector: three pointers
        std::memcpy(&begin, reinterpret_cast<const void*>(vector), 8);
        std::memcpy(&end, reinterpret_cast<const void*>(vector + 8), 8);
        std::memcpy(&cap, reinterpret_cast<const void*>(vector + 16), 8);
        if (end < begin || cap < end || (end - begin) % 8 != 0 || (cap - begin) % 8 != 0) return false;
        workers = static_cast<std::int64_t>((end - begin) / 8);
        capacity = static_cast<std::int64_t>((cap - begin) / 8);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

int CountProcessThreads(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    int count = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid) ++count;
    }
    CloseHandle(snapshot);
    return count;
}

}  // namespace

ThreadsSnapshot ReadThreads(std::uintptr_t image_base, const ThreadsLayout& layout, std::uint32_t main_thread_id) {
    ThreadsSnapshot t;
    std::memcpy(&t.threads_setting, reinterpret_cast<const void*>(image_base + layout.threads_setting_rva), 4);
    bool pools_ok = true;
    for (int i = 0; i < 2; ++i) {
        t.pools[i].rva = layout.pool_rvas[i];
        if (!ReadPool(image_base + layout.vector_rvas[i], t.pools[i].workers, t.pools[i].capacity)) {
            t.pools[i].workers = t.pools[i].capacity = -1;
            pools_ok = false;
        }
    }
    DWORD_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    t.process_mask = process_mask;
    t.system_mask = system_mask;
    t.process_threads = CountProcessThreads(GetCurrentProcessId());
    t.main_thread_id = main_thread_id;
    if (main_thread_id != 0) {
        if (HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, main_thread_id)) {
            GROUP_AFFINITY affinity{};
            if (GetThreadGroupAffinity(thread, &affinity)) t.main_mask = affinity.Mask;
            PROCESSOR_NUMBER ideal{};
            if (GetThreadIdealProcessorEx(thread, &ideal)) t.main_ideal_cpu = ideal.Group * 64 + ideal.Number;
            t.main_priority = GetThreadPriority(thread);
            CloseHandle(thread);
        }
    }
    t.ok = pools_ok;
    if (!pools_ok) t.reason = "a pool's worker vector is not readable yet";
    return t;
}

}  // namespace tongs
