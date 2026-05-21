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
    -Wl,-no-as-needed \
    $(AM_LDFLAGS)

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
