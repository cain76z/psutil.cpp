#include "psutil.hpp"
#include <iostream>
#include <iomanip>
#include <ctime>
#include <algorithm>

// 콘솔 출력을 보기 좋게 정리하기 위한 유틸리티
void print_section(const std::string& title) {
    std::cout << "\n╔═══════════════════════════════════════════════════════════════╗\n"
              << "║ " << std::left << std::setw(59) << title << "   ║\n"
              << "╚═══════════════════════════════════════════════════════════════╝\n";
}

std::string current_time_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t t_now = std::chrono::system_clock::to_time_t(now);
    std::string time_str = std::ctime(&t_now);
    if (!time_str.empty()) time_str.pop_back(); // 개행 문자 제거
    return time_str;
}

int main() {
    std::cout << "psutil.hpp Demo Execution Time: " << current_time_str() << "\n";

    try {
        // ========================================================
        // 1. CPU 정보
        // ========================================================
        print_section("CPU Information");
        std::cout << "Logical Cores   : " << psutil::cpu_count(true) << "\n";
        std::cout << "Physical Cores  : " << psutil::cpu_count(false) << "\n";
        
        // cpu_percent는 측정을 위해 기본적으로 0.1초 대기합니다.
        std::cout << "CPU Usage (0.1s): " << psutil::cpu_percent(0.1) << "%\n";
        
        auto per_cpu = psutil::cpu_percent_percpu(0.1);
        if (!per_cpu.empty()) {
            std::cout << "Per-Core Usage  : ";
            for (size_t i = 0; i < per_cpu.size(); ++i) {
                std::cout << std::fixed << std::setprecision(1) << per_cpu[i] << "% ";
                if (i > 8) { std::cout << "..."; break; } // 너무 길면 생략
            }
            std::cout << "\n";
        } else {
            std::cout << "Per-Core Usage  : Not supported on this platform\n";
        }

        auto cpu_times = psutil::cpu_times();
        std::cout << "CPU Times        : User=" << cpu_times.user << "s, System=" << cpu_times.system << "s, Idle=" << cpu_times.idle << "s\n";
        
        auto cpu_freq = psutil::cpu_freq();
        if (cpu_freq.current > 0)
            std::cout << "CPU Frequency   : Current=" << cpu_freq.current << " MHz, Max=" << cpu_freq.max << " MHz\n";

        // ========================================================
        // 2. 메모리 정보
        // ========================================================
        print_section("Memory Information");
        auto vm = psutil::virtual_memory();
        std::cout << "Total Memory    : " << psutil::bytes_to_human(vm.total) << "\n";
        std::cout << "Available Memory: " << psutil::bytes_to_human(vm.available) << "\n";
        std::cout << "Used Memory     : " << psutil::bytes_to_human(vm.used) << " (" << vm.percent << "%)\n";
        if (vm.cached > 0) std::cout << "Cached Memory   : " << psutil::bytes_to_human(vm.cached) << "\n";

        auto sm = psutil::swap_memory();
        if (sm.total > 0) {
            std::cout << "Swap Total      : " << psutil::bytes_to_human(sm.total) << "\n";
            std::cout << "Swap Used       : " << psutil::bytes_to_human(sm.used) << " (" << sm.percent << "%)\n";
        }

        // ========================================================
        // 3. 디스크 정보
        // ========================================================
        print_section("Disk Information");
        auto partitions = psutil::disk_partitions();
        std::cout << "Partitions (" << partitions.size() << " found):\n";
        for (const auto& p : partitions) {
            std::cout << "  - " << std::left << std::setw(15) << p.mountpoint 
                      << " Type: " << std::setw(10) << p.fstype 
                      << " Device: " << p.device << "\n";
        }

#ifdef _WIN32
        auto disk = psutil::disk_usage("C:\\");
#else
        auto disk = psutil::disk_usage("/");
#endif
        std::cout << "Root Disk Usage : " << psutil::bytes_to_human(disk.used) << " / " 
                  << psutil::bytes_to_human(disk.total) << " (" << disk.percent << "%)\n";

        auto disk_io = psutil::disk_io_counters(false);
        if (!disk_io.empty() && disk_io[0].read_bytes > 0) {
            std::cout << "Disk I/O (Total): Read=" << psutil::bytes_to_human(disk_io[0].read_bytes) 
                      << ", Write=" << psutil::bytes_to_human(disk_io[0].write_bytes) << "\n";
        }

        // ========================================================
        // 4. 네트워크 정보
        // ========================================================
        print_section("Network Information");
        auto net_io = psutil::net_io_counters(false);
        if (!net_io.empty()) {
            std::cout << "Total I/O        : Sent=" << psutil::bytes_to_human(net_io[0].bytes_sent) 
                      << ", Recv=" << psutil::bytes_to_human(net_io[0].bytes_recv) << "\n";
        }

        auto net_addrs = psutil::net_if_addrs();
        std::cout << "Interfaces (" << net_addrs.size() << " found):\n";
        for (const auto& [iface, addrs] : net_addrs) {
            for (const auto& addr : addrs) {
                if (addr.family == "AF_INET") {
                    std::cout << "  - " << std::left << std::setw(10) << iface 
                              << " IPv4: " << addr.address << "\n";
                    break; // IPv4 하나만 출력
                }
            }
        }

        // ========================================================
        // 5. 배터리 센서
        // ========================================================
        print_section("Sensor Information (Battery)");
        auto battery = psutil::sensors_battery();
        if (battery) {
            std::cout << "Battery Percent : " << battery->percent << "%\n";
            std::cout << "Power Plugged   : " << (battery->power_plugged ? "Yes" : "No") << "\n";
            if (battery->secsleft > 0)
                std::cout << "Time Remaining  : " << battery->secsleft / 60 << " mins\n";
        } else {
            std::cout << "Battery status  : Not found or unsupported\n";
        }

        // ========================================================
        // 6. GPU 정보
        // ========================================================
        print_section("GPU Information");
        auto gpus = psutil::gpu_info();
        std::cout << "Detected GPUs    : " << gpus.size() << "\n";
        for (const auto& gpu : gpus) {
            std::cout << "  [" << gpu.index << "] " << gpu.name << " (" << gpu.vendor << ")\n";
            if (gpu.mem_total > 0) 
                std::cout << "      VRAM     : " << psutil::bytes_to_human(gpu.mem_used) << " / " 
                          << psutil::bytes_to_human(gpu.mem_total) << " (" << gpu.mem_percent << "%)\n";
            if (gpu.gpu_util > 0)
                std::cout << "      GPU Util : " << gpu.gpu_util << "%\n";
            if (gpu.temperature > 0)
                std::cout << "      Temp     : " << gpu.temperature << " °C\n";
            if (gpu.power_watts > 0)
                std::cout << "      Power    : " << gpu.power_watts << " W\n";
        }

        // ========================================================
        // 7. 시스템 및 사용자 정보
        // ========================================================
        print_section("System Information");
        double boot = psutil::boot_time();
        std::cout << "Boot Time       : " << current_time_str() // 임시 출력
                  << " (Unix: " << boot << ")\n";
        // Boot time을 읽기 쉽게 변환
        std::time_t boot_time_t = static_cast<std::time_t>(boot);
        std::cout << "                 Exact: " << std::ctime(&boot_time_t);

        auto users_vec = psutil::users();
        if (!users_vec.empty()) {
            std::cout << "Logged-in Users : ";
            for (const auto& u : users_vec) {
                std::cout << u.name << " ";
            }
            std::cout << "\n";
        }

        // ========================================================
        // 8. 프로세스 정보 (현재 프로세스 집중 분석)
        // ========================================================
        print_section("Process Information (Current Process)");
        psutil::Process self(psutil::pid_exists(getpid()) ? getpid() : 0);
        
        std::cout << "PID             : " << self.pid() << "\n";
        std::cout << "Name            : " << self.name() << "\n";
        std::cout << "Executable      : " << self.exe() << "\n";
        std::cout << "Status          : " << self.status().str << "\n";
        std::cout << "Parent PID      : " << self.ppid() << "\n";
        std::cout << "Threads         : " << self.num_threads() << "\n";

        auto mem_info = self.memory_info();
        std::cout << "Memory (RSS)    : " << psutil::bytes_to_human(mem_info.rss) << "\n";
        std::cout << "Memory (VMS)    : " << psutil::bytes_to_human(mem_info.vms) << "\n";
        std::cout << "Memory %        : " << std::fixed << std::setprecision(2) << self.memory_percent() << "%\n";

        // 프로세스 CPU 사용량 측정 (0.2초 대기)
        double cpu_usage = self.cpu_percent(0.2);
        std::cout << "CPU Usage (0.2s): " << cpu_usage << "%\n";

        auto children = self.children(false); // 직계 자식만
        if (!children.empty()) {
            std::cout << "Child PIDs      : ";
            for (int cpid : children) std::cout << cpid << " ";
            std::cout << "\n";
        }

        // ========================================================
        // 9. 시스템 전체 프로세스 리스트 요약 (상위 5개)
        // ========================================================
        print_section("Top 5 Processes by Memory Usage");
        auto all_pids = psutil::pids();
        std::vector<std::pair<uint64_t, int>> pid_mem_pairs;
        
        for (int pid : all_pids) {
            try {
                psutil::Process p(pid);
                pid_mem_pairs.push_back({p.memory_info().rss, pid});
            } catch (...) {
                // 접근 권한 없음 또는 종료된 프로세스 무시
            }
        }

        // 메모리 사용량 기준 내림차순 정렬
        std::sort(pid_mem_pairs.begin(), pid_mem_pairs.end(), std::greater<>());

        std::cout << std::setw(10) << "PID" 
                  << std::setw(15) << "RSS Memory" 
                  << std::setw(10) << "% Mem" 
                  << "  Name\n";
        std::cout << std::string(55, '-') << "\n";
        
        uint64_t total_mem = psutil::virtual_memory().total;
        for (size_t i = 0; i < std::min(size_t(5), pid_mem_pairs.size()); ++i) {
            try {
                psutil::Process p(pid_mem_pairs[i].second);
                double pct = total_mem > 0 ? (100.0 * pid_mem_pairs[i].first / total_mem) : 0.0;
                std::cout << std::setw(10) << p.pid() 
                          << std::setw(15) << psutil::bytes_to_human(pid_mem_pairs[i].first)
                          << std::setw(9) << std::fixed << std::setprecision(2) << pct << "% "
                          << p.name() << "\n";
            } catch (...) {}
        }

    } catch (const psutil::Error& e) {
        std::cerr << "\n[psutil Error] " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "\n[Standard Error] " << e.what() << "\n";
        return 1;
    }

    std::cout << "\nDemo finished successfully.\n";
    return 0;
}