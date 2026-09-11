#ifndef SST_QUETZ_MULTICORE_LAUNCH_H
#define SST_QUETZ_MULTICORE_LAUNCH_H
#include <cstdint>
#include <string>
#include <vector>

namespace SST { namespace Quetz {
// The initial two-core contract uses QEMU-owned shared RAM and independent
// trace/MMIO slots. CPU1 starts from a second ELF when GPIO0 bit7 releases it;
// hardware cache coherence and per-core interrupt controllers are not modeled.
inline const char* multicoreLaunchError(uint32_t vcpus, bool cached_window,
                                      const std::vector<std::string>& args)
{
    if (vcpus != 2) return "system multicore requires exactly two configured vCPUs";
    if (cached_window) return "system multicore does not support sst_window_cache";
    bool machine_seen = false, cpu_seen = false, smp_seen = false, accel_seen = false;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string option = args[i];
        if (option.rfind("--", 0) == 0) option.erase(0, 1);
        auto equal = option.find('=');
        std::string key = option.substr(0, equal);
        if (key == "-readconfig" || key == "-global" || key == "-enable-kvm")
            return "system multicore forbids external machine/CPU overrides";
        bool machine = key == "-M" || key == "-machine";
        if (!machine && key != "-cpu" && key != "-smp" && key != "-accel") continue;
        std::string value;
        if (equal != std::string::npos) value = option.substr(equal + 1);
        else if (++i < args.size()) value = args[i];
        if (machine) {
            if (machine_seen) return "system multicore requires exactly one machine selection";
            machine_seen = true;
            const std::string prefix = "raptor-core2,";
            if (value.rfind(prefix, 0) != 0)
                return "system multicore requires raptor-core2 with secondary-kernel";
            bool secondary = false, edma_irq = false, edma_error_irq = false;
            size_t begin = prefix.size();
            while (begin < value.size()) {
                auto end = value.find(',', begin);
                std::string property = value.substr(begin, end == std::string::npos ? end : end - begin);
                if (property.rfind("secondary-kernel=", 0) == 0 && property.size() > 17) {
                    if (secondary) return "system multicore requires one secondary-kernel";
                    secondary = true;
                } else if (property.rfind("edma-irq=", 0) == 0 ||
                           property.rfind("edma-error-irq=", 0) == 0) {
                    bool& seen = property.rfind("edma-irq=", 0) == 0 ? edma_irq : edma_error_irq;
                    if (seen) return "duplicate eDMA interrupt route";
                    seen = true;
                    const std::string number = property.substr(property.find('=') + 1);
                    unsigned route = 0;
                    if (number.empty()) return "eDMA interrupt route must be 0..63";
                    for (char digit : number) {
                        if (digit < '0' || digit > '9') return "eDMA interrupt route must be 0..63";
                        route = route * 10 + unsigned(digit - '0');
                        if (route > 63) return "eDMA interrupt route must be 0..63";
                    }
                } else if (property != "strict-mmio=on") {
                    return "unsupported system multicore machine property";
                }
                if (end == std::string::npos) break;
                begin = end + 1;
                if (begin == value.size()) return "empty system multicore machine property";
            }
            if (!secondary) return "system multicore requires a secondary-kernel ELF";
        } else if (key == "-cpu") {
            if (cpu_seen || value != "cfv4e") return "system multicore only supports cfv4e";
            cpu_seen = true;
        } else if (key == "-smp") {
            if (smp_seen || value != "2") return "system multicore requires exactly -smp 2";
            smp_seen = true;
        } else {
            if (accel_seen || value != "tcg,thread=single")
                return "system multicore requires single-thread TCG";
            accel_seen = true;
        }
    }
    if (!machine_seen) return "system multicore requires -machine raptor-core2,secondary-kernel=<ELF>";
    if (!smp_seen) return "system multicore requires -smp 2";
    if (!accel_seen) return "system multicore requires -accel tcg,thread=single";
    return nullptr;
}
} }
#endif
