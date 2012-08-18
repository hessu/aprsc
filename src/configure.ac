dnl
dnl	aprsc
dnl
dnl	(c) Matti Aarnio, OH2MQK,  <oh2mqk@sral.fi>
dnl	(c) Heikki Hannikainen, OH7LZB
dnl
dnl	This program is licensed under the BSD license, which can be found
dnl	in the file LICENSE.
dnl

dnl Process this file with autoconf to produce a configure script.
AC_INIT
AC_CONFIG_SRCDIR([aprsc.c])

dnl For automake
VERSION="`cat VERSION`"
PACKAGE=aprsc
dnl AM_INIT_AUTOMAKE($PACKAGE, $VERSION)

AC_PROG_MAKE_SET

AC_CONFIG_HEADERS([ac-hdrs.h])

dnl Checks for programs.
AC_PROG_CC
AC_PROG_GCC_TRADITIONAL


dnl AC_PATH_PROG(LD, ld, ld)dnl
if test -z "$LD" ; then
  LD="$CC"
fi
AC_SUBST(LD,"$LD")

dnl If on i686, we'll need -march=i686 to get the atomic instructions
MACHINE="`uname -m`"
if test "$MACHINE" == "i686"; then
  CFLAGS_ARCH="-march=i686"
fi
AC_SUBST(CFLAGS_ARCH)

dnl Check for GNU make
AX_CHECK_GNU_MAKE()

dnl Check for libevent
AC_CHECK_HEADER([event2/event.h],,[AC_MSG_ERROR([libevent2 header not found - remember to install the development package too])])
AC_CHECK_HEADER([event2/http.h],,[AC_MSG_ERROR([libevent2 http header not found - remember to install the development package too])])
AC_CHECK_HEADER([event2/buffer.h],,[AC_MSG_ERROR([libevent2 buffer header not found - remember to install the development package too])])
AC_SUBST(LIBEVENT)
AC_CHECK_LIB(event,event_base_new,[LIBEVENT="-levent"],[AC_MSG_ERROR([libevent2 not found])])

dnl Checks for system headers
AC_CHECK_HEADERS([alloca.h], AC_DEFINE([HAVE_ALLOCA_H]))

dnl Checks for libraries.

dnl Checks for library functions.
AC_SUBST(LIBM)
AC_CHECK_FUNCS(atan2f,,
	       AC_CHECK_LIB(m, atan2f,
			    [LIBM="-lm"]))

AC_SUBST(LIBPTHREAD)
dnl AC_CHECK_FUNCS(pthread_exit,,
	       AC_CHECK_LIB(pthread, pthread_exit,
			    [LIBPTHREAD="-lpthread"])
dnl)

dnl Solaris resolver solution:
AC_SUBST(LIBGETNAMEINFO)
AC_CHECK_FUNCS(getnameinfo,,
	       AC_CHECK_LIB(nsl, getnameinfo,
			    [LIBGETNAMEINFO="-lnsl"]))

dnl
dnl We check for various libraries
dnl  - SysVr4 style of "-lsocket" at first (unless in libc)
dnl    The hallmark is  connect()  routine (we presume)
dnl
AC_SUBST(LIBSOCKET)dnl
ac_cv_libsocket_both=1
AC_CHECK_FUNC(connect, ac_cv_libsocket_both=0)
AC_CHECK_FUNC(gethostbyname, ac_cv_libsocket_both=0)
if test "$ac_cv_libsocket_both" = 1 ; then
  # Check cache
  if test "$ac_cv_func_socket_lxnet" = yes ; then
    AC_MSG_RESULT([need -lxnet library (cached)])
    LIBSOCKET="-lnsl -lsocket -lxnet"
  else
  if test "$ac_cv_func_socket_lsocket" = yes ; then
    AC_MSG_RESULT([need -lsocket library (cached)])
    LIBSOCKET="-lsocket"
    if test "$ac_cv_func_gethostbyname_lnsl" = yes ; then
        LIBSOCKET="-lnsl -lsocket"
    fi
  else
    # Well, will this work ?  SysVR4, but not Sun Solaris ?
    AC_CHECK_LIB(xnet, connect, [LIBSOCKET="-lnsl -lsocket -lxnet"
                                 ac_cv_func_socket_lsocket=no
                                 ac_cv_func_socket_lxnet=yes],[
      AC_CHECK_LIB(socket, connect, [LIBSOCKET="-lsocket"
                                     ac_cv_func_socket_lsocket=yes],
                                     ac_cv_func_socket_lsocket=no)
      if test "$ac_cv_func_socket_lsocket" = yes ; then
        t_oldLibs="$LIBS"
        LIBS="$LIBS -lsocket"
        AC_TRY_LINK([],[gethostbyname();], ,[
          LIBS="$LIBS -lnsl" # Add this Solaris library..
          AC_TRY_LINK([],[gethostbyname();],[
                        LIBSOCKET="-lsocket -lnsl"
                        ac_cv_func_gethostbyname_lnsl=yes
                ], [
                   AC_MSG_ERROR([Weird, '$LIBS' not enough to find  gethostnyname() ?!])
                ])
          ])
        LIBS="$t_oldLibs"
      fi
    ])
  fi
  fi
fi

# See about the routines that possibly exist at the libraries..
LIBS="$t_oldLibs $LIBSOCKET"
AC_CHECK_FUNCS(socket socketpair)
LIBS="$t_oldLibs"

if test "$ac_cv_func_socket" = no -a "$LIBSOCKET" != ""; then
  LIBS="$LIBS $LIBSOCKET"
  AC_TRY_LINK([],[socket();], ac_cv_func_socket=yes)
  if test $ac_cv_func_socket = yes; then
    AC_DEFINE(HAVE_SOCKET)
    AC_MSG_RESULT([Has  socket()  when using  $LIBSOCKET])
  fi
  LIBS="$t_oldLibs"
fi
if test "$ac_cv_func_socketpair" = no -a "$LIBSOCKET" != ""; then
  LIBS="$LIBS $LIBSOCKET"
  AC_TRY_LINK([],[socketpair();], ac_cv_func_socketpair=yes)
  if test $ac_cv_func_socketpair = yes; then
    AC_DEFINE(HAVE_SOCKETPAIR)
    AC_MSG_RESULT([Has  socketpair()  when using  $LIBSOCKET])
  fi
  LIBS="$t_oldLibs"
fi

ac_with_ipv6_replacement_libc=0
AC_ARG_WITH(ipv6-replacement-libc, [  --with-ipv6-replacement-libc If system has broken  getaddrinfo() and friends],
        ac_with_ipv6_replacement_libc=1)

LIBS="$LIBSOCKET $LIBS"
AC_CHECK_FUNCS(getnameinfo gai_strerror inet_ntop inet_pton)
LIBS="$t_oldLibs"


t_oldLibs="$LIBS"
LIBS="$LIBS $LIBSOCKET"

#  Following stuff may exist in our LIBC, or in separate -linet6
# AC_CHECK_FUNCS(inet_ntop inet_pton getaddrinfo getnameinfo gai_strerror)
#  If not found in LIBC, try to use  -linet6,  if it fails too,
#  THEN call the AC_REPLACE_FUNCS()
if test "$ac_cv_func_gai_strerror" = no ; then
    LIBS="$LIBS -linet6"
    AC_TRY_LINK([],[gai_strerror();], ac_cv_func_gai_strerror=yes)
    LIBS="$t_oldLibs"
    if test "$ac_cv_func_gai_strerror" = yes; then
        LIBSOCKET="$LIBSOCKET -linet6"
        AC_MSG_RESULT([Has IPv6 API  gai_strerror()  when using  $LIBSOCKET])
    else
      AC_LIBOBJ([inet_ntop])
      AC_LIBOBJ([inet_pton])
      AC_LIBOBJ([getnameinfo])
      AC_LIBOBJ([gai_strerror])
    fi
fi

LIBS="$t_oldLibs"

AC_SUBST(LIBS,"$LIBS")

dnl Check for the __sync_fetch_and_add builtin
AC_CACHE_CHECK([for __sync_fetch_and_add], ac_cv_func_sync_fetch_and_add,
[AC_TRY_LINK([],[int x;__sync_fetch_and_add(&x,1);],ac_cv_func_sync_fetch_and_add=yes,ac_cv_func_sync_fetch_and_add=no)])
if test "$ac_cv_func_sync_fetch_and_add" = yes; then
  AC_DEFINE(HAVE_SYNC_FETCH_AND_ADD,1,[Define if you have the __sync_fetch_and_add function])
fi


dnl Output files
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

dnl If GNU make is not the default, deal with it
if  ! test x$_cv_gnu_make_command = x ; then
	mv Makefile GNUmakefile
	echo all clean distclean install: > Makefile ;
	echo \  $_cv_gnu_make_command \$@ >> Makefile;
fi

