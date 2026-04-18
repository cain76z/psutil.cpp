```markdown
# psutil.hpp

**Python의 psutil을 C++17 single-header로 완전 포팅 + GPU 지원 추가**

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![C++](https://img.shields.io/badge/C%2B%2B-17%2B-orange.svg)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)

**단일 헤더**만 include하면 바로 사용 가능합니다.  
별도의 빌드, CMake, `PSUTIL_IMPLEMENTATION` 매크로가 필요 없습니다.

---

## ✨ 특징

- **완전한 Python psutil API 호환**
  - CPU, Memory, Disk, Network, Process, System, Users, Battery
- **강력한 GPU 지원 (이번 포팅의 핵심)**  
  - **NVIDIA**: NVML (동적 로드, SDK 설치 불필요) — 모든 지표 지원  
  - **AMD**: Linux sysfs / Windows Registry  
  - **Intel**: Linux sysfs  
  - **Apple**: macOS IOKit (Apple Silicon 포함)
- **플랫폼별 최적화** + **RAII 안전 처리**
- **Windows에서 ws2_32.lib 링크 완전 제거** (`inet_ntop` 순수 C++ 구현)
- **예외 안전성** 극대화 (모든 RAII 가드 사용)
- **C++17 이상**만 필요

---

## 🚀 빠른 시작

```cpp
#include "psutil.hpp"
#include <iostream>

int main() {
    std::cout << "CPU: " << psutil::cpu_percent() << "%\n";

    auto vm = psutil::virtual_memory();
    std::cout << "RAM 사용량: " << vm.percent << "% (" 
              << psutil::bytes_to_human(vm.used) << " / "
              << psutil::bytes_to_human(vm.total) << ")\n";

    auto gpus = psutil::gpu_info();
    for (const auto& gpu : gpus) {
        std::cout << "GPU " << gpu.index << " (" << gpu.vendor << ") "
                  << gpu.name << " — " << gpu.gpu_util << "% / "
                  << psutil::bytes_to_human(gpu.mem_used) << " / "
                  << gpu.temperature << "°C\n";
    }
}
```

---

## 📦 설치 방법

1. `psutil.hpp` 파일을 프로젝트에 복사
2. `#include "psutil.hpp"` 만 추가
3. **Windows** 링커 설정:
   ```cmake
   target_link_libraries(your_target PRIVATE psapi iphlpapi pdh)
   # ws2_32.lib는 더 이상 필요 없습니다!
   ```

**MinGW / Clang** 사용자도 동일하게 `psapi iphlpapi pdh`만 링크하면 됩니다.

---

## 📋 지원 API (Python psutil과 거의 동일)

### CPU
- `cpu_count(bool logical = true)`
- `cpu_percent(double interval_sec = 0.1)`
- `cpu_percent_percpu(double interval_sec = 0.1)`
- `cpu_times()`, `cpu_freq()`, `cpu_stats()`

### 메모리
- `virtual_memory()`
- `swap_memory()`

### 디스크
- `disk_partitions(bool all = false)`
- `disk_usage(const std::string& path = "/")`
- `disk_io_counters(bool perdisk = false)`

### 네트워크
- `net_io_counters(bool pernic = false)`
- `net_if_addrs()`, `net_if_stats()`
- `net_connections(const std::string& kind = "inet")`

### 프로세스
- `pids()`, `pid_exists(int pid)`
- `Process` 클래스 (`name()`, `exe()`, `cmdline()`, `memory_info()`, `cpu_percent()`, `children()`, `kill()` 등)

### 시스템
- `boot_time()`
- `users()`

### 배터리
- `sensors_battery()` (Linux/Windows)

### **GPU** (추가된 핵심 기능)
- `gpu_count()`
- `gpu_info()` → `std::vector<GpuStats>`

**GpuStats** 구조체는 NVIDIA/AMD/Intel/Apple 모두에서 공통으로 사용됩니다.

---

## 🔧 GPU 지원 상세

| GPU 제조사 | 플랫폼       | 지원 지표                                      | 백엔드          |
|------------|--------------|------------------------------------------------|-----------------|
| NVIDIA     | All          | utilization, VRAM, clock, temp, power, fan, PCIe | NVML (동적 로드) |
| AMD        | Linux        | utilization, VRAM, clock, temp, power, fan, PCIe | sysfs           |
| AMD        | Windows      | 이름, VRAM, 드라이버 버전                      | Registry        |
| Intel      | Linux        | clock, temperature                             | sysfs           |
| Apple      | macOS        | utilization, VRAM, encoder, temperature, power | IOKit           |

---

## ⚙️ Windows 빌드 주의사항

- `ws2_32.lib` **링크하지 마세요** (이미 내부에서 순수 C++ `psutil_inet_ntop`으로 대체)
- 필요한 라이브러리: `psapi`, `iphlpapi`

---

## 📄 라이선스

**MIT License**

원본 Python psutil의 MIT 라이선스를 그대로 따릅니다.

---

## ❤️ 감사 인사

- **원본 Python psutil** — https://github.com/giampaolo/psutil  
  (이 라이브러리는 psutil의 C++ 완전 포트입니다)
- GPU 지원 구현에 도움을 주신 모든 분들께 감사드립니다.

---

**Made with ❤️ in Seoul**  
C++로 psutil을 쓰고 싶었던 모든 개발자들을 위해.

---
**This file writed by Grok**
