#include "release_identity.h"
#include "runtime_utils.h"
#define NODLLEXPORT
#pragma warning(push)
#pragma warning(disable : 4100)
#include "vdjPlugin8.h"
#pragma warning(pop)
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <new>
#include <cmath>
#include <initializer_list>
#include <cstddef>
#include "deck_sampler.h"

#include "bridge_ipc.h"
#include "startup_input.h"
#include "screen_takeover.h"
#include "jog_ring.h"
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

#include "helper_launcher.h"
#include "haptic_cue.h"
#include <algorithm>

// ZH: DLL 采集 VDJ/HID 状态、计算灯环目标相位、同步启动输入并异步发送屏幕；EXE 执行 NI 电机、灯环和触觉输出。
// EN: The DLL samples VDJ/HID, computes ring target phase, synchronizes startup input and sends screens asynchronously; the EXE performs NI motor, ring and haptic output.
// ZH: 共享内存通过奇偶序列号提交；禁止把日志或硬件写入放进 VDJ 的绘制回调。
// EN: Commit shared memory using odd/even sequence numbers; keep logging and hardware writes out of VDJ draw callbacks.
class RingProbe final : public IVdjPlugin8
{
    HANDLE stop = nullptr, worker = nullptr;
    bool helperRequested = false;
    std::wstring logPath;
    const char *connectionReason = "not_checked";
    DWORD lastSpeedMode = 99, lastRingMode = 99;
    int lastPhysicalInput = -1;
    ULONGLONG lastHeartbeat = 0, publishedFrames = 0, lastPublication = 0, maxPublicationGap = 0;
    DWORD lastValid = ~0u, lastDeck[2]{};
    bool timelineRejected = false;
    vh90::Publisher timelinePublisher;
    ULONGLONG lastTimelineError = 0;
    using PhaseFrame = phase18::Frame;
    framewake::Signals dataSignals;
    physicalJog::Clock ringClock;
    HANDLE phaseMapping = nullptr, phaseChanged = nullptr;
    PhaseFrame *phase = nullptr;
    ULONGLONG notifiedGeneration = 0;
    wchar_t sessionToken[33]{};
    ULONGLONG generation = 0;

    // ZH: 连接等待或卸载时先撤销有效位，助手据此停止消费旧状态。
    // EN: Clear validity while waiting or unloading so the helper stops consuming stale state.
    void publishState(DWORD state)
    {
        if (!phase)
            return;
        const bool changed = phase->state != state;
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&phase->sequence));
        phase->tick = GetTickCount64();
        phase->valid = 0;
        phase->state = state;
        phase->generation = generation;
        phase->padding = 0;
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&phase->sequence));
        if (changed)
        {
            if (phaseChanged)
                SetEvent(phaseChanged);
            dataSignals.notifyAll();
        }
    }

    // ZH: 每轮发布最新快照；只记录有效性/Deck 变化和低频摘要，不记录每个角度。
    // EN: Publish the latest snapshot each cycle; log validity/deck transitions and periodic summaries rather than every angle.
    void publishPhase(HANDLE pipe)
    {
        if (!phase)
            return;
        PhaseFrame next{};
        next.magic = phase->magic;
        next.version = phase->version;
        next.pid = phase->pid;
        next.created = phase->created;
        next.state = phase18::Active;
        next.generation = generation;
        next.pipe = reinterpret_cast<ULONGLONG>(pipe);
        char ringText[128]{};
        next.padding = 0;
        if (cb && GetStringInfo("setting 'TRAKTORS4MK3-JogLEDMode'", ringText, sizeof(ringText)) == S_OK)
        {
            ringText[127] = 0;
            next.padding =
                (!strcmp(ringText, "Physical Jog") || !strcmp(ringText, "33.3 RPM") || !strcmp(ringText, "45 RPM"))
                    ? 1u
                    : 0u;
        }
        if (next.padding != lastRingMode)
        {
            log("jog_led_mode", next.padding);
            if (FILE *file = openLiveLog(logPath.c_str()))
            {
                runtimediag::event(file, "DLL.ring", "INFO", "jog_led_policy",
                                   "mode=%s manual_source=hid_report_3 absolute_source=vdj_get_rotation",
                                   next.padding ? "physical_jog_hybrid" : "absolute_position");
                fclose(file);
            }
            lastRingMode = next.padding;
            lastPhysicalInput = -1;
        }
        char speedText[128]{};
        next.speedMode = 0;
        if (cb && GetStringInfo("setting 'TRAKTORS4MK3-TTTurntableSpeed'", speedText, sizeof(speedText)) == S_OK)
        {
            speedText[127] = 0;
            next.speedMode = !strcmp(speedText, "33.3 RPM")                 ? 1
                             : !strcmp(speedText, "45 RPM")                 ? 2
                             : !strcmp(speedText, "BPM / 4 (Experimental)") ? 3
                                                                            : 0;
        }
        if (next.speedMode != lastSpeedMode)
        {
            log("tt_speed_setting_mode", next.speedMode);
            lastSpeedMode = next.speedMode;
        }
        char ticksText[64]{}, tensionText[64]{};
        if (cb && GetStringInfo("setting 'TRAKTORS4MK3-TicksWhenNudging'", ticksText, sizeof(ticksText)) == S_OK)
        {
            ticksText[63] = 0;
            next.ticks = !strcmp(ticksText, "Coarse") ? 1 : !strcmp(ticksText, "Fine") ? 2 : 0;
        }
        if (cb && GetStringInfo("setting 'TRAKTORS4MK3-JogwheelTension'", tensionText, sizeof(tensionText)) == S_OK)
        {
            tensionText[63] = 0;
            char *end = nullptr;
            double v = strtod(tensionText, &end);
            if (end != tensionText && !*end && std::isfinite(v) && v >= 0 && v <= 255 && std::floor(v) == v)
                next.tension = static_cast<DWORD>(v);
        }
        for (unsigned side = 0; side < 2; ++side)
        {
            auto value = sample18::read(
                side != 0,
                [this](const char *q, double &v) {
                    return cb && GetInfo(q, &v) == S_OK;
                },
                [this](DWORD deck, DWORD &rgb) {
                    unsigned value = 0;
                    bool ok = deckColor(deck, value);
                    rgb = value;
                    return ok;
                });
            double cuePreview = 0;
            std::string cueQuery = "deck " + std::to_string(value.deck) + " var 's4mk3_cue_preview'";
            if (value.valid && cb && GetInfo(cueQuery.c_str(), &cuePreview) == S_OK && cuePreview == 1)
                next.padding |= 1u << (side + 4);
            double effectiveTouch = 0;
            std::string touchQuery = "deck " + std::to_string(value.deck) + " touchwheel_touch";
            if (value.valid && cb && GetInfo(touchQuery.c_str(), &effectiveTouch) == S_OK && effectiveTouch == 1)
                next.padding |= 1u << (side + 2);
            next.deck[side] = value.deck;
            next.bpm[side] = value.bpm;
            next.tempo[side] = value.tempo;
            if (value.valid)
            {
                next.valid |= 1u << side;
                next.facts[side] = value.facts;
                next.rgb[side] = value.rgb;
                if (side)
                    next.right = value.rotation;
                else
                    next.left = value.rotation;
            }
        }
        initial23::physicalAngles(next);
        if (next.padding & 3u)
        {
            int available = std::isfinite(next.left) && std::isfinite(next.right) ? 1 : 0;
            if (available != lastPhysicalInput)
            {
                if (FILE *file = openLiveLog(logPath.c_str()))
                {
                    runtimediag::event(
                        file, "DLL.ring", available ? "INFO" : "WARN", "physical_jog_input_state",
                        "available=%u generation=%llu report=3 payload_offsets=15,43 scale=2880 behavior=%s",
                        unsigned(available), generation,
                        available ? "hybrid_playback_manual_tt_direct" : "pointer_disabled_until_valid_input");
                    fclose(file);
                }
                lastPhysicalInput = available;
            }
        }
        ringClock.apply(next, GetTickCount64());
        const bool changed = !phase18::samePayload(*phase, next);
        const bool refresh = dataSignals.takeRefresh();
        const bool hapticConfigChanged = phase->state != next.state || phase->generation != generation ||
                                         phase->pipe != next.pipe || phase->ticks != next.ticks ||
                                         phase->tension != next.tension ||
                                         memcmp(phase->deck, next.deck, sizeof(next.deck));
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&phase->sequence));
        phase->tick = GetTickCount64();
        phase->left = next.left;
        phase->right = next.right;
        phase->valid = next.valid;
        phase->state = phase18::Active;
        phase->generation = generation;
        phase->pipe = next.pipe;
        for (unsigned i = 0; i < 2; ++i)
        {
            phase->facts[i] = next.facts[i];
            phase->rgb[i] = next.rgb[i];
            phase->deck[i] = next.deck[i];
            phase->bpm[i] = next.bpm[i];
            phase->tempo[i] = next.tempo[i];
        }
        phase->padding = next.padding;
        phase->speedMode = next.speedMode;
        phase->ticks = next.ticks;
        phase->tension = next.tension;
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&phase->sequence));
        if (notifiedGeneration != generation)
        {
            notifiedGeneration = generation;
            if (phaseChanged)
                SetEvent(phaseChanged);
        }
        // ZH: 主帧提交后立即通知，不等待触觉SDK查询。
        // EN: Notify immediately after the phase commit, before haptic SDK queries.
        if (changed || refresh)
            dataSignals.phaseCommitted();
        const auto now = GetTickCount64();
        ++publishedFrames;
        if (lastPublication)
            maxPublicationGap = (std::max)(maxPublicationGap, now - lastPublication);
        lastPublication = now;
        if (next.valid != lastValid || next.deck[0] != lastDeck[0] || next.deck[1] != lastDeck[1])
        {
            FILE *f = openLiveLog(logPath.c_str());
            if (f)
            {
                runtimediag::event(
                    f, "DLL", "INFO", "publisher_state",
                    "generation=%llu valid=%lu deck_left=%lu deck_right=%lu facts_left=%lu facts_right=%lu", generation,
                    next.valid, next.deck[0], next.deck[1], next.facts[0], next.facts[1]);
                fclose(f);
            }
            lastValid = next.valid;
            lastDeck[0] = next.deck[0];
            lastDeck[1] = next.deck[1];
        }
        double enabled = 0;
        bool on = cb && GetInfo("setting 'TRAKTORS4MK3-HapticHotcues'", &enabled) == S_OK && enabled == 1;
        unsigned failures = timelinePublisher.publish(
            *phase, on,
            [this](const char *q, double &value) {
                return cb && GetInfo(q, &value) == S_OK;
            },
            [this](const char *q, std::string &value) {
                char buffer[8192]{};
                if (!cb || GetStringInfo(q, buffer, sizeof(buffer)) != S_OK)
                    return false;
                buffer[sizeof(buffer) - 1] = 0;
                value = buffer;
                return true;
            });
        // ZH: 触觉时间线完整提交后才通知其消费者；配置变化也必须及时生效。
        // EN: Wake haptics after its timeline commit; configuration changes must also take effect promptly.
        const bool lateRefresh = dataSignals.takeRefresh();
        if (lateRefresh)
            dataSignals.phaseCommitted();
        if (refresh || lateRefresh || hapticConfigChanged || timelinePublisher.changed())
            dataSignals.hapticCommitted();
        if (!failures && timelineRejected)
        {
            log("haptic_timeline_source_recovered");
            timelineRejected = false;
        }
        if (failures)
            timelineRejected = true;
        if (failures && GetTickCount64() - lastTimelineError > 10000)
        {
            log("haptic_timeline_source_rejected", failures);
            lastTimelineError = GetTickCount64();
            FILE *diagnostic = openLiveLog(logPath.c_str());
            if (diagnostic)
            {
                for (unsigned side = 0; side < 2; ++side)
                    if (!timelinePublisher.lastFailure(side).empty())
                        liveLogPrintf(diagnostic, "HAPTIC_SOURCE_REJECT side=%u deck=%lu reason=%s\n", side,
                                      next.deck[side], timelinePublisher.lastFailure(side).c_str());
                std::fclose(diagnostic);
            }
        }
    }

    void log(const char *event, DWORD error = 0)
    {
        const bool warning = strstr(event, "failed") || strstr(event, "rejected") || strstr(event, "exception");
        FILE *f = openLiveLog(logPath.c_str());
        if (f)
        {
            runtimediag::event(f, "DLL", warning ? "WARN" : "INFO", event, error ? "value=%lu" : "", error);
            fclose(f);
        }
    }

    // ZH: 无设备时也输出摘要，区分进程仍存活、连接未建立与工作线程退出。
    // EN: Report status even without a device to distinguish a live process, a missing connection and an exited worker.
    void heartbeat()
    {
        const auto now = GetTickCount64();
        if (now - lastHeartbeat < 30000)
            return;
        lastHeartbeat = now;
        FILE *f = openLiveLog(logPath.c_str());
        if (f)
        {
            runtimediag::event(
                f, "DLL", "INFO", "publisher_heartbeat",
                "generation=%llu state=%lu frames=%llu publication_age_ms=%llu max_gap_ms=%llu helper_requested=%u connection=%s",
                generation, phase ? phase->state : 0, publishedFrames, lastPublication ? now - lastPublication : 0,
                maxPublicationGap, helperRequested ? 1u : 0u, connectionReason);
            fclose(f);
        }
    }

    template <class T> bool read(ULONG_PTR at, T &value)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), &value, sizeof(value), &got) &&
               got == sizeof(value);
    }

    HANDLE connection(ULONG_PTR *owner = nullptr)
    {
        connectionReason = "display_location_unavailable";
        ULONG_PTR vec[2]{};
        if (!vdjcompat::addresses.displayVector || !vdjcompat::addresses.displayVtable)
            return nullptr;
        connectionReason = "display_vector_unreadable_or_invalid";
        if (!read(vdjcompat::addresses.displayVector, vec) || vec[1] < vec[0] || (vec[1] - vec[0]) % 8 ||
            vec[1] - vec[0] > 128)
            return nullptr;
        connectionReason = "no_matching_display_or_pipe";
        for (auto at = vec[0]; at < vec[1]; at += 8)
        {
            ULONG_PTR obj = 0, vt = 0, raw = 0, again = 0;
            DWORD type = 0, width = 0, height = 0;
            if (!read(at, obj) || !read(obj, vt) || vt != vdjcompat::addresses.displayVtable ||
                !read(obj + vdjcompat::addresses.displayType, type) || type != 11 ||
                !read(obj + vdjcompat::addresses.displayWidth, width) || width != 640 ||
                !read(obj + vdjcompat::addresses.displayHeight, height) || height != 240 ||
                !read(obj + vdjcompat::addresses.displayPipe, raw) || !raw || raw == ~ULONG_PTR(0))
                continue;
            HANDLE dup = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(raw), GetCurrentProcess(), &dup, 0,
                                 FALSE, DUPLICATE_SAME_ACCESS))
                continue;
            DWORD flags = 0, pid = 0;
            bool ok = read(obj + vdjcompat::addresses.displayPipe, again) && again == raw &&
                      GetFileType(dup) == FILE_TYPE_PIPE && GetNamedPipeInfo(dup, &flags, nullptr, nullptr, nullptr) &&
                      (flags & PIPE_TYPE_MESSAGE) && !(flags & PIPE_SERVER_END) &&
                      GetNamedPipeServerProcessId(dup, &pid);
            SC_HANDLE scm = ok ? OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT) : nullptr;
            SC_HANDLE service = scm ? OpenServiceW(scm, L"NIHardwareService", SERVICE_QUERY_STATUS) : nullptr;
            SERVICE_STATUS_PROCESS status{};
            DWORD needed = 0;
            ok = service &&
                 QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status),
                                      sizeof(status), &needed) &&
                 status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId == pid;
            if (service)
                CloseServiceHandle(service);
            if (scm)
                CloseServiceHandle(scm);
            if (ok)
            {
                connectionReason = "validated";
                if (owner)
                    *owner = obj;
                return dup;
            }
            connectionReason = "pipe_or_ni_service_identity_rejected";
            CloseHandle(dup);
        }
        return nullptr;
    }

    int numericOrder = 0;

    void calibrateColor()
    {
        double r = 0, g = 0, b = 0;
        bool ok = cb && GetInfo("color '#FF0000'", &r) == S_OK && GetInfo("color '#00FF00'", &g) == S_OK &&
                  GetInfo("color '#0000FF'", &b) == S_OK;
        numericOrder = ok ? colorOrder(r, g, b) : 0;
        FILE *f = nullptr;
        if ((f = openLiveLog(logPath.c_str())) != nullptr)
        {
            liveLogPrintf(f, "color_calibration red=%.17g green=%.17g blue=%.17g order=%d\n", r, g, b, numericOrder);
            std::fclose(f);
        }
    }

    bool deckColor(DWORD deck, unsigned &code)
    {
        if (!cb || deck < 1 || deck > 4)
            return false;
        uint32_t rgb = 0;
        bool ok = configuredDeckColor(
            deck, numericOrder,
            [this](const char *query, double &value) {
                return GetInfo(query, &value) == S_OK;
            },
            [this](const char *query, std::string &value) {
                char text[128]{};
                if (GetStringInfo(query, text, sizeof(text)) != S_OK)
                    return false;
                text[sizeof(text) - 1] = 0;
                value = text;
                return true;
            },
            rgb);
        if (ok)
            code = rgb;
        return ok;
    }

    void screenOutputSetting(ULONG_PTR owner = 0, HANDLE pipe = nullptr)
    {
        char value[32]{};
        // ZH: 设置不可读取时使用 ON。
        // EN: Use ON when the setting is unavailable.
        bool on = !(cb && GetStringInfo("setting 'TRAKTORS4MK3-ScreenOutputTakeover'", value, sizeof(value)) == S_OK &&
                    !strcmp(value, "OFF"));
        if (screenasync::setTakeover(on, owner, pipe))
            log(on ? "screen_output_takeover_on" : "screen_output_takeover_off_native");
    }

    // ZH: 工作线程异常必须留下明确退出原因，禁止异常穿过 Windows 线程入口。
    // EN: Record worker exceptions explicitly; exceptions must not cross the Windows thread entry.
    static DWORD WINAPI run(void *ptr)
    {
        auto self = static_cast<RingProbe *>(ptr);
        self->log("publisher_worker_started");
        try
        {
            self->test();
        }
        catch (...)
        {
            self->log("publisher_worker_exception", ERROR_UNHANDLED_EXCEPTION);
        }
        self->log("publisher_worker_exited");
        return 0;
    }

    void test()
    {
        runtimetime::PublisherTimer sampling;
        if (!sampling.valid())
        {
            log("publisher_timer_create_failed", GetLastError());
            return;
        }
        log("publisher_sampling_10ms_notifications");
        const bool hostSupported = vdjcompat::host();
        const bool scanned = vdjcompat::locate();
        vdjcompat::report(logPath);
        const bool hardwareSupported =
            scanned && vdjcompat::addresses.displayVtable && vdjcompat::addresses.displayVector;
        if (hostSupported)
        {
            if (!hardwareSupported)
                vdjcompat::warning(
                    1, logPath, "native_connection",
                    L"Native controller connection could not be located safely. Custom TT motor, ring and haptic output are disabled. Native VDJ controls and independent input/screen features remain available. See the Bridge log.");
            if (!initial23::start(logPath))
                vdjcompat::warning(
                    2, logPath, "initial_input",
                    L"The HID input entry could not be located or enabled safely. Cold-start input synchronization and physical Jog angle sampling are disabled. Absolute Position ring mode and other independent features remain available. See the Bridge log.");
            if (!hardwareSupported || !screenasync::start(logPath))
                vdjcompat::warning(
                    4, logPath, "screen_takeover",
                    L"The screen output entry could not be located or enabled safely. Screen Output Takeover is disabled; native VDJ screen output remains in use. Other controller features are unchanged. See the Bridge log.");
            if (hardwareSupported)
            {
                calibrateColor();
                log("data_publisher_waiting_for_native_screen");
            }
        }
        else
            log("unsupported_host_no_hardware_access");
        while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT)
        {
            heartbeat();
            if (!hardwareSupported)
            {
                if (hostSupported)
                    initial23::tick();
                publishState(phase18::Waiting);
                WaitForSingleObject(stop, framewake::DisconnectedMs);
                continue;
            }
            ULONG_PTR screenOwner = 0;
            HANDLE pipe = connection(&screenOwner);
            if (!pipe)
            {
                screenasync::idle();
                initial23::tick();
                publishState(phase18::Waiting);
                WaitForSingleObject(stop, framewake::DisconnectedMs);
                continue;
            }
            HANDLE check = connection();
            bool stable = check && samePipeObject(pipe, check);
            if (check)
                CloseHandle(check);
            if (!stable)
            {
                CloseHandle(pipe);
                WaitForSingleObject(stop, framewake::DisconnectedMs);
                continue;
            }
            screenOutputSetting(screenOwner, pipe);
            screenasync::bind(screenOwner, pipe);
            ++generation;
            log("connection_lease_published");
            // ZH: 连接身份经过两次验证后立即发布，不增加等待定时器。
            // EN: Connection identity validated twice; publish immediately, without a settling timer.
            publishPhase(pipe);
            if (!helperRequested)
            {
                helperRequested = true;
                launchNativeHelper(stop, logPath, hInstance, sessionToken);
            }
            initial23::tick();
            sampling.reset();
            DWORD sampleWait = WAIT_OBJECT_0;
            while ((sampleWait = sampling.wait(stop)) == WAIT_OBJECT_0 + 1)
            {
                heartbeat();
                HANDLE current = connection();
                bool same = current && samePipeObject(pipe, current);
                if (current)
                    CloseHandle(current);
                if (!same)
                {
                    log("native_connection_lost_waiting_for_reconnect");
                    break;
                }
                screenOutputSetting(screenOwner, pipe);
                publishPhase(pipe);
                initial23::tick();
            }
            if (sampleWait == WAIT_FAILED)
            {
                log("publisher_timer_wait_failed", GetLastError());
                SetEvent(stop);
            }
            screenasync::unbind();
            publishState(phase18::Waiting);
            CloseHandle(pipe);
        }
        if (hostSupported)
        {
            screenasync::stop();
            initial23::stop();
        }
        log("publisher_stopped_no_device_commands");
    }

  public:
    HRESULT VDJ_API OnLoad() override
    {
        wchar_t path[32768];
        DWORD n = GetModuleFileNameW(hInstance, path, 32768);
        if (!n || n >= 32768)
            return E_FAIL;
        logPath.assign(path, n);
        auto p = logPath.find_last_of(L"\\/");
        if (p == std::wstring::npos)
            return E_FAIL;
        logPath.resize(p + 1);
        logPath += S4_RING_LOG;
        if (FILE *f = openLiveLog(logPath.c_str()))
        {
            runtimediag::section(f, "BRIDGE START");
            fclose(f);
        }
        log("plugin_load_started");
        FILE *identity = openLiveLog(logPath.c_str());
        if (identity)
        {
            runtimediag::event(identity, "DLL", "INFO", "runtime_identity", "version=%s pointer_bits=%u",
                               S4_RELEASE_VERSION, unsigned(sizeof(void *) * 8));
            fclose(identity);
        }
        BYTE random[16];
        if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            log("session_random_failed", ERROR_GEN_FAILURE);
            return E_FAIL;
        }
        for (unsigned i = 0; i < 16; ++i)
            swprintf_s(sessionToken + 2 * i, 3, L"%02x", random[i]);
        wchar_t mappingName[128], stopName[128];
        phase18::name(mappingName, L"Phase", GetCurrentProcessId(), sessionToken);
        phase18::name(stopName, L"Stop", GetCurrentProcessId(), sessionToken);
        phaseMapping =
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PhaseFrame), mappingName);
        if (!phaseMapping)
        {
            log("phase_mapping_create_failed", GetLastError());
            return E_FAIL;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            log("phase_mapping_already_exists", ERROR_ALREADY_EXISTS);
            CloseHandle(phaseMapping);
            phaseMapping = nullptr;
            return E_FAIL;
        }
        phase = static_cast<PhaseFrame *>(MapViewOfFile(phaseMapping, FILE_MAP_WRITE, 0, 0, sizeof(PhaseFrame)));
        if (!phase)
        {
            log("phase_mapping_view_failed", GetLastError());
            CloseHandle(phaseMapping);
            phaseMapping = nullptr;
            return E_FAIL;
        }
        *phase = {phase18::Magic,
                  phase18::Version,
                  GetCurrentProcessId(),
                  0,
                  GetTickCount64(),
                  0,
                  0,
                  0,
                  phase18::Waiting,
                  0,
                  phase18::creation(GetCurrentProcess())};
        if (!timelinePublisher.create(GetCurrentProcessId(), phase->created, sessionToken))
        {
            log("haptic_mapping_create_failed", GetLastError());
            return E_FAIL;
        }
        stop = CreateEventW(nullptr, TRUE, FALSE, stopName);
        if (!stop)
        {
            log("stop_event_create_failed", GetLastError());
            return E_FAIL;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            log("stop_event_already_exists", ERROR_ALREADY_EXISTS);
            CloseHandle(stop);
            stop = nullptr;
            return E_FAIL;
        }
        // ZH: 生命周期与数据通知分开；每个通道独占自动复位事件，多次发布可合并。
        // EN: Separate lifecycle and data notifications; each lane owns an auto-reset event and bursts coalesce.
        wchar_t changedName[128];
        phase18::name(changedName, L"Changed", GetCurrentProcessId(), sessionToken);
        phaseChanged = CreateEventW(nullptr, FALSE, FALSE, changedName);
        if (!phaseChanged)
            log("publisher_notification_unavailable", GetLastError());
        if (!dataSignals.connect(GetCurrentProcessId(), sessionToken, true))
            log("data_notification_create_failed_fallback", GetLastError());
        else
            log("data_notifications_ready");
        worker = CreateThread(nullptr, 0, run, this, 0, nullptr);
        if (!worker)
        {
            log("publisher_thread_create_failed", GetLastError());
            CloseHandle(stop);
            stop = nullptr;
            return E_FAIL;
        }
        log("plugin_load_complete");
        return S_OK;
    }

    HRESULT VDJ_API OnGetPluginInfo(TVdjPluginInfo8 *p) override
    {
        if (!p)
            return E_POINTER;
        *p = {S4_PLUGIN_NAME,
              "S4 MK3 Integration Project",
              "VDJ facts publisher; EXE owns all ring control",
              S4_RELEASE_VERSION,
              nullptr,
              0};
        return S_OK;
    }

    HRESULT VDJ_API OnGetUserInterface(TVdjPluginInterface8 *p) override
    {
        if (!p)
            return E_POINTER;
        p->Type = VDJINTERFACE_DEFAULT;
        return S_OK;
    }

    ULONG VDJ_API Release() override
    {
        log("plugin_release_requested");
        if (stop)
            SetEvent(stop);
        if (worker)
        {
            WaitForSingleObject(worker, INFINITE);
            CloseHandle(worker);
        }
        if (stop)
            CloseHandle(stop);
        if (phase)
        {
            publishState(phase18::Stopping);
            UnmapViewOfFile(phase);
        }
        if (phaseMapping)
            CloseHandle(phaseMapping);
        if (phaseChanged)
            CloseHandle(phaseChanged);
        log("plugin_release_complete");
        delete this;
        return 0;
    }
};

extern "C" HRESULT __stdcall DllGetClassObject(const GUID &cls, const GUID &iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (std::memcmp(&cls, &CLSID_VdjPlugin8, sizeof(GUID)) || std::memcmp(&iid, &IID_IVdjPluginBasic8, sizeof(GUID)))
        return CLASS_E_CLASSNOTAVAILABLE;
    *out = new (std::nothrow) RingProbe();
    return *out ? S_OK : E_OUTOFMEMORY;
}
