
dnl ----------------------
dnl Initialization macros
dnl ----------------------

AC_INIT(htp/htp.h)
AC_CONFIG_HEADERS([config.h])


dnl -----------------------------------------------
dnl Package name and version number (user defined)
dnl -----------------------------------------------

GENERIC_LIBRARY_NAME=htp

GENERIC_MAJOR_VERSION=0
GENERIC_MINOR_VERSION=5
GENERIC_MICRO_VERSION=X

# API version (often = GENERIC_MAJOR_VERSION.GENERIC_MINOR_VERSION)
GENERIC_API_VERSION=1.0
AC_SUBST(GENERIC_API_VERSION)

# Shared library versioning
GENERIC_LIBRARY_VERSION=1:2:0
#                       | | |
#                +------+ | +---+
#                |        |     |
#             current:revision:age
#                |        |     |
#                |        |     +- increment if interfaces have been added
#                |        |        set to zero if interfaces have been removed
#                                  or changed
#                |        +- increment if source code has changed
#                |           set to zero if current is incremented
#                +- increment if interfaces have been added, removed or changed


dnl --------------------------------
dnl Package name and version number
dnl --------------------------------

AC_SUBST(GENERIC_LIBRARY_VERSION)

PACKAGE=$GENERIC_LIBRARY_NAME
AC_SUBST(GENERIC_LIBRARY_NAME)

GENERIC_VERSION=$GENERIC_MAJOR_VERSION.$GENERIC_MINOR_VERSION.$GENERIC_MICRO_VERSION
GENERIC_RELEASE=$GENERIC_MAJOR_VERSION.$GENERIC_MINOR_VERSION
AC_SUBST(GENERIC_RELEASE)
AC_SUBST(GENERIC_VERSION)

VERSION=$GENERIC_VERSION

AM_INIT_AUTOMAKE($PACKAGE, $VERSION, no-define)

AC_CONFIG_MACRO_DIR([m4])

AC_ARG_ENABLE(debug, [  --enable-debug  Enable debug mode], [ enable_debug=yes ])
if test "$enable_debug" = "yes"; then
    CFLAGS="${CFLAGS} -DHTP_DEBUG"
    echo "Debug mode enabled"
fi

OLEVEL=2

AC_ARG_ENABLE(devmode, [  --enable-devmode  Enable development mode], [ enable_devmode=yes ])
if test "$enable_devmode" = "yes"; then
    OLEVEL=0
    CFLAGS="${CFLAGS} -Werror -Wfatal-errors"
    CPPFLAGS="${CPPFLAGS} -Werror -Wfatal-errors"
    echo "Development mode enabled"
fi

AC_ARG_ENABLE(gcov, [  --enable-gcov  Enable gcov support], [ enable_gcov=yes ])
if test "$enable_gcov" = "yes"; then
    OLEVEL=0
    CFLAGS="${CFLAGS} --coverage -fprofile-arcs -ftest-coverage"
    CPPFLAGS="${CPPFLAGS} --coverage -fprofile-arcs -ftest-coverage"
    LDFLAGS="${LDFLAGS} -lgcov --coverage -fprofile-arcs"
    echo "gcov support enabled"
fi

CFLAGS="${CFLAGS} -O${OLEVEL}"
CPPFLAGS="${CPPFLAGS} -O${OLEVEL}"

dnl -----------------------------------------------
dnl Checks for programs.
dnl -----------------------------------------------

AC_PROG_CC
AC_PROG_CXX
AM_PROG_LIBTOOL
AM_SANITY_CHECK

dnl -----------------------------------------------
dnl Checks for libs.
dnl -----------------------------------------------
AC_CHECK_HEADER(zlib.h,,[AC_ERROR(zlib.h not found ...)])
ZLIB=""
AC_CHECK_LIB(z, inflate,, ZLIB="no")
if test "$ZLIB" = "no"; then
    echo
    echo "   ERROR! zlib library not found"
    echo
    exit 1
fi

# Determine the OS 
AC_MSG_CHECKING([OS])
OS=`uname -s`
case "$OS" in
        CYGWIN*)
                AC_MSG_RESULT(Cygwin)
                OS_CYGWIN="true"
                NO_STACK_PROTECTOR="true"
        ;;
        FreeBSD*)
                AC_MSG_RESULT(FreeBSD)
                OS_FREEBSD="true"
                NO_STACK_PROTECTOR="true"
                CPPFLAGS="${CPPFLAGS} -I/usr/local/include"
                LDFLAGS="${LDFLAGS} -L/usr/local/lib"
        ;;
        Linux*)
                AC_MSG_RESULT(Linux)
                OS_LINUX="true"
        ;;
        *)
                AC_MSG_RESULT(no)
        ;;
esac
#We need to call the iconv macro after OS detection for FreeBSD to work properly
sinclude(m4/iconv.m4)
sinclude(m4/lib-ld.m4)
sinclude(m4/lib-link.m4)
sinclude(m4/lib-prefix.m4)
AM_ICONV

dnl -----------------------------------------------
dnl Check and enable the GCC opts we want to use.
dnl We may need to add more checks
dnl -----------------------------------------------

dnl -----------------------------------------------
dnl Check for GCC signed overflow warning support
dnl -----------------------------------------------
AC_MSG_CHECKING(for gcc support of -Wstrict-overflow=1)
TMPCFLAGS="${CFLAGS}"
CFLAGS="${CFLAGS} -Wstrict-overflow=1"
AC_TRY_COMPILE(,,[gcc_have_strict_overflow=yes],[gcc_have_strict_overflow=no])
AC_MSG_RESULT($gcc_have_strict_overflow)
if test "$gcc_have_strict_overflow" != "yes"; then
    CFLAGS="${TMPCFLAGS}"
fi

if test "$NO_STACK_PROTECTOR" != "true"; then
dnl -----------------------------------------------
dnl Check for GCC stack smashing protection
dnl -----------------------------------------------
AC_MSG_CHECKING(for gcc support of stack smashing protection)
TMPCFLAGS="${CFLAGS}"
CFLAGS="${CFLAGS} -fstack-protector"
AC_TRY_COMPILE(,,[gcc_have_fstack_protector=yes],[gcc_have_fstack_protector=no])
AC_MSG_RESULT($gcc_have_fstack_protector)
if test "$gcc_have_fstack_protector" != "yes"; then
    CFLAGS="${TMPCFLAGS}"  
fi
fi

dnl -----------------------------------------------
dnl Check for GCC -D_FORTIFY_SOURCE support
dnl -----------------------------------------------
AC_MSG_CHECKING(for gcc support of FORTIFY_SOURCE)
TMPCFLAGS="${CFLAGS}"
CFLAGS="${CFLAGS} -D_FORTIFY_SOURCE=2"
AC_TRY_COMPILE(,,[gcc_have_fortify_source=yes],[gcc_have_fortify_source=no])
AC_MSG_RESULT($gcc_have_fortify_source)
if test "$gcc_have_fortify_source" != "yes"; then
    CFLAGS="${TMPCFLAGS}"
fi

dnl -----------------------------------------------
dnl Check for GCC -Wformat-security support
dnl -----------------------------------------------
AC_MSG_CHECKING(for gcc support of -Wformat -Wformat-security)
TMPCFLAGS="${CFLAGS}"
CFLAGS="${CFLAGS} -Wformat -Wformat-security"
AC_TRY_COMPILE(,,[gcc_have_format_security=yes],[gcc_have_format_security=no])
AC_MSG_RESULT($gcc_have_format_security)
if test "$gcc_have_format_security" != "yes"; then
    CFLAGS="${TMPCFLAGS}"
fi

dnl -----------------------------------------------
dnl Check for doxygen
dnl -----------------------------------------------
AC_ARG_WITH([doxygen],
            [  --with-doxygen=PROG doxygen executable],
            [doxygen="$withval"],[doxygen=no])

if test "$doxygen" != "no"; then
    AC_MSG_NOTICE([Using doxygen: $doxygen])
else
    AC_PATH_PROGS([doxygen],[doxygen],[])
fi

DOXYGEN=$doxygen
AC_SUBST(DOXYGEN)

dnl -----------------------------------------------
dnl Check for lcov
dnl -----------------------------------------------
AC_PATH_PROG(LCOV, lcov, [no])
AC_SUBST(LCOV)


dnl -----------------------------------------------
dnl Generates Makefiles, configuration files and scripts
dnl -----------------------------------------------

AC_PREFIX_DEFAULT(/usr/local)
AC_OUTPUT(Makefile \
          htp.pc \
          htp/Makefile \
          test/Makefile \
          docs/Makefile
)
