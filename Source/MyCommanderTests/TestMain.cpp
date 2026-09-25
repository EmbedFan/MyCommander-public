#include "TestFramework.h"

#include <windows.h>
#include <objbase.h>

#pragma comment(lib, "ole32.lib")

int wmain() {
    // DeleteItems' Recycle Bin path goes through SHFileOperationW, same as
    // production code, so COM is initialized the same way here.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = test::RunAll();
    CoUninitialize();
    return exitCode;
}
