#!/usr/bin/env python3

import os
import sys

destdir = os.environ.get('DESTDIR', '')
localstatedir = os.path.normpath(destdir + os.sep + sys.argv[1])

# FIXME: meson will not track the creation of these directories
#        https://github.com/mesonbuild/meson/blob/master/mesonbuild/scripts/uninstall.py#L39
dst_dirs = [
  (os.path.join(localstatedir, 'lib', 'AccountsService', 'icons'), 0o775),
  (os.path.join(localstatedir, 'lib', 'AccountsService', 'users'), 0o700),
]

for (dst_dir, dst_dir_mode) in dst_dirs:
  if not os.path.exists(dst_dir):
    os.makedirs(dst_dir, mode=dst_dir_mode)
