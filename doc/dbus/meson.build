ifaces_refs = []

ifaces = [
  act_namespace,
  act_namespace + '.User',
]

xsltproc = find_program('xsltproc')
spec_to_docbook = files('spec-to-docbook.xsl')

foreach iface: ifaces
  iface_ref = iface + '.ref.xml'

  ifaces_refs += custom_target(
    iface_ref,
    input: join_paths(data_dir, iface + '.xml'),
    output: iface_ref,
    command: [xsltproc, '--output', '@OUTPUT@', spec_to_docbook, '@INPUT@'],
  )
endforeach

output = 'AccountsService.xml'

docbook_conf = configuration_data()
docbook_conf.set('srcdir', meson.current_build_dir())
docbook_conf.set('VERSION', act_version)

docbook = 'AccountsService.xml'

docbook_xml = configure_file(
  input: docbook + '.in',
  output: docbook,
  configuration: docbook_conf,
)

html = 'AccountsService.html'

custom_target(
  html,
  input: docbook_xml,
  output: html,
  command: [find_program('xmlto'), 'xhtml-nochunks', '-o', meson.current_build_dir(), '-m', files('config.xsl'), '@INPUT@'],
  depends: ifaces_refs,
  install: true,
  install_dir: join_paths(act_datadir, 'doc', act_name, 'spec'),
)
