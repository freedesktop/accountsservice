project(
  'accountsservice', 'c',
  version: '0.6.55',
  license: 'GPL3+',
  default_options: 'buildtype=debugoptimized',
  meson_version: '>= 0.46.0',
)

act_name = meson.project_name()
act_version = meson.project_version()

act_api_version = '1.0'
act_api_name = '@0@-@1@'.format(act_name, act_api_version)

act_id = 'Act'

act_prefix = get_option('prefix')
act_datadir = join_paths(act_prefix, get_option('datadir'))
act_includedir = join_paths(act_prefix, get_option('includedir'))
act_libexecdir = join_paths(act_prefix, get_option('libexecdir'))
act_localstatedir = join_paths(act_prefix, get_option('localstatedir'))
act_sysconfdir = join_paths(act_prefix, get_option('sysconfdir'))

act_pkgincludedir = join_paths(act_includedir, act_api_name)

act_namespace = 'org.freedesktop.Accounts'

act_gettext = 'accounts-service'

soversion = 0
current = 0
revision = 0
libversion = '@0@.@1@.@2@'.format(soversion, current, revision)

act_buildtype = get_option('buildtype')

gnome = import('gnome')
i18n = import('i18n')
pkg = import('pkgconfig')

data_dir = join_paths(meson.current_source_dir(), 'data')
po_dir = join_paths(meson.current_source_dir(), 'po')

top_inc = include_directories('.')

cc = meson.get_compiler('c')

config_h = configuration_data()

# defines
config_h.set_quoted('VERSION', act_version)
config_h.set('_DEFAULT_SOURCE', true)
config_h.set('_GNU_SOURCE', true)

# i18n
config_h.set_quoted('GETTEXT_PACKAGE', act_gettext)

# headers
check_headers = [
  'paths.h',
  'shadow.h',
  'utmpx.h',
]

foreach header: check_headers
  config_h.set('HAVE_' + header.underscorify().to_upper(), cc.has_header(header))
endforeach

# functions
check_functions = [
  'getusershell',
  'setutxdb',
  'fgetpwent',
]

foreach func: check_functions
  config_h.set('HAVE_' + func.underscorify().to_upper(), cc.has_function(func))
endforeach

if cc.has_header_symbol('utmpx.h', 'WTMPX_FILENAME', prefix: '#define _GNU_SOURCE')
  config_h.set('PATH_WTMP', 'WTMPX_FILENAME')
elif cc.has_header_symbol('paths.h', '_PATH_WTMPX')
  config_h.set('PATH_WTMP', '_PATH_WTMPX')
else
  assert(run_command('test', '-e', '/var/log/utx.log').returncode() == 0, 'Do not know which filename to watch for wtmp changes')
  config_h.set_quoted('PATH_WTMP', '/var/log/utx.log')
endif

# compiler flags
common_flags = []

# Only add this when optimizing is enabled
optimized_src = '''
  #ifdef __OPTIMIZE__
  #error No optimization
  #endif
'''

act_optimized = act_buildtype.contains('optimized') and cc.compiles(optimized_src)
message('whether optimization is enabled: ' + act_optimized.to_string())
if act_optimized
  common_flags += '-Wp,-D_FORTIFY_SOURCE=2'
endif

if act_buildtype.contains('debug')
  common_flags += cc.get_supported_arguments([
    '-Wcast-align',
    '-Winit-self',
    '-Wmissing-declarations',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wno-deprecated-declarations',
    '-Wswitch-enum',
    '-Wunsafe-loop-optimizations',
    '-Wwrite-strings',
  ])
endif

add_project_arguments(common_flags, language: 'c')

gio_dep = dependency('gio-2.0', version: '>= 2.37.3')
gio_unix_dep = dependency('gio-unix-2.0')
glib_dep = dependency('glib-2.0', version: '>= 2.44')
polkit_gobject_dep = dependency('polkit-gobject-1')

crypt_dep = cc.find_library('crypt')

dbus_dep = dependency('dbus-1')
dbus_conf_dir = join_paths(dbus_dep.get_pkgconfig_variable('sysconfdir', define_variable: ['sysconfdir', act_sysconfdir]), 'dbus-1', 'system.d')
dbus_ifaces_dir = dbus_dep.get_pkgconfig_variable('interfaces_dir', define_variable: ['datadir', act_datadir])
dbus_sys_dir = dbus_dep.get_pkgconfig_variable('system_bus_services_dir', define_variable: ['datadir', act_datadir])

policy_dir = polkit_gobject_dep.get_pkgconfig_variable('policydir', define_variable: ['prefix', act_prefix])

# FIXME: systemd.pc file does not use variables with relative paths, so `define_variable` cannot be used
systemd_system_unit_dir = get_option('systemdsystemunitdir')
install_systemd_unit_dir = (systemd_system_unit_dir != 'no')

if install_systemd_unit_dir and systemd_system_unit_dir == ''
  systemd_dep = dependency('systemd', required: false)
  assert(systemd_dep.found(), 'systemd required but not found, please provide a valid systemd user unit dir or disable it')
  systemd_system_unit_dir = systemd_dep.get_pkgconfig_variable('systemdsystemunitdir')
endif

# Core configuration
admin_group = get_option('admin_group')
if admin_group == ''
  if run_command('test', '-e', '/etc/debian_version').returncode() == 0
    admin_group = 'sudo'
  # FIXME: this has been left for documentation purposes
  elif run_command('test', '-e', '/etc/sysconfig/network-scripts').returncode() == 0
    admin_group = 'wheel'
  else
    admin_group = 'wheel'
  endif
endif

extra_admin_groups = ','.join(get_option('extra_admin_groups'))

config_h.set_quoted('ADMIN_GROUP', admin_group)
config_h.set_quoted('EXTRA_ADMIN_GROUPS', extra_admin_groups)

config_h.set('ENABLE_USER_HEURISTICS', get_option('user_heuristics'))
config_h.set('MINIMUM_UID', get_option('minimum_uid'))

# GDM
gdm_conf_file = get_option('gdmconffile')
config_h.set_quoted('PATH_GDM_CUSTOM', gdm_conf_file)

enable_systemd = get_option('systemd')
enable_elogind = get_option('elogind')
assert(not enable_systemd or not enable_elogind, 'systemd and elogind support requested, please choose only one.')

if enable_systemd
  logind_dep = dependency('libsystemd', version: '>= 186')
endif

if enable_elogind
  logind_dep = dependency('libelogind', version: '>= 229.4')
endif
config_h.set('WITH_SYSTEMD', enable_systemd or enable_elogind)

subdir('data')
subdir('src')
subdir('po')

enable_docbook = get_option('docbook')
if enable_docbook
  subdir('doc/dbus')
endif

if get_option('gtk_doc')
  subdir('doc/libaccountsservice')
endif

configure_file(
  output: 'config.h',
  configuration: config_h,
)

meson.add_install_script(
  'meson_post_install.py',
  act_localstatedir,
)

output = '\n' + meson.project_name() + ' was configured with the following options:\n'
output += '** DocBook documentation build: ' + enable_docbook.to_string() + '\n'
output += '** Administrator group: ' + admin_group + '\n'
output += '** Extra administrator groups: ' + extra_admin_groups + '\n'
output += '** GDM configuration: ' + gdm_conf_file
message(output)
