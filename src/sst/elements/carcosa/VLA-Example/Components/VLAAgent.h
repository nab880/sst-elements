#ifndef CARCOSA_VLAAGENT_H
#define CARCOSA_VLAAGENT_H

#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/rng/marsaglia.h>
#include <sst/elements/carcosa/Components/InterceptionAgentAPI.h>
#include <sst/elements/carcosa/VLA-Example/Components/vla-fsm.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <climits>
#include <cstdint>

namespace SST {
namespace Carcosa {

class VLAAgent : public InterceptionAgentAPI
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        VLAAgent,
        "Carcosa",
        "VLAAgent",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "VLA FSM agent: command/status MMIO for 18 kernel states, layer loops, decode coin flip",
        SST::Carcosa::InterceptionAgentAPI
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"num_vit_layers", "Number of Vision Transformer layers (L).", "24"},
        {"num_llm_layers", "Number of LLM layers (L_LLM) for prefill and decode.", "32"},
        {"max_cycles", "Number of full pipeline cycles (Idle->Actuate) before sending exit (-1). 0 = run forever.", "1"},
        {"initial_seq_len", "Sequence length after prefill (num_patches + num_text_tokens); must match binary.", "228"},
        {"max_seq_len", "KV-cache capacity in the RISC-V binary (must match MAX_SEQ_LEN compile-time macro in vla_shared.h / vla.c). The agent fatal-errors if initial_seq_len + (num_action_tokens - 1) would exceed this.", "64"},
        {"hyades_role", "Value returned when reading HYADES_ROLE (+0x10).", "0"},
        {"num_action_tokens", "Number of decode tokens to generate per pipeline cycle (GEMV_PROJECT->KV_CACHE_ATTN->DECODE_FFN->LM_HEAD loop iterations).", "1"},
        {"verbose", "Enable verbose output.", "false"}
    )

    VLAAgent(ComponentId_t id, Params& params);
    VLAAgent() : InterceptionAgentAPI() {}
    ~VLAAgent();

    bool handleInterceptedEvent(SST::MemHierarchy::MemEvent* ev, SST::Link* highlink) override;
    void agentSetup() override;
    void setInterceptBase(uint64_t base) override;
    void setHighlink(SST::Link* highlink) override;

private:
    void advanceFSM();
    void sendCommandResponse(SST::MemHierarchy::MemEvent* request, int value);
    void sendWriteAck(SST::MemHierarchy::MemEvent* ev);

    SST::Output* out_;
    SST::Link* highlink_ = nullptr;
    uint64_t controlAddrBase_ = 0;
    VlaFsm fsm_;
    int hyadesRole_ = 0;
    SST::RNG::MarsagliaRNG* rng_ = nullptr;
    bool verbose_ = false;
};

} // namespace Carcosa
} // namespace SST

#endif /* CARCOSA_VLAAGENT_H */
