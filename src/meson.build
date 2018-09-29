sources = []

gdbus_headers = []

ifaces = [
  ['accounts-generated', 'org.freedesktop.', 'Accounts'],
  ['accounts-user-generated', act_namespace + '.', 'User'],
]

foreach iface: ifaces
  gdbus_sources = gnome.gdbus_codegen(
    iface[0],
    join_paths(data_dir, iface[1] + iface[2] + '.xml'),
    interface_prefix: iface[1],
    namespace: 'Accounts',
  )
  sources += gdbus_sources
  gdbus_headers += gdbus_sources[1]
endforeach

deps = [
  gio_dep,
  gio_unix_dep,
]

cflags = [
  '-DLOCALSTATEDIR="@0@"'.format(act_localstatedir),
  '-DDATADIR="@0@"'.format(act_datadir),
  '-DICONDIR="@0@"'.format(join_paths(act_localstatedir, 'lib', 'AccountsService', 'icons')),
  '-DUSERDIR="@0@"'.format(join_paths(act_localstatedir, 'lib', 'AccountsService', 'users')),
]

libaccounts_generated = static_library(
  'accounts-generated',
  sources: sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
)

libaccounts_generated_dep = declare_dependency(
  sources: gdbus_headers,
  include_directories: include_directories('.'),
  dependencies: gio_dep,
  link_with: libaccounts_generated,
)

sources = files(
  'daemon.c',
  'extensions.c',
  'main.c',
  'user.c',
  'user-classify.c',
  'util.c',
  'wtmp-helper.c',
)

deps = [
  gio_unix_dep,
  glib_dep,
  libaccounts_generated_dep,
  polkit_gobject_dep,
]

executable(
  'accounts-daemon',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_dir: act_libexecdir,
)

subdir('libaccountsservice')
