#include "sst_config.h"
#include "sst/elements/carcosa/components/eccTestComponents.h"
#include "sst/elements/carcosa/components/eccModelMath.h"
#include "sst/elements/carcosa/components/configParse.h"
#include <cmath>
#include <sstream>

using namespace SST;
using namespace SST::Carcosa;
using namespace SST::MemHierarchy;

EccModelTest::EccModelTest(ComponentId_t id, Params&) : Component(id) {
    Output out("", 1, 0, Output::STDOUT);
    auto require = [&](bool ok, const char* what) {
        if (!ok) out.fatal(CALL_INFO, -1, "EccModelTest: FAIL %s\n", what);
    };
    // 100 FIT/Mbit/h * 1024 MiB * 8 Mbit/MiB * 100 ns/event / 3.6e12 ns/h.
    const double want_fit = 100.0 * 1e-9 * 1024.0 * 8.0 * 100.0 / 3.6e12;
    require(std::fabs(EccModelMath::fitEventRate(100, 1024, 100) - want_fit) < 1e-30,
            "FIT conversion (including byte-to-bit x8)");
    require(EccModelMath::wordCount(10, EccScheme::SECDED_64) == 2 &&
            EccModelMath::wordBits(10, EccScheme::SECDED_64, 0) == 64 &&
            EccModelMath::wordBits(10, EccScheme::SECDED_64, 1) == 16,
            "partial SECDED word sizing");
    require(EccModelMath::jedecEventRate(0.25, 0.9, 64) == 0.25,
            "explicit fault_event_rate precedence");
    require(EccModelMath::jedecEventRate(0.0, 0.001, 10) == 0.08,
            "BER payload-bit fallback");
    std::string prior = "TARGET"; uint64_t count = 2;
    require(!EccModelMath::resetCampaignEntry("TARGET", prior, count) && count == 2,
            "same campaign entry preserves count");
    require(EccModelMath::resetCampaignEntry("OTHER", prior, count) && count == 0,
            "off-target transition resets count");
    count = 1;
    require(EccModelMath::resetCampaignEntry("TARGET", prior, count) && count == 0,
            "target re-entry resets count");
    out.output("EccModelTest: PASS deterministic ECC math and campaign reset.\n");
}

EccRuntimeTestDriver::EccRuntimeTestDriver(ComponentId_t id, Params& params)
    : Component(id) {
    out_ = new Output("", 1, 0, Output::STDOUT);
    state_key_ = params.find<std::string>("state_key", "ecc_runtime_test");
    requests_ = params.find<int>("requests", 1);
    payload_size_ = params.find<int>("payload_size", 8);
    elapsed_min_ps_ = params.find<int64_t>("test_elapsed_ps_min", -1);
    elapsed_max_ps_ = params.find<int64_t>("test_elapsed_ps_max", -1);
    mutated_min_ = params.find<int>("test_mutated_min", -1);
    mutated_max_ = params.find<int>("test_mutated_max", -1);
    min_changed_bits_ = params.find<unsigned>("test_min_changed_bits", 0);
    expect_mutated_ = params.find<int>("expect_mutated", -1);
    if (params.contains("expect_abort"))
        out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: expect_abort requires frame integration.\n");
    expect_escapes_ = params.find<int64_t>("expect_escapes", -1);
    virtual_offset_ = params.find<uint64_t>("virtual_offset", 0);
    expect_same_payload_ = params.find<bool>("expect_same_payload", false);
    std::stringstream ss(params.find<std::string>("kernel_sequence", ""));
    std::string tok;
    while (std::getline(ss, tok, ',')) kernels_.push_back(tok);
    ss.clear();
    ss.str(params.find<std::string>("request_addresses", ""));
    while (std::getline(ss, tok, ',')) {
        uint64_t address = 0;
        if (!ConfigParse::parseUint64(tok, address))
            out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: invalid request address '%s'.\n", tok.c_str());
        request_addresses_.push_back(address);
    }
    ss.clear();
    ss.str(params.find<std::string>("expect_mutated_sequence", ""));
    while (std::getline(ss, tok, ',')) {
        int expected = 0;
        if (!ConfigParse::parseInt(tok, expected) || (expected != 0 && expected != 1))
            out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: mutation expectations must be 0 or 1.\n");
        expect_mutated_sequence_.push_back(expected);
    }
    if (!expect_mutated_sequence_.empty() && expect_mutated_sequence_.size() != static_cast<size_t>(requests_))
        out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: mutation sequence must cover every request.\n");
    state_ = PipelineStateRegistry<PipelineStateBase>::getOrCreate(state_key_);
    state_->currentKernelName = "TEST";
    const std::string region_name = params.find<std::string>("region_name", "");
    if (!region_name.empty()) {
        state_->publishRegion(0, params.find<uint64_t>("region_base", 0x4000),
                              params.find<uint64_t>("region_size", 4096), region_name);
    }
    cpu_ = configureLink("cpu_side", new Event::Handler<EccRuntimeTestDriver,
                         &EccRuntimeTestDriver::cpuEvent>(this));
    mem_ = configureLink("mem_side", new Event::Handler<EccRuntimeTestDriver,
                         &EccRuntimeTestDriver::memEvent>(this));
    registerClock("1GHz", new Clock::Handler<EccRuntimeTestDriver,
                  &EccRuntimeTestDriver::tick>(this));
    registerAsPrimaryComponent(); primaryComponentDoNotEndSim();
}

bool EccRuntimeTestDriver::tick(Cycle_t) {
    if (!started_) { started_ = true; issue(); }
    return false;
}

void EccRuntimeTestDriver::issue() {
    if (issued_ >= requests_) return;
    if (issued_ == 0) start_time_ps_ = getCurrentSimTime("1ps");
    if (issued_ < static_cast<int>(kernels_.size()))
        state_->currentKernelName = kernels_[issued_];
    const uint64_t addr = request_addresses_.empty()
        ? 0x4000 + static_cast<uint64_t>(issued_ % 64) * 64
        : request_addresses_[issued_ % request_addresses_.size()];
    auto* request = new MemEvent(getName(), addr, addr & ~63ull,
                                 Command::GetS, payload_size_);
    if (virtual_offset_ != 0) request->setVirtualAddress(addr + virtual_offset_);
    cpu_->send(request);
    ++issued_;
}

void EccRuntimeTestDriver::memEvent(Event* ev) {
    auto* req = dynamic_cast<MemEvent*>(ev);
    if (!req) out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: non-MemEvent request.\n");
    if (req->getPayloadSize() != 0)
        out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: guard fabricated a read-request payload.\n");
    MemEvent* resp = req->makeResponse();
    std::vector<uint8_t> payload(payload_size_, 0xA5);
    resp->setPayload(payload);
    mem_->send(resp); delete req;
}

void EccRuntimeTestDriver::cpuEvent(Event* ev) {
    auto* resp = dynamic_cast<MemEvent*>(ev);
    if (!resp) out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: non-MemEvent response.\n");
    if (expect_same_payload_) {
        if (completed_ == 0) reference_payload_ = resp->getPayload();
        else if (resp->getPayload() != reference_payload_)
            out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: resident corruption changed with request offset.\n");
    }
    bool changed = false;
    for (uint8_t b : resp->getPayload()) if (b != 0xA5) { changed = true; break; }
    if (!expect_mutated_sequence_.empty() && changed != (expect_mutated_sequence_[completed_] != 0))
        out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: response %d has unexpected mutation state.\n", completed_);
    if (changed) {
        unsigned changed_bits = 0;
        for (uint8_t byte : resp->getPayload()) {
            unsigned difference = byte ^ 0xA5u;
            while (difference) { difference &= difference - 1; ++changed_bits; }
        }
        if (changed_bits < min_changed_bits_)
            out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: leaked %u changed bits (minimum %u).\n",
                        changed_bits, min_changed_bits_);
        ++mutated_;
    }
    delete resp;
    ++completed_;
    if (completed_ == requests_) {
        elapsed_ps_ = getCurrentSimTime("1ps") - start_time_ps_;
        primaryComponentOKToEndSim();
    }
    else issue();
}

void EccRuntimeTestDriver::finish() {
    bool ok = completed_ == requests_;
    if ((mutated_min_ >= 0 && mutated_ < mutated_min_) ||
        (mutated_max_ >= 0 && mutated_ > mutated_max_)) ok = false;
    if ((elapsed_min_ps_ >= 0 && elapsed_ps_ < static_cast<uint64_t>(elapsed_min_ps_)) ||
        (elapsed_max_ps_ >= 0 && elapsed_ps_ > static_cast<uint64_t>(elapsed_max_ps_))) {
        out_->fatal(CALL_INFO, -1, "EccRuntimeTestDriver: elapsed time %" PRIu64
                    " ps outside [%" PRId64 ",%" PRId64 "].\n",
                    elapsed_ps_, elapsed_min_ps_, elapsed_max_ps_);
    }
    if (expect_mutated_ >= 0 && mutated_ != expect_mutated_) ok = false;
    if (expect_escapes_ >= 0 && state_->eccCumulativeEscapes !=
        static_cast<uint64_t>(expect_escapes_)) ok = false;
    if (!ok) {
        out_->fatal(CALL_INFO, -1,
            "EccRuntimeTestDriver: FAIL completed=%d/%d mutated=%d escapes=%" PRIu64 "\n",
            completed_, requests_, mutated_,
            state_->eccCumulativeEscapes);
    }
    out_->output("EccRuntimeTestDriver: PASS completed=%d mutated=%d escapes=%" PRIu64 "\n",
                 completed_, mutated_,
                 state_->eccCumulativeEscapes);
    delete out_; out_ = nullptr;
}
