// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights
// in this software.

#include <sst_config.h>

#include "emberaitraining.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace SST::Ember;

EmberAITrainingGenerator::EmberAITrainingGenerator(SST::ComponentId_t id, Params& params) :
    EmberMessagePassingGenerator(id, params, "AITraining"),
    m_epochs(params.find<uint32_t>("arg.epochs", 8)),
    m_parameters(params.find<uint32_t>("arg.parameters", 1)),
    m_epoch(0),
    m_compute(params.find<uint64_t>("arg.compute", 500)),
    m_learningRate(params.find<double>("arg.learning_rate", 0.5)),
    m_syncLoss(params.find<bool>("arg.sync_loss", false)),
    m_verify(params.find<bool>("arg.verify", true)),
    m_totalStart(0),
    m_totalStop(0),
    m_collectiveStart(0),
    m_collectiveStop(0),
    m_collectiveTotal(0),
    m_model(m_parameters, 0.0),
    m_expectedGradient(m_parameters, 0.0),
    m_expectedLoss(0.0),
    m_gradientSend(nullptr),
    m_gradientRecv(nullptr),
    m_lossSend(nullptr),
    m_lossRecv(nullptr)
{
    if ( m_epochs == 0 ) {
        fatal(CALL_INFO, -1, "AITraining requires at least one epoch\n");
    }
    if ( m_parameters == 0 ) {
        fatal(CALL_INFO, -1, "AITraining requires at least one model parameter\n");
    }
    if ( !std::isfinite(m_learningRate) || m_learningRate <= 0.0 || m_learningRate > 1.0 ) {
        fatal(CALL_INFO, -1, "AITraining learning_rate must be finite and in the range (0, 1]\n");
    }

    memSetBacked();
    m_gradientSend = static_cast<double*>(memAlloc(sizeof(double) * m_parameters));
    m_gradientRecv = static_cast<double*>(memAlloc(sizeof(double) * m_parameters));
    if ( m_syncLoss ) {
        m_lossSend = static_cast<double*>(memAlloc(sizeof(double)));
        m_lossRecv = static_cast<double*>(memAlloc(sizeof(double)));
    }
}

EmberAITrainingGenerator::~EmberAITrainingGenerator()
{
    if ( m_gradientSend != nullptr ) {
        memFree(m_gradientSend);
    }
    if ( m_gradientRecv != nullptr ) {
        memFree(m_gradientRecv);
    }
    if ( m_lossSend != nullptr ) {
        memFree(m_lossSend);
    }
    if ( m_lossRecv != nullptr ) {
        memFree(m_lossRecv);
    }
}

void EmberAITrainingGenerator::verifyClose(
    const char* valueName, uint32_t index, double actual, double expected)
{
    const double tolerance = 1.0e-10 * std::max(1.0, std::abs(expected));
    if ( !std::isfinite(actual) || std::abs(actual - expected) > tolerance ) {
        fatal(CALL_INFO, -1,
            "AITraining verification failed at rank %d epoch %u for %s[%u]: "
            "expected %.12f, got %.12f\n",
            rank(), m_epoch - 1, valueName, index, expected, actual);
    }
}

void EmberAITrainingGenerator::consumeEpoch()
{
    m_collectiveTotal += m_collectiveStop - m_collectiveStart;

    for ( uint32_t parameter = 0; parameter < m_parameters; ++parameter ) {
        if ( m_verify ) {
            verifyClose("gradient", parameter, m_gradientRecv[parameter],
                m_expectedGradient[parameter]);
        }
        m_model[parameter] -=
            m_learningRate * m_gradientRecv[parameter] / static_cast<double>(size());
    }

    if ( m_syncLoss && m_verify ) {
        verifyClose("loss", 0, *m_lossRecv, m_expectedLoss);
    }
}

void EmberAITrainingGenerator::prepareEpoch()
{
    const double localBaseTarget = static_cast<double>(rank() + 1);
    const double rankCount = static_cast<double>(size());
    const double targetSumBase = rankCount * (rankCount + 1.0) / 2.0;
    m_expectedLoss = 0.0;

    for ( uint32_t parameter = 0; parameter < m_parameters; ++parameter ) {
        const double offset = static_cast<double>(parameter);
        const double localTarget = localBaseTarget + offset;
        const double localGradient = m_model[parameter] - localTarget;
        m_gradientSend[parameter] = localGradient;
        m_gradientRecv[parameter] = std::numeric_limits<double>::quiet_NaN();
        m_expectedGradient[parameter] =
            rankCount * m_model[parameter] - targetSumBase - rankCount * offset;

        if ( m_syncLoss ) {
            const double residual = m_model[parameter] - localTarget;
            *m_lossSend += 0.5 * residual * residual;

            for ( int worker = 0; worker < size(); ++worker ) {
                const double target = static_cast<double>(worker + 1) + offset;
                const double globalResidual = m_model[parameter] - target;
                m_expectedLoss += 0.5 * globalResidual * globalResidual;
            }
        }
    }

    if ( m_syncLoss ) {
        *m_lossRecv = std::numeric_limits<double>::quiet_NaN();
    }
}

bool EmberAITrainingGenerator::generate(std::queue<EmberEvent*>& evQ)
{
    if ( m_epoch > 0 ) {
        consumeEpoch();
    }

    if ( m_epoch == m_epochs ) {
        if ( m_verify ) {
            const double decay = std::pow(1.0 - m_learningRate, static_cast<double>(m_epochs));
            const double meanTargetBase = (static_cast<double>(size()) + 1.0) / 2.0;
            for ( uint32_t parameter = 0; parameter < m_parameters; ++parameter ) {
                const double expected =
                    (meanTargetBase + static_cast<double>(parameter)) * (1.0 - decay);
                verifyClose("model", parameter, m_model[parameter], expected);
            }
            output(
                "Ember AI training verify rank %d parameters %u epochs %u "
                "weight0 %.6f target0 %.6f PASS\n",
                rank(), m_parameters, m_epochs, m_model.front(), meanTargetBase);
        }

        const double stepTimeUs =
            static_cast<double>(m_totalStop - m_totalStart) /
            static_cast<double>(m_epochs) / 1000.0;
        const double collectiveTimeUs =
            static_cast<double>(m_collectiveTotal) /
            static_cast<double>(m_epochs) / 1000.0;
        output(
            "AITraining rank %d: ranks %d, epochs %u, parameters %u, loss-sync %s, "
            "step %.6f us, collectives %.6f us\n",
            rank(), size(), m_epochs, m_parameters, m_syncLoss ? "on" : "off",
            stepTimeUs, collectiveTimeUs);
        return true;
    }

    if ( m_epoch == 0 ) {
        enQ_getTime(evQ, &m_totalStart);
    }

    if ( m_syncLoss ) {
        *m_lossSend = 0.0;
    }
    prepareEpoch();

    enQ_compute(evQ, m_compute);
    enQ_getTime(evQ, &m_collectiveStart);
    enQ_allreduce(evQ, m_gradientSend, m_gradientRecv, m_parameters,
        DOUBLE, Hermes::MP::SUM, GroupWorld);
    if ( m_syncLoss ) {
        enQ_allreduce(evQ, m_lossSend, m_lossRecv, 1,
            DOUBLE, Hermes::MP::SUM, GroupWorld);
    }
    enQ_getTime(evQ, &m_collectiveStop);

    ++m_epoch;
    if ( m_epoch == m_epochs ) {
        enQ_getTime(evQ, &m_totalStop);
    }
    return false;
}
