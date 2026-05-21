#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>

#include "../../quetz_config_manager.h"

namespace SST {
namespace Quetz {
const std::vector<QuetzPlatformProfile>& quetzPlatformRegistry();
} // namespace Quetz
} // namespace SST

using SST::Quetz::QuetzPlatformProfile;
using SST::Quetz::quetzPlatformRegistry;

static const QuetzPlatformProfile* findPlatform(const std::string& name) {
    for (const auto& p : quetzPlatformRegistry())
        if (p.name == name)
            return &p;
    return nullptr;
}

TEST_CASE("platform registry") {
    const auto* p = findPlatform("riscv64_virt");
    REQUIRE(p != nullptr);
    CHECK(p->name == "riscv64_virt");
    CHECK(p->region_handlers.size() == 1);
    CHECK(p->region_handlers[0].type == "quetz.FilteredRegionHandler");

    const auto* uart = findPlatform("riscv64_virt_uart");
    REQUIRE(uart != nullptr);
    CHECK(uart->region_handlers.size() == 2);
    CHECK(uart->region_handlers[0].type == "quetz.UartRegionHandler");
    CHECK(uart->region_handlers[1].type == "quetz.FilteredRegionHandler");

    CHECK(findPlatform("nonexistent") == nullptr);
}

TEST_CASE("platform qemu defaults") {
    const auto* p = findPlatform("riscv64_virt");
    REQUIRE(p != nullptr);
    bool has_qemu = false;
    bool has_sysmode = false;
    for (const auto& kv : p->param_defaults) {
        if (kv.first == "qemu" && kv.second.find("riscv64") != std::string::npos)
            has_qemu = true;
        if (kv.first == "system_mode" && kv.second == "1")
            has_sysmode = true;
    }
    CHECK(has_qemu);
    CHECK(has_sysmode);
}
