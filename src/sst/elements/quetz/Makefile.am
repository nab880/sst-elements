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
    quetzcpu.h  \
    quetzcore.cc \
    quetzcore.h  \
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
#
# Compiled as a plain shared library (not a libtool module) so that QEMU
# can dlopen() it.  The plugin uses only SST's header-only interprocess
# templates — no link-time dependency on libsst.
#
# qemu-plugin.h is NOT shipped in-tree; it is found from the QEMU
# installation via QEMU_PLUGIN_H_DIR (set by configure.m4).
# ---------------------------------------------------------------------------
plugindir = $(libexecdir)
plugin_LTLIBRARIES = libqemu_sst_plugin.la

libqemu_sst_plugin_la_SOURCES = \
    qemu_plugin/qemu_sst_plugin.cpp \
    quetz_shmem.h

libqemu_sst_plugin_la_LDFLAGS = \
    -module \
    -avoid-version \
    -shared \
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
    quetz_shmem.h

# ---------------------------------------------------------------------------
# Post-install hook: register with sst-register
# ---------------------------------------------------------------------------
install-exec-hook:
	$(SST_REGISTER_TOOL) SST_ELEMENT_SOURCE quetz=$(abs_srcdir)
	$(SST_REGISTER_TOOL) SST_ELEMENT_TESTS  quetz=$(abs_srcdir)/tests
