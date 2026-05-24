dnl -*- Autoconf -*-
dnl Configure macros for the SST Quetz element library.

AC_DEFUN([SST_quetz_CONFIG], [
    sst_check_quetz="yes"

    dnl -----------------------------------------------------------------------
    dnl --with-qemu-prefix: root of a QEMU installation.
    dnl
    dnl Used to locate two things:
    dnl   1. qemu-plugin.h  (compile-time; at $prefix/include/qemu-plugin.h)
    dnl   2. qemu-riscv64   (runtime;      at $prefix/bin/qemu-riscv64)
    dnl
    dnl If not specified we search the standard PATH and system include dirs.
    dnl -----------------------------------------------------------------------
    AC_ARG_WITH([qemu-prefix],
        [AS_HELP_STRING([--with-qemu-prefix=PATH],
            [Root of a QEMU installation.  Quetz looks for
             \$PATH/include/qemu-plugin.h (required at build time) and
             \$PATH/bin/qemu-riscv64 (required at run time).])],
        [QEMU_PREFIX="$withval"],
        [QEMU_PREFIX=""])

    dnl -----------------------------------------------------------------------
    dnl Locate qemu-plugin.h
    dnl -----------------------------------------------------------------------
    QEMU_PLUGIN_H_DIR=""

    AS_IF([test -n "$QEMU_PREFIX"],
    [
        dnl User supplied a prefix — check there first.
        AS_IF([test -f "$QEMU_PREFIX/include/qemu-plugin.h"],
            [QEMU_PLUGIN_H_DIR="$QEMU_PREFIX/include"],
            [AC_MSG_ERROR([--with-qemu-prefix=$QEMU_PREFIX given but $QEMU_PREFIX/include/qemu-plugin.h not found.])])
    ],
    [
        dnl No prefix: search common install locations.
        for _dir in /usr/include /usr/local/include; do
            AS_IF([test -f "$_dir/qemu-plugin.h"],
                [QEMU_PLUGIN_H_DIR="$_dir"; break])
        done
    ])

    AC_MSG_CHECKING([for qemu-plugin.h])
    AS_IF([test -n "$QEMU_PLUGIN_H_DIR"],
        [AC_MSG_RESULT([$QEMU_PLUGIN_H_DIR/qemu-plugin.h])],
        [AC_MSG_RESULT([not found])
         AC_MSG_ERROR([qemu-plugin.h not found.  Install QEMU development headers or use --with-qemu-prefix=<qemu-install-root>.])])

    AC_SUBST([QEMU_PLUGIN_H_DIR])

    dnl -----------------------------------------------------------------------
    dnl Find glib-2.0 include path (required by qemu-plugin.h in QEMU 9.1+)
    dnl -----------------------------------------------------------------------
    AC_MSG_CHECKING([for glib-2.0 CFLAGS (needed by qemu-plugin.h)])
    GLIB_CFLAGS=""
    AS_IF([pkg-config --exists glib-2.0 2>/dev/null],
        [GLIB_CFLAGS=`pkg-config --cflags glib-2.0`
         AC_MSG_RESULT([$GLIB_CFLAGS])],
        [AC_MSG_RESULT([pkg-config not found; trying /usr/include/glib-2.0])
         AS_IF([test -f "/usr/include/glib-2.0/glib.h"],
             [GLIB_CFLAGS="-I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include"],
             [AC_MSG_WARN([glib-2.0 headers not found; plugin may fail to compile against QEMU 9.1+ headers])])])
    AC_SUBST([GLIB_CFLAGS])

    dnl -----------------------------------------------------------------------
    dnl Locate qemu-riscv64 user-mode binary (runtime only)
    dnl -----------------------------------------------------------------------
    AS_IF([test -n "$QEMU_PREFIX"],
        [QEMU_BIN="$QEMU_PREFIX/bin/qemu-riscv64"],
        [AC_PATH_PROG([QEMU_BIN], [qemu-riscv64], [not_found])])

    AC_MSG_CHECKING([for QEMU user-mode binary])
    AS_IF([test "x$QEMU_BIN" = "xnot_found" || test -z "$QEMU_BIN"],
        [AC_MSG_RESULT([not found (simulations will need qemu-riscv64 at runtime)])],
        [AC_MSG_RESULT([$QEMU_BIN])])

    AC_SUBST([QEMU_BIN])

    dnl Linux ld only; Darwin rejects -Wl,-no-as-needed.
    AS_CASE([$host_os],
        [darwin*], [QUETZ_PLUGIN_LD_EXTRA=],
        [QUETZ_PLUGIN_LD_EXTRA=-Wl,-no-as-needed])
    AC_SUBST([QUETZ_PLUGIN_LD_EXTRA])

    dnl -----------------------------------------------------------------------
    dnl Plugin install directory (used in quetzcpu.cc to auto-resolve the
    dnl plugin path when qemu_plugin= is not specified).
    dnl
    dnl The QEMU_PLUGIN_INSTALL_DIR preprocessor symbol is defined at compile
    dnl time by Makefile.am as -DQEMU_PLUGIN_INSTALL_DIR=\"$(libexecdir)\"; we
    dnl do NOT AC_DEFINE it here because $(libexecdir) is a Makefile variable
    dnl (not a configure-time shell variable), so its expansion only happens
    dnl correctly during make.
    dnl -----------------------------------------------------------------------

    AS_IF([test "$sst_check_quetz" = "yes"], [$1], [$2])
])
