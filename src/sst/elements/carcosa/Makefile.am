# -*- Makefile -*-
#
#

AM_CPPFLAGS += \
	$(MPI_CPPFLAGS) \
	-I$(top_srcdir)/src

compdir = $(pkglibdir)
comp_LTLIBRARIES = libcarcosa.la
libcarcosa_la_SOURCES =                     \
    Components/Hali.cc                      \
    Components/Hali.h                       \
    Components/FaultInjManager.cc           \
    Components/FaultInjManager.h            \
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
    Components/PMDataRegistry.h             \
    Components/CarcosaCPUBase.cc            \
    Components/CarcosaCPUBase.h             \
    Components/CarcosaCPU.cc                \
    Components/CarcosaCPU.h                 \
    Components/FaultInjCPU.cc               \
    Components/FaultInjCPU.h                \
    Components/CpuEvent.h                   \
    Components/FaultInjEvent.h              \
    Components/FaultInjManagerAPI.h         \
    Components/InterceptionAgentAPI.h       \
    Components/PingPongAgent.cc             \
    Components/PingPongAgent.h              \
    VLA-Example/Components/vla-fsm.cc       \
    VLA-Example/Components/vla-fsm.h        \
    VLA-Example/Components/VLAAgent.cc      \
    VLA-Example/Components/VLAAgent.h       \
    VLA-Example/Components/VLAKernelComplexity.h \
    VLA-Example/Components/VLACpuAgent.cc   \
    VLA-Example/Components/VLACpuAgent.h    \
    VLA-Example/Components/VLAGpuAgent.cc   \
    VLA-Example/Components/VLAGpuAgent.h    \
    VLA-Example/Components/VLACpuDelayAgent.cc \
    VLA-Example/Components/VLACpuDelayAgent.h  \
    VLA-Example/Components/VLAGpuDelayAgent.cc \
    VLA-Example/Components/VLAGpuDelayAgent.h  \
    Components/CarcosaMemCtrl.cc            \
    Components/CarcosaMemCtrl.h             \
    Components/SensorComponent.cc           \
    Components/SensorComponent.h           \
    Components/SensorEvent.h                \
    Components/HaliEvent.h

EXTRA_DIST = \
    hyades.h                                      \
    tests/pingpong.c                              \
    tests/testCarcosaPingPong.py                  \
    VLA-Example/tests/vla.c                       \
    VLA-Example/tests/vla_shared.h                \
    VLA-Example/tests/testCarcosaVLA.py           \
    VLA-Example/tests/testCarcosaVLA_earlyexit.py \
    VLA-Example/tests/vla_cpu.c                   \
    VLA-Example/tests/vla_gpu.c                   \
    VLA-Example/tests/stub_cpu.c                  \
    VLA-Example/tests/stub_gpu.c                  \
    VLA-Example/tests/testCarcosaVLA_GPUCPU.py    \
    VLA-Example/tests/testCarcosaVLA_GPUCPU_Synth.py \
    VLA-Example/scripts/extract_baselines.py      \
    VLA-Example/scripts/run_phase2_from_phase1.sh \
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
    tests/testDynamicPM.py                        \
    tests/testHaliBacking.py                      \
    tests/testHaliMemH.py                         \
    tests/testHaliPM.py                           \
    tests/testManagerLogic.py

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
    Components/PMDataRegistry.h             \
    Components/FaultInjManagerAPI.h        \
    Components/InterceptionAgentAPI.h      \
    Components/PingPongAgent.h              \
    VLA-Example/Components/vla-fsm.h        \
    VLA-Example/Components/VLAAgent.h       \
    VLA-Example/Components/VLAKernelComplexity.h \
    VLA-Example/Components/VLACpuAgent.h    \
    VLA-Example/Components/VLAGpuAgent.h    \
    VLA-Example/Components/VLACpuDelayAgent.h \
    VLA-Example/Components/VLAGpuDelayAgent.h

libcarcosa_la_LDFLAGS = -module -avoid-version
libcarcosa_la_LIBADD =

AM_CPPFLAGS += $(HMC_FLAG)
install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE     carcosa=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS      carcosa=$(abs_srcdir)/tests
