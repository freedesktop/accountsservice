doc_module = 'libaccountsservice'

gnome.gtkdoc(
  doc_module,
  main_xml: doc_module + '-docs.xml',
  src_dir: libaccountsservice_dir,
  dependencies: libaccountsservice_dep,
  namespace: act_id.to_lower(),
  fixxref_args: '--html-dir=' + join_paths(act_prefix, gnome.gtkdoc_html_dir(doc_module)),
  gobject_typesfile: files(doc_module + '.types'),
  install: true,
)
