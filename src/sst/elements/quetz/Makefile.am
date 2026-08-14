## Makefile.am for the SST Quetz element library
##
## Two build products:
##   libquetz.la           — the SST element library
##   libqemu_sst_plugin.la — the QEMU TCG plugin (dlopen'd by QEMU at runtime)
##
## QEMU_PLUGIN_H_DIR is set by configure.m4 to the directory containing
## qemu-plugin.h (e.g. /usr/include or $QEMU_PREFIX/include).

AM_CPPFLAGS += \
    -I$(top_srcdir)/src \
    -DQEMU_PLUGIN_INSTALL_DIR=\"$(libexecdir)\"

# ---------------------------------------------------------------------------
# SST element library
# ---------------------------------------------------------------------------
compdir = $(pkglibdir)
comp_LTLIBRARIES = libquetz.la

libquetz_la_SOURCES = \
    quetzcpu.cc \
    quetzcpu.h \
    quetzcore.cc \
    quetzcore.h \
    quetz_config.cc \
    quetz_config.h \
    quetz_config_manager.cc \
    quetz_config_manager.h \
    quetz_platform_profiles.cc \
    quetz_core_backend.cc \
    quetz_core_backend.h \
    quetzcpu_mmio_sync.cc \
    quetz_mmio_sync.h \
    quetz_accelerator_port.h \
    quetz_balar_accelerator_port.cc \
    quetz_balar_accelerator_port.h \
    quetz_balar_flush_range.h \
    quetz_frontend.h \
    quetz_qemu_frontend.cc \
    quetz_qemu_frontend.h \
    quetz_launcher.cc \
    quetz_launcher.h \
    quetz_region_handler.h \
    quetz_region_table.cc \
    quetz_region_table.h \
    quetz_region_handlers.cc \
    quetz_region_handlers.h \
    quetz_gpu_device.cc \
    quetz_gpu_device.h \
    quetz_kernel_api.h \
    quetz_fft_kernel.cc \
    quetz_fft_kernel.h \
    quetz_scale_offset.h \
    quetz_scale_offset_kernel.cc \
    quetz_scale_offset_kernel.h \
    quetz_stream_device.cc \
    quetz_stream_device.h \
    quetz_sink_device.cc \
    quetz_sink_device.h \
    quetz_irq_event.h \
    quetz_mem_access.cc \
    quetz_mem_access.h \
    quetz_mem_issue.cc \
    quetz_mem_issue.h \
    quetz_pipeline.cc \
    quetz_pipeline.h \
    quetz_pipeline_api.h \
    quetz_pipeline_input.cc \
    quetz_pipeline_filter.cc \
    quetz_pipeline_transform.cc \
    quetz_pipeline_output.cc \
    quetz_logging_pipeline_output.cc \
    quetz_stats.cc \
    quetz_stats.h \
    quetz_statistics_collector.cc \
    quetz_statistics_collector.h \
    quetz_ipc_types.h \
    quetz_command_buffer.h \
    quetz_sync_manager.h \
    quetz_shmem.h

libquetz_la_LDFLAGS = \
    -module \
    -avoid-version \
    $(AM_LDFLAGS)

libquetz_la_CPPFLAGS = \
    $(AM_CPPFLAGS) \
    $(CPPFLAGS)

# ---------------------------------------------------------------------------
# QEMU TCG plugin shared library
# ---------------------------------------------------------------------------
plugindir = $(libexecdir)
plugin_LTLIBRARIES = libqemu_sst_plugin.la

libqemu_sst_plugin_la_SOURCES = \
    qemu_plugin/plugin_main.cpp \
    qemu_plugin/plugin_state.cpp \
    qemu_plugin/plugin_state.h \
    qemu_plugin/instrument.cpp \
    qemu_plugin/instrument.h \
    qemu_plugin/insn_classifier.cpp \
    qemu_plugin/insn_classifier.h \
    qemu_plugin/mem_access_handler.cpp \
    qemu_plugin/mem_access_handler.h \
    qemu_plugin/registry.h \
    qemu_plugin/decoder_riscv.cpp \
    qemu_plugin/decoder_aarch64.cpp \
    qemu_plugin/decoder_generic.cpp \
    qemu_plugin/decoder_riscv.h \
    qemu_plugin/decoder_aarch64.h \
    qemu_plugin/decoder_generic.h \
    quetz_ipc_types.h \
    quetz_command_buffer.h \
    quetz_sync_manager.h \
    quetz_statistics_collector.h \
    quetz_shmem.h

libqemu_sst_plugin_la_LDFLAGS = \
    -module \
    -avoid-version \
    -shared \
    $(AM_LDFLAGS)
libqemu_sst_plugin_la_LDFLAGS += @QUETZ_PLUGIN_LD_EXTRA@

libqemu_sst_plugin_la_CPPFLAGS = \
    -fPIC \
    -I$(top_srcdir)/src \
    -I$(QEMU_PLUGIN_H_DIR) \
    $(GLIB_CFLAGS) \
    $(AM_CPPFLAGS) \
    $(CPPFLAGS)

# ---------------------------------------------------------------------------
# Exported headers
# ---------------------------------------------------------------------------
sstdir = $(includedir)/sst/elements/quetz
nobase_sst_HEADERS = \
    quetz_ipc_types.h \
    quetz_command_buffer.h \
    quetz_sync_manager.h \
    quetz_statistics_collector.h \
    quetz_shmem.h

# ---------------------------------------------------------------------------
# Post-install hook: register with sst-register
# ---------------------------------------------------------------------------
install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE quetz=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS  quetz=$(abs_srcdir)/tests

# ---------------------------------------------------------------------------
# Quetz unit / integration test targets (no install)
# ---------------------------------------------------------------------------
.PHONY: check-quetz-unit check-quetz-integration

check-quetz-unit:
	@SST_HOME="$${SST_HOME:-/opt/sst}" $(abs_srcdir)/tests/unit/run_unit_tests.sh

check-quetz-integration:
	@"$${SST_HOME:-/opt/sst}/bin/sst-test-elements" -p $(abs_srcdir)/tests/testsuite_default_quetz.py
	@"$${SST_HOME:-/opt/sst}/bin/sst-test-elements" -p $(abs_srcdir)/tests/expanded_coldfire_tests.py

# ---------------------------------------------------------------------------
# Distribution: test suite, SDL decks, firmware, gold files, and docs, so a
# make-dist tarball can run the quetz tests (mirrors the balar convention).
# ---------------------------------------------------------------------------
EXTRA_DIST =	GETTING-STARTED.md \
	BSP-COMPATIBILITY.md \
	QUETZ_OUTLINE.md \
	README.md \
	profiles/raptor-init.json \
	qemu-overlay/apply-qemu-overlay.sh \
	qemu-overlay/hw/m68k/raptor.c \
	qemu-overlay/hw/misc/mcf_bsp_compat.c \
	qemu-overlay/hw/misc/mcf_dtimer.c \
	qemu-overlay/hw/misc/mcf_gpio.c \
	qemu-overlay/hw/misc/raptor_bsp_blocks.h \
	qemu-overlay/hw/misc/raptor_dtimer_blocks.h \
	qemu-overlay/hw/misc/raptor_gpio_blocks.h \
	qemu-overlay/hw/misc/sst_mmio_bridge.c \
	qemu-overlay/include/quetz/quetz_ipc_client.h \
	qemu-overlay/include/quetz/quetz_ipc_types.h \
	qemu-overlay/linux-user/sst_mmio.c \
	qemu-overlay/linux-user/sst_mmio.h \
	qemu-overlay/quetz_ipc_client.c \
	SIMULATING-YOUR-SYSTEM.md \
	TESTING.md \
	tests/.gitignore \
	tests/binaries/build_microbench.sh \
	tests/binaries/false_share \
	tests/binaries/gpu_kernel_user \
	tests/binaries/gpu_trace_user \
	tests/binaries/hello_aarch64 \
	tests/binaries/hello_multiisa.c \
	tests/binaries/hello_x86_64 \
	tests/binaries/m68k_balar_async_user \
	tests/binaries/m68k_balar_user \
	tests/binaries/m68k_mmio_roundtrip \
	tests/binaries/pointer_chase \
	tests/binaries/rv64_balar_user \
	tests/binaries/rv64_mmio_roundtrip \
	tests/binaries/rv64_mmio_roundtrip_rvc \
	tests/binaries/rvv_saxpy \
	tests/binaries/rvv_saxpy.c \
	tests/binaries/rvv_vlen_sweep \
	tests/binaries/src/false_share.c \
	tests/binaries/src/pointer_chase.c \
	tests/binaries/src/rvv_vlen_sweep.c \
	tests/binaries/src/stride_read.c \
	tests/binaries/src/write_burst.c \
	tests/binaries/stride_read_1 \
	tests/binaries/stride_read_64 \
	tests/binaries/write_burst \
	tests/manual/validate_against_libmem.py \
	tests/quetz_test_helpers.py \
	tests/sysmode/basic_quetz_balar_coldfire.py \
	tests/sysmode/basic_quetz_balar.py \
	tests/sysmode/basic_quetz_coldfire_system.py \
	tests/sysmode/data/gps_nmea.txt \
	tests/sysmode/data/sensor_stream.bin \
	tests/sysmode/basic_quetz_gpu_coldfire.py \
	tests/sysmode/basic_quetz_gpu_compute_coldfire.py \
	tests/sysmode/basic_quetz_gpu_compute.py \
	tests/sysmode/basic_quetz_gpu.py \
	tests/sysmode/basic_quetz_mmio.py \
	tests/sysmode/basic_quetz_sysmode.py \
	tests/sysmode/firmware/arm_m7_hello \
	tests/sysmode/firmware/bsp_torture \
	tests/sysmode/firmware/bsp_torture.c \
	tests/sysmode/firmware/arm_m7_hello.c \
	tests/sysmode/firmware/arm_m7_startup.S \
	tests/sysmode/firmware/balar_fw_common.h \
	tests/sysmode/firmware/build.sh \
	tests/sysmode/firmware/coldfire_accel_scale \
	tests/sysmode/firmware/coldfire_accel_scale.c \
	tests/sysmode/firmware/coldfire_accel_sink \
	tests/sysmode/firmware/coldfire_accel_sink.c \
	tests/sysmode/firmware/coldfire_balar.h \
	tests/sysmode/firmware/coldfire_gpu_async.c \
	tests/sysmode/firmware/coldfire_gpu_fft \
	tests/sysmode/firmware/coldfire_gpu_fft_offload \
	tests/sysmode/firmware/coldfire_gpu_fft_offload.c \
	tests/sysmode/firmware/coldfire_gpu_fft.c \
	tests/sysmode/firmware/coldfire_devices.h \
	tests/sysmode/firmware/coldfire_fault.h \
	tests/sysmode/firmware/coldfire_gpu.c \
	tests/sysmode/firmware/coldfire_hello.c \
	tests/sysmode/firmware/coldfire_intc.h \
	tests/sysmode/firmware/coldfire_scale_ref.h \
	tests/sysmode/firmware/coldfire_irq_demo \
	tests/sysmode/firmware/coldfire_irq_demo.c \
	tests/sysmode/firmware/coldfire_monitor.c \
	tests/sysmode/firmware/coldfire_startup.S \
	tests/sysmode/firmware/coldfire_system \
	tests/sysmode/firmware/coldfire_system.c \
	tests/sysmode/firmware/coldfire_system_gps1 \
	tests/sysmode/firmware/coldfire_uart.h \
	tests/sysmode/firmware/coldfire_v4_fpu \
	tests/sysmode/firmware/coldfire_v4_fpu.c \
	tests/sysmode/firmware/expanded/build.sh \
	tests/sysmode/firmware/expanded/coldfire_xt_be_alias.c \
	tests/sysmode/firmware/expanded/coldfire_xt_dma_escape.c \
	tests/sysmode/firmware/expanded/coldfire_xt_doorbell_flood.c \
	tests/sysmode/firmware/expanded/coldfire_xt_irq_burst.c \
	tests/sysmode/firmware/expanded/coldfire_xt_irq_zero_latency.c \
	tests/sysmode/firmware/expanded/coldfire_xt_kernel_overflow.c \
	tests/sysmode/firmware/expanded/coldfire_xt_scale_stress.c \
	tests/sysmode/firmware/expanded/coldfire_xt_stream_ack_early.c \
	tests/sysmode/firmware/expanded/coldfire_xt_wild_access.c \
	tests/sysmode/firmware/cuda_runtime_types_firmware.h \
	tests/sysmode/firmware/fft_firmware_data_256.h \
	tests/sysmode/firmware/fft_synth_compute.h \
	tests/sysmode/firmware/link_arm_m7.ld \
	tests/sysmode/firmware/link_m68k.ld \
	tests/sysmode/firmware/link_rv64.ld \
	tests/sysmode/firmware/link_x86.ld \
	tests/sysmode/firmware/riscv_virt_balar_async.c \
	tests/sysmode/firmware/riscv_virt_balar_fft.c \
	tests/sysmode/firmware/riscv_virt_balar_kernel \
	tests/sysmode/firmware/riscv_virt_balar_kernel.c \
	tests/sysmode/firmware/riscv_virt_gpu_async_engine.c \
	tests/sysmode/firmware/riscv_virt_gpu_async_queue.c \
	tests/sysmode/firmware/riscv_virt_gpu_async.c \
	tests/sysmode/firmware/riscv_virt_gpu_fft \
	tests/sysmode/firmware/riscv_virt_gpu_fft_offload \
	tests/sysmode/firmware/riscv_virt_gpu_fft_offload.c \
	tests/sysmode/firmware/riscv_virt_gpu_fft.c \
	tests/sysmode/firmware/riscv_virt_gpu_kernel \
	tests/sysmode/firmware/riscv_virt_gpu_kernel.c \
	tests/sysmode/firmware/riscv_virt_gpu_trace \
	tests/sysmode/firmware/riscv_virt_gpu_trace.c \
	tests/sysmode/firmware/riscv_virt_hello \
	tests/sysmode/firmware/riscv_virt_hello.c \
	tests/sysmode/firmware/riscv_virt_mmio_poke \
	tests/sysmode/firmware/riscv_virt_mmio_poke.c \
	tests/sysmode/firmware/riscv_virt_uart_echo \
	tests/sysmode/firmware/riscv_virt_uart_echo.c \
	tests/sysmode/firmware/x86_hello \
	tests/sysmode/firmware/x86_hello.c \
	tests/sysmode/preset_quetz_sysmode.py \
	tests/sysmode/README-coldfire-demo.md \
	tests/sysmode/run_mmio_test.sh \
	tests/sysmode/template_system.py \
	tests/sysmode/small/arm_m7_hello/sst.stdout.gold \
	tests/sysmode/small/riscv64_virt_gpu_trace/sst.stdout.gold \
	tests/sysmode/small/riscv64_virt_hello/sst.stdout.gold \
	tests/sysmode/small/uart_echo/sst.stdout.gold \
	tests/testsuite_default_quetz.py \
	tests/expanded_coldfire_tests.py \
	tools/check_nmea.py \
	tools/bsp_profile.py \
	tools/gen_bsp_compat_blocks.py \
	tools/dump_stream.py \
	tools/make_stream.py \
	tools/serial_feeder.py \
	tests/unit/doctest.h \
	tests/unit/run_unit_tests.sh \
	tests/unit/sst_stubs.h \
	tests/unit/test_command_buffer.cc \
	tests/unit/test_config_manager.cc \
	tests/unit/test_decoder_aarch64.cc \
	tests/unit/test_decoder_generic.cc \
	tests/unit/test_decoder_riscv.cc \
	tests/unit/test_fft_compute.cc \
	tests/unit/test_scale_offset.cc \
	tests/unit/test_ipc_layout.cc \
	tests/unit/test_mem_issue_split.cc \
	tests/unit/test_balar_flush_range.cc \
	tests/unit/test_bsp_profile.py \
	tests/unit/test_quetz_config.cc \
	tests/unit/test_region_table.cc \
	tests/unit/test_smoke.cc \
	tests/usermode/basic_quetz_balar.py \
	tests/usermode/basic_quetz_gpu_mmio.py \
	tests/usermode/basic_quetz_gpu_trace.py \
	tests/usermode/basic_quetz_gpu.py \
	tests/usermode/basic_quetz.py \
	tests/usermode/custom_pipeline_output.py \
	tests/usermode/small/aarch64_hello/sst.stdout.gold \
	tests/usermode/small/riscv64_hello/sst.stdout.gold \
	tests/usermode/small/riscv64_l1cache/sst.stdout.gold \
	tests/usermode/small/x86_64_hello/sst.stdout.gold \
	tests/usermode/sources/build.sh \
	tests/usermode/sources/gpu_kernel_user.c \
	tests/usermode/sources/gpu_trace_user.c \
	tests/usermode/sources/m68k_balar_async_user.c \
	tests/usermode/sources/m68k_balar_user.c \
	tests/usermode/sources/m68k_mmio_roundtrip.c \
	tests/usermode/sources/rv64_balar_user.c \
	tests/usermode/sources/rv64_mmio_roundtrip.c \
	tests/usermode/test_config_bad.py \
	tests/usermode/test_merlin_2node.py \
	tests/usermode/test_mips_hello.py \
	tests/usermode/test_multicore.py \
	tests/usermode/test_rvv_saxpy.py \
	tests/usermode/test_sqrt_fp.py \
	tests/usermode/test_stream.py \
	tests/usermode/test_wide_split.py
