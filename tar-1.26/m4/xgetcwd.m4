#serial 6
dnl Copyright (C) 2002-2006, 2009-2011 Free Software Foundation, Inc.
dnl This file is free software; the Free Software Foundation
dnl gives unlimited permission to copy and/or distribute it,
dnl with or without modifications, as long as this notice is preserved.

AC_DEFUN([gl_XGETCWD],
[
  AC_LIBOBJ([xgetcwd])

  AC_REQUIRE([gl_FUNC_GETCWD])
])
