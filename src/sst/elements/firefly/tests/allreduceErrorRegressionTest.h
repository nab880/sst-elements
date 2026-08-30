// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

#ifndef SST_ELEMENTS_FIREFLY_TESTS_ALLREDUCE_ERROR_REGRESSION_TEST_H
#define SST_ELEMENTS_FIREFLY_TESTS_ALLREDUCE_ERROR_REGRESSION_TEST_H

#include <sst/core/component.h>
#include <sst/core/link.h>

namespace SST::Firefly {

class AllreduceErrorRegressionTest final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(AllreduceErrorRegressionTest, "firefly",
        "allreduce_error_regression_test", SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Exercise terminal Allreduce handling after an accepted RecoverableError",
        COMPONENT_CATEGORY_PROCESSOR)

    SST_ELI_DOCUMENT_PARAMS()
    SST_ELI_DOCUMENT_PORTS()

    AllreduceErrorRegressionTest(ComponentId_t id, Params& params);

    void setup() override;

private:
    void run(SST::Event* event);
    [[noreturn]] void fail(const char* reason) const;

    SST::Link* run_link_ = nullptr;
};

} // namespace SST::Firefly

#endif // SST_ELEMENTS_FIREFLY_TESTS_ALLREDUCE_ERROR_REGRESSION_TEST_H
