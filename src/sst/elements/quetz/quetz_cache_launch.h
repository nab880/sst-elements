#ifndef SST_QUETZ_CACHE_LAUNCH_H
#define SST_QUETZ_CACHE_LAUNCH_H
#include <string>
#include <vector>

namespace SST { namespace Quetz {
// Fail closed: the functional cache implements V4e controls, whereas other
// ColdFire CPUs use different CACR fields. The dedicated board defaults to
// cfv4e. Allow the canonical strict-MMIO deck; reject config files and other
// composite model options that could override the CPU or machine.
inline const char* windowCacheLaunchError(const std::vector<std::string>& args)
{
    bool found_machine = false;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string option = args[i];
        if (option.rfind("--", 0) == 0) option.erase(0, 1);
        const auto equal = option.find('=');
        const std::string key = option.substr(0, equal);
        if (key == "-readconfig")
            return "cache window does not support -readconfig model overrides";
        const bool machine = key == "-M" || key == "-machine";
        const bool cpu = key == "-cpu";
        if (!machine && !cpu) continue;
        std::string value;
        if (equal != std::string::npos) value = option.substr(equal + 1);
        else if (++i < args.size()) value = args[i];
        if (machine) {
            if (value != "raptor-core2" && value != "raptor-core2,strict-mmio=on")
                return "cache window requires raptor-core2 (optional strict-mmio=on only)";
            found_machine = true;
        } else if (value != "cfv4e") {
            return "cache window only supports the cfv4e CPU";
        }
    }
    return found_machine ? nullptr : "cache window requires -machine raptor-core2";
}
} }
#endif
