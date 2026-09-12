#ifndef SST_QUETZ_CACHE_LAUNCH_H
#define SST_QUETZ_CACHE_LAUNCH_H
#include <string>
#include <vector>
#include "quetz_multicore_launch.h"

namespace SST { namespace Quetz {
// Fail closed: the functional cache implements V4e controls, whereas other
// ColdFire CPUs use different CACR fields. The dedicated board defaults to
// cfv4e. Allow the canonical strict-MMIO deck; reject config files and other
// composite model options that could override the CPU or machine.
inline const char* windowCacheLaunchError(const std::vector<std::string>& args,
                                         uint32_t vcpus = 1)
{
    if (vcpus == 2) return multicoreLaunchError(vcpus, true, args);
    if (vcpus != 1) return "cache window requires one or two CPUs";
    bool found_machine = false, found_cpu = false, found_smp = false, found_accel = false;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string option = args[i];
        if (option.rfind("--", 0) == 0) option.erase(0, 1);
        const auto equal = option.find('=');
        const std::string key = option.substr(0, equal);
        if (key == "-readconfig" || key == "-global" || key == "-enable-kvm")
            return "cache window does not support external model overrides";
        const bool machine = key == "-M" || key == "-machine";
        const bool cpu = key == "-cpu";
        if (!machine && !cpu && key != "-smp" && key != "-accel") continue;
        std::string value;
        if (equal != std::string::npos) value = option.substr(equal + 1);
        else if (++i < args.size()) value = args[i];
        if (machine) {
            if (found_machine) return "cache window requires one machine selection";
            if (value != "raptor-core2" && value != "raptor-core2,strict-mmio=on")
                return "cache window requires raptor-core2 (optional strict-mmio=on only)";
            found_machine = true;
        } else if (cpu) {
            if (found_cpu || value != "cfv4e")
                return "cache window only supports one cfv4e CPU selection";
            found_cpu = true;
        } else if (key == "-smp") {
            if (found_smp || value != "1")
                return "cache window QEMU topology does not match one configured CPU";
            found_smp = true;
        } else {
            if (found_accel || (value != "tcg" && value != "tcg,thread=single"))
                return "cache window requires single-thread TCG";
            found_accel = true;
        }
    }
    return found_machine ? nullptr : "cache window requires -machine raptor-core2";
}
} }
#endif
