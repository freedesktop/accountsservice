AC_INIT([AccountsService],[0.6.50])
AM_INIT_AUTOMAKE([1.11.2 no-dist-gzip dist-xz tar-ustar foreign])

# Support silent build rules, requires at least automake-1.11. Enable
# by either passing --enable-silent-rules to configure or passing V=0
# to make
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
PKG_PROG_PKG_CONFIG

# i18n
AX_REQUIRE_DEFINED([AM_GNU_GETTEXT_VERSION])
AM_GNU_GETTEXT_VERSION([0.19.8])

AX_REQUIRE_DEFINED([AM_GNU_GETTEXT])
AM_GNU_GETTEXT([external])

GETTEXT_PACKAGE=accountsservice
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE",
                   [the gettext translation domain])

LT_INIT
LT_CURRENT=0
LT_REVISION=0
LT_AGE=0
AC_SUBST(LT_CURRENT)
AC_SUBST(LT_REVISION)
AC_SUBST(LT_AGE)

PKG_CHECK_MODULES(LIBACCOUNTSSERVICE, glib-2.0 >= 2.44 gio-2.0 >= 2.37.3 gio-unix-2.0)
PKG_CHECK_MODULES(ACCOUNTS_DAEMON, glib-2.0 >= 2.44 gio-unix-2.0 polkit-gobject-1)

AM_MAINTAINER_MODE([enable])

GOBJECT_INTROSPECTION_CHECK([0.9.12])

dnl ---------------------------------------------------------------------------
dnl - Core configuration
dnl ---------------------------------------------------------------------------

AC_ARG_ENABLE(admin-group,
        [AS_HELP_STRING([--enable-admin-group],[Set group for administrative accounts @<:@default=auto@:>@])],
        ,enable_admin_group=auto)
AS_IF([test x$enable_admin_group = xauto], [
  AC_CHECK_FILE(/etc/redhat-release, enable_admin_group=wheel)
  AC_CHECK_FILE(/etc/debian_version, enable_admin_group=sudo)
  AS_IF([test x$enable_admin_group = xauto], [
    enable_admin_group=wheel
  ])
])
AC_DEFINE_UNQUOTED([ADMIN_GROUP], ["$enable_admin_group"], [Define to the group for administrator users])

AC_ARG_WITH(extra-admin-groups,
        [AS_HELP_STRING([--with-extra-admin-groups],[Comma-separated list of extra groups that administrator users are part of])],
        ,with_extra_admin_groups="")
AC_DEFINE_UNQUOTED([EXTRA_ADMIN_GROUPS], ["$with_extra_admin_groups"], [Define to the list of extra groups administrator users are part of])

AC_ARG_ENABLE(user-heuristics,
        [AS_HELP_STRING([--enable-user-heuristics],[Enable heuristics for guessing system vs. human users in the range 500-minimum-uid])],
        [if test "$enableval" = yes; then
           AC_DEFINE([ENABLE_USER_HEURISTICS], , [System vs. human user heuristics enabled])
        fi])

AC_ARG_WITH(minimum-uid,
        [AS_HELP_STRING([--with-minimum-uid],[Set minimum uid for human users])],
        ,with_minimum_uid=1000)

AC_DEFINE_UNQUOTED([MINIMUM_UID], $with_minimum_uid, [Define to the minumum UID of human users])

dnl ---------------------------------------------------------------------------
dnl - coverage
dnl ---------------------------------------------------------------------------

AC_MSG_CHECKING([whether to build with gcov testing])
AC_ARG_ENABLE([coverage],
              AS_HELP_STRING([--enable-coverage],
                             [Whether to enable gcov code coverage]),
              [], [enable_coverage=no])
AC_MSG_RESULT([$enable_coverage])

if test "$enable_coverage" = "yes"; then
	if test "$GCC" != "yes"; then
		AC_MSG_ERROR(Coverage testing requires GCC)
	fi
	CFLAGS="$CFLAGS -O0 -g --coverage"
fi

AM_CONDITIONAL([WITH_COVERAGE], [test "$enable_coverage" = "yes"])

dnl ---------------------------------------------------------------------------
dnl - Warnings
dnl ---------------------------------------------------------------------------

AC_ARG_ENABLE(more-warnings,
              AS_HELP_STRING([--enable-more-warnings],
                             [Maximum compiler warnings]),
              set_more_warnings="$enableval",[
              if test -d $srcdir/.git; then
                set_more_warnings=yes
              else
                set_more_warnings=no
              fi])

AC_MSG_CHECKING(for more warnings)
if test "$GCC" = "yes" -a "$set_more_warnings" != "no"; then
  AC_MSG_RESULT(yes)
  MAYBE_WARN="\
   -Wall -Wextra  \
   -Wchar-subscripts -Wmissing-declarations -Wmissing-prototypes \
   -Wwrite-strings -Wnested-externs -Wpointer-arith \
   -Wswitch-enum -Wstrict-aliasing=2 -Winit-self -Wunsafe-loop-optimizations \
   -Wno-missing-field-initializers -Wno-unused-parameter \
   -Wcast-align -Wsign-compare"
elif test "$GCC" = "yes"; then
  AC_MSG_RESULT(no)
  MAYBE_WARN="-Wall -Wno-sign-compare -Wno-deprecated-declarations"
else
  AC_MSG_RESULT(no)
fi

# invalidate cached value if MAYBE_WARN has changed
if test "x$accountsservice_cv_warn_maybe" != "x$MAYBE_WARN"; then
  unset accountsservice_cv_warn_cflags
fi

AC_DEFUN([ACCOUNTSSERVICE_CC_TRY_FLAG], [
  AC_MSG_CHECKING([whether $CC supports $1])

  accountsservice_save_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS $1"

  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
                    [accountsservice_cc_flag=yes],
                    [accountsservice_cc_flag=no])
  CFLAGS="$accountsservice_save_CFLAGS"

  if test "x$accountsservice_cc_flag" = "xyes"; then
    ifelse([$2], , :, [$2])
  else
    ifelse([$3], , :, [$3])
  fi
  AC_MSG_RESULT([$accountsservice_cc_flag])
])

AC_CACHE_CHECK([for supported warning flags], accountsservice_cv_warn_cflags, [
  echo
  WARN_CFLAGS=""

  # Some warning options are not supported by all versions of
  # gcc, so test all desired options against the current
  # compiler.
  #
  # Note that there are some order dependencies
  # here. Specifically, an option that disables a warning will
  # have no net effect if a later option then enables that
  # warnings, (perhaps implicitly). So we put some grouped
  # options (-Wall and -Wextra) up front and the -Wno options
  # last.

  for W in $MAYBE_WARN; do
    ACCOUNTSSERVICE_CC_TRY_FLAG([$W], [WARN_CFLAGS="$WARN_CFLAGS $W"])
  done

  accountsservice_cv_warn_cflags=$WARN_CFLAGS
  accountsservice_cv_warn_maybe=$MAYBE_WARN

  AC_MSG_CHECKING([which warning flags were supported])
])

WARN_CFLAGS="$accountsservice_cv_warn_cflags"

if test "$GCC" = "yes" -a "$set_more_warnings" != "no"; then
  # Only add this when optimizing is enabled (default)
  AC_MSG_CHECKING([whether optimization is enabled])
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#if __OPTIMIZE__ == 0
                                       #error No optimization
                                       #endif
                                     ]], [[]])],
                    [has_optimization=yes],
                    [has_optimization=no])
  if test $has_optimization = yes; then
    WARN_CFLAGS="$WARN_CFLAGS -Wp,-D_FORTIFY_SOURCE=2"
  fi
  AC_MSG_RESULT($has_optimization)
fi

AC_SUBST(WARN_CFLAGS)

AC_CHECK_HEADERS([paths.h shadow.h utmpx.h])

AC_CHECK_FUNCS([fgetpwent setutxdb])

wtmpx_found="no"
AC_CHECK_DECLS([WTMPX_FILENAME], [wtmpx_found="WTMPX_FILENAME"],
               [], [AC_INCLUDES_DEFAULT
#include <utmpx.h>
               ])
if test "$wtmpx_found" = "no" ; then
  AC_CHECK_DECLS([_PATH_WTMPX], [wtmpx_found="_PATH_WTMPX"],
                 [], [AC_INCLUDES_DEFAULT
#ifdef HAVE_PATHS_H
#include <paths.h>
#endif
#ifdef HAVE_UTMPX_H
#include <utmpx.h>
#endif
                 ])
  if test "$wtmpx_found" = "no" ; then
    AC_CHECK_FILE([/var/log/utx.log], [wtmpx_found='"/var/log/utx.log"'])
    if test "$wtmpx_found" = "no" ; then
      AC_MSG_FAILURE([Do not know which filename to watch for wtmp changes])
    fi
  fi
fi
AC_DEFINE_UNQUOTED([PATH_WTMP], [$wtmpx_found], [Path to wtmpx file])

AC_CHECK_LIB(c, getusershell, have_getusershell=yes, have_getusershell=no)
if test x$have_getusershell = xyes; then
  AC_DEFINE(HAVE_GETUSERSHELL, 1, [Define if getusershell() is available])
fi

dnl ---------------------------------------------------------------------------
dnl - gtk-doc Documentation
dnl ---------------------------------------------------------------------------

m4_ifdef([GTK_DOC_CHECK], [
GTK_DOC_CHECK([1.15], [--flavour no-tmpl])
],[
AM_CONDITIONAL([ENABLE_GTK_DOC],[false])
])

dnl ---------------------------------------------------------------------------
dnl - DocBook Documentation
dnl ---------------------------------------------------------------------------

AC_ARG_ENABLE(docbook-docs,
        [AS_HELP_STRING([--enable-docbook-docs],[build documentation (requires xmlto)])],
        enable_docbook_docs=$enableval,enable_docbook_docs=no)
AC_PATH_PROG(XMLTO, xmlto, no)
AC_MSG_CHECKING([whether to build DocBook documentation])
if test x$XMLTO = xno ; then
        have_docbook=no
else
        have_docbook=yes
fi
if test x$enable_docbook_docs = xauto ; then
        if test x$have_docbook = xno ; then
                enable_docbook_docs=no
        else
                enable_docbook_docs=yes
        fi
fi
if test x$enable_docbook_docs = xyes; then
        if test x$have_docbook = xno; then
                AC_MSG_ERROR([Building DocBook docs explicitly required, but DocBook not found])
        fi
fi
AM_CONDITIONAL(DOCBOOK_DOCS_ENABLED, test x$enable_docbook_docs = xyes)
AC_MSG_RESULT(yes)
AC_ARG_VAR([XMLTO],[Define/override the 'xmlto' location.])
AC_ARG_VAR([XMLTO_FLAGS],[Define/override 'xmlto' options, like '--skip-validation'.])

dnl ---------------------------------------------------------------------------
dnl Check for xsltproc
dnl ---------------------------------------------------------------------------
AC_PATH_PROG([XSLTPROC], [xsltproc])

# systemd or elogind

have_systemd=no

AC_MSG_CHECKING([whether to use systemd])

AC_ARG_ENABLE([systemd],
              AS_HELP_STRING([--enable-systemd], [Use systemd]),
              [enable_systemd=$enableval],
              [enable_systemd=auto])

AC_MSG_RESULT($enable_systemd)

if test x$enable_systemd != xno; then
        PKG_CHECK_MODULES(SYSTEMD, [libsystemd >= 186],
                                   [have_systemd=yes])
fi

if test x$enable_systemd = xauto ; then
        if test x$have_systemd = xno ; then
                enable_systemd=no
        else
                enable_systemd=yes
        fi
fi

have_elogind=no

AC_MSG_CHECKING([whether to use elogind])

AC_ARG_ENABLE([elogind],
              AS_HELP_STRING([--enable-elogind], [Use elogind]),
              [enable_elogind=$enableval],
              [enable_elogind=no])

AC_MSG_RESULT($enable_elogind)

if test x$enable_systemd = xyes; then
        if test x$have_systemd = xno; then
                AC_MSG_ERROR([Systemd support explicitly requested, but libsystemd not found])
        fi

        dnl Requesting to use the elogind session tracker makes no sense where
        dnl systemd-login is (or might be) running.
        if test x$enable_elogind = xyes; then
                AC_MSG_WARN([elogind support requested, but systemd was found.])
                AC_MSG_ERROR([Disable either elogind or systemd support.])
        fi

        AC_SUBST(SYSTEMD_CFLAGS)
        AC_SUBST(SYSTEMD_LIBS)

        LIBACCOUNTSSERVICE_LIBS="$LIBACCOUNTSSERVICE_LIBS $SYSTEMD_LIBS"
        LIBACCOUNTSSERVICE_CFLAGS="$LIBACCOUNTSSERVICE_CFLAGS $SYSTEMD_CFLAGS"

        AC_DEFINE(HAVE_SYSTEMD, 1, [Define if systemd is used])
fi

if test "x$enable_elogind" != "xno"; then
        PKG_CHECK_MODULES(ELOGIND, [libelogind >= 229.4],
                                   [have_elogind=yes])

        if test "x$have_elogind" != "xyes"; then
                AC_MSG_ERROR([elogind support requested, but libelogind not found])
        else
                AC_SUBST(ELOGIND_CFLAGS)
                AC_SUBST(ELOGIND_LIBS)

                LIBACCOUNTSSERVICE_LIBS="$LIBACCOUNTSSERVICE_LIBS $ELOGIND_LIBS"
                LIBACCOUNTSSERVICE_CFLAGS="$LIBACCOUNTSSERVICE_CFLAGS $ELOGIND_CFLAGS"
        fi
fi

if test "x${have_systemd}" != "xno" -o "x${have_elogind}" != "xno" ; then
        AC_DEFINE(WITH_SYSTEMD, 1, [Define to enable systemd support])
fi

AC_ARG_WITH([systemdsystemunitdir],
        AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files]),
        [],
        [with_systemdsystemunitdir=$have_systemd])
if test "x$with_systemdsystemunitdir" = "xyes"; then
    with_systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)
fi
if test "x$with_systemdsystemunitdir" != "xno"; then
    AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])
fi
AM_CONDITIONAL(HAVE_SYSTEMD, [test -n "$with_systemdsystemunitdir" -a "x$with_systemdsystemunitdir" != "xno" ])

AC_ARG_WITH(gdmconffile,
            AS_HELP_STRING([--with-gdmconffile=FILE], [GDM configuration file, default=/etc/gdm/custom.conf]),
            gdmconffile=${withval}, gdmconffile=/etc/gdm/custom.conf)
AC_DEFINE_UNQUOTED([PATH_GDM_CUSTOM], ["$gdmconffile"], [GDM configuration file])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
Makefile
po/Makefile.in
data/Makefile
src/Makefile
src/libaccountsservice/Makefile
src/libaccountsservice/accountsservice.pc
doc/Makefile
doc/dbus/Makefile
doc/dbus/AccountsService.xml
doc/libaccountsservice/Makefile
])
AC_OUTPUT

echo
AC_MSG_NOTICE([accountsservice was configured with the following options:])
if test "x$enable_docbook_docs" = "xyes"; then
        AC_MSG_NOTICE([** DocBook documentation build enabled])
else
        AC_MSG_NOTICE([** DocBook documentation build disabled])
fi
        AC_MSG_NOTICE([** Administrator group: $enable_admin_group])
        AC_MSG_NOTICE([** Extra administrator groups: $with_extra_admin_groups])
        AC_MSG_NOTICE([** GDM configuration: $gdmconffile])
echo


