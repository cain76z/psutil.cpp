/**
 * psutil.hpp — Single-Header C++ port of Python's psutil
 * =========================================================
 * Supported platforms : Linux · Windows · macOS
 * C++ standard        : C++17 or later
 * License             : MIT
 *
 * Usage example
 * -------------
 *   // All functions are `inline` — simply include the header. No separate
 *   // translation unit or #define is required.
 *   #include "psutil.hpp"
 *
 *   int main() {
 *       std::cout << psutil::cpu_percent(0.5) << "% CPU\n";
 *       auto vm = psutil::virtual_memory();
 *       std::cout << "RAM used: " << vm.percent << "%\n";
 *   }
 *
 * Note: The old `#define PSUTIL_IMPLEMENTATION` guard shown in earlier
 * versions is no longer needed and can be safely removed.
 *
 * API overview (mirrors Python psutil)
 * -------------------------------------
 *  CPU      : cpu_count(), cpu_percent(), cpu_times(), cpu_freq(),
 *             cpu_stats(), cpu_percent_percpu()
 *  Memory   : virtual_memory(), swap_memory()
 *  Disk     : disk_partitions(), disk_usage(), disk_io_counters()
 *  Network  : net_io_counters(), net_if_addrs(), net_if_stats(),
 *             net_connections()
 *  Sensors  : sensors_battery() [Linux/Windows]
 *  GPU      : gpu_count(), gpu_info()
 *             — NVIDIA : full stats via NVML (runtime dynamic-load, no SDK install needed)
 *             — AMD    : utilization/VRAM/clock/temp/power/fan via sysfs [Linux], registry [Windows]
 *             — Intel  : clock/temp via sysfs [Linux]
 *             — Apple  : utilization/VRAM via IOKit [macOS]
 *  Process  : pids(), pid_exists(), Process class
 *  System   : boot_time(), users()
 */

#pragma once

#if defined(_WIN32) || defined(_WIN64)
#  define NOMINMAX
#endif

#include <algorithm>
#include <mutex>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────
//  Platform detection
// ─────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
#  define PSUTIL_WINDOWS 1
#elif defined(__APPLE__)
#  define PSUTIL_MACOS 1
#elif defined(__linux__)
#  define PSUTIL_LINUX 1
#else
#  error "psutil.hpp: unsupported platform"
#endif

// ── Windows headers ──────────────────────────
#ifdef PSUTIL_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <pdh.h>
#  include <psapi.h>
#  include <iphlpapi.h>
#  include <tlhelp32.h>
#  include <winbase.h>
#  include <winioctl.h>     // IOCTL_DISK_PERFORMANCE, DISK_PERFORMANCE
//  MSVC only — MinGW / Clang-cl users must link manually:
//    -lpsapi -liphlpapi -lpdh  (or CMake: target_link_libraries(... psapi iphlpapi pdh))
#  ifdef _MSC_VER
#    pragma comment(lib, "pdh.lib")
#    pragma comment(lib, "psapi.lib")
#    pragma comment(lib, "iphlpapi.lib")
#  endif
#endif

// ── POSIX headers ────────────────────────────
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
#  include <arpa/inet.h>
#  include <dirent.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/ioctl.h>
#  include <sys/resource.h>
#  include <sys/socket.h>
#  include <sys/statvfs.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <utmpx.h>
#  include <pwd.h>
#  include <signal.h>
#endif

// ── Linux-only POSIX headers ─────────────────
#ifdef PSUTIL_LINUX
#  include <sys/sysinfo.h>   // FIX #1: Linux-only; was incorrectly shared with macOS
#endif

// ── macOS extras ─────────────────────────────
#ifdef PSUTIL_MACOS
#  include <sys/mount.h>     // FIX #3: required for getmntinfo() in disk_partitions()
#  include <sys/sysctl.h>
#  include <mach/mach.h>
#  include <mach/mach_host.h>
#  include <mach/processor_info.h>
#  include <libproc.h>
#  include <IOKit/ps/IOPowerSources.h>
#  include <IOKit/ps/IOPSKeys.h>
#  include <IOKit/IOKitLib.h>          // GPU: IOAccelerator / IOPCIDevice queries
#  include <CoreFoundation/CoreFoundation.h>
#  include <sys/proc_info.h>           // net_connections: proc_pidfdinfo / socket_fdinfo
#  include <sys/socketvar.h>           // net_connections: socket_fdinfo helpers
#endif

// ── GPU dynamic-loader runtime helper ────────
// dlfcn.h needed on POSIX for NVML / ROCm runtime loading
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
#  include <dlfcn.h>
#  include <glob.h>          // GPU: hwmon path glob expansion
#endif

namespace psutil {

// ═══════════════════════════════════════════════════════════════
//  Exception type
// ═══════════════════════════════════════════════════════════════
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& msg) : std::runtime_error(msg) {}
};

class AccessDenied : public Error {
public:
    explicit AccessDenied(const std::string& msg = "Access denied")
        : Error(msg) {}
};

class NoSuchProcess : public Error {
public:
    explicit NoSuchProcess(int pid)
        : Error("No such process: " + std::to_string(pid)) {}
};

// ═══════════════════════════════════════════════════════════════
//  Data structures
// ═══════════════════════════════════════════════════════════════

// ── CPU ─────────────────────────────────────
struct CpuTimes {
    double user    = 0;
    double system  = 0;
    double idle    = 0;
    double nice    = 0;   // Linux/macOS
    double iowait  = 0;   // Linux
    double irq     = 0;   // Linux
    double softirq = 0;   // Linux
    double steal   = 0;   // Linux
    double guest   = 0;   // Linux
};

struct CpuFreq {
    double current = 0;   // MHz
    double min     = 0;
    double max     = 0;
};

struct CpuStats {
    uint64_t ctx_switches  = 0;
    uint64_t interrupts    = 0;
    uint64_t soft_interrupts = 0;
    uint64_t syscalls      = 0;
};

// ── Memory ──────────────────────────────────
struct VirtualMemory {
    uint64_t total     = 0;
    uint64_t available = 0;
    uint64_t used      = 0;
    uint64_t free      = 0;
    double   percent   = 0;
    uint64_t buffers   = 0;  // Linux
    uint64_t cached    = 0;  // Linux
    uint64_t shared    = 0;  // Linux
    uint64_t active    = 0;
    uint64_t inactive  = 0;
};

struct SwapMemory {
    uint64_t total   = 0;
    uint64_t used    = 0;
    uint64_t free    = 0;
    double   percent = 0;
    uint64_t sin     = 0;
    uint64_t sout    = 0;
};

// ── Disk ────────────────────────────────────
struct DiskPartition {
    std::string device;
    std::string mountpoint;
    std::string fstype;
    std::string opts;
    int         maxfile = 0;
    int         maxpath = 0;
};

struct DiskUsage {
    uint64_t total   = 0;
    uint64_t used    = 0;
    uint64_t free    = 0;
    double   percent = 0;
};

struct DiskIOCounters {
    uint64_t    read_count  = 0;
    uint64_t    write_count = 0;
    uint64_t    read_bytes  = 0;
    uint64_t    write_bytes = 0;
    double      read_time   = 0;  // ms
    double      write_time  = 0;  // ms
    std::string name;
    DiskIOCounters(std::string n = {}) : name(std::move(n)) {}
};

// ── Network ─────────────────────────────────
struct NetIOCounters {
    std::string name;
    uint64_t bytes_sent   = 0;
    uint64_t bytes_recv   = 0;
    uint64_t packets_sent = 0;
    uint64_t packets_recv = 0;
    uint64_t errin        = 0;
    uint64_t errout       = 0;
    uint64_t dropin       = 0;
    uint64_t dropout      = 0;
};

struct NetAddress {
    std::string family;   // "AF_INET", "AF_INET6", "AF_LINK"
    std::string address;
    std::string netmask;
    std::string broadcast;
    std::string ptp;
};

struct NetIfStats {
    bool        isup    = false;
    int         duplex  = 0;   // 0=UNKNOWN, 1=HALF, 2=FULL
    int         speed   = 0;   // Mbps
    int         mtu     = 0;
};

struct NetConnection {
    int         fd      = -1;
    std::string family;
    std::string type;
    std::string laddr;
    int         lport   = 0;
    std::string raddr;
    int         rport   = 0;
    std::string status;
    int         pid     = -1;
};

// ── Battery ─────────────────────────────────
struct BatteryInfo {
    double percent         = 0;
    double secsleft        = -1;  // -1 = unknown
    bool   power_plugged   = false;
};

// ── GPU ─────────────────────────────────────
/**
 * GpuStats — returned by gpu_info() for each detected GPU.
 *
 * Field availability per backend:
 *   NVML (NVIDIA)     : all fields
 *   AMD sysfs (Linux) : gpu_util, mem_*, clock_*, temperature, power_*, fan_speed_pct, pcie_*
 *   Intel sysfs(Linux): clock_core/clock_core_max, temperature (where hwmon available)
 *   macOS IOKit       : gpu_util, enc_util, mem_* (where driver exposes them)
 *   Windows registry  : name, vendor, driver_version, mem_total (static only)
 *
 * Fields left at their zero default indicate "not available on this platform/backend".
 */
struct GpuStats {
    int         index          = 0;
    std::string name;
    std::string vendor;          ///< "NVIDIA" | "AMD" | "Intel" | "Apple" | "Unknown"
    std::string driver_version;

    // ── Utilisation (0–100 %) ────────────────
    double      gpu_util       = 0;  ///< GPU core / shader utilisation
    double      mem_util       = 0;  ///< Memory-controller busy %  (NVML only)
    double      enc_util       = 0;  ///< Video encoder %           (NVML / Apple)
    double      dec_util       = 0;  ///< Video decoder %           (NVML)

    // ── VRAM (bytes) ─────────────────────────
    uint64_t    mem_total      = 0;
    uint64_t    mem_used       = 0;
    uint64_t    mem_free       = 0;
    double      mem_percent    = 0;

    // ── Clock speeds (MHz) ───────────────────
    double      clock_core     = 0;  ///< Current shader / SM clock
    double      clock_mem      = 0;  ///< Current memory clock
    double      clock_core_max = 0;  ///< Boost cap
    double      clock_mem_max  = 0;

    // ── Thermal / power ──────────────────────
    double      temperature    = 0;  ///< °C
    double      power_watts    = 0;  ///< Current draw (W)
    double      power_limit    = 0;  ///< TDP / power cap (W)

    // ── Fan ──────────────────────────────────
    double      fan_speed_pct  = 0;  ///< 0–100 %

    // ── PCIe link ────────────────────────────
    uint32_t    pcie_gen       = 0;
    uint32_t    pcie_width     = 0;  ///< lanes (×1, ×4, ×8, ×16)
};

// ── Users ────────────────────────────────────
struct UserInfo {
    std::string name;
    std::string terminal;
    std::string host;
    double      started = 0;
    int         pid     = -1;
};

// ── Process ─────────────────────────────────
struct ProcessMemoryInfo {
    uint64_t rss  = 0;   // Resident Set Size
    uint64_t vms  = 0;   // Virtual Memory Size
    uint64_t shared = 0;
    uint64_t text   = 0;
    uint64_t data   = 0;
};

struct ProcessCpuTimes {
    double user   = 0;
    double system = 0;
    double children_user   = 0;
    double children_system = 0;
};

struct ProcessStatus {
    enum class State { Running, Sleeping, DiskSleep, Stopped, Zombie, Dead, Other };
    State state = State::Other;
    std::string str;
};

// ═══════════════════════════════════════════════════════════════
//  Internal helpers (declared here, implemented below)
// ═══════════════════════════════════════════════════════════════
namespace detail {

// ────────────────────────────────────────────────────────────────────
//  RAII resource guards (platform-independent)
// ────────────────────────────────────────────────────────────────────

// Fix-R1: RAII guard for DIR* — guarantees closedir() even when exceptions occur
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
struct DirGuard {
    DIR* d = nullptr;
    explicit DirGuard(DIR* d_) noexcept : d(d_) {}
    ~DirGuard() noexcept { if (d) closedir(d); }
    DirGuard(const DirGuard&)            = delete;
    DirGuard& operator=(const DirGuard&) = delete;
    explicit operator bool() const noexcept { return d != nullptr; }
};

// Fix-R3: RAII guard for getifaddrs — guarantees freeifaddrs() on scope exit
struct IfAddrsGuard {
    struct ifaddrs* p = nullptr;
    IfAddrsGuard() noexcept { getifaddrs(&p); }
    ~IfAddrsGuard() noexcept { if (p) freeifaddrs(p); }
    IfAddrsGuard(const IfAddrsGuard&)            = delete;
    IfAddrsGuard& operator=(const IfAddrsGuard&) = delete;
    explicit operator bool() const noexcept { return p != nullptr; }
};

// Fix-R4: RAII guard for setutxent/endutxent — guarantees endutxent() on scope exit
struct UtmpxGuard {
    UtmpxGuard()  noexcept { setutxent(); }
    ~UtmpxGuard() noexcept { endutxent(); }
    UtmpxGuard(const UtmpxGuard&)            = delete;
    UtmpxGuard& operator=(const UtmpxGuard&) = delete;
};
#endif // POSIX

// Returns current monotonic seconds
inline double now_sec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#ifdef PSUTIL_LINUX
// ────────────────────────────────────────────
//  Linux /proc helpers
// ────────────────────────────────────────────
inline std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::map<std::string, std::string> parse_key_value(const std::string& text,
                                                           const std::string& sep = ":") {
    std::map<std::string, std::string> result;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find(sep);
        if (pos == std::string::npos) continue;
        auto key = line.substr(0, pos);
        auto val = line.substr(pos + sep.size());
        // trim
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);
        result[key] = val;
    }
    return result;
}

struct ProcStat {
    // /proc/stat cpu line values
    uint64_t user=0, nice=0, system=0, idle=0,
             iowait=0, irq=0, softirq=0, steal=0, guest=0, guest_nice=0;
};

inline std::vector<ProcStat> read_proc_stat() {
    std::string text = read_file("/proc/stat");
    std::istringstream ss(text);
    std::string line;
    std::vector<ProcStat> result;
    while (std::getline(ss, line)) {
        if (line.rfind("cpu", 0) != 0) continue;
        std::istringstream ls(line);
        std::string name;
        ProcStat s;
        ls >> name >> s.user >> s.nice >> s.system >> s.idle
           >> s.iowait >> s.irq >> s.softirq >> s.steal >> s.guest >> s.guest_nice;
        result.push_back(s);
    }
    return result;
}

inline double calc_cpu_percent(const ProcStat& a, const ProcStat& b) {
    uint64_t idle_a = a.idle + a.iowait;
    uint64_t idle_b = b.idle + b.iowait;
    uint64_t total_a = a.user + a.nice + a.system + a.idle + a.iowait
                     + a.irq + a.softirq + a.steal;
    uint64_t total_b = b.user + b.nice + b.system + b.idle + b.iowait
                     + b.irq + b.softirq + b.steal;
    // FIX #8a: Guard against counter wrap/reset (unsigned underflow → bogus 100%)
    if (total_b < total_a || idle_b < idle_a) return 0.0;
    uint64_t d_total = total_b - total_a;
    uint64_t d_idle  = idle_b  - idle_a;
    if (d_total == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(d_idle) / d_total);
}


// FIX #4: Non-blocking cpu_percent state — lives inside PSUTIL_LINUX
// because ProcStat is Linux-only.
struct CpuPercentCache {
    std::mutex            mtx;
    std::vector<ProcStat> prev;  // [0]=aggregate [1..]=per-core
};
inline CpuPercentCache& cpu_cache() {
    static CpuPercentCache c;
    return c;
}
// FIX #6: Lightweight ppid reader — avoids constructing a full Process object
// for each pid when scanning children. Reads /proc/<pid>/status directly.
inline int read_ppid(int pid) {
    auto kv = parse_key_value(read_file("/proc/" + std::to_string(pid) + "/status"), ":");
    auto it = kv.find("PPid");
    if (it != kv.end()) {
        try { return std::stoi(it->second); } catch (...) {}
    }
    return -1;
}
#endif // PSUTIL_LINUX

#ifdef PSUTIL_WINDOWS
// ────────────────────────────────────────────
//  Windows helpers
// ────────────────────────────────────────────

// RAII wrapper — ensures RegCloseKey is always called
struct RegKeyGuard {
    HKEY h = nullptr;
    explicit RegKeyGuard(HKEY h_) noexcept : h(h_) {}
    ~RegKeyGuard() noexcept { if (h) RegCloseKey(h); }
    RegKeyGuard(const RegKeyGuard&)            = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;
    explicit operator bool() const noexcept { return h != nullptr; }
};

// RAII wrapper — ensures CloseHandle is always called
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE h_) : h(h_) {}
    ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

inline double filetime_to_sec(const FILETIME& ft) {
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<double>(ui.QuadPart) / 1e7;
}

inline double filetime_diff_sec(const FILETIME& a, const FILETIME& b) {
    // b - a in seconds
    ULARGE_INTEGER ua, ub;
    ua.LowPart = a.dwLowDateTime; ua.HighPart = a.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime; ub.HighPart = b.dwHighDateTime;
    // BUG FIX #14: ULONGLONG subtraction is unsigned; guard against b < a
    // (shouldn't happen in practice, but protects against clock skew or bad reads)
    if (ub.QuadPart < ua.QuadPart) return 0.0;
    return static_cast<double>(ub.QuadPart - ua.QuadPart) / 1e7;
}

// ─────────────────────────────────────────────────────────────────────
//  Pure C++ inet_ntop 구현 (ws2_32 링크 완전 제거)
//  AF_INET / AF_INET6만 지원하며, :: 압축까지 제대로 처리
// ─────────────────────────────────────────────────────────────────────
inline const char* psutil_inet_ntop(int af, const void* src, char* dst, size_t size) {
    if (!src || !dst || size == 0) return nullptr;

    if (af == AF_INET) {
        if (size < INET_ADDRSTRLEN) return nullptr;
        const uint8_t* p = static_cast<const uint8_t*>(src);
        snprintf(dst, size, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
        return dst;
    }

    if (af == AF_INET6) {
        if (size < INET6_ADDRSTRLEN) return nullptr;
        const uint8_t* bytes = static_cast<const uint8_t*>(src);

        // 가장 긴 연속 0 구간 찾기 (:: 압축용)
        int longest_start = -1, longest_len = 0;
        int cur_start = -1, cur_len = 0;
        for (int i = 0; i < 8; ++i) {
            uint16_t word = (bytes[i*2] << 8) | bytes[i*2 + 1];
            if (word == 0) {
                if (cur_start == -1) cur_start = i;
                ++cur_len;
            } else {
                if (cur_len > longest_len) {
                    longest_start = cur_start;
                    longest_len = cur_len;
                }
                cur_start = -1;
                cur_len = 0;
            }
        }
        if (cur_len > longest_len) {
            longest_start = cur_start;
            longest_len = cur_len;
        }
        if (longest_len < 2) longest_start = -1;   // 1개 0은 :: 안 함

        char* out = dst;
        bool first = true;
        for (int i = 0; i < 8; ++i) {
            if (i == longest_start) {
                *out++ = ':';
                *out++ = ':';
                i += longest_len - 1;
                first = false;
                continue;
            }
            if (!first) *out++ = ':';
            first = false;

            uint16_t word = (bytes[i*2] << 8) | bytes[i*2 + 1];
            snprintf(out, 5, "%x", word);
            out += strlen(out);
        }
        *out = '\0';
        return dst;
    }
    return nullptr; // 지원하지 않는 주소 패밀리
}

#endif // PSUTIL_WINDOWS

} // namespace detail

// ═══════════════════════════════════════════════════════════════
//  CPU
// ═══════════════════════════════════════════════════════════════

/**
 * cpu_count(logical=true)
 * Returns number of logical (or physical) CPU cores.
 */
inline int cpu_count(bool logical = true) {
#ifdef PSUTIL_WINDOWS
    if (logical) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<int>(si.dwNumberOfProcessors);
    }

    // === 물리 코어 정확히 세는 코드 (하이브리드 CPU 완벽 지원) ===
    DWORD bufferSize = 0;
    BOOL result = GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufferSize);
    if (!result && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<char> buffer(bufferSize);
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = 
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

        if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bufferSize)) {
            int physicalCores = 0;
            DWORD offset = 0;
            while (offset < bufferSize) {
                if (info->Relationship == RelationProcessorCore) {
                    ++physicalCores;                    // 하나의 PROCESSOR_RELATIONSHIP = 1 물리 코어
                }
                offset += info->Size;
                info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    reinterpret_cast<char*>(info) + info->Size);
            }
            return physicalCores;
        }
    }
    // fallback (구형 Windows)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return std::max(1, static_cast<int>(si.dwNumberOfProcessors) / 2);
#elif defined(PSUTIL_LINUX)
    if (logical) return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    // Count unique physical ids in /proc/cpuinfo
    std::string text = detail::read_file("/proc/cpuinfo");
    std::set<std::string> physical_ids;
    std::istringstream ss(text);
    std::string line;
    std::string cur_phys;
    while (std::getline(ss, line)) {
        if (line.find("physical id") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos) cur_phys = line.substr(pos + 1);
        } else if (line.find("core id") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos) physical_ids.insert(cur_phys + ":" + line.substr(pos+1));
        }
    }
    int c = static_cast<int>(physical_ids.size());
    return c > 0 ? c : static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
#elif defined(PSUTIL_MACOS)
    int count = 0;
    size_t size = sizeof(count);
    const char* key = logical ? "hw.logicalcpu" : "hw.physicalcpu";
    if (sysctlbyname(key, &count, &size, nullptr, 0) == 0) return count;
    return 1;
#endif
}

/**
 * cpu_times()
 * Returns aggregate CPU time counters (seconds).
 */
inline CpuTimes cpu_times() {
    CpuTimes ct;
#ifdef PSUTIL_LINUX
    auto stats = detail::read_proc_stat();
    if (stats.empty()) return ct;
    auto& s = stats[0]; // "cpu" line = aggregate
    // BUG FIX #1: sysconf(_SC_CLK_TCK) may return -1 on error; guard with safe default
    long clk_raw = sysconf(_SC_CLK_TCK);
    double hz = (clk_raw > 0) ? static_cast<double>(clk_raw) : 100.0;
    ct.user    = s.user    / hz;
    ct.nice    = s.nice    / hz;
    ct.system  = s.system  / hz;
    ct.idle    = s.idle    / hz;
    ct.iowait  = s.iowait  / hz;
    ct.irq     = s.irq     / hz;
    ct.softirq = s.softirq / hz;
    ct.steal   = s.steal   / hz;
    ct.guest   = s.guest   / hz;
#elif defined(PSUTIL_WINDOWS)
    FILETIME idle_time, kernel_time, user_time;
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        ct.idle   = detail::filetime_to_sec(idle_time);
        ct.user   = detail::filetime_to_sec(user_time);
        ct.system = detail::filetime_to_sec(kernel_time) - ct.idle;
    }
#elif defined(PSUTIL_MACOS)
    natural_t cpu_count_m = 0;
    processor_cpu_load_info_t cpu_load;
    mach_msg_type_number_t count;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                            &cpu_count_m,
                            (processor_info_array_t*)&cpu_load, &count) == KERN_SUCCESS) {
        for (natural_t i = 0; i < cpu_count_m; ++i) {
            ct.user   += cpu_load[i].cpu_ticks[CPU_STATE_USER];
            ct.system += cpu_load[i].cpu_ticks[CPU_STATE_SYSTEM];
            ct.idle   += cpu_load[i].cpu_ticks[CPU_STATE_IDLE];
            ct.nice   += cpu_load[i].cpu_ticks[CPU_STATE_NICE];
        }
        vm_deallocate(mach_task_self(), (vm_address_t)cpu_load,
                      count * sizeof(*cpu_load));
        double hz = 100.0; // macOS reports in 100 Hz ticks
        ct.user   /= hz; ct.system /= hz; ct.idle /= hz; ct.nice /= hz;
    }
#endif
    return ct;
}

/**
 * cpu_percent(interval_sec)
 * Measures CPU utilisation over the given interval.
 * Pass interval<=0 to get a non-blocking estimate (compared to last call).
 */
inline double cpu_percent(double interval_sec = 0.1) {
#ifdef PSUTIL_LINUX
    // FIX #4: support non-blocking (interval=0) mode via static cache
    auto& cache = detail::cpu_cache();
    std::unique_lock<std::mutex> lock(cache.mtx);
    // BUG FIX #15: capture emptiness state BEFORE releasing the lock to avoid TOCTOU race
    bool was_empty = cache.prev.empty();
    auto s1 = was_empty ? detail::read_proc_stat() : cache.prev;
    lock.unlock();
    if (interval_sec > 0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(interval_sec));
    } else if (was_empty) {
        // First ever call with interval=0: store snapshot, return 0
        std::lock_guard<std::mutex> lg(cache.mtx);
        cache.prev = s1;
        return 0.0;
    }
    auto s2 = detail::read_proc_stat();
    { std::lock_guard<std::mutex> lg(cache.mtx); cache.prev = s2; }
    if (s1.empty() || s2.empty()) return 0.0;
    return detail::calc_cpu_percent(s1[0], s2[0]);
#elif defined(PSUTIL_WINDOWS)
    FILETIME idle1, kernel1, user1;
    // Fix-4: guard GetSystemTimes failure — uninitialised FILETIMEs would
    // produce garbage CPU% values (e.g. after a system suspend/resume event)
    if (!GetSystemTimes(&idle1, &kernel1, &user1)) return 0.0;
    if (interval_sec > 0) {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(interval_sec));
    }
    FILETIME idle2, kernel2, user2;
    if (!GetSystemTimes(&idle2, &kernel2, &user2)) return 0.0;
    double d_idle   = detail::filetime_diff_sec(idle1,   idle2);
    double d_kernel = detail::filetime_diff_sec(kernel1, kernel2);
    double d_user   = detail::filetime_diff_sec(user1,   user2);
    double d_total  = d_kernel + d_user;
    if (d_total == 0) return 0.0;
    return 100.0 * (d_total - d_idle) / d_total;
#elif defined(PSUTIL_MACOS)
    auto read_ticks = [&]() {
        natural_t nc = 0;
        processor_cpu_load_info_t cl;
        mach_msg_type_number_t cnt;
        std::vector<std::array<uint64_t,4>> ticks;
        if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                &nc, (processor_info_array_t*)&cl, &cnt) == KERN_SUCCESS) {
            for (natural_t i = 0; i < nc; ++i) {
                ticks.push_back({cl[i].cpu_ticks[CPU_STATE_USER],
                                 cl[i].cpu_ticks[CPU_STATE_SYSTEM],
                                 cl[i].cpu_ticks[CPU_STATE_IDLE],
                                 cl[i].cpu_ticks[CPU_STATE_NICE]});
            }
            vm_deallocate(mach_task_self(), (vm_address_t)cl, cnt * sizeof(*cl));
        }
        return ticks;
    };
    auto t1 = read_ticks();
    if (interval_sec > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(interval_sec));
    auto t2 = read_ticks();
    uint64_t total = 0, idle = 0;
    for (size_t i = 0; i < std::min(t1.size(), t2.size()); ++i) {
        // BUG FIX #2: guard against unsigned underflow if tick counter wraps
        for (int j = 0; j < 4; ++j) {
            if (t2[i][j] >= t1[i][j]) total += t2[i][j] - t1[i][j];
        }
        if (t2[i][2] >= t1[i][2]) idle += t2[i][2] - t1[i][2];
    }
    return total ? 100.0 * (1.0 - static_cast<double>(idle)/total) : 0.0;
#endif
}

/**
 * cpu_percent_percpu(interval_sec)
 * Returns per-core CPU usage percentages.
 */
inline std::vector<double> cpu_percent_percpu(double interval_sec = 0.1) {
    std::vector<double> result;
#ifdef PSUTIL_LINUX
    auto s1 = detail::read_proc_stat();
    if (interval_sec > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(interval_sec));
    auto s2 = detail::read_proc_stat();
    // index 0 = aggregate, 1.. = per core
    for (size_t i = 1; i < std::min(s1.size(), s2.size()); ++i)
        result.push_back(detail::calc_cpu_percent(s1[i], s2[i]));
#elif defined(PSUTIL_WINDOWS)
    // Fix-11: Per-core breakdown requires PDH counter setup which is complex in
    // a single-header library. Returning the aggregate value for every core was
    // misleading — callers could not distinguish "unsupported" from real data.
    // Return an empty vector so callers can detect the unsupported case cleanly.
    (void)interval_sec;
    // result stays empty — indicates per-core data is not available on Windows
#elif defined(PSUTIL_MACOS)
    auto read_ticks = [&]() {
        natural_t nc = 0; processor_cpu_load_info_t cl; mach_msg_type_number_t cnt;
        std::vector<std::array<uint64_t,4>> ticks;
        if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                &nc, (processor_info_array_t*)&cl, &cnt) == KERN_SUCCESS) {
            for (natural_t i = 0; i < nc; ++i)
                ticks.push_back({cl[i].cpu_ticks[CPU_STATE_USER],
                                 cl[i].cpu_ticks[CPU_STATE_SYSTEM],
                                 cl[i].cpu_ticks[CPU_STATE_IDLE],
                                 cl[i].cpu_ticks[CPU_STATE_NICE]});
            vm_deallocate(mach_task_self(), (vm_address_t)cl, cnt * sizeof(*cl));
        }
        return ticks;
    };
    auto t1 = read_ticks();
    if (interval_sec > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(interval_sec));
    auto t2 = read_ticks();
    for (size_t i = 0; i < std::min(t1.size(), t2.size()); ++i) {
        uint64_t total=0, idle=0;
        // BUG FIX #3: guard against unsigned underflow if tick counter wraps
        for (int j=0;j<4;++j) {
            if (t2[i][j] >= t1[i][j]) total += t2[i][j]-t1[i][j];
        }
        if (t2[i][2] >= t1[i][2]) idle = t2[i][2]-t1[i][2];
        result.push_back(total ? 100.0*(1.0-static_cast<double>(idle)/total) : 0.0);
    }
#endif
    return result;
}

/**
 * cpu_freq()
 * Returns current / min / max CPU frequency in MHz.
 */
inline CpuFreq cpu_freq() {
    CpuFreq f;
#ifdef PSUTIL_LINUX
    // Try /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq (kHz → MHz)
    auto read_mhz = [](const std::string& path) -> double {
        std::ifstream file(path);
        double v = 0;
        if (file >> v) return v / 1000.0;
        return 0;
    };
    f.current = read_mhz("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    f.min     = read_mhz("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq");
    f.max     = read_mhz("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
    if (f.current == 0) {
        // Fallback: /proc/cpuinfo "cpu MHz"
        std::string text = detail::read_file("/proc/cpuinfo");
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("cpu MHz") != std::string::npos) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    f.current = std::stod(line.substr(pos+1));
                    break;
                }
            }
        }
    }
#elif defined(PSUTIL_WINDOWS)
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD mhz = 0, size = sizeof(mhz);
        RegQueryValueExA(hKey, "~MHz", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&mhz), &size);
        RegCloseKey(hKey);
        // BUG FIX #4: f.min was never set; assign same value as fallback (registry
        // does not expose min freq without WMI or CPPC; this is best-effort).
        f.current = f.min = f.max = static_cast<double>(mhz);
    }
#elif defined(PSUTIL_MACOS)
    uint64_t freq = 0;
    size_t size = sizeof(freq);
    sysctlbyname("hw.cpufrequency", &freq, &size, nullptr, 0);
    f.current = freq / 1e6;
    sysctlbyname("hw.cpufrequency_min", &freq, &size, nullptr, 0);
    f.min = freq / 1e6;
    sysctlbyname("hw.cpufrequency_max", &freq, &size, nullptr, 0);
    f.max = freq / 1e6;
#endif
    return f;
}

/**
 * cpu_stats()
 * Returns misc CPU statistics.
 */
inline CpuStats cpu_stats() {
    CpuStats cs;
#ifdef PSUTIL_LINUX
    std::string text = detail::read_file("/proc/stat");
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        std::string key; uint64_t val = 0;
        ls >> key >> val;
        if      (key == "ctxt")     cs.ctx_switches  = val;
        else if (key == "intr")     cs.interrupts     = val;
        else if (key == "softirq")  cs.soft_interrupts = val;
    }
#elif defined(PSUTIL_WINDOWS)
    // Context switches via NtQuerySystemInformation is complex; leave 0
#endif
    return cs;
}

// ═══════════════════════════════════════════════════════════════
//  Memory
// ═══════════════════════════════════════════════════════════════

/**
 * virtual_memory()
 */
inline VirtualMemory virtual_memory() {
    VirtualMemory vm;
#ifdef PSUTIL_LINUX
    auto kv = detail::parse_key_value(detail::read_file("/proc/meminfo"));
    auto kib = [&](const std::string& k) -> uint64_t {
        auto it = kv.find(k);
        if (it == kv.end()) return 0;
        // value is "12345 kB"
        std::istringstream ss(it->second);
        uint64_t v = 0; ss >> v;
        return v * 1024;
    };
    vm.total    = kib("MemTotal");
    vm.free     = kib("MemFree");
    vm.buffers  = kib("Buffers");
    // FIX #8b: Guard against unsigned underflow if Shmem > Cached + SReclaimable
    {
        uint64_t cached_raw   = kib("Cached") + kib("SReclaimable");
        uint64_t shmem        = kib("Shmem");
        vm.cached = (cached_raw >= shmem) ? (cached_raw - shmem) : 0;
    }
    vm.shared   = kib("Shmem");
    vm.active   = kib("Active");
    vm.inactive = kib("Inactive");
    vm.available = kib("MemAvailable");
    if (vm.available == 0)
        vm.available = vm.free + vm.buffers + vm.cached;
    vm.used    = vm.total - vm.available;
    // Fix-17: cap at 100.0 (Python psutil compatibility)
    vm.percent = vm.total ? std::min(100.0, 100.0 * vm.used / vm.total) : 0.0;
#elif defined(PSUTIL_WINDOWS)
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    vm.total     = ms.ullTotalPhys;
    vm.available = ms.ullAvailPhys;
    // IMPROVEMENT: guard underflow and make cast explicit
    vm.used      = (ms.ullTotalPhys >= ms.ullAvailPhys) ? ms.ullTotalPhys - ms.ullAvailPhys : 0;
    vm.free      = vm.available;
    vm.percent   = static_cast<double>(ms.dwMemoryLoad); // already 0-100
#elif defined(PSUTIL_MACOS)
    vm_size_t page = vm_page_size;
    vm_statistics64_data_t vms;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64,
                      (host_info64_t)&vms, &cnt);
    uint64_t pages_total = 0;
    size_t size = sizeof(pages_total);
    sysctlbyname("hw.memsize", &pages_total, &size, nullptr, 0);
    vm.total     = pages_total;
    vm.free      = vms.free_count * page;
    vm.active    = vms.active_count * page;
    vm.inactive  = vms.inactive_count * page;
    // BUG FIX #16: guard double unsigned subtraction; free + speculative could exceed total
    {
        uint64_t spec = static_cast<uint64_t>(vms.speculative_count) * page;
        uint64_t deduct = vm.free + spec;
        vm.used = (vm.total >= deduct) ? (vm.total - deduct) : 0;
    }
    vm.available = vm.free + vm.inactive;
    // Fix-17: cap at 100.0 (Python psutil compatibility)
    vm.percent   = vm.total ? std::min(100.0, 100.0 * vm.used / vm.total) : 0.0;
#endif
    return vm;
}

/**
 * swap_memory()
 */
inline SwapMemory swap_memory() {
    SwapMemory sm;
#ifdef PSUTIL_LINUX
    auto kv = detail::parse_key_value(detail::read_file("/proc/meminfo"));
    auto kib = [&](const std::string& k) -> uint64_t {
        auto it = kv.find(k);
        if (it == kv.end()) return 0;
        std::istringstream ss(it->second); uint64_t v=0; ss>>v; return v*1024;
    };
    sm.total = kib("SwapTotal");
    sm.free  = kib("SwapFree");
    // BUG FIX #5: guard unsigned underflow; free > total can occur on corrupted /proc data
    sm.used  = (sm.total >= sm.free) ? sm.total - sm.free : 0;
    sm.percent = sm.total ? 100.0 * sm.used / sm.total : 0.0;
    // sin/sout from /proc/vmstat
    std::string vmstat = detail::read_file("/proc/vmstat");
    std::istringstream ss(vmstat); std::string line;
    // FIX #5: Use sysconf(_SC_PAGESIZE) instead of hardcoded 4096 (ARM uses 64KB pages)
    const uint64_t page_sz = static_cast<uint64_t>(
        std::max(1L, sysconf(_SC_PAGESIZE)));
    while(std::getline(ss, line)){
        std::istringstream ls(line); std::string k; uint64_t v;
        ls >> k >> v;
        if      (k == "pswpin")  sm.sin  = v * page_sz;
        else if (k == "pswpout") sm.sout = v * page_sz;
    }
#elif defined(PSUTIL_WINDOWS)
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    sm.total = ms.ullTotalPageFile;
    sm.free  = ms.ullAvailPageFile;
    // Fix-7: guard unsigned underflow (consistent with Linux fix)
    sm.used  = (sm.total >= sm.free) ? sm.total - sm.free : 0;
    sm.percent = sm.total ? 100.0 * sm.used / sm.total : 0.0;
#elif defined(PSUTIL_MACOS)
    struct xsw_usage swu;
    size_t size = sizeof(swu);
    sysctlbyname("vm.swapusage", &swu, &size, nullptr, 0);
    sm.total   = swu.xsu_total;
    sm.used    = swu.xsu_used;
    sm.free    = swu.xsu_avail;
    sm.percent = sm.total ? 100.0 * sm.used / sm.total : 0.0;
#endif
    return sm;
}

// ═══════════════════════════════════════════════════════════════
//  Disk
// ═══════════════════════════════════════════════════════════════

/**
 * disk_partitions(all=false)
 */
inline std::vector<DiskPartition> disk_partitions(bool all = false) {
    std::vector<DiskPartition> result;
#ifdef PSUTIL_LINUX
    std::string text = detail::read_file("/proc/mounts");
    std::istringstream ss(text); std::string line;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        DiskPartition dp;
        ls >> dp.device >> dp.mountpoint >> dp.fstype >> dp.opts;
        if (!all && (dp.device == "none" || dp.device.find("tmpfs") != std::string::npos))
            continue;
        result.push_back(dp);
    }
#elif defined(PSUTIL_WINDOWS)
    char drives[512]; DWORD n = GetLogicalDriveStringsA(sizeof(drives), drives);
    for (char* p = drives; p < drives + n; p += strlen(p) + 1) {
        DiskPartition dp;
        dp.mountpoint = p;
        char fs[MAX_PATH], vol[MAX_PATH];
        if (GetVolumeInformationA(p, vol, MAX_PATH, nullptr, nullptr, nullptr,
                                   fs, MAX_PATH)) dp.fstype = fs;
        dp.device = p;
        result.push_back(dp);
    }
#elif defined(PSUTIL_MACOS)
    struct statfs* mounts; int n = getmntinfo(&mounts, MNT_NOWAIT);
    for (int i = 0; i < n; ++i) {
        DiskPartition dp;
        dp.device     = mounts[i].f_mntfromname;
        dp.mountpoint = mounts[i].f_mntonname;
        dp.fstype     = mounts[i].f_fstypename;
        result.push_back(dp);
    }
#endif
    return result;
}

/**
 * disk_usage(path)
 */
inline DiskUsage disk_usage(const std::string& path = "/") {
    DiskUsage du;
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
    struct statvfs st;
    if (statvfs(path.c_str(), &st) == 0) {
        du.total   = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
        du.free    = static_cast<uint64_t>(st.f_bfree)  * st.f_frsize;
        uint64_t avail = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
        // IMPROVEMENT: guard unsigned underflow if statvfs returns inconsistent values
        du.used    = (du.total >= du.free) ? (du.total - du.free) : 0;
        // Fix-5: Use total as denominator (mirrors Python psutil + Windows behaviour).
        // The (used+avail) form excluded reserved-blocks space causing cross-platform
        // inconsistency. Python psutil also uses total as denominator since v5.4.
        (void)avail;
        du.percent = du.total ? 100.0 * du.used / du.total : 0.0;
    }
#elif defined(PSUTIL_WINDOWS)
    ULARGE_INTEGER free_bytes, total_bytes, total_free;
    if (GetDiskFreeSpaceExA(path.c_str(), &free_bytes, &total_bytes, &total_free)) {
        du.total   = total_bytes.QuadPart;
        du.free    = free_bytes.QuadPart;
        // Fix-3: underflow guard — consistent with Linux/macOS treatment
        du.used    = (du.total >= du.free) ? (du.total - du.free) : 0;
        du.percent = du.total ? 100.0 * du.used / du.total : 0.0;
    }
#endif
    return du;
}

/**
 * disk_io_counters(perdisk=false)
 * Returns I/O stats per disk (or aggregate if perdisk=false).
 */
inline std::vector<DiskIOCounters> disk_io_counters(bool perdisk = false) {
    std::vector<DiskIOCounters> result;
#ifdef PSUTIL_LINUX
    std::string text = detail::read_file("/proc/diskstats");
    std::istringstream ss(text); std::string line;
    DiskIOCounters agg;
    while (std::getline(ss, line)) {
        // https://www.kernel.org/doc/Documentation/ABI/testing/procfs-diskstats
        std::istringstream ls(line);
        unsigned int maj, min_; std::string dev;
        uint64_t rd_ios, rd_merges, rd_sec, rd_ms,
                 wr_ios, wr_merges, wr_sec, wr_ms;
        ls >> maj >> min_ >> dev
           >> rd_ios >> rd_merges >> rd_sec >> rd_ms
           >> wr_ios >> wr_merges >> wr_sec >> wr_ms;
        if (ls.fail()) continue;
        // skip partitions (e.g. sda1) to avoid double-counting
        // FIX #3: use /sys/class/block/<dev>/partition to reliably
        // detect partitions instead of fragile suffix heuristics.
        if (!perdisk) {
            std::string part_file = "/sys/class/block/" + dev + "/partition";
            std::ifstream pf(part_file);
            if (pf.good()) continue;  // has a 'partition' file → it is a partition
        }
        DiskIOCounters c;
        c.name        = dev;
        c.read_count  = rd_ios;
        c.write_count = wr_ios;
        c.read_bytes  = rd_sec * 512;
        c.write_bytes = wr_sec * 512;
        c.read_time   = static_cast<double>(rd_ms);
        c.write_time  = static_cast<double>(wr_ms);
        if (perdisk) { result.push_back(c); }
        else {
            agg.read_count  += c.read_count;
            agg.write_count += c.write_count;
            agg.read_bytes  += c.read_bytes;
            agg.write_bytes += c.write_bytes;
            agg.read_time   += c.read_time;
            agg.write_time  += c.write_time;
        }
    }
    if (!perdisk) { agg.name = "_total"; result.push_back(agg); }
#elif defined(PSUTIL_WINDOWS)
    // Scan PhysicalDrive0…31 via IOCTL_DISK_PERFORMANCE.
    // Note: disk performance counters must be enabled; on most systems they are.
    // If empty results are seen, run 'diskperf -y' once as Administrator.
    DiskIOCounters agg_win; agg_win.name = "_total";
    bool any = false;
    for (int i = 0; i < 32; ++i) {
        char dev[32];
        std::snprintf(dev, sizeof(dev), "\\\\.\\PhysicalDrive%d", i);
        detail::HandleGuard h(CreateFileA(dev,
            GENERIC_READ,                        // GENERIC_READ: more compatible than 0
            FILE_SHARE_READ | FILE_SHARE_WRITE,  // on some OEM drivers that reject 0
            nullptr, OPEN_EXISTING, 0, nullptr));
        if (!h) continue;

        DISK_PERFORMANCE dp{};
        DWORD returned = 0;
        if (!DeviceIoControl(h.h, IOCTL_DISK_PERFORMANCE,
                             nullptr, 0,
                             &dp, sizeof(dp),
                             &returned, nullptr)) continue;

        DiskIOCounters c;
        c.name        = "PhysicalDrive" + std::to_string(i);
        c.read_count  = static_cast<uint64_t>(dp.ReadCount);
        c.write_count = static_cast<uint64_t>(dp.WriteCount);
        c.read_bytes  = dp.BytesRead.QuadPart;
        c.write_bytes = dp.BytesWritten.QuadPart;
        // LARGE_INTEGER time is in 100-ns units; convert to ms (Python psutil style)
        c.read_time   = static_cast<double>(dp.ReadTime.QuadPart)  / 10000.0;
        c.write_time  = static_cast<double>(dp.WriteTime.QuadPart) / 10000.0;

        if (perdisk) {
            result.push_back(c);
        } else {
            agg_win.read_count  += c.read_count;
            agg_win.write_count += c.write_count;
            agg_win.read_bytes  += c.read_bytes;
            agg_win.write_bytes += c.write_bytes;
            agg_win.read_time   += c.read_time;
            agg_win.write_time  += c.write_time;
            any = true;
        }
    }
    if (!perdisk) result.push_back(any ? agg_win : DiskIOCounters{"_total"});

#elif defined(PSUTIL_MACOS)
    // IOKit IOMedia statistics — mirrors Python psutil's disk_io_counters().
    // Each IOMedia object represents one physical or logical disk.
    {
        io_iterator_t iter = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOMedia"), &iter) != KERN_SUCCESS) {
            DiskIOCounters c; c.name = "_total"; result.push_back(c);
        } else {
            DiskIOCounters agg_mac; agg_mac.name = "_total";
            bool any_mac = false;
            io_registry_entry_t entry;
            while ((entry = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
                // Only whole-disk IOMedia (not partitions)
                CFBooleanRef whole = (CFBooleanRef)IORegistryEntryCreateCFProperty(
                    entry, CFSTR("Whole"), kCFAllocatorDefault, 0);
                bool is_whole = whole && CFBooleanGetValue(whole);
                if (whole) CFRelease(whole);
                if (!is_whole) { IOObjectRelease(entry); continue; }

                // Get BSD name (e.g. "disk0")
                CFStringRef bsd = (CFStringRef)IORegistryEntryCreateCFProperty(
                    entry, CFSTR("BSD Name"), kCFAllocatorDefault, 0);
                std::string disk_name;
                if (bsd) {
                    char nbuf[64] = {};
                    CFStringGetCString(bsd, nbuf, sizeof(nbuf), kCFStringEncodingUTF8);
                    disk_name = nbuf;
                    CFRelease(bsd);
                }

                // Statistics dictionary
                CFDictionaryRef stats_dict = (CFDictionaryRef)IORegistryEntryCreateCFProperty(
                    entry, CFSTR("Statistics"), kCFAllocatorDefault, 0);
                if (!stats_dict) { IOObjectRelease(entry); continue; }

                auto get_u64 = [&](const char* key) -> uint64_t {
                    CFStringRef ks = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                               kCFStringEncodingUTF8);
                    CFNumberRef n = (CFNumberRef)CFDictionaryGetValue(stats_dict, ks);
                    CFRelease(ks);
                    if (!n) return 0;
                    int64_t v = 0;
                    CFNumberGetValue(n, kCFNumberSInt64Type, &v);
                    return static_cast<uint64_t>(v < 0 ? 0 : v);
                };

                DiskIOCounters c;
                c.name        = disk_name.empty() ? "disk" : disk_name;
                c.read_count  = get_u64("Operations (Read)");
                c.write_count = get_u64("Operations (Write)");
                c.read_bytes  = get_u64("Bytes (Read)");
                c.write_bytes = get_u64("Bytes (Write)");
                // IOKit reports in nanoseconds; convert to ms
                c.read_time   = static_cast<double>(get_u64("Total Time (Read)"))  / 1e6;
                c.write_time  = static_cast<double>(get_u64("Total Time (Write)")) / 1e6;

                CFRelease(stats_dict);
                IOObjectRelease(entry);

                if (perdisk) {
                    result.push_back(c);
                } else {
                    agg_mac.read_count  += c.read_count;
                    agg_mac.write_count += c.write_count;
                    agg_mac.read_bytes  += c.read_bytes;
                    agg_mac.write_bytes += c.write_bytes;
                    agg_mac.read_time   += c.read_time;
                    agg_mac.write_time  += c.write_time;
                    any_mac = true;
                }
            }
            IOObjectRelease(iter);
            if (!perdisk) result.push_back(any_mac ? agg_mac : DiskIOCounters{"_total"});
        }
    }
#endif
    return result;
}

// ═══════════════════════════════════════════════════════════════
//  Network
// ═══════════════════════════════════════════════════════════════

/**
 * net_io_counters(pernic=false)
 */
inline std::vector<NetIOCounters> net_io_counters(bool pernic = false) {
    std::vector<NetIOCounters> result;
#ifdef PSUTIL_LINUX
    std::string text = detail::read_file("/proc/net/dev");
    std::istringstream ss(text); std::string line;
    std::getline(ss, line); std::getline(ss, line); // skip 2 header lines
    NetIOCounters agg; agg.name = "_total";
    while (std::getline(ss, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string iface = line.substr(0, colon);
        iface.erase(0, iface.find_first_not_of(" \t"));
        iface.erase(iface.find_last_not_of(" \t\r\n")+1);
        std::istringstream ls(line.substr(colon+1));
        NetIOCounters c; c.name = iface;
        // FIX #2: /proc/net/dev has 8 recv fields then 8 send fields.
        // After the 4 we care about (bytes,packets,errs,drop) there are 4 more
        // recv-only fields (fifo, frame, compressed, multicast) to skip.
        // std::ws only strips whitespace — we must read-and-discard into a dummy.
        uint64_t _skip;
        ls >> c.bytes_recv >> c.packets_recv >> c.errin >> c.dropin
           >> _skip >> _skip >> _skip >> _skip   // fifo, frame, compressed, multicast
           >> c.bytes_sent >> c.packets_sent >> c.errout >> c.dropout;
        if (pernic) result.push_back(c);
        else {
            agg.bytes_recv   += c.bytes_recv;
            agg.bytes_sent   += c.bytes_sent;
            agg.packets_recv += c.packets_recv;
            agg.packets_sent += c.packets_sent;
            agg.errin        += c.errin;
            agg.errout       += c.errout;
            agg.dropin       += c.dropin;
            agg.dropout      += c.dropout;
        }
    }
    if (!pernic) result.push_back(agg);
#elif defined(PSUTIL_WINDOWS)
    // Fix-9: GetIfTable retry loop — new interfaces can appear between the size query
    // and the actual call (race condition). Retry up to 3× on ERROR_INSUFFICIENT_BUFFER.
    {
        DWORD size = 0;
        GetIfTable(nullptr, &size, FALSE);
        MIB_IFTABLE* table = nullptr;
        std::vector<char> iftable_buf;
        for (int retry = 0; retry < 3; ++retry) {
            iftable_buf.resize(size);
            table = reinterpret_cast<MIB_IFTABLE*>(iftable_buf.data());
            DWORD ret = GetIfTable(table, &size, FALSE);
            if (ret == NO_ERROR) break;
            if (ret != ERROR_INSUFFICIENT_BUFFER) { table = nullptr; break; }
            // Buffer was too small (new interface appeared); size is updated — retry
        }
    if (table) {
        NetIOCounters agg; agg.name = "_total";
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            auto& row = table->table[i];
            NetIOCounters c;
            c.name         = row.bDescr[0]
                             ? std::string(reinterpret_cast<char*>(row.bDescr))
                             : "unknown";
            c.bytes_sent   = row.dwOutOctets;
            c.bytes_recv   = row.dwInOctets;
            c.packets_sent = row.dwOutUcastPkts;
            c.packets_recv = row.dwInUcastPkts;
            c.errin        = row.dwInErrors;
            c.errout       = row.dwOutErrors;
            if (pernic) result.push_back(c);
            else {
                agg.bytes_sent   += c.bytes_sent;
                agg.bytes_recv   += c.bytes_recv;
                agg.packets_sent += c.packets_sent;
                agg.packets_recv += c.packets_recv;
                agg.errin        += c.errin;
                agg.errout       += c.errout;
            }
        }
        if (!pernic) result.push_back(agg);
    }
    } // end retry block
#elif defined(PSUTIL_MACOS)
    // Use sysctl net.route.0.0.dump or getifaddrs - simplified version via ifconfig data
    // Returns zero-filled aggregate
    NetIOCounters c; c.name = "_total";
    result.push_back(c);
#endif
    return result;
}

/**
 * net_if_addrs()
 * Returns map of interface name → list of addresses.
 */
inline std::map<std::string, std::vector<NetAddress>> net_if_addrs() {
    std::map<std::string, std::vector<NetAddress>> result;
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
    // Fix-R3: IfAddrsGuard RAII — freeifaddrs() guaranteed even if push_back throws
    detail::IfAddrsGuard ifa_guard;
    if (!ifa_guard) return result;
    char buf[INET6_ADDRSTRLEN];
    for (auto* ifa = ifa_guard.p; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        NetAddress na;
        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            na.family = "AF_INET";
            inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, buf, sizeof(buf));
            na.address = buf;
            if (ifa->ifa_netmask) {
                inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_netmask)->sin_addr, buf, sizeof(buf));
                na.netmask = buf;
            }
        } else if (family == AF_INET6) {
            na.family = "AF_INET6";
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr, buf, sizeof(buf));
            na.address = buf;
        } else {
            na.family = "AF_LINK";
        }
        result[ifa->ifa_name].push_back(na);
    }
#elif defined(PSUTIL_WINDOWS)
    // FIX #2 + FIX #5: RAII + GetAdaptersAddresses (IPv6-capable, not deprecated)
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size);
    std::vector<char> aa_buf(size);
    auto* head = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(aa_buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX,
                              nullptr, head, &size) == NO_ERROR) {
        char addr_str[INET6_ADDRSTRLEN];
        for (auto* p = head; p; p = p->Next) {
            std::string iface_name = p->AdapterName;
            for (auto* ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
                NetAddress na;
                int af = ua->Address.lpSockaddr->sa_family;
                if (af == AF_INET) {
                    na.family = "AF_INET";
                    detail::psutil_inet_ntop(AF_INET,
                        &reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr)->sin_addr,
                        addr_str, sizeof(addr_str));
                    na.address = addr_str;
                } else if (af == AF_INET6) {
                    na.family = "AF_INET6";
                    detail::psutil_inet_ntop(AF_INET6,
                        &reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr)->sin6_addr,
                        addr_str, sizeof(addr_str));
                    na.address = addr_str;
                } else {
                    na.family = "AF_LINK";
                }
                result[iface_name].push_back(na);
            }
        }
    }
#endif
    return result;
}

/**
 * net_if_stats()
 * Returns per-interface statistics.
 */
inline std::map<std::string, NetIfStats> net_if_stats() {
    std::map<std::string, NetIfStats> result;
#ifdef PSUTIL_LINUX
    auto addrs = net_if_addrs();
    for (auto& [iface, _] : addrs) {
        NetIfStats s;
        // Check if up via /sys/class/net/<iface>/operstate
        std::string state = detail::read_file("/sys/class/net/" + iface + "/operstate");
        s.isup = (state.find("up") != std::string::npos);
        // MTU
        std::ifstream mtu_f("/sys/class/net/" + iface + "/mtu");
        mtu_f >> s.mtu;
        // Speed: virtual/loopback interfaces return -1; clamp to 0
        {
            int raw_speed = 0;
            std::ifstream spd_f("/sys/class/net/" + iface + "/speed");
            if (spd_f >> raw_speed)
                s.speed = (raw_speed > 0) ? raw_speed : 0; // IMPROVEMENT: -1 → 0
        }
        result[iface] = s;
    }
#elif defined(PSUTIL_WINDOWS)
    // Fix-5: Same GetIfTable retry pattern as net_io_counters (Fix-9) for consistency.
    // A new interface can appear between the size-query call and the real call.
    {
        DWORD size = 0;
        GetIfTable(nullptr, &size, FALSE);
        MIB_IFTABLE* table = nullptr;
        std::vector<char> ifstats_buf;
        for (int retry = 0; retry < 3; ++retry) {
            ifstats_buf.resize(size);
            table = reinterpret_cast<MIB_IFTABLE*>(ifstats_buf.data());
            DWORD ret = GetIfTable(table, &size, FALSE);
            if (ret == NO_ERROR) break;
            if (ret != ERROR_INSUFFICIENT_BUFFER) { table = nullptr; break; }
        }
        if (table) {
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                auto& row = table->table[i];
                NetIfStats s;
                s.isup  = (row.dwOperStatus == IF_OPER_STATUS_OPERATIONAL);
                s.speed = static_cast<int>(row.dwSpeed / 1000000);
                s.mtu   = static_cast<int>(row.dwMtu);
                std::string name(reinterpret_cast<char*>(row.bDescr));
                result[name] = s;
            }
        }
    } // end retry block
#endif
    return result;
}

/**
 * net_connections(kind="inet")
 * Returns active network connections.
 */
inline std::vector<NetConnection> net_connections(const std::string& kind = "inet") {
    std::vector<NetConnection> result;
#ifdef PSUTIL_LINUX
    // Fix-6: IPv6-capable address parser.
    // Format: "<hex_ip>:<hex_port>" where hex_ip is 8 chars (IPv4) or 32 chars (IPv6).
    // Both are little-endian byte groups as written by the Linux kernel.
    auto parse_hex_addr = [](const std::string& hex, bool is_v6,
                              std::string& addr, int& port) {
        auto colon = hex.find(':');
        if (colon == std::string::npos) return;
        try {
            port = static_cast<int>(std::stoul(hex.substr(colon + 1), nullptr, 16));
            std::string ip_hex = hex.substr(0, colon);
            if (is_v6) {
                // IPv6: 32 hex chars — four little-endian 4-byte words
                if (ip_hex.size() < 32) return;
                struct in6_addr in6;
                std::memset(&in6, 0, sizeof(in6));
                // Each group of 8 hex chars is one little-endian uint32; byte-reverse per group
                for (int grp = 0; grp < 4; ++grp) {
                    unsigned long word = std::stoul(ip_hex.substr(grp * 8, 8), nullptr, 16);
                    for (int b = 0; b < 4; ++b)
                        in6.s6_addr[grp * 4 + b] = static_cast<uint8_t>(word >> (b * 8));
                }
                char buf[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &in6, buf, sizeof(buf));
                addr = buf;
            } else {
                // IPv4: 8 hex chars, little-endian uint32
                if (ip_hex.size() < 8) return;
                uint32_t ip = static_cast<uint32_t>(std::stoul(ip_hex, nullptr, 16));
                struct in_addr in; in.s_addr = ip;
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &in, buf, sizeof(buf));
                addr = buf;
            }
        } catch (...) {}
    };

    auto parse_file = [&](const std::string& path, const std::string& type, bool is_v6) {
        std::ifstream f(path); if (!f) return;
        std::string line;
        std::getline(f, line); // skip header
        while (std::getline(f, line)) {
            std::istringstream ls(line);
            // BUG FIX #13 (Critical): /proc/net/tcp first field is "0:" (digit+colon).
            // Reading into int 'idx' consumes only the digit and leaves ':' in the stream,
            // so the next >> reads ':' into 'local' instead of the actual hex address.
            // Fix: read the entire "0:" token as a string (sl) to drain the colon too.
            std::string sl, local, remote, state_hex;
            ls >> sl >> local >> remote >> state_hex;
            if (ls.fail()) continue;
            NetConnection c;
            c.type   = type;
            c.family = is_v6 ? "AF_INET6" : "AF_INET";
            parse_hex_addr(local,  is_v6, c.laddr, c.lport);
            parse_hex_addr(remote, is_v6, c.raddr, c.rport);
            // state map (TCP)
            static const std::map<int,std::string> states = {
                {1,"ESTABLISHED"},{2,"SYN_SENT"},{3,"SYN_RECV"},
                {4,"FIN_WAIT1"},{5,"FIN_WAIT2"},{6,"TIME_WAIT"},
                {7,"CLOSE"},{8,"CLOSE_WAIT"},{9,"LAST_ACK"},
                {10,"LISTEN"},{11,"CLOSING"},{0,"NONE"}
            };
            int st = 0;
            try { st = static_cast<int>(std::stoul(state_hex, nullptr, 16)); } catch (...) {}
            auto it = states.find(st);
            c.status = it != states.end() ? it->second : "UNKNOWN";
            result.push_back(c);
        }
    };

    if (kind == "inet" || kind == "tcp"  || kind == "tcp4")
        parse_file("/proc/net/tcp",  "SOCK_STREAM", false);
    if (kind == "inet" || kind == "tcp"  || kind == "tcp6")
        parse_file("/proc/net/tcp6", "SOCK_STREAM", true);
    if (kind == "inet" || kind == "udp"  || kind == "udp4")
        parse_file("/proc/net/udp",  "SOCK_DGRAM",  false);
    if (kind == "inet" || kind == "udp"  || kind == "udp6")
        parse_file("/proc/net/udp6", "SOCK_DGRAM",  true);

#elif defined(PSUTIL_WINDOWS)
    // TCP state number → string (WinAPI MIB_TCP_STATE values start at 1)
    static const char* const kTcpStates[] = {
        "", "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECV",
        "ESTABLISHED", "FIN_WAIT1", "FIN_WAIT2", "CLOSE_WAIT",
        "CLOSING", "LAST_ACK", "TIME_WAIT", "DELETE_TCB"
    };
    constexpr DWORD kStateCount = sizeof(kTcpStates) / sizeof(kTcpStates[0]);

    // Helper: format sockaddr bytes to dotted string
    auto fmt4 = [](DWORD addr, char* buf) {
        struct in_addr ia; ia.s_addr = addr;
        detail::psutil_inet_ntop(AF_INET, &ia, buf, INET_ADDRSTRLEN);
    };
    auto fmt6 = [](const UCHAR* addr, char* buf) {
        struct in6_addr ia;
        std::memcpy(ia.s6_addr, addr, 16);
        detail::psutil_inet_ntop(AF_INET6, &ia, buf, INET6_ADDRSTRLEN);
    };

    // Generic retry-resize helper that works for all four table types
    // Returns filled buffer or empty vector on error
    auto fetch_table = [](auto getter, auto... args) -> std::vector<char> {
        DWORD size = 0;
        getter(nullptr, &size, TRUE, args...);
        for (int retry = 0; retry < 3; ++retry) {
            std::vector<char> buf(size);
            DWORD ret = getter(buf.data(), &size, TRUE, args...);
            if (ret == NO_ERROR) return buf;
            if (ret != ERROR_INSUFFICIENT_BUFFER) return {};
        }
        return {};
    };

    bool want_tcp  = (kind == "inet" || kind.find("tcp") != std::string::npos);
    bool want_udp  = (kind == "inet" || kind.find("udp") != std::string::npos);
    bool want_ipv4 = (kind == "inet" || kind.find('4') != std::string::npos
                      || (kind.find('6') == std::string::npos));
    bool want_ipv6 = (kind == "inet" || kind.find('6') != std::string::npos);

    // ── TCP IPv4 ──────────────────────────────────────────────
    if (want_tcp && want_ipv4) {
        auto buf = fetch_table(GetExtendedTcpTable,
                               AF_INET, TCP_TABLE_OWNER_PID_ALL, DWORD(0));
        if (!buf.empty()) {
            auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            char la[INET_ADDRSTRLEN], ra[INET_ADDRSTRLEN];
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                auto& r = t->table[i];
                NetConnection c;
                c.family = "AF_INET"; c.type = "SOCK_STREAM";
                c.pid    = static_cast<int>(r.dwOwningPid);
                fmt4(r.dwLocalAddr,  la); c.laddr = la;
                fmt4(r.dwRemoteAddr, ra); c.raddr = ra;
                c.lport = ntohs(static_cast<u_short>(r.dwLocalPort));
                c.rport = ntohs(static_cast<u_short>(r.dwRemotePort));
                c.status = (r.dwState < kStateCount) ? kTcpStates[r.dwState] : "UNKNOWN";
                result.push_back(std::move(c));
            }
        }
    }

    // ── TCP IPv6 ──────────────────────────────────────────────
    if (want_tcp && want_ipv6) {
        auto buf = fetch_table(GetExtendedTcpTable,
                               AF_INET6, TCP_TABLE_OWNER_PID_ALL, DWORD(0));
        if (!buf.empty()) {
            auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            char la[INET6_ADDRSTRLEN], ra[INET6_ADDRSTRLEN];
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                auto& r = t->table[i];
                NetConnection c;
                c.family = "AF_INET6"; c.type = "SOCK_STREAM";
                c.pid    = static_cast<int>(r.dwOwningPid);
                fmt6(r.ucLocalAddr,  la); c.laddr = la;
                fmt6(r.ucRemoteAddr, ra); c.raddr = ra;
                c.lport = ntohs(static_cast<u_short>(r.dwLocalPort));
                c.rport = ntohs(static_cast<u_short>(r.dwRemotePort));
                c.status = (r.dwState < kStateCount) ? kTcpStates[r.dwState] : "UNKNOWN";
                result.push_back(std::move(c));
            }
        }
    }

    // ── UDP IPv4 ──────────────────────────────────────────────
    if (want_udp && want_ipv4) {
        auto buf = fetch_table(GetExtendedUdpTable,
                               AF_INET, UDP_TABLE_OWNER_PID, DWORD(0));
        if (!buf.empty()) {
            auto* t = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
            char la[INET_ADDRSTRLEN];
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                auto& r = t->table[i];
                NetConnection c;
                c.family = "AF_INET"; c.type = "SOCK_DGRAM";
                c.pid    = static_cast<int>(r.dwOwningPid);
                fmt4(r.dwLocalAddr, la); c.laddr = la;
                c.lport  = ntohs(static_cast<u_short>(r.dwLocalPort));
                c.raddr  = "*"; c.rport = 0; c.status = "NONE";
                result.push_back(std::move(c));
            }
        }
    }

    // ── UDP IPv6 ──────────────────────────────────────────────
    if (want_udp && want_ipv6) {
        auto buf = fetch_table(GetExtendedUdpTable,
                               AF_INET6, UDP_TABLE_OWNER_PID, DWORD(0));
        if (!buf.empty()) {
            auto* t = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buf.data());
            char la[INET6_ADDRSTRLEN];
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                auto& r = t->table[i];
                NetConnection c;
                c.family = "AF_INET6"; c.type = "SOCK_DGRAM";
                c.pid    = static_cast<int>(r.dwOwningPid);
                fmt6(r.ucLocalAddr, la); c.laddr = la;
                c.lport  = ntohs(static_cast<u_short>(r.dwLocalPort));
                c.raddr  = "*"; c.rport = 0; c.status = "NONE";
                result.push_back(std::move(c));
            }
        }
    }

#elif defined(PSUTIL_MACOS)
    // macOS: enumerate all PIDs, then query each FD for socket info via
    // proc_pidfdinfo (PROC_PIDFDSOCKETINFO). Requires <libproc.h>.
    // This mirrors Python psutil's approach on macOS.
    {
        // Phase 1: get all PIDs
        int pid_buf_size = proc_listallpids(nullptr, 0);
        if (pid_buf_size <= 0) { (void)kind; return result; }
        std::vector<pid_t> pids(static_cast<size_t>(pid_buf_size) + 64);
        int npids = proc_listallpids(pids.data(),
            static_cast<int>(pids.size() * sizeof(pid_t)));
        if (npids <= 0) return result;
        pids.resize(static_cast<size_t>(npids));

        bool want_tcp  = (kind == "inet" || kind.find("tcp") != std::string::npos);
        bool want_udp  = (kind == "inet" || kind.find("udp") != std::string::npos);

        for (pid_t pid : pids) {
            // Phase 2: list file descriptors for this PID
            int fd_buf_size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
            if (fd_buf_size <= 0) continue;
            std::vector<proc_fdinfo> fds(
                static_cast<size_t>(fd_buf_size) / sizeof(proc_fdinfo) + 4);
            int nfds = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(),
                static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
            if (nfds <= 0) continue;
            int count = nfds / static_cast<int>(sizeof(proc_fdinfo));

            for (int fi = 0; fi < count; ++fi) {
                if (fds[fi].proc_fdtype != PROX_FDTYPE_SOCKET) continue;

                // Phase 3: get socket details
                struct socket_fdinfo si;
                std::memset(&si, 0, sizeof(si));
                int r = proc_pidfdinfo(pid, fds[fi].proc_fd,
                                       PROC_PIDFDSOCKETINFO, &si, sizeof(si));
                if (r < static_cast<int>(sizeof(si))) continue;

                auto& s = si.psi;
                int family = s.soi_family;
                int stype  = s.soi_type;
                bool is_tcp = (stype == SOCK_STREAM);
                bool is_udp = (stype == SOCK_DGRAM);

                if (family != AF_INET && family != AF_INET6) continue;
                if (is_tcp && !want_tcp) continue;
                if (is_udp && !want_udp) continue;

                NetConnection c;
                c.pid    = static_cast<int>(pid);
                c.family = (family == AF_INET) ? "AF_INET" : "AF_INET6";
                c.type   = is_tcp ? "SOCK_STREAM" : "SOCK_DGRAM";

                char la[INET6_ADDRSTRLEN] = {}, ra[INET6_ADDRSTRLEN] = {};
                if (family == AF_INET) {
                    auto& in = s.soi_proto.pri_tcp.tcpsi_ini;
                    inet_ntop(AF_INET,
                              &in.insi_laddr.ina_46.i46a_addr4, la, sizeof(la));
                    inet_ntop(AF_INET,
                              &in.insi_faddr.ina_46.i46a_addr4, ra, sizeof(ra));
                    c.lport = ntohs(in.insi_lport);
                    c.rport = ntohs(in.insi_fport);
                } else { // AF_INET6
                    auto& in = s.soi_proto.pri_tcp.tcpsi_ini;
                    inet_ntop(AF_INET6, &in.insi_laddr.ina_6, la, sizeof(la));
                    inet_ntop(AF_INET6, &in.insi_faddr.ina_6, ra, sizeof(ra));
                    c.lport = ntohs(in.insi_lport);
                    c.rport = ntohs(in.insi_fport);
                }
                c.laddr = la; c.raddr = ra;

                if (is_tcp) {
                    static const char* const kMacTcpStates[] = {
                        "CLOSED","LISTEN","SYN_SENT","SYN_RECV",
                        "ESTABLISHED","CLOSE_WAIT","FIN_WAIT1","CLOSING",
                        "LAST_ACK","FIN_WAIT2","TIME_WAIT"
                    };
                    int st = s.soi_proto.pri_tcp.tcpsi_state;
                    c.status = (st >= 0 && st < 11) ? kMacTcpStates[st] : "UNKNOWN";
                } else {
                    c.status = "NONE";
                }
                result.push_back(std::move(c));
            }
        }
    }
#endif
    return result;
}

// ═══════════════════════════════════════════════════════════════
//  System
// ═══════════════════════════════════════════════════════════════

/**
 * boot_time()
 * Returns system boot time as Unix timestamp (seconds).
 */
inline double boot_time() {
#ifdef PSUTIL_LINUX
    std::string text = detail::read_file("/proc/stat");
    std::istringstream ss(text); std::string line;
    while (std::getline(ss, line)) {
        if (line.rfind("btime", 0) == 0) {
            std::istringstream ls(line);
            std::string k; double t; ls >> k >> t;
            return t;
        }
    }
    return 0;
#elif defined(PSUTIL_WINDOWS)
    // Fix-8: Use FILETIME-based calculation instead of GetTickCount64 + system_clock.
    // GetTickCount64 and system_clock can drift apart; FILETIME stays consistent.
    // Steps:
    //  1. Get current wall clock as FILETIME (100-ns intervals since 1601-01-01 UTC)
    //  2. Convert to Unix epoch (subtract 116444736000000000 × 100ns = 11644473600s)
    //  3. Subtract uptime (GetTickCount64 ms → seconds) to get boot time
    {
        FILETIME ft_now;
        GetSystemTimeAsFileTime(&ft_now);
        ULARGE_INTEGER now_ui;
        now_ui.LowPart  = ft_now.dwLowDateTime;
        now_ui.HighPart = ft_now.dwHighDateTime;
        constexpr double FILETIME_TO_UNIX_EPOCH = 11644473600.0; // seconds 1601→1970
        constexpr double HNSEC_TO_SEC           = 1e-7;          // 100-ns → seconds
        double wall_unix = now_ui.QuadPart * HNSEC_TO_SEC - FILETIME_TO_UNIX_EPOCH;
        double uptime_s  = GetTickCount64() / 1000.0;
        return wall_unix - uptime_s;
    }
#elif defined(PSUTIL_MACOS)
    struct timeval tv;
    size_t size = sizeof(tv);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    sysctl(mib, 2, &tv, &size, nullptr, 0);
    return static_cast<double>(tv.tv_sec) + tv.tv_usec / 1e6;
#endif
}

/**
 * users()
 * Returns currently logged-in users.
 */
inline std::vector<UserInfo> users() {
    std::vector<UserInfo> result;
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
    {
        // Fix-R4: UtmpxGuard RAII — endutxent() guaranteed even if push_back throws
        detail::UtmpxGuard utmpx_guard;
        struct utmpx* entry;
        while ((entry = getutxent()) != nullptr) {
            if (entry->ut_type != USER_PROCESS) continue;
            UserInfo u;
            u.name     = entry->ut_user;
            u.terminal = entry->ut_line;
            u.host     = entry->ut_host;
            u.started  = static_cast<double>(entry->ut_tv.tv_sec);
            u.pid      = entry->ut_pid;
            result.push_back(u);
        }
    } // UtmpxGuard destructor calls endutxent() here
#elif defined(PSUTIL_WINDOWS)
    // WTSEnumerateSessions is available but requires wtsapi32 — stub
    UserInfo u;
    char buf[256]; DWORD size = sizeof(buf);
    if (GetUserNameA(buf, &size)) u.name = buf;
    u.terminal = "console";
    result.push_back(u);
#endif
    return result;
}

// ═══════════════════════════════════════════════════════════════
//  Battery
// ═══════════════════════════════════════════════════════════════

/**
 * sensors_battery()
 * Returns battery information (nullopt if no battery).
 */
inline std::optional<BatteryInfo> sensors_battery() {
#ifdef PSUTIL_LINUX
    // Try /sys/class/power_supply/BAT0
    for (const auto* bat : {"BAT0","BAT1","BAT2"}) {
        std::string base = std::string("/sys/class/power_supply/") + bat + "/";
        std::ifstream cap(base + "capacity");
        if (!cap) continue;
        BatteryInfo bi;
        cap >> bi.percent;
        std::string status = detail::read_file(base + "status");
        bi.power_plugged = (status.find("Discharging") == std::string::npos);
        std::ifstream energy_now(base + "energy_now");
        std::ifstream power_now(base + "power_now");
        if (energy_now && power_now && !bi.power_plugged) {
            double en, pw;
            energy_now >> en; power_now >> pw;
            if (pw > 0) bi.secsleft = (en / pw) * 3600.0;
        }
        return bi;
    }
    return std::nullopt;
#elif defined(PSUTIL_WINDOWS)
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return std::nullopt;
    if (ps.BatteryFlag == 128) return std::nullopt; // no battery
    BatteryInfo bi;
    // BUG FIX #12: BatteryLifePercent == 255 means "unknown" per WinAPI docs, not 255%
    bi.percent       = (ps.BatteryLifePercent <= 100)
                       ? static_cast<double>(ps.BatteryLifePercent) : -1.0;
    bi.power_plugged = (ps.ACLineStatus == 1);
    bi.secsleft      = ps.BatteryLifeTime == 0xFFFFFFFF ? -1
                       : static_cast<double>(ps.BatteryLifeTime);
    return bi;
#elif defined(PSUTIL_MACOS)
    // IOKit CFDictionary approach
    return std::nullopt; // simplified
#endif
}

// ═══════════════════════════════════════════════════════════════
//  Process list
// ═══════════════════════════════════════════════════════════════

/**
 * pids()
 * Returns list of all running process IDs.
 */
inline std::vector<int> pids() {
    std::vector<int> result;
#ifdef PSUTIL_LINUX
    // Fix-R1: DirGuard RAII — closedir() guaranteed even if push_back throws
    detail::DirGuard dir(opendir("/proc"));
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir.d)) != nullptr) {
        // BUG FIX #7: some filesystems return DT_UNKNOWN; treat it as a potential directory
        if (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) {
            bool is_num = true;
            for (char* p = entry->d_name; *p; ++p)
                // Fix-12: std::isdigit + unsigned char cast to avoid UB on negative char values
                if (!std::isdigit(static_cast<unsigned char>(*p))) { is_num = false; break; }
            if (is_num) result.push_back(std::stoi(entry->d_name));
        }
    }
#elif defined(PSUTIL_WINDOWS)
    // Fix-2a: HandleGuard RAII — CloseHandle guaranteed even if push_back throws
    detail::HandleGuard snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return result;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    if (Process32First(snap.h, &pe)) {
        do { result.push_back(static_cast<int>(pe.th32ProcessID)); }
        while (Process32Next(snap.h, &pe));
    }
#elif defined(PSUTIL_MACOS)
    int n = proc_listallpids(nullptr, 0);
    if (n <= 0) return result;
    std::vector<pid_t> buf(n);
    n = proc_listallpids(buf.data(), n * sizeof(pid_t));
    for (int i = 0; i < n; ++i) result.push_back(buf[i]);
#endif
    std::sort(result.begin(), result.end());
    return result;
}

/**
 * pid_exists(pid)
 */
inline bool pid_exists(int pid) {
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
    return kill(pid, 0) == 0 || errno != ESRCH;
#elif defined(PSUTIL_WINDOWS)
    // Fix-2b: HandleGuard RAII — CloseHandle guaranteed on all return paths
    detail::HandleGuard h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      static_cast<DWORD>(pid)));
    if (!h) return false;
    DWORD exit_code = 0;
    GetExitCodeProcess(h.h, &exit_code);
    return exit_code == STILL_ACTIVE;
#endif
}

// ═══════════════════════════════════════════════════════════════
//  Process class
// ═══════════════════════════════════════════════════════════════

class Process {
public:
    /**
     * @throws NoSuchProcess if pid does not exist at construction time.
     * @warning TOCTOU: The process may exit at any moment after construction.
     *          Subsequent method calls may throw NoSuchProcess even when the
     *          constructor succeeded. Always catch NoSuchProcess at call sites.
     */
    explicit Process(int pid) : pid_(pid) {
        if (!pid_exists(pid)) throw NoSuchProcess(pid);
    }

    int  pid()        const noexcept { return pid_; }
    /** True if the process is still alive */
    bool is_running() const noexcept { return pid_exists(pid_); }

    /** Process name */
    std::string name() const {
#ifdef PSUTIL_LINUX
        std::string s = detail::read_file("/proc/" + std::to_string(pid_) + "/comm");
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
#elif defined(PSUTIL_WINDOWS)
        // Fix-R2: HandleGuard RAII — CloseHandle() guaranteed on early return or exception
        detail::HandleGuard hg(open_handle(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ));
        char buf[MAX_PATH] = {};
        if (hg) GetModuleBaseNameA(hg.h, nullptr, buf, MAX_PATH);
        return buf;
#elif defined(PSUTIL_MACOS)
        char buf[PROC_PIDPATHINFO_MAXSIZE] = {};
        proc_name(pid_, buf, sizeof(buf));
        return buf;
#endif
    }

    /** Full executable path */
    std::string exe() const {
#ifdef PSUTIL_LINUX
        char buf[4096] = {};
        ssize_t len = readlink(("/proc/" + std::to_string(pid_) + "/exe").c_str(),
                               buf, sizeof(buf)-1);
        return len > 0 ? std::string(buf, len) : "";
#elif defined(PSUTIL_WINDOWS)
        // Fix-R2: HandleGuard RAII
        detail::HandleGuard hg(open_handle(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ));
        char buf[MAX_PATH] = {};
        if (hg) GetModuleFileNameExA(hg.h, nullptr, buf, MAX_PATH);
        return buf;
#elif defined(PSUTIL_MACOS)
        char buf[PROC_PIDPATHINFO_MAXSIZE] = {};
        proc_pidpath(pid_, buf, sizeof(buf));
        return buf;
#endif
    }

    /** Command-line arguments */
    std::vector<std::string> cmdline() const {
        std::vector<std::string> result;
#ifdef PSUTIL_LINUX
        std::string raw = detail::read_file("/proc/" + std::to_string(pid_) + "/cmdline");
        std::string token;
        for (char c : raw) {
            if (c == '\0') { if (!token.empty()) { result.push_back(token); token.clear(); } }
            else token += c;
        }
        if (!token.empty()) result.push_back(token);
#elif defined(PSUTIL_WINDOWS)
        // GetCommandLine only returns current process's cmdline
        result.push_back(exe());
#endif
        return result;
    }

    /** Current working directory */
    std::string cwd() const {
#ifdef PSUTIL_LINUX
        char buf[4096] = {};
        ssize_t len = readlink(("/proc/" + std::to_string(pid_) + "/cwd").c_str(),
                               buf, sizeof(buf)-1);
        return len > 0 ? std::string(buf, len) : "";
#else
        return "";
#endif
    }

    /** User name that owns the process */
    std::string username() const {
#ifdef PSUTIL_LINUX
        std::string status = detail::read_file("/proc/" + std::to_string(pid_) + "/status");
        auto kv = detail::parse_key_value(status, ":");
        auto it = kv.find("Uid");
        if (it != kv.end()) {
            std::istringstream ss(it->second);
            uid_t uid; ss >> uid;
            // BUG FIX #8: getpwuid() is not thread-safe; use reentrant getpwuid_r
            struct passwd pw_buf, *pw = nullptr;
            char buf[1024];
            if (getpwuid_r(uid, &pw_buf, buf, sizeof(buf), &pw) == 0 && pw)
                return pw->pw_name;
        }
        return "";
#else
        return "";
#endif
    }

    /** Process status */
    ProcessStatus status() const {
        ProcessStatus ps;
#ifdef PSUTIL_LINUX
        std::string status_text = detail::read_file(
            "/proc/" + std::to_string(pid_) + "/status");
        auto kv = detail::parse_key_value(status_text, ":");
        auto it = kv.find("State");
        if (it != kv.end()) {
            ps.str = it->second;
            char c = it->second.empty() ? '?' : it->second[0];
            using S = ProcessStatus::State;
            switch (c) {
                case 'R': ps.state = S::Running;   break;
                case 'S': ps.state = S::Sleeping;  break;
                case 'D': ps.state = S::DiskSleep; break;
                case 'T': ps.state = S::Stopped;   break;
                case 'Z': ps.state = S::Zombie;    break;
                case 'X': ps.state = S::Dead;      break;
                default:  ps.state = S::Other;
            }
        }
#elif defined(PSUTIL_WINDOWS)
        ps.str = "running"; ps.state = ProcessStatus::State::Running;
#endif
        return ps;
    }

    /** Parent process ID */
    int ppid() const {
#ifdef PSUTIL_LINUX
        auto kv = detail::parse_key_value(
            detail::read_file("/proc/" + std::to_string(pid_) + "/status"), ":");
        auto it = kv.find("PPid");
        // BUG FIX #9: std::stoi throws on non-numeric input; wrap in try-catch
        if (it != kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
        return -1;
#elif defined(PSUTIL_WINDOWS)
        // FIX #4: Use HandleGuard RAII — avoids handle leak when
        // CreateToolhelp32Snapshot returns INVALID_HANDLE_VALUE.
        detail::HandleGuard snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snap) return -1;
        PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
        if (Process32First(snap.h, &pe)) {
            do {
                if (static_cast<int>(pe.th32ProcessID) == pid_) {
                    return static_cast<int>(pe.th32ParentProcessID);
                }
            } while (Process32Next(snap.h, &pe));
        }
        return -1;
#else
        return -1;
#endif
    }

    /** Number of threads */
    int num_threads() const {
#ifdef PSUTIL_LINUX
        auto kv = detail::parse_key_value(
            detail::read_file("/proc/" + std::to_string(pid_) + "/status"), ":");
        auto it = kv.find("Threads");
        // BUG FIX #10: std::stoi throws on non-numeric input; wrap in try-catch
        if (it != kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
        return 1;
#elif defined(PSUTIL_WINDOWS)
        // FIX #4: Use HandleGuard RAII — avoids handle leak on INVALID_HANDLE_VALUE.
        detail::HandleGuard snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snap) return 1;
        PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
        if (Process32First(snap.h, &pe)) {
            do {
                if (static_cast<int>(pe.th32ProcessID) == pid_) {
                    return static_cast<int>(pe.cntThreads);
                }
            } while (Process32Next(snap.h, &pe));
        }
        return 1;
#else
        return 1;
#endif
    }

    /** Memory info */
    ProcessMemoryInfo memory_info() const {
        ProcessMemoryInfo mi;
#ifdef PSUTIL_LINUX
        // /proc/<pid>/statm: pages  size resident shared text lib data dirty
        std::string raw = detail::read_file("/proc/" + std::to_string(pid_) + "/statm");
        std::istringstream ss(raw);
        uint64_t size, rss, shared, text, lib, data, dirty;
        ss >> size >> rss >> shared >> text >> lib >> data >> dirty;
        // FIX #6: sysconf can return -1 on error; clamp to a safe minimum
        long page_raw = sysconf(_SC_PAGESIZE);
        uint64_t page = (page_raw > 0) ? static_cast<uint64_t>(page_raw) : 4096u;
        mi.vms    = size   * page;
        mi.rss    = rss    * page;
        mi.shared = shared * page;
        mi.text   = text   * page;
        mi.data   = data   * page;
#elif defined(PSUTIL_WINDOWS)
        // Fix-R2: HandleGuard RAII
        detail::HandleGuard hg(open_handle(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ));
        if (hg) {
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(hg.h, &pmc, sizeof(pmc))) {
                mi.rss = pmc.WorkingSetSize;
                mi.vms = pmc.PagefileUsage;
            }
        }
#endif
        return mi;
    }

    /** Memory percentage */
    double memory_percent() const {
        auto vm = virtual_memory();
        if (vm.total == 0) return 0;
        return 100.0 * memory_info().rss / vm.total;
    }

    /** CPU times for this process */
    ProcessCpuTimes cpu_times() const {
        ProcessCpuTimes pt;
#ifdef PSUTIL_LINUX
        std::string raw = detail::read_file("/proc/" + std::to_string(pid_) + "/stat");
        // FIX #7: `ss` was constructed but never used; parse via rfind(')') directly.
        // fields: pid comm state ppid ... utime(14) stime(15) cutime(16) cstime(17)
        // skip to field 14 (1-indexed), after closing paren of comm
        auto paren = raw.rfind(')');
        if (paren == std::string::npos) return pt;
        std::istringstream ss2(raw.substr(paren + 2));
        std::string state;
        int ppid, pgrp, session, tty, tpgid;
        unsigned flags;
        uint64_t minflt, cminflt, majflt, cmajflt;
        uint64_t utime, stime; int64_t cutime, cstime;
        ss2 >> state >> ppid >> pgrp >> session >> tty >> tpgid
            >> flags >> minflt >> cminflt >> majflt >> cmajflt
            >> utime >> stime >> cutime >> cstime;
        // BUG FIX #11: sysconf(_SC_CLK_TCK) may return -1; guard with safe default
        long clk_raw2 = sysconf(_SC_CLK_TCK);
        double hz = (clk_raw2 > 0) ? static_cast<double>(clk_raw2) : 100.0;
        pt.user            = utime  / hz;
        pt.system          = stime  / hz;
        pt.children_user   = cutime / hz;
        pt.children_system = cstime / hz;
#elif defined(PSUTIL_WINDOWS)
        // Fix-R2: HandleGuard RAII
        detail::HandleGuard hg(open_handle(PROCESS_QUERY_INFORMATION));
        if (hg) {
            FILETIME ct, et, kt, ut;
            if (GetProcessTimes(hg.h, &ct, &et, &kt, &ut)) {
                pt.user   = detail::filetime_to_sec(ut);
                pt.system = detail::filetime_to_sec(kt);
            }
        }
#endif
        return pt;
    }

    /** CPU percent over an interval */
    double cpu_percent(double interval_sec = 0.1) const {
        auto measure = [this]() -> double {
            auto t = cpu_times();
            return t.user + t.system;
        };
        double t1 = measure();
        auto wall1 = detail::now_sec();
        if (interval_sec > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(interval_sec));
        double t2 = measure();
        auto wall2 = detail::now_sec();
        double d_wall = wall2 - wall1;
        if (d_wall <= 0) return 0.0;
        // IMPROVEMENT: guard cpu_count against 0 or negative to avoid division by zero
        int ncpu = cpu_count(true);
        if (ncpu <= 0) ncpu = 1;
        return 100.0 * (t2 - t1) / d_wall / ncpu;
    }

    /** Send signal to process */
    void send_signal(int sig) const {
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
        if (::kill(pid_, sig) != 0)
            throw Error("send_signal failed for pid " + std::to_string(pid_));
#elif defined(PSUTIL_WINDOWS)
        if (sig == 9 /* SIGKILL */) {
            // Fix-R2: HandleGuard RAII
            detail::HandleGuard hg(open_handle(PROCESS_TERMINATE));
            if (hg) TerminateProcess(hg.h, 1);
        }
#endif
    }

    void kill()     const { send_signal(9);  }
    void terminate()const { send_signal(15); }

    /** @see is_running() declared above alongside pid() */

    /** Open files (Linux only) */
    std::vector<std::string> open_files() const {
        std::vector<std::string> result;
#ifdef PSUTIL_LINUX
        std::string fd_dir = "/proc/" + std::to_string(pid_) + "/fd";
        // Fix-R1: DirGuard RAII — closedir() guaranteed even if readlink/push_back throws
        detail::DirGuard dir(opendir(fd_dir.c_str()));
        if (!dir) return result;
        struct dirent* entry;
        while ((entry = readdir(dir.d)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string link = fd_dir + "/" + entry->d_name;
            char buf[4096] = {};
            ssize_t len = readlink(link.c_str(), buf, sizeof(buf)-1);
            if (len > 0) result.push_back(std::string(buf, len));
        }
#endif
        return result;
    }

    /** Environment variables */
    std::map<std::string, std::string> get_environ() const {
        std::map<std::string, std::string> env;
#ifdef PSUTIL_LINUX
        std::string raw = detail::read_file("/proc/" + std::to_string(pid_) + "/environ");
        std::string entry;
        for (char c : raw) {
            if (c == '\0') {
                auto eq = entry.find('=');
                if (eq != std::string::npos)
                    env[entry.substr(0,eq)] = entry.substr(eq+1);
                entry.clear();
            } else entry += c;
        }
#endif
        return env;
    }

    /** Nice / priority value */
    int nice() const {
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
        return getpriority(PRIO_PROCESS, pid_);
#else
        return 0;
#endif
    }

    /**
     * Children (recursive=false: only direct)
     */
    /**
     * children(recursive)
     * FIX #6: O(N) single-pass — no heavyweight Process objects per pid.
     * Reads ppid directly from /proc/<pid>/status.
     * Recursive mode builds a parent-map once, then does a BFS walk.
     */
    std::vector<int> children(bool recursive = false) const {
        std::vector<int> result;
#ifdef PSUTIL_LINUX
        auto all = pids();
        if (!recursive) {
            for (int p : all) {
                if (p == pid_) continue;
                try { if (detail::read_ppid(p) == pid_) result.push_back(p); }
                catch (...) {}
            }
        } else {
            // Build parent→children map in one O(N) pass
            std::map<int, std::vector<int>> par_map;
            for (int p : all) {
                if (p == pid_) continue;
                try {
                    int pp = detail::read_ppid(p);
                    if (pp >= 0) par_map[pp].push_back(p);
                } catch (...) {}
            }
            // BFS from pid_
            std::vector<int> queue = {pid_};
            while (!queue.empty()) {
                int cur = queue.back(); queue.pop_back();
                auto it = par_map.find(cur);
                if (it == par_map.end()) continue;
                for (int child : it->second) {
                    result.push_back(child);
                    queue.push_back(child);
                }
            }
        }
#elif defined(PSUTIL_WINDOWS)
        detail::HandleGuard snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snap) {
            PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
            if (Process32First(snap.h, &pe))
                do {
                    if (static_cast<int>(pe.th32ParentProcessID) == pid_)
                        result.push_back(static_cast<int>(pe.th32ProcessID));
                } while (Process32Next(snap.h, &pe));
        }
#endif
        return result;
    }

private:
    int pid_;

#ifdef PSUTIL_WINDOWS
    HANDLE open_handle(DWORD access) const {
        HANDLE h = OpenProcess(access, FALSE, static_cast<DWORD>(pid_));
        return h; // caller must CloseHandle
    }
#endif
};

// ═══════════════════════════════════════════════════════════════
//  Convenience: process_iter()
//  Returns Process objects for all running pids.
// ═══════════════════════════════════════════════════════════════

inline std::vector<Process> process_iter() {
    auto all = pids();
    std::vector<Process> result;
    result.reserve(all.size());  // FIX #6: avoid repeated re-allocation
    for (int pid : all) {
        try { result.emplace_back(pid); }
        catch (...) {}
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════
//  Pretty-print helpers
// ═══════════════════════════════════════════════════════════════

inline std::string bytes_to_human(uint64_t bytes) {
    const char* units[] = {"B","KB","MB","GB","TB","PB"};
    double val = static_cast<double>(bytes);
    int i = 0;
    while (val >= 1024 && i < 5) { val /= 1024; ++i; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %s", val, units[i]);
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════
//  GPU backend — detail namespace
// ═══════════════════════════════════════════════════════════════════════
namespace detail {
namespace gpu_detail {

// ───────────────────────────────────────────────────────────────────────
//  NVML inline type definitions  (no nvml.h dependency)
//  All types mirror the official NVIDIA NVML C API exactly.
// ───────────────────────────────────────────────────────────────────────
using nvmlReturn_t   = int;
using nvmlDevice_t   = void*;
static constexpr nvmlReturn_t NVML_OK = 0;

enum nvmlClockType_t        { NVML_CLOCK_GRAPHICS=0, NVML_CLOCK_SM=1,
                              NVML_CLOCK_MEM=2, NVML_CLOCK_VIDEO=3 };
enum nvmlTemperatureSensors { NVML_TEMPERATURE_GPU=0 };

struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
struct nvmlMemory_t      { unsigned long long total, free, used; };

// ── NvmlLib: dynamic loader & singleton ──────────────────────────────
struct NvmlLib {
#define NVML_DECL(ret,name,args) \
    using PFN_##name = ret(*) args; PFN_##name name{};

    NVML_DECL(nvmlReturn_t, nvmlInit_v2,                         ())
    NVML_DECL(nvmlReturn_t, nvmlShutdown,                        ())
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetCount_v2,               (unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetHandleByIndex_v2,       (unsigned int, nvmlDevice_t*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetName,                   (nvmlDevice_t, char*, unsigned int))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetUtilizationRates,       (nvmlDevice_t, nvmlUtilization_t*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetMemoryInfo,             (nvmlDevice_t, nvmlMemory_t*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetClockInfo,              (nvmlDevice_t, nvmlClockType_t, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetMaxClockInfo,           (nvmlDevice_t, nvmlClockType_t, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetTemperature,            (nvmlDevice_t, nvmlTemperatureSensors, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetPowerUsage,             (nvmlDevice_t, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetPowerManagementLimit,   (nvmlDevice_t, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetFanSpeed,               (nvmlDevice_t, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlSystemGetDriverVersion,          (char*, unsigned int))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetCurrPcieLinkGeneration, (nvmlDevice_t, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetCurrPcieLinkWidth,      (nvmlDevice_t, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetEncoderUtilization,     (nvmlDevice_t, unsigned int*, unsigned int*))
    NVML_DECL(nvmlReturn_t, nvmlDeviceGetDecoderUtilization,     (nvmlDevice_t, unsigned int*, unsigned int*))
#undef NVML_DECL

    void*  handle = nullptr;
    bool   ready  = false;

    void* sym(const char* fn) noexcept {
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
        return dlsym(handle, fn);
#elif defined(PSUTIL_WINDOWS)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), fn));
#else
        (void)fn; return nullptr;
#endif
    }

    void load() noexcept {
#if defined(PSUTIL_LINUX)
        handle = dlopen("libnvidia-ml.so.1", RTLD_LAZY|RTLD_LOCAL);
        if (!handle) handle = dlopen("libnvidia-ml.so",   RTLD_LAZY|RTLD_LOCAL);
#elif defined(PSUTIL_MACOS)
        handle = dlopen("/usr/local/lib/libnvidia-ml.dylib", RTLD_LAZY|RTLD_LOCAL);
        if (!handle) handle = dlopen("/usr/lib/libnvidia-ml.dylib", RTLD_LAZY|RTLD_LOCAL);
#elif defined(PSUTIL_WINDOWS)
        handle = static_cast<void*>(LoadLibraryA("nvml.dll"));
        if (!handle) {
            char pf[MAX_PATH] = {};
            GetEnvironmentVariableA("ProgramFiles", pf, MAX_PATH);
            std::string path = std::string(pf) + "\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            handle = static_cast<void*>(LoadLibraryA(path.c_str()));
        }
#endif
        if (!handle) return;

#define NVML_LOAD(fn) fn = reinterpret_cast<PFN_##fn>(sym(#fn))
        NVML_LOAD(nvmlInit_v2);
        NVML_LOAD(nvmlShutdown);
        NVML_LOAD(nvmlDeviceGetCount_v2);
        NVML_LOAD(nvmlDeviceGetHandleByIndex_v2);
        NVML_LOAD(nvmlDeviceGetName);
        NVML_LOAD(nvmlDeviceGetUtilizationRates);
        NVML_LOAD(nvmlDeviceGetMemoryInfo);
        NVML_LOAD(nvmlDeviceGetClockInfo);
        NVML_LOAD(nvmlDeviceGetMaxClockInfo);
        NVML_LOAD(nvmlDeviceGetTemperature);
        NVML_LOAD(nvmlDeviceGetPowerUsage);
        NVML_LOAD(nvmlDeviceGetPowerManagementLimit);
        NVML_LOAD(nvmlDeviceGetFanSpeed);
        NVML_LOAD(nvmlSystemGetDriverVersion);
        NVML_LOAD(nvmlDeviceGetCurrPcieLinkGeneration);
        NVML_LOAD(nvmlDeviceGetCurrPcieLinkWidth);
        NVML_LOAD(nvmlDeviceGetEncoderUtilization);
        NVML_LOAD(nvmlDeviceGetDecoderUtilization);
#undef NVML_LOAD

        if (!nvmlInit_v2 || nvmlInit_v2() != NVML_OK) return;
        ready = true;
    }

    NvmlLib()  { load(); }
    ~NvmlLib() noexcept {
        // Fix-13: nvmlShutdown() is a C function and shouldn't throw, but guard
        // anyway — noexcept destructors that let exceptions escape call std::terminate.
        if (ready && nvmlShutdown) {
            try { nvmlShutdown(); } catch (...) {}
        }
        if (!handle) return;
#if defined(PSUTIL_LINUX) || defined(PSUTIL_MACOS)
        dlclose(handle);
#elif defined(PSUTIL_WINDOWS)
        FreeLibrary(static_cast<HMODULE>(handle));
#endif
    }

    // Non-copyable, non-movable (singleton)
    NvmlLib(const NvmlLib&)            = delete;
    NvmlLib& operator=(const NvmlLib&) = delete;
};

// Thread-safe singleton
inline NvmlLib& nvml() { static NvmlLib inst; return inst; }

// ── Scan all NVIDIA GPUs via NVML ────────────────────────────────────
inline std::vector<GpuStats> scan_nvidia(int base_index = 0) {
    auto& nv = nvml();
    std::vector<GpuStats> result;
    if (!nv.ready) return result;

    unsigned int count = 0;
    if (nv.nvmlDeviceGetCount_v2(&count) != NVML_OK || count == 0) return result;

    char drv[80] = {};
    if (nv.nvmlSystemGetDriverVersion) nv.nvmlSystemGetDriverVersion(drv, sizeof(drv));

    for (unsigned int i = 0; i < count; ++i) {
        nvmlDevice_t dev{};
        if (nv.nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_OK) continue;

        GpuStats s;
        s.index          = base_index + static_cast<int>(i);
        s.vendor         = "NVIDIA";
        s.driver_version = drv;

        char name[96] = {};
        if (nv.nvmlDeviceGetName(dev, name, sizeof(name)) == NVML_OK) s.name = name;

        nvmlUtilization_t util{};
        if (nv.nvmlDeviceGetUtilizationRates(dev, &util) == NVML_OK) {
            s.gpu_util = util.gpu;
            s.mem_util = util.memory;
        }

        unsigned int eu=0, du=0, period=0;
        if (nv.nvmlDeviceGetEncoderUtilization &&
            nv.nvmlDeviceGetEncoderUtilization(dev, &eu, &period) == NVML_OK) s.enc_util = eu;
        if (nv.nvmlDeviceGetDecoderUtilization &&
            nv.nvmlDeviceGetDecoderUtilization(dev, &du, &period) == NVML_OK) s.dec_util = du;

        nvmlMemory_t mem{};
        if (nv.nvmlDeviceGetMemoryInfo(dev, &mem) == NVML_OK) {
            s.mem_total   = mem.total;
            s.mem_used    = mem.used;
            s.mem_free    = mem.free;
            s.mem_percent = mem.total ? 100.0 * mem.used / mem.total : 0.0;
        }

        unsigned int clk = 0;
        if (nv.nvmlDeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &clk) == NVML_OK) s.clock_core     = clk;
        if (nv.nvmlDeviceGetClockInfo(dev, NVML_CLOCK_MEM,      &clk) == NVML_OK) s.clock_mem      = clk;
        if (nv.nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_GRAPHICS, &clk) == NVML_OK) s.clock_core_max = clk;
        if (nv.nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_MEM,      &clk) == NVML_OK) s.clock_mem_max  = clk;

        unsigned int temp = 0;
        if (nv.nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_OK) s.temperature = temp;

        unsigned int pwr = 0;
        if (nv.nvmlDeviceGetPowerUsage(dev, &pwr)             == NVML_OK) s.power_watts = pwr / 1000.0;
        if (nv.nvmlDeviceGetPowerManagementLimit(dev, &pwr)   == NVML_OK) s.power_limit  = pwr / 1000.0;

        unsigned int fan = 0;
        if (nv.nvmlDeviceGetFanSpeed(dev, &fan) == NVML_OK) s.fan_speed_pct = fan;

        unsigned int gen = 0, width = 0;
        if (nv.nvmlDeviceGetCurrPcieLinkGeneration(dev, &gen)   == NVML_OK) s.pcie_gen   = gen;
        if (nv.nvmlDeviceGetCurrPcieLinkWidth(dev, &width)      == NVML_OK) s.pcie_width = width;

        result.push_back(std::move(s));
    }
    return result;
}

// ───────────────────────────────────────────────────────────────────────
//  Linux sysfs helpers for AMD / Intel GPUs
// ───────────────────────────────────────────────────────────────────────
#ifdef PSUTIL_LINUX

// Read a single uint64 from a sysfs file; returns 0 on error
inline uint64_t sysfs_u64(const std::string& path) {
    std::ifstream f(path);
    uint64_t v = 0;
    f >> v;
    return v;
}

// Expand first glob match (used for hwmon/hwmonN)
inline std::string glob_first(const std::string& pattern) {
    glob_t g{};
    std::string found;
    if (glob(pattern.c_str(), GLOB_NOSORT, nullptr, &g) == 0 && g.gl_pathc > 0)
        found = g.gl_pathv[0];
    globfree(&g);
    return found;
}

// Parse AMD pp_dpm_sclk / pp_dpm_mclk — returns MHz of active state (line with '*')
inline double parse_dpm_clk_cur(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find('*') == std::string::npos) continue;
        auto col = line.find(':');
        if (col == std::string::npos) continue;
        try { return std::stod(line.substr(col + 1)); } catch (...) {}
    }
    return 0;
}

// Parse pp_dpm_* — returns MHz of the last (max) clock state
inline double parse_dpm_clk_max(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    std::string line, last;
    while (std::getline(f, line)) if (!line.empty()) last = line;
    auto col = last.find(':');
    if (col == std::string::npos) return 0;
    try { return std::stod(last.substr(col + 1)); } catch (...) { return 0; }
}

// GPU name: try product_name, fallback to PCI_ID from uevent
inline std::string drm_gpu_name(const std::string& dev, const std::string& vendor) {
    {
        std::ifstream f(dev + "/product_name");
        std::string n;
        if (f && std::getline(f, n) && !n.empty()) return n;
    }
    {
        std::ifstream f(dev + "/uevent");
        std::string line;
        while (f && std::getline(f, line)) {
            if (line.rfind("PCI_ID=", 0) == 0)
                return vendor + " GPU [" + line.substr(7) + "]";
        }
    }
    return vendor + " GPU";
}

// Convert PCIe speed string ("8.0 GT/s PCIe") to generation number
inline uint32_t pcie_speed_to_gen(const std::string& speed_str) {
    double gts = 0;
    try { gts = std::stod(speed_str); } catch (...) {}
    if (gts >= 32.0) return 5;
    if (gts >= 16.0) return 4;
    if (gts >=  8.0) return 3;
    if (gts >=  5.0) return 2;
    if (gts >=  2.5) return 1;
    return 0;
}

// Populate PCIe fields from sysfs
inline void fill_pcie_sysfs(GpuStats& s, const std::string& dev) {
    s.pcie_width = static_cast<uint32_t>(sysfs_u64(dev + "/current_link_width"));
    std::ifstream sf(dev + "/current_link_speed");
    std::string sp;
    if (sf && std::getline(sf, sp)) s.pcie_gen = pcie_speed_to_gen(sp);
}

// Scan AMD GPU via sysfs
inline void fill_amd_sysfs(GpuStats& s, const std::string& dev) {
    s.gpu_util  = static_cast<double>(sysfs_u64(dev + "/gpu_busy_percent"));

    s.mem_total = sysfs_u64(dev + "/mem_info_vram_total");
    s.mem_used  = sysfs_u64(dev + "/mem_info_vram_used");
    s.mem_free  = (s.mem_total >= s.mem_used) ? s.mem_total - s.mem_used : 0;
    s.mem_percent = s.mem_total ? 100.0 * s.mem_used / s.mem_total : 0.0;

    s.clock_core     = parse_dpm_clk_cur(dev + "/pp_dpm_sclk");
    s.clock_core_max = parse_dpm_clk_max(dev + "/pp_dpm_sclk");
    s.clock_mem      = parse_dpm_clk_cur(dev + "/pp_dpm_mclk");
    s.clock_mem_max  = parse_dpm_clk_max(dev + "/pp_dpm_mclk");

    std::string hwmon = glob_first(dev + "/hwmon/hwmon*");
    if (!hwmon.empty()) {
        uint64_t tc = sysfs_u64(hwmon + "/temp1_input");
        s.temperature = tc / 1000.0;

        uint64_t pw = sysfs_u64(hwmon + "/power1_average");
        if (!pw) pw  = sysfs_u64(hwmon + "/power2_average");
        s.power_watts = pw / 1e6;

        uint64_t cap = sysfs_u64(hwmon + "/power1_cap");
        s.power_limit = cap / 1e6;

        // Fan: prefer pwm % → fallback to RPM/max ratio
        uint64_t pwm = sysfs_u64(hwmon + "/pwm1");
        if (pwm) {
            s.fan_speed_pct = pwm * 100.0 / 255.0;
        } else {
            uint64_t rpm = sysfs_u64(hwmon + "/fan1_input");
            uint64_t mx  = sysfs_u64(hwmon + "/fan1_max");
            if (mx) s.fan_speed_pct = rpm * 100.0 / mx;
        }
    }
}

// Scan Intel GPU via sysfs
inline void fill_intel_sysfs(GpuStats& s, const std::string& card, const std::string& dev) {
    std::string base = "/sys/class/drm/" + card;
    auto try_mhz = [](std::initializer_list<std::string> paths) -> double {
        for (auto& p : paths) {
            std::ifstream f(p); double v = 0;
            if (f >> v && v > 0) return v;
        }
        return 0;
    };
    s.clock_core     = try_mhz({base + "/gt/gt0/freq_cur_mhz", base + "/gt_cur_freq_mhz"});
    s.clock_core_max = try_mhz({base + "/gt/gt0/freq_max_mhz", base + "/gt_max_freq_mhz"});

    std::string hwmon = glob_first(dev + "/hwmon/hwmon*");
    if (!hwmon.empty()) {
        uint64_t tc = sysfs_u64(hwmon + "/temp1_input");
        s.temperature = tc / 1000.0;
        uint64_t pw   = sysfs_u64(hwmon + "/power1_average");
        s.power_watts = pw / 1e6;
    }
}

// Enumerate /sys/class/drm/cardN and build GpuStats for non-NVIDIA (or all) cards
inline std::vector<GpuStats> scan_drm_sysfs(int base_index, bool skip_nvidia) {
    std::vector<GpuStats> result;

    // Fix-R1: DirGuard RAII — closedir() guaranteed even if cards.push_back throws
    detail::DirGuard drm_dir(opendir("/sys/class/drm"));
    if (!drm_dir) return result;

    std::vector<std::string> cards;
    dirent* e;
    while ((e = readdir(drm_dir.d)) != nullptr) {
        std::string n = e->d_name;
        // Match "cardN" exactly — skip connector entries like "card0-HDMI-A-1"
        if (n.rfind("card", 0) != 0 || n.size() <= 4) continue;
        if (n.find('-') != std::string::npos) continue;
        bool digits_only = true;
        for (size_t k = 4; k < n.size(); ++k)
            if (!std::isdigit(static_cast<unsigned char>(n[k]))) { digits_only = false; break; }
        if (digits_only) cards.push_back(n);
    }
    std::sort(cards.begin(), cards.end());

    int idx = base_index;
    for (auto& card : cards) {
        std::string dev = "/sys/class/drm/" + card + "/device";

        // Read PCI vendor id
        std::ifstream vf(dev + "/vendor");
        std::string vendor_hex; vf >> vendor_hex;

        const bool is_nvidia = (vendor_hex == "0x10de");
        const bool is_amd    = (vendor_hex == "0x1002");
        const bool is_intel  = (vendor_hex == "0x8086");

        if (is_nvidia && skip_nvidia) continue;
        if (!is_nvidia && !is_amd && !is_intel) continue;

        GpuStats s;
        s.index  = idx++;
        s.vendor = is_nvidia ? "NVIDIA" : (is_amd ? "AMD" : "Intel");
        s.name   = drm_gpu_name(dev, s.vendor);

        if (is_amd)         fill_amd_sysfs(s, dev);
        else if (is_intel)  fill_intel_sysfs(s, card, dev);
        // NVIDIA via sysfs: NVML preferred; sysfs only gives hwmon basics
        else {
            std::string hwmon = glob_first(dev + "/hwmon/hwmon*");
            if (!hwmon.empty()) {
                uint64_t tc = sysfs_u64(hwmon + "/temp1_input");
                s.temperature = tc / 1000.0;
            }
        }

        fill_pcie_sysfs(s, dev);
        result.push_back(std::move(s));
    }
    return result;
}
#endif // PSUTIL_LINUX

// ───────────────────────────────────────────────────────────────────────
//  macOS IOKit GPU scan
// ───────────────────────────────────────────────────────────────────────
#ifdef PSUTIL_MACOS
inline std::vector<GpuStats> scan_iokit_gpu() {
    std::vector<GpuStats> result;

    io_iterator_t iter = IO_OBJECT_NULL;
    // IOAccelerator covers Metal-capable GPUs (AMD, NVIDIA, Intel, Apple Silicon)
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("IOAccelerator"), &iter) != KERN_SUCCESS)
        return result;

    int idx = 0;
    io_registry_entry_t entry;
    while ((entry = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        GpuStats s;
        s.index = idx++;

        // Fix-15: use RAII wrappers for CF/IO objects to prevent leaks if an
        // exception or early return occurs between the Create call and CFRelease.
        struct CFTypeDeleter {
            void operator()(CFTypeRef r) const noexcept { if (r) CFRelease(r); }
        };
        // Helper: typed unique_ptr over a CFTypeRef (non-owning void* alias)
        using CFAutoRef = std::unique_ptr<const void, CFTypeDeleter>;

        // Walk up to IOPCIDevice / IOGPU to get the "model" string
        io_registry_entry_t parent = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) == KERN_SUCCESS) {
            // RAII: IOObjectRelease(parent) on scope exit
            struct IOParentGuard {
                io_registry_entry_t& p;
                ~IOParentGuard() noexcept { if (p != IO_OBJECT_NULL) IOObjectRelease(p); }
            } pg{parent};

            CFAutoRef model(IORegistryEntryCreateCFProperty(
                parent, CFSTR("model"), kCFAllocatorDefault, 0));
            if (model) {
                char buf[128] = {};
                CFStringGetCString((CFStringRef)model.get(), buf, sizeof(buf),
                                   kCFStringEncodingUTF8);
                s.name = buf;
            }
        }
        if (s.name.empty()) s.name = "Unknown GPU";

        // Infer vendor from model name
        auto contains = [&](const char* sub) {
            return s.name.find(sub) != std::string::npos;
        };
        if      (contains("AMD") || contains("Radeon"))               s.vendor = "AMD";
        else if (contains("NVIDIA") || contains("GeForce"))           s.vendor = "NVIDIA";
        else if (contains("Intel"))                                    s.vendor = "Intel";
        else if (contains("Apple") || contains("M1") || contains("M2")
              || contains("M3")    || contains("M4"))                  s.vendor = "Apple";
        else                                                           s.vendor = "Unknown";

        // Fix-15b: PerformanceStatistics — use CFAutoRef so CFRelease is guaranteed
        {
        CFMutableDictionaryRef raw_props = nullptr;
        if (IORegistryEntryCreateCFProperties(entry, &raw_props,
            kCFAllocatorDefault, kNilOptions) == KERN_SUCCESS && raw_props) {
        // Transfer ownership to smart ptr immediately
        CFAutoRef props_owner(raw_props);
        CFMutableDictionaryRef props = raw_props;

            CFDictionaryRef perf = (CFDictionaryRef)CFDictionaryGetValue(
                props, CFSTR("PerformanceStatistics"));
            if (perf) {
                auto get_i64 = [&](const char* key) -> int64_t {
                    CFStringRef ks = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                               kCFStringEncodingUTF8);
                    CFNumberRef n  = (CFNumberRef)CFDictionaryGetValue(perf, ks);
                    CFRelease(ks);
                    if (!n) return 0;
                    int64_t v = 0;
                    CFNumberGetValue(n, kCFNumberSInt64Type, &v);
                    return v;
                };

                // Core utilisation — different keys across GPU generations
                int64_t gu = get_i64("Device Utilization %");
                if (!gu) gu = get_i64("Renderer Utilization %");
                if (!gu) gu = get_i64("GPU Activity(%)");
                s.gpu_util = static_cast<double>(gu);

                // Apple Silicon: Tiler = geometry/vertex stage ≈ enc_util slot
                s.enc_util = static_cast<double>(get_i64("Tiler Utilization %"));

                // VRAM  (Apple / discrete AMD / Intel)
                int64_t vram_used = get_i64("vramUsedBytes");
                int64_t vram_free = get_i64("vramFreeBytes");
                int64_t vram_tot  = vram_used + vram_free;
                if (vram_tot > 0) {
                    s.mem_used    = static_cast<uint64_t>(vram_used);
                    s.mem_free    = static_cast<uint64_t>(vram_free);
                    s.mem_total   = static_cast<uint64_t>(vram_tot);
                    s.mem_percent = s.mem_total ? 100.0 * s.mem_used / s.mem_total : 0.0;
                }

                // Core clock (MHz) — exposed by some AMD/Intel drivers
                int64_t clk_mhz = get_i64("Core Clock(MHz)");
                if (!clk_mhz) clk_mhz = get_i64("GPU Core Clock(MHz)");
                s.clock_core = static_cast<double>(clk_mhz);

                // Temperature / power
                int64_t temp_c = get_i64("Temperature(C)");
                s.temperature  = static_cast<double>(temp_c);
                int64_t pwr_mw = get_i64("Power(mW)");
                s.power_watts  = pwr_mw / 1000.0;
            }

        }
        } // end CFAutoRef props_owner scope (CFRelease called automatically)

        IOObjectRelease(entry);
        result.push_back(std::move(s));
    }
    IOObjectRelease(iter);
    return result;
}
#endif // PSUTIL_MACOS

// ───────────────────────────────────────────────────────────────────────
//  Windows: registry-based GPU enumeration for AMD / Intel
//  (NVIDIA is always handled by NVML; this fills in the rest)
// ───────────────────────────────────────────────────────────────────────
#ifdef PSUTIL_WINDOWS
inline std::vector<GpuStats> scan_win_registry_gpu(int base_index, bool skip_nvidia) {
    std::vector<GpuStats> result;

    // Video class key — enumerates all installed display adapters
    const char* class_key =
        "SYSTEM\\CurrentControlSet\\Control\\Class\\"
        "{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY raw_hClass = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, class_key, 0, KEY_READ, &raw_hClass) != ERROR_SUCCESS)
        return result;
    // Fix-1b: RegKeyGuard RAII — RegCloseKey(hClass) guaranteed on any exit path
    detail::RegKeyGuard class_guard(raw_hClass);
    HKEY hClass = raw_hClass;

    int idx = base_index;
    for (DWORD i = 0; ; ++i) {
        char subkey[16] = {};
        DWORD len = sizeof(subkey);
        if (RegEnumKeyExA(hClass, i, subkey, &len,
            nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (std::string(subkey) == "Properties") continue;

        HKEY raw_hDev = nullptr;
        if (RegOpenKeyExA(hClass, subkey, 0, KEY_READ, &raw_hDev) != ERROR_SUCCESS) continue;
        // Fix-1b: RegKeyGuard RAII — RegCloseKey(hDev) guaranteed even if push_back throws
        detail::RegKeyGuard dev_guard(raw_hDev);
        HKEY hDev = raw_hDev;

        // Helper lambdas for registry reads
        auto read_str = [&](const char* val) -> std::string {
            char buf[512] = {}; DWORD sz = sizeof(buf);
            RegQueryValueExA(hDev, val, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf), &sz);
            return buf;
        };
        auto read_u64 = [&](const char* val) -> uint64_t {
            uint64_t v = 0; DWORD sz = sizeof(v);
            RegQueryValueExA(hDev, val, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&v), &sz);
            return v;
        };

        std::string desc  = read_str("DriverDesc");
        std::string hw_id = read_str("MatchingDeviceId"); // "pci\ven_XXXX&..."
        if (desc.empty()) { continue; } // dev_guard closes hDev

        // Normalise hw_id to lower-case for easier matching
        for (char& c : hw_id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        bool is_nvidia = hw_id.find("ven_10de") != std::string::npos
                      || desc.find("NVIDIA") != std::string::npos
                      || desc.find("GeForce") != std::string::npos;
        bool is_amd    = hw_id.find("ven_1002") != std::string::npos
                      || desc.find("AMD") != std::string::npos
                      || desc.find("Radeon") != std::string::npos;
        bool is_intel  = hw_id.find("ven_8086") != std::string::npos
                      || desc.find("Intel") != std::string::npos;

        if (is_nvidia && skip_nvidia) { continue; } // dev_guard closes hDev
        if (!is_nvidia && !is_amd && !is_intel) { continue; }

        GpuStats s;
        s.index  = idx++;
        s.name   = desc;
        s.vendor = is_nvidia ? "NVIDIA" : (is_amd ? "AMD" : "Intel");
        s.driver_version = read_str("DriverVersion");

        // VRAM total (static — registry holds the value set at driver install time)
        uint64_t vram = read_u64("HardwareInformation.MemorySize");
        if (!vram) vram = read_u64("qwMemorySize");
        s.mem_total = vram;

        // RegCloseKey(hDev) called automatically by dev_guard destructor
        result.push_back(std::move(s));
    }
    // RegCloseKey(hClass) called automatically by class_guard destructor
    return result;
}
#endif // PSUTIL_WINDOWS

} // namespace gpu_detail
} // namespace detail

// ═══════════════════════════════════════════════════════════════════════
//  Public GPU API
// ═══════════════════════════════════════════════════════════════════════

/**
 * gpu_info()
 * Returns one GpuStats per detected GPU, ordered by index.
 *
 * Backend priority:
 *   1. NVML   — NVIDIA GPUs on any platform (requires libnvidia-ml at runtime)
 *   2. sysfs  — AMD / Intel GPUs on Linux
 *   3. IOKit  — All GPUs on macOS (via IOAccelerator PerformanceStatistics)
 *   4. Registry — AMD / Intel on Windows (static info only; no live utilisation)
 *
 * The function is safe to call repeatedly; NVML is initialised at most once
 * (singleton) and torn down at program exit.
 */
inline std::vector<GpuStats> gpu_info() {
    using namespace detail::gpu_detail;
    std::vector<GpuStats> result;

    // ── NVIDIA via NVML (all platforms) ─────────────────────────────
    auto nvidia_gpus = scan_nvidia(0);
    bool has_nvml = !nvidia_gpus.empty();
    for (auto& g : nvidia_gpus) result.push_back(std::move(g));
    int next_idx = static_cast<int>(result.size());

#if defined(PSUTIL_LINUX)
    // ── AMD / Intel via sysfs ────────────────────────────────────────
    // skip_nvidia=true so we don't double-count cards already in NVML results
    auto sysfs_gpus = scan_drm_sysfs(next_idx, has_nvml);
    for (auto& g : sysfs_gpus) result.push_back(std::move(g));

#elif defined(PSUTIL_MACOS)
    // ── All GPUs via IOKit ───────────────────────────────────────────
    // If NVML found NVIDIA GPU(s), IOKit may also see them — deduplicate
    // by skipping IOKit entries that look like NVIDIA when NVML is active.
    auto iokit_gpus = scan_iokit_gpu();
    for (auto& g : iokit_gpus) {
        if (has_nvml && g.vendor == "NVIDIA") continue; // already covered
        g.index = next_idx++;
        result.push_back(std::move(g));
    }

#elif defined(PSUTIL_WINDOWS)
    // ── AMD / Intel via registry ─────────────────────────────────────
    auto reg_gpus = scan_win_registry_gpu(next_idx, has_nvml);
    for (auto& g : reg_gpus) result.push_back(std::move(g));
#endif

    // Re-number indices sequentially in case of deduplication
    for (int i = 0; i < static_cast<int>(result.size()); ++i)
        result[i].index = i;

    return result;
}

/**
 * gpu_count()
 * Returns the total number of detected GPUs across all backends.
 */
inline int gpu_count() {
    return static_cast<int>(gpu_info().size());
}

} // namespace psutil