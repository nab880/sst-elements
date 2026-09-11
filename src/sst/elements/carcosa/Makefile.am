# -*- Makefile -*-
#
#

AM_CPPFLAGS += \
	$(MPI_CPPFLAGS) \
	-I$(top_srcdir)/src

compdir = $(pkglibdir)
comp_LTLIBRARIES = libcarcosa.la
libcarcosa_la_SOURCES =                     \
    components/hali.cc                      \
    components/hali.h                       \
    components/faultInjManager.cc           \
    components/faultInjManager.h            \
    injectors/faultInjectorBase.cc          \
    injectors/faultInjectorBase.h           \
    injectors/stuckAtFaultInjector.cc       \
    injectors/stuckAtFaultInjector.h        \
    injectors/corruptMemFaultInjector.cc    \
    injectors/corruptMemFaultInjector.h     \
    injectors/randomDropFaultInjector.cc    \
    injectors/randomDropFaultInjector.h     \
    injectors/randomFlipFaultInjector.cc    \
    injectors/randomFlipFaultInjector.h     \
    injectors/dropFlipFaultInjector.cc      \
    injectors/dropFlipFaultInjector.h       \
    injectors/faultInjectorMemH.cc          \
    injectors/faultInjectorMemH.h           \
    faultlogic/faultBase.cc                 \
    faultlogic/faultBase.h                  \
    faultlogic/stuckAtFault.cc              \
    faultlogic/stuckAtFault.h               \
    faultlogic/corruptMemFault.cc           \
    faultlogic/corruptMemFault.h            \
    faultlogic/randomDropFault.cc           \
    faultlogic/randomDropFault.h            \
    faultlogic/randomFlipFault.cc           \
    faultlogic/randomFlipFault.h            \
    faultlogic/randomFlipMemHFault.cc       \
    faultlogic/randomFlipMemHFault.h        \
    components/pmDataRegistry.h             \
    components/carcosaCPUBase.cc            \
    components/carcosaCPUBase.h             \
    components/carcosaCPU.cc                \
    components/carcosaCPU.h                 \
    components/faultInjCPU.cc               \
    components/faultInjCPU.h                \
    components/cpuEvent.h                   \
    components/faultInjEvent.h              \
    components/faultInjManagerAPI.h         \
    components/interceptionAgentAPI.h       \
    components/pingPongAgent.cc             \
    components/pingPongAgent.h              \
    components/carcosaMemCtrl.cc            \
    components/carcosaMemCtrl.h             \
    components/sensorComponent.cc           \
    components/sensorComponent.h           \
    components/sensorEvent.h                \
    components/haliEvent.h

EXTRA_DIST = \
    hyades.h                                      \
    tests/pingpong.c                              \
    tests/testcarcosaPingPong.py                  \
    tests/testCorruptMemBasic.py                  \
    tests/testCorruptMemDouble.py                 \
    tests/testCorruptMemDoubleOverlap.py          \
    tests/testRandomDrop.py                       \
    tests/testRandomFlip.py                       \
    tests/testStuckAtBasic.py                     \
    tests/testStuckAtMultiple.py                  \
    tests/testStuckAtOverlap.py                   \
    tests/testStuckAtSameByte.py                  \
    tests/mhlib.py                                \
    tests/testdynamicPM.py                        \
    tests/testhaliBacking.py                      \
    tests/testhaliMemH.py                         \
    tests/testhaliPM.py                           \
    tests/testmanagerLogic.py

sstdir = $(includedir)/sst/elements/carcosa
nobase_sst_HEADERS =                        \
    injectors/faultInjectorBase.h           \
    injectors/stuckAtFaultInjector.h        \
    injectors/corruptMemFaultInjector.h     \
    injectors/randomDropFaultInjector.h     \
    injectors/randomFlipFaultInjector.h     \
    injectors/dropFlipFaultInjector.h       \
    injectors/faultInjectorMemH.h           \
    faultlogic/faultBase.h                  \
    faultlogic/stuckAtFault.h               \
    faultlogic/corruptMemFault.h            \
    faultlogic/randomDropFault.h            \
    faultlogic/randomFlipFault.h            \
    faultlogic/randomFlipMemHFault.h        \
    components/pmDataRegistry.h             \
    components/faultInjManagerAPI.h        \
    components/interceptionAgentAPI.h      \
    components/pingPongAgent.h

libcarcosa_la_LDFLAGS = -module -avoid-version
libcarcosa_la_LIBADD =

AM_CPPFLAGS += $(HMC_FLAG)
install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE     carcosa=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS      carcosa=$(abs_srcdir)/tests

# Fix Carcosa CPU traffic selection and documented defaults
EXTRA_DIST += \
    tests/testCarcosaTrafficStandalone.py

# Add transport-neutral Hali control and MMIO routing
libcarcosa_la_SOURCES += \
    components/haliTestComponents.cc \
    components/haliTestComponents.h \
    components/hyadesProtocol.h \
    components/ringProtocol.h \
    examples/MmioControl/mmioControlExample.cc \
    examples/MmioControl/mmioControlExample.h
EXTRA_DIST += \
    examples/MmioControl/tests/testMmioControl.py \
    tests/haliEdgeCommon.py \
    tests/testHaliDeferredComplete.py \
    tests/testHaliDoubleDeferred.py \
    tests/testHaliMmioRouting.py \
    tests/testHaliPayloadGetX.py \
    tests/testHaliPayloadlessGetX.py \
    tests/testHaliPostedWrite.py
nobase_sst_HEADERS += \
    components/haliEvent.h \
    components/hyadesProtocol.h \
    components/ringProtocol.h \
    examples/MmioControl/mmioControlExample.h

# Add core ECC schemes, Poisson faults, and access policies
libcarcosa_la_SOURCES += \
    components/componentTestBounds.h \
    components/configParse.h \
    components/eccGuard.cc \
    components/eccGuard.h \
    components/eccModelMath.h \
    components/eccPayloadCorruptor.cc \
    components/eccPayloadCorruptor.h \
    components/eccPolicy.h \
    components/eccScheme.h \
    components/eccTestComponents.cc \
    components/eccTestComponents.h \
    components/memEventPayload.h \
    components/pipelineStateRegistry.h
EXTRA_DIST += \
    tests/eccRuntimeCommon.py \
    tests/testEccCoreRuntime.py \
    tests/testEccGuardSmoke.py \
    tests/testEccModelDeterministic.py \
    tests/testEccPoissonDistribution.py
nobase_sst_HEADERS += \
    components/componentTestBounds.h \
    components/configParse.h \
    components/eccGuard.h \
    components/eccModelMath.h \
    components/eccPayloadCorruptor.h \
    components/eccPolicy.h \
    components/eccScheme.h \
    components/memEventPayload.h \
    components/pipelineStateRegistry.h

# Add correlated, campaign, and resident ECC fault models
EXTRA_DIST += \
    tests/testEccCampaignFilterLength.py \
    tests/testEccCampaignPayloadlessRead.py \
    tests/testEccCampaignReentry.py \
    tests/testEccGuardJedecMix.py \
    tests/testEccGuardResident.py \
    tests/testEccJedecDistribution.py \
    tests/testEccResidentDistribution.py \
    tests/testEccResidentPayloadAddress.py \
    tests/testEccRuntimeCorrectable.py \
    tests/testEccRuntimeEscape.py

# Add kernel and region predicates for port-module faults
libcarcosa_la_SOURCES += \
    examples/SimplePipeline/simplePipelineExample.cc \
    examples/SimplePipeline/simplePipelineExample.h \
    injectors/portModuleStateGate.cc \
    injectors/portModuleStateGate.h
EXTRA_DIST += \
    examples/SimplePipeline/tests/testPortModuleStateGate.py \
    examples/SimplePipeline/tests/testSimplePipeline.py
nobase_sst_HEADERS += \
    examples/SimplePipeline/simplePipelineExample.h \
    injectors/portModuleStateGate.h

# Add pipeline frame control, fault observation, and action scoring
libcarcosa_la_SOURCES += \
    components/actionScorer.cc \
    components/actionScorer.h \
    components/carcosaHash.h \
    components/criticalActionWatcher.cc \
    components/criticalActionWatcher.h \
    components/fourStateAgent.cc \
    components/fourStateAgent.h \
    components/framePipelineDriver.cc \
    components/framePipelineDriver.h \
    components/vlaRegions.h
EXTRA_DIST += \
    tests/fourstate.c \
    tests/framePipelineCommon.py \
    tests/testActionScorerDropDivergence.py \
    tests/testActionScorerTaxonomy.py \
    tests/testActionScorerTokenFallback.py \
    tests/testEccGuardRegionPolicy.py \
    tests/testEccRuntimeDueDrop.py \
    tests/testFourStateRegistry.py \
    tests/testFourStateRegistryGated.py \
    tests/testFramePipelineCachedResponse.py \
    tests/testFramePipelineClean.py \
    tests/testFramePipelineCorrupt.py \
    tests/testFramePipelineFallback.py \
    tests/testFramePipelineMissingGolden.py \
    tests/testStateGateRegionNoOverlap.py \
    tests/testStateGateRegionOverlap.py
nobase_sst_HEADERS += \
    components/actionScorer.h \
    components/carcosaHash.h \
    components/criticalActionWatcher.h \
    components/fourStateAgent.h \
    components/vlaRegions.h

# Add optional Balar CUDA replay bridge with retry and DMA regressions
if SST_CARCOSA_HAVE_BALAR
libcarcosa_la_SOURCES += \
    components/balarRingBridge.cc \
    components/balarRingBridge.h \
    components/balarTraceParser.cc \
    components/balarTraceParser.h
AM_CPPFLAGS += $(CUDA_CPPFLAGS) -DHAVE_BALAR_BRIDGE=1
endif
EXTRA_DIST += \
    tests/testBalarRingBridgeStandalone.py
