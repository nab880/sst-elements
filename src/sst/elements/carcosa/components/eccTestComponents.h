#ifndef SST_ELEMENTS_CARCOSA_ECC_TEST_COMPONENTS_H
#define SST_ELEMENTS_CARCOSA_ECC_TEST_COMPONENTS_H

#include "sst/elements/memHierarchy/memEvent.h"
#include <sst/core/link.h>
#include "sst/elements/carcosa/components/pipelineStateRegistry.h"
#include <sst/core/component.h>
#include <sst/core/output.h>
#include <string>
#include <vector>

namespace SST { namespace Carcosa {

class EccModelTest : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(EccModelTest, "carcosa", "EccModelTest",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Self-asserting ECC model math test.",
        COMPONENT_CATEGORY_UNCATEGORIZED)
    EccModelTest(ComponentId_t id, Params& params);
};

class EccRuntimeTestDriver : public SST::Component {
public:
    SST_ELI_REGISTER_COMPONENT(EccRuntimeTestDriver, "carcosa", "EccRuntimeTestDriver",
        SST_ELI_ELEMENT_VERSION(1,0,0), "Self-asserting EccGuard runtime driver.",
        COMPONENT_CATEGORY_UNCATEGORIZED)
    SST_ELI_DOCUMENT_PARAMS(
        {"state_key", "Pipeline registry key.", "ecc_runtime_test"},
        {"requests", "Number of serialized reads.", "1"},
        {"payload_size", "Response payload bytes.", "8"},
        {"kernel_sequence", "Optional CSV kernel name per request.", ""},
        {"request_addresses", "Optional CSV physical addresses, repeated as needed.", ""},
        {"virtual_offset", "If nonzero, stamp virtual address as physical plus offset.", "0"},
        {"region_name", "Optional published region name.", ""},
        {"region_base", "Published region base.", "16384"},
        {"region_size", "Published region bytes.", "4096"},
        {"expect_same_payload", "Require every response to match the first response byte for byte.", "false"},
        {"expect_mutated_sequence", "Optional CSV 0/1 mutation expectation for each response.", ""},
        {"test_elapsed_ps_min", "Minimum time from first request to final response (-1 disables).", "-1"},
        {"test_elapsed_ps_max", "Maximum time from first request to final response (-1 disables).", "-1"},
        {"test_mutated_min", "Minimum number of changed response payloads (-1 disables).", "-1"},
        {"test_mutated_max", "Maximum number of changed response payloads (-1 disables).", "-1"},
        {"test_min_changed_bits", "Minimum bit differences in every changed response (0 disables).", "0"},
        {"expect_mutated", "Expected mutated responses (-1 disables).", "-1"},
        {"expect_abort", "Unsupported until frame integration; omit this parameter.", "-1"},
        {"expect_escapes", "Expected cumulative escapes (-1 disables).", "-1"})
    SST_ELI_DOCUMENT_PORTS(
        {"cpu_side", "Connect to EccGuard highlink.", {"memHierarchy.MemEventBase"}},
        {"mem_side", "Connect to EccGuard lowlink.", {"memHierarchy.MemEventBase"}})
    EccRuntimeTestDriver(ComponentId_t id, Params& params);
    void finish() override;
private:
    bool tick(Cycle_t);
    void cpuEvent(Event*);
    void memEvent(Event*);
    void issue();
    Output* out_ = nullptr;
    Link *cpu_ = nullptr, *mem_ = nullptr;
    PipelineStateBase* state_ = nullptr;
    std::string state_key_;
    std::vector<std::string> kernels_;
    std::vector<uint64_t> request_addresses_;
    std::vector<int> expect_mutated_sequence_;
    std::vector<uint8_t> reference_payload_;
    uint64_t virtual_offset_ = 0;
    bool expect_same_payload_ = false;
    int requests_ = 1, payload_size_ = 8, issued_ = 0, completed_ = 0;
    int mutated_ = 0, expect_mutated_ = -1;
    int64_t expect_escapes_ = -1;
    int64_t elapsed_min_ps_ = -1, elapsed_max_ps_ = -1;
    int mutated_min_ = -1, mutated_max_ = -1;
    unsigned min_changed_bits_ = 0;
    uint64_t start_time_ps_ = 0, elapsed_ps_ = 0;
    bool started_ = false;
};

}} // namespace SST::Carcosa
#endif
