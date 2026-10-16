#pragma once

// FEA-0010: runs a function on a new detached Win32 thread. std::thread is not usable in MyCommanderTests (its
// include path has MyCommander\Process.h, which shadows the system <process.h> that <thread> needs; JobWorker.cpp
// has the same note), and the plugin shares these sources with the tests, so both use CreateThread.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>

namespace netinfo {

namespace detail {
struct DetachedJob {
    std::function<void()> fn;
    HMODULE module = nullptr;  // this module, with one extra reference held until the thread is done
};

inline DWORD WINAPI DetachedThreadMain(LPVOID parameter) {
    DetachedJob* job = static_cast<DetachedJob*>(parameter);
    try {
        job->fn();
    } catch (...) {
    }
    HMODULE module = job->module;
    delete job;  // destroys the function object, which is code of this module, so before it can be unmapped
    // The host may have unloaded the plugin (FreeLibrary) while this worker was still inside a slow Windows
    // call. The extra reference taken in RunDetached kept the code mapped; dropping it and ending the thread in
    // one call is the only safe way to leave, because returning would run unmapped code if this was the last one.
    if (module != nullptr) FreeLibraryAndExitThread(module, 0);
    return 0;
}
}  // namespace detail

// False when the thread could not be created (the function is not run). While the function runs, the module
// holding this code (the plugin DLL) cannot be unmapped, even if the host unloads the plugin meanwhile.
inline bool RunDetached(std::function<void()> fn) {
    auto* job = new detail::DetachedJob{std::move(fn), nullptr};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&detail::DetachedThreadMain),
                       &job->module);  // adds a reference; stays null if it fails (then no pin, as before)
    HANDLE h = CreateThread(nullptr, 0, &detail::DetachedThreadMain, job, 0, nullptr);
    if (!h) {
        if (job->module != nullptr) FreeLibrary(job->module);
        delete job;
        return false;
    }
    CloseHandle(h);
    return true;
}

}  // namespace netinfo
