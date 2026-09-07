// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights
// in this software.

#ifndef SST_ELEMENTS_EMBER_MPI_MOTIFS_EMBERAITRAINING_H
#define SST_ELEMENTS_EMBER_MPI_MOTIFS_EMBERAITRAINING_H

#include "mpi/embermpigen.h"

#include <cstdint>
#include <queue>
#include <vector>

namespace SST {
namespace Ember {

/**
 * A functional synchronous data-parallel training loop.
 *
 * Every rank owns a different quadratic training sample.  An epoch computes
 * the local gradient, sums gradients with Allreduce, and applies the same
 * averaged update on every rank.  An optional scalar loss reduction models
 * the globally synchronized metric common to distributed training loops.
 */
class EmberAITrainingGenerator : public EmberMessagePassingGenerator
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        EmberAITrainingGenerator,
        "ember",
        "AITrainingMotif",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Functional synchronous data-parallel SGD workload",
        SST::Ember::EmberGenerator)

    SST_ELI_DOCUMENT_PARAMS(
        { "arg.epochs", "Number of synchronous SGD epochs", "8" },
        { "arg.compute", "Modeled forward/backward compute per epoch in ns", "500" },
        { "arg.parameters", "Number of model parameters and gradient elements", "1" },
        { "arg.learning_rate", "SGD learning rate in the range (0, 1]", "0.5" },
        { "arg.sync_loss", "Allreduce one scalar loss metric after each gradient", "false" },
        { "arg.verify", "Verify every collective result and final model", "true" })

    EmberAITrainingGenerator(SST::ComponentId_t id, Params& params);
    ~EmberAITrainingGenerator() override;

    bool generate(std::queue<EmberEvent*>& evQ) override;

private:
    void consumeEpoch();
    void prepareEpoch();
    void verifyClose(const char* valueName, uint32_t index, double actual, double expected);

    uint32_t m_epochs;
    uint32_t m_parameters;
    uint32_t m_epoch;
    uint64_t m_compute;
    double m_learningRate;
    bool m_syncLoss;
    bool m_verify;

    uint64_t m_totalStart;
    uint64_t m_totalStop;
    uint64_t m_collectiveStart;
    uint64_t m_collectiveStop;
    uint64_t m_collectiveTotal;

    std::vector<double> m_model;
    std::vector<double> m_expectedGradient;
    double m_expectedLoss;

    double* m_gradientSend;
    double* m_gradientRecv;
    double* m_lossSend;
    double* m_lossRecv;
};

} // namespace Ember
} // namespace SST

#endif // SST_ELEMENTS_EMBER_MPI_MOTIFS_EMBERAITRAINING_H
