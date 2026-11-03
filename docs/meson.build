# SPDX-License-Identifier: GPL-3.0-or-later
if not get_option('introspection')
  error('-Ddocumentation=true requires -Dintrospection=true')
endif

gidocgen = find_program('gi-docgen')

toml_conf = configuration_data()
toml_conf.set('version', meson.project_version())
docs_toml = configure_file(
  input: 'gtk4-frdp.toml.in',
  output: 'gtk4-frdp.toml',
  configuration: toml_conf,
)

docs_dir = get_option('datadir') / 'doc'

custom_target('gtk4-frdp-doc',
  input: [ docs_toml, gtk4_frdp_gir[0] ],
  output: package_api_name,
  command: [
    gidocgen, 'generate',
    '--quiet',
    '--add-include-path=@0@'.format(meson.project_build_root() / 'src'),
    '--config=@INPUT0@',
    '--output-dir=@OUTPUT@',
    '--no-namespace-dir',
    '--content-dir=@0@'.format(meson.current_source_dir()),
    '@INPUT1@',
  ],
  depend_files: [ files('design.md') ],
  build_by_default: true,
  install: true,
  install_dir: docs_dir,
)
