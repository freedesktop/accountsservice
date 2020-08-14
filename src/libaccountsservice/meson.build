libaccountsservice_dir = meson.current_source_dir()

subdir = act_id.to_lower()

headers = files(
  'act-user.h',
  'act-user-manager.h',
)

install_headers(
  headers + ['act.h'],
  install_dir: join_paths(act_pkgincludedir, subdir),
)

sources = files(
  'act-user.c',
  'act-user-manager.c',
)

enum_types = 'act-user-enum-types'

enum_sources = gnome.mkenums(
  enum_types,
  sources: headers,
  c_template: enum_types + '.c.template',
  h_template: enum_types + '.h.template',
  install_header: true,
  install_dir: join_paths(act_pkgincludedir, subdir),
)

deps = [
  crypt_dep,
  gio_unix_dep,
  glib_dep,
  libaccounts_generated_dep,
  logind_dep,
]

symbol_map = join_paths(meson.current_source_dir(), 'symbol.map')
ldflags = cc.get_supported_link_arguments('-Wl,--version-script,@0@'.format(symbol_map))

libaccountsservice = shared_library(
  act_name,
  sources: sources + enum_sources,
  version: libversion,
  include_directories: top_inc,
  dependencies: deps,
  c_args: '-DG_LOG_DOMAIN="@0@"'.format(meson.project_name()),
  link_args: ldflags,
  link_depends: symbol_map,
  install: true,
)

libaccountsservice_dep = declare_dependency(
  sources: enum_sources[1],
  include_directories: include_directories('.'),
  dependencies: [gio_dep, glib_dep],
  link_with: libaccountsservice,
)

pkg.generate(
  libaccountsservice,
  name: 'Accounts Service',
  description: 'Client Library for communicating with accounts service',
  filebase: act_name,
  subdirs: act_api_name,
  variables: [
    'exec_prefix=${prefix}',
    'datadir=${prefix}/' + get_option('datadir'),
    'interfacesdir=${datadir}/accountsservice/interfaces',
  ],
  requires_private : [
    'gio-2.0',
    'glib-2.0',
    'gobject-2.0',
    'polkit-gobject-1',
  ],
)

if get_option('introspection')
  as_gir_gen_sources = gnome.generate_gir(
    libaccountsservice,
    sources: sources + headers + [enum_sources[1]],
    includes: ['GObject-2.0', 'Gio-2.0'],
    dependencies: libaccountsservice_dep,
    namespace: 'AccountsService',
    nsversion: act_api_version,
    identifier_prefix: act_id,
    header: join_paths(subdir, 'act.h'),
    install: true,
  )

  if get_option('vapi')
    gnome.generate_vapi(act_name,
      sources : as_gir_gen_sources[0],
      packages : 'gio-2.0',
      install : true,
    )
  endif
endif
