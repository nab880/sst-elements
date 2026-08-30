// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_MERLIN_NETWORK_SERVICE_PASS_H
#define SST_ELEMENTS_MERLIN_NETWORK_SERVICE_PASS_H

#include "networkService.h"

#include <exception>

namespace SST::Merlin {

/** Merlin-owned processor used to validate transparent tagged carriage. */
class NetworkServicePassProcessor final : public NetworkServiceProcessor
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(NetworkServicePassProcessor, "merlin", "network_service_pass",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Pass tagged packets through ordinary Merlin routing",
        SST::Merlin::NetworkServiceProcessor)

    SST_ELI_DOCUMENT_PARAMS(
        { "service_id", "Nonzero 16-bit network-service ID to pass", "" }
    )

    NetworkServicePassProcessor(ComponentId_t id, Params& params, NetworkServiceHost* host);
    NetworkServicePassProcessor() = default;

    NetworkServiceID getServiceID() const override { return service_id_; }
    NetworkServiceDecision inspect(const NetworkServiceIngress& ingress) const override;
    void consume(NetworkServiceOwnedIngress) noexcept override { std::terminate(); }
    bool hasScheduledWork() const override { return false; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        NetworkServiceProcessor::serialize_order(ser);
        SST_SER(service_id_);
    }

private:
    NetworkServiceID service_id_ = SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE;

    ImplementSerializable(SST::Merlin::NetworkServicePassProcessor)
};

} // namespace SST::Merlin

#endif // SST_ELEMENTS_MERLIN_NETWORK_SERVICE_PASS_H
