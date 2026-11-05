// ZH: 退出清理专用的短暂 NI 会话；不依赖 VirtualDJ 的管道，不发送启动电机的命令。
// EN: Short-lived NI shutdown connection independent of VirtualDJ's pipe. Never starts a motor.
#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <functional>
#include <cstring>
#include <algorithm>
#include <mutex>

namespace nicleanup
{
inline constexpr DWORD True = 0x74727565;
// One cleanup deadline spans original and recovery routes; discovery has a separate startup budget.
namespace timing {
// Reserve the final 250ms for process exit within the 2s shutdown limit.
constexpr DWORD CleanupMs=1750, OriginalMs=250, IoMs=200, VerifyMs=100, PollMs=10;
constexpr DWORD DiscoveryMs=1000, DiscoveryQuietMs=100, ExitGraceMs=250;
inline DWORD remaining(ULONGLONG end, DWORD cap, ULONGLONG now=GetTickCount64()) {
    return now < end ? static_cast<DWORD>((std::min)(end-now, static_cast<ULONGLONG>(cap))) : 0;
}
}


inline bool ports(const BYTE *data, size_t size, std::string &request, std::string &notification, bool deviceScope = false)
{
    auto word = [&](size_t at) { DWORD v = 0; memcpy(&v, data + at, 4); return v; };
    if (size < 12 || word(0) != True)
        return false;
    DWORD n = word(4);
    if (!n || n > 512 || size < size_t(12) + n)
        return false;
    DWORD m = word(8 + n);
    const size_t end = size_t(12) + n + m;
    if (!m || m > 512 || (size != end && size != end + 4))
        return false;
    // Device-scope registration can append a nonzero service tag; instance replies require zero.
    if (size == end + 4 && word(end) != 0 && !deviceScope)
        return false;
    auto name = [](const BYTE *p, DWORD length, std::string &out) {
        if (length < 2 || p[length - 1]) return false;
        for (DWORD i = 0; i + 1 < length; ++i)
            if (p[i] < 32 || p[i] > 126 || p[i] == '\\' || p[i] == '/') return false;
        out.assign(reinterpret_cast<const char *>(p), length - 1);
        return true;
    };
    return name(data + 8, n, request) && name(data + 12 + n, m, notification);
}

inline bool serialText(const BYTE *data, size_t size, std::string &out)
{
    if (size < 2 || size > 256 || data[size - 1]) return false;
    for (size_t i = 0; i + 1 < size; ++i)
        if (data[i] < 33 || data[i] > 126 || data[i] == '\\' || data[i] == '/') return false;
    out.assign(reinterpret_cast<const char *>(data), size - 1);
    return true;
}

// Device-state notifications describe online units; this never reads historical USB registry entries.
inline bool serialNotice(const BYTE *data, size_t size, std::string &serial, bool &present)
{
    if (size < 16) return false;
    DWORD op = 0, length = 0;
    memcpy(&op, data, 4); memcpy(&length, data + 12, 4);
    if (op != 0x03444e2b && op != 0x03444e2d) return false;
    if (length > size - 16 || !serialText(data + 16, length, serial)) return false;
    present = op == 0x03444e2b;
    return true;
}

class Connection
{
    HANDLE request = INVALID_HANDLE_VALUE, notification = INVALID_HANDLE_VALUE;
    HANDLE received = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE serialChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread reader;
    std::atomic<bool> stopping{false};
    std::atomic<DWORD> receiveError{0};
    ULONGLONG deadline = 0;
    DWORD service = 0;
    const char *step = "not_started";
    std::mutex serialMutex;
    std::vector<std::string> onlineSerials;
    bool serialOverflow = false;
    ULONGLONG lastSerialNotice=0;

    template<class Start> bool io(HANDLE handle, Start start, DWORD &count)
    {
        if (GetTickCount64() >= deadline) { SetLastError(ERROR_TIMEOUT); return false; }
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return false;
        OVERLAPPED ov{}; ov.hEvent = event;
        BOOL started = start(ov, count);
        DWORD error = started ? ERROR_SUCCESS : GetLastError();
        if (!started && error == ERROR_IO_PENDING)
        {
            const auto now = GetTickCount64();
            DWORD timeout = now < deadline ? timing::remaining(deadline, timing::IoMs, now) : 0;
            DWORD wait = WaitForSingleObject(event, timeout);
            if (wait == WAIT_OBJECT_0)
                error = GetOverlappedResult(handle, &ov, &count, FALSE) ? ERROR_SUCCESS : GetLastError();
            else
            {
                error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                CancelIoEx(handle, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(handle, &ov, &ignored, TRUE);
            }
        }
        CloseHandle(event);
        SetLastError(error);
        return error == ERROR_SUCCESS;
    }

    HANDLE open(const std::wstring &name)
    {
        HANDLE h = CreateFileW((L"\\\\.\\pipe\\" + name).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return h;
        ULONG pid = 0;
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!GetNamedPipeServerProcessId(h, &pid) || pid != service ||
            !SetNamedPipeHandleState(h, &mode, nullptr, nullptr))
        {
            CloseHandle(h);
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        return h;
    }

    static std::vector<BYTE> words(std::initializer_list<DWORD> values)
    {
        const auto *p = reinterpret_cast<const BYTE *>(values.begin());
        return std::vector<BYTE>(p, p + values.size() * sizeof(DWORD));
    }

    bool write(const std::vector<BYTE> &data)
    {
        if (GetTickCount64() >= deadline) { SetLastError(ERROR_TIMEOUT); return false; }
        DWORD count = 0;
        bool ok = io(request, [&](OVERLAPPED &ov, DWORD &n) {
            return WriteFile(request, data.data(), static_cast<DWORD>(data.size()), &n, &ov);
        }, count);
        if (ok && count != data.size()) { SetLastError(ERROR_WRITE_FAULT); return false; }
        return ok;
    }

  public:
    const char *stage() const { return step; }
    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection()
    {
        stopping = true;
        if (notification != INVALID_HANDLE_VALUE) CancelIoEx(notification, nullptr);
        if (reader.joinable()) reader.join();
        if (request != INVALID_HANDLE_VALUE) CloseHandle(request);
        if (notification != INVALID_HANDLE_VALUE) CloseHandle(notification);
        if (received) CloseHandle(received);
        if (serialChanged) CloseHandle(serialChanged);
    }

    // ZH: 注册和接管前再次检查同一设备/允许的归属；整个连接有固定时间预算。
    // EN: Recheck the same device and allowed owner before registration/focus; bound the whole session.
    bool connect(const std::string &serial, DWORD servicePid, const std::function<bool(unsigned)> &allowed,
                 const std::wstring &mainName = L"NIHWMainHandler", bool discovery = false, ULONGLONG until = 0)
    {
        const auto began=GetTickCount64();
        deadline = until ? until : began + (discovery ? timing::DiscoveryMs : timing::CleanupMs);
        service = servicePid;
        if (!received || !serialChanged || GetTickCount64() >= deadline || (!discovery && serial.empty()) || serial.size() > 255 || !allowed(0)) return false;
        step = "main_pipe";
        HANDLE main = open(mainName);
        if (main == INVALID_HANDLE_VALUE) return false;
        auto packet = words({discovery ? 0x03447500u : 0x03444900u, 0x1720, 0x4e694d32, 0x70726d79,
                             discovery ? 0u : static_cast<DWORD>(serial.size() + 1)});
        if (!discovery) { packet.insert(packet.end(), serial.begin(), serial.end()); packet.push_back(0); }
        BYTE reply[4096]{}; DWORD count = 0;
        step = "registration";
        bool ok = io(main, [&](OVERLAPPED &ov, DWORD &n) {
            return TransactNamedPipe(main, packet.data(), static_cast<DWORD>(packet.size()), reply,
                                      sizeof(reply), &n, &ov);
        }, count);
        DWORD error = GetLastError(); CloseHandle(main); SetLastError(error);
        std::string requestName, notificationName;
        step = "registration_reply";
        if (!ok) return false;
        if (!ports(reply, count, requestName, notificationName, discovery))
        { SetLastError(ERROR_INVALID_DATA); return false; }
        step = "registered_owner";
        if (!allowed(1)) { SetLastError(ERROR_ACCESS_DENIED); return false; }
        std::wstring notifyPath = L"\\\\.\\pipe\\" + std::wstring(notificationName.begin(), notificationName.end());
        notification = CreateNamedPipeW(notifyPath.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                        PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
        if (notification == INVALID_HANDLE_VALUE) return false;
        reader = std::thread([this] {
            DWORD count = 0;
            bool ok = io(notification, [&](OVERLAPPED &ov, DWORD &) {
                if (ConnectNamedPipe(notification, &ov)) return TRUE;
                if (GetLastError() == ERROR_PIPE_CONNECTED) return TRUE;
                return FALSE;
            }, count);
            ULONG client = 0;
            if (ok && (!GetNamedPipeClientProcessId(notification, &client) || client != service))
            { ok = false; SetLastError(ERROR_ACCESS_DENIED); }
            while (ok && !stopping)
            {
                BYTE data[4096]{};
                ok = io(notification, [&](OVERLAPPED &ov, DWORD &n) {
                    return ReadFile(notification, data, sizeof(data), &n, &ov);
                }, count);
                if (!ok && GetLastError() == ERROR_TIMEOUT && !stopping && GetTickCount64() < deadline)
                { ok = true; continue; }
                if (!ok || count < 4) break;
                std::string reported; bool present = false;
                if (serialNotice(data, count, reported, present))
                {
                    std::lock_guard<std::mutex> lock(serialMutex);
                    auto found = std::find(onlineSerials.begin(), onlineSerials.end(), reported);
                    if (present && found == onlineSerials.end())
                    {
                        if (onlineSerials.size() < 2) onlineSerials.push_back(reported);
                        else serialOverflow = true;
                    }
                    else if (!present && found != onlineSerials.end()) onlineSerials.erase(found);
                    lastSerialNotice=GetTickCount64();
                    SetEvent(serialChanged);
                }
                ok = io(notification, [&](OVERLAPPED &ov, DWORD &n) {
                    return WriteFile(notification, data, 0, &n, &ov);
                }, count);
                if (ok) SetEvent(received);
            }
            if (!stopping) { receiveError = ok ? ERROR_INVALID_DATA : GetLastError(); SetEvent(received); SetEvent(serialChanged); }
        });
        step = "request_pipe";
        request = open(std::wstring(requestName.begin(), requestName.end()));
        if (request == INVALID_HANDLE_VALUE || !allowed(1)) return false;
        packet = words({0x03404300, True, 0, static_cast<DWORD>(notificationName.size())});
        packet.insert(packet.end(), notificationName.begin(), notificationName.end()); packet.push_back(0);
        step = "notification_handshake";
        if (!write(packet) || !allowed(1) || !write(words({0x03447143})) ||
            WaitForSingleObject(received, timing::remaining(deadline,timing::IoMs)) != WAIT_OBJECT_0 || receiveError) return false;
        if (discovery)
        {
            step = "device_serial_notifications";
            for (;;) {
                if (!allowed(1) || receiveError) return false;
                const auto now=GetTickCount64();
                DWORD wait=timing::remaining(deadline,timing::DiscoveryMs,now);
                {
                    std::lock_guard<std::mutex> lock(serialMutex);
                    if (serialOverflow || onlineSerials.size()>1) { SetLastError(ERROR_MORE_DATA); return false; }
                    if (onlineSerials.size()==1) {
                        const auto quietEnd=lastSerialNotice+timing::DiscoveryQuietMs;
                        if (now>=quietEnd) return true;
                        wait=(std::min)(wait,timing::remaining(quietEnd,timing::DiscoveryQuietMs,now));
                    }
                }
                if (!wait) { SetLastError(ERROR_TIMEOUT); return false; }
                if (WaitForSingleObject(serialChanged,wait)==WAIT_FAILED) return false;
            }
        }

        const char label[] = "S4MK3 shutdown cleanup";
        packet = words({0x0349734e, 0x70001006, 0xf6b24000, sizeof(label)});
        packet.insert(packet.end(), label, label + sizeof(label));
        step = "session_start";
        if (!allowed(1) || !write(packet) || !allowed(1) || !write(words({0x03434300, 0x73747274}))) return false;
        step = "claimed_owner";
        const auto claimEnd=(std::min)(deadline,GetTickCount64()+timing::VerifyMs);
        while (GetTickCount64()<claimEnd)
        {
            if (allowed(2)) { step = "connected"; return true; }
            Sleep(timing::remaining(claimEnd,timing::PollMs));
        }
        return false;
    }

    bool discoverSerial(DWORD servicePid, const std::function<bool(unsigned)> &allowed, std::string &serial,
                        const std::wstring &mainName = L"NIHWMainHandler")
    {
        if (!connect("", servicePid, allowed, mainName, true)) return false;
        std::lock_guard<std::mutex> lock(serialMutex);
        if (serialOverflow || onlineSerials.size() != 1) return false;
        serial = onlineSerials.front();
        return true;
    }

    bool send(const void *data, size_t size)
    {
        if (request == INVALID_HANDLE_VALUE || receiveError || GetTickCount64() >= deadline || !size || size > 4096)
        { SetLastError(ERROR_OPERATION_ABORTED); return false; }
        const auto *bytes = static_cast<const BYTE *>(data);
        return write(std::vector<BYTE>(bytes, bytes + size));
    }
};
}
