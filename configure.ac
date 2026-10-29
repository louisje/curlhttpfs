AC_INIT([curlhttpfs], [0.1.0], [https://github.com/louisje/curlhttpfs/issues])
AC_CONFIG_SRCDIR([curlhttpfs.c])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_PROG_CC
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([FUSE3], [fuse3])
PKG_CHECK_MODULES([LIBCURL], [libcurl])
PKG_CHECK_MODULES([OPENSSL], [openssl])
AC_CHECK_HEADERS([sys/file.h fcntl.h pthread.h], [], [AC_MSG_ERROR([Required POSIX headers are missing])])
AC_CHECK_FUNCS([flock], [], [AC_MSG_ERROR([flock is required for cross-process cache safety])])
AC_SEARCH_LIBS([pthread_create], [pthread], [], [AC_MSG_ERROR([POSIX threads are required])])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
