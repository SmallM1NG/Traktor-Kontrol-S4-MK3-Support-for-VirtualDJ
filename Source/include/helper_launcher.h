// ZH: DLL 的提权启动器：验证同包 EXE 完整性后启动助手；后台线程持有模块，直到助手退出再释放。
// EN: Elevated DLL launcher: validate the bundled helper before launch; retain the module until the helper exits.
#include "runtime_utils.h"
#pragma once
#include <bcrypt.h>
#include <cstring>
#include <new>
#include "release_identity.h"
#include <shellapi.h>
#include <objbase.h>
#include "release_digest.h"
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

inline bool helperHash(HANDLE file)
{
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE digest[32]{};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok)
        ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    BYTE buf[65536];
    DWORD got = 0;
    while (ok)
    {
        if (!ReadFile(file, buf, sizeof(buf), &got, nullptr))
        {
            ok = false;
            break;
        }
        if (!got)
            break;
        ok = BCryptHashData(hash, buf, got, 0) >= 0;
    }
    if (ok)
        ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65]{};
    for (unsigned i = 0; i < 32; ++i)
        sprintf_s(hex + 2 * i, 3, "%02x", digest[i]);
    return ok && !strcmp(hex, NativeHelperSha256);
}

struct NativeLaunch
{
    HANDLE stop = nullptr;
    HMODULE module = nullptr;
    std::wstring log, exe, token;
};

// ZH: 启动器写入 DLL 日志；EXE 从自身入口开始写独立日志，便于定位 UAC 前后的断点。
// EN: The launcher writes to the DLL log; the EXE starts its own log at entry to distinguish failures before and after elevation.
inline void nativeLaunchLog(const std::wstring &path, const char *event, DWORD value)
{
    runtimediag::pathEvent(path, "DLL.launcher", value ? "WARN" : "INFO", event, value);
}

inline DWORD WINAPI nativeLaunchThread(void *raw)
{
    auto c = static_cast<NativeLaunch *>(raw);
    nativeLaunchLog(c->log, "launcher_thread_started", 0);
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init))
        nativeLaunchLog(c->log, "com_initialize_failed", static_cast<DWORD>(init));
    HANDLE file = CreateFileW(c->exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        nativeLaunchLog(c->log, "missing_executable", GetLastError());
    else
    {
        if (!helperHash(file))
            nativeLaunchLog(c->log, "executable_hash_rejected", ERROR_INVALID_DATA);
        else if (WaitForSingleObject(c->stop, 0) == WAIT_TIMEOUT)
        {
            nativeLaunchLog(c->log, "helper_hash_validated_requesting_elevation", 0);
            wchar_t args[128];
            swprintf_s(args, L"--pid %lu --session %s --apply", GetCurrentProcessId(), c->token.c_str());
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.lpVerb = L"runas";
            info.lpFile = c->exe.c_str();
            info.lpParameters = args;
            info.nShow = SW_HIDE;
            if (!ShellExecuteExW(&info))
            {
                DWORD error = GetLastError();
                nativeLaunchLog(c->log, error == ERROR_CANCELLED ? "uac_cancelled_no_retry" : "launch_failed", error);
            }
            else
            {
                nativeLaunchLog(c->log, "launched_continuous", 0);
                if (info.hProcess)
                {
                    WaitForSingleObject(info.hProcess, INFINITE);
                    DWORD code = 0;
                    GetExitCodeProcess(info.hProcess, &code);
                    nativeLaunchLog(c->log, "exited", code);
                    CloseHandle(info.hProcess);
                }
            }
        }
        else
            nativeLaunchLog(c->log, "launch_cancelled_plugin_stopping", 0);
        CloseHandle(file);
    }
    nativeLaunchLog(c->log, "launcher_thread_exited", 0);
    if (SUCCEEDED(init))
        CoUninitialize();
    CloseHandle(c->stop);
    HMODULE module = c->module;
    delete c;
    FreeLibraryAndExitThread(module, 0);
}

inline void launchNativeHelper(HANDLE stop, const std::wstring &log, HINSTANCE instance, const wchar_t *token)
{
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(instance, path, 32768);
    if (!n || n >= 32768)
    {
        nativeLaunchLog(log, "launcher_module_path_failed", GetLastError());
        return;
    }
    auto c = new (std::nothrow) NativeLaunch;
    if (!c)
    {
        nativeLaunchLog(log, "launcher_allocation_failed", ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    c->log = log;
    c->token = token;
    c->exe.assign(path, n);
    auto slash = c->exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        nativeLaunchLog(log, "launcher_invalid_module_path", ERROR_BAD_PATHNAME);
        delete c;
        return;
    }
    c->exe.resize(slash + 1);
    c->exe += S4_HELPER_EXE;
    if (!DuplicateHandle(GetCurrentProcess(), stop, GetCurrentProcess(), &c->stop, SYNCHRONIZE, FALSE, 0))
    {
        nativeLaunchLog(log, "launcher_stop_duplicate_failed", GetLastError());
        delete c;
        return;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&NativeHelperSha256),
                            &c->module))
    {
        nativeLaunchLog(log, "launcher_module_pin_failed", GetLastError());
        CloseHandle(c->stop);
        delete c;
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, nativeLaunchThread, c, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
    {
        nativeLaunchLog(log, "launcher_thread_create_failed", GetLastError());
        CloseHandle(c->stop);
        FreeLibrary(c->module);
        delete c;
    }
}
