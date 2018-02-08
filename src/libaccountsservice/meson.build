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

enum_sources = []
enum_types = 'act-user-enum-types'

enum_sources += gnome.mkenums(
  enum_types + '.c',
  sources: headers,
  fhead: '#include "act-user.h"\n#include "act-user-manager.h"\n#include "act-user-enum-types.h"\n#include <glib-object.h>\n\n',
  fprod: '\n/* enumerations from "@filename@" */',
  vhead: 'GType\n@enum_name@_get_type (void)\n{\n  static GType etype = 0;\n  if (etype == 0) {\n    static const G@Type@Value values[] = {',
  vprod: '      { @VALUENAME@, "@VALUENAME@", "@valuenick@" },',
  vtail: '      { 0, NULL, NULL }\n    };\n    etype = g_@type@_register_static ("@EnumName@", values);\n  }\n  return etype;\n}\n',
)

enum_sources += gnome.mkenums(
  enum_types + '.h',
  sources: headers,
  fhead: '#ifndef __ACT_USER_ENUM_TYPES_H__\n#define __ACT_USER_ENUM_TYPES_H__\n\n#include <glib-object.h>\n\nG_BEGIN_DECLS\n',
  fprod: '/* enumerations from "@filename@" */\n',
  vhead: 'GType @enum_name@_get_type (void);\n#define ACT_USER_TYPE_@ENUMSHORT@ (@enum_name@_get_type())\n',
  ftail: 'G_END_DECLS\n\n#endif /* __ACT_USER_ENUM_TYPES_H__ */',
  install_header: true,
  install_dir: join_paths(act_pkgincludedir, subdir),
)

dbus_sources = []

ifaces = [
  'Manager',
  'Seat',
  'Session',
]

namespace = 'ConsoleKit'
prefix = 'org.freedesktop.' + namespace

foreach iface: ifaces
  dbus_sources += gnome.gdbus_codegen(
    'ck-@0@-generated'.format(iface.to_lower()),
    '@0@.@1@.xml'.format(prefix, iface),
    interface_prefix: prefix,
    namespace: namespace,
  )
endforeach

deps = [
  crypt_dep,
  gio_unix_dep,
  glib_dep,
  libaccounts_generated_dep,
]

symbol_map = join_paths(meson.current_source_dir(), 'symbol.map')
ldflags = cc.get_supported_link_arguments('-Wl,--version-script,@0@'.format(symbol_map))

if enable_systemd or enable_elogind
  deps += logind_dep
endif

libaccountsservice = shared_library(
  act_name,
  sources: sources + enum_sources + dbus_sources,
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
  variables: 'exec_prefix=${prefix}',
)

if get_option('introspection')
  gnome.generate_gir(
    libaccountsservice,
    sources: sources + headers + [enum_sources[1]],
    includes: ['GObject-2.0', 'Gio-2.0'],
    dependencies: libaccountsservice_dep,
    namespace: meson.project_name(),
    nsversion: act_api_version,
    identifier_prefix: act_id,
    header: join_paths(subdir, 'act.h'),
    install: true,
  )
endif
