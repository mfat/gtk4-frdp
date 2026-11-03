# SPDX-License-Identifier: GPL-3.0-or-later
project('gtk4-frdp', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.62.0',
  default_options: [ 'warning_level=2', 'c_std=gnu11' ],
)

# API version: the GObject Introspection namespace is GtkFrdp-4.0 (the "4"
# follows GTK 4, the way upstream GTK 3 gtk-frdp uses GtkFrdp-0.2). It only
# changes on an incompatible API break.
api_version = '4.0'
package_api_name = 'gtk4-frdp-' + api_version

gnome = import('gnome')
pkg = import('pkgconfig')

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-Wno-sign-compare',
  ]),
  language: 'c')

subdir('src')

if get_option('examples')
  subdir('examples')
endif

if get_option('tests')
  subdir('tests')
endif

if get_option('documentation')
  subdir('docs')
endif

summary({
  'Introspection': get_option('introspection'),
  'Examples': get_option('examples'),
  'Tests': get_option('tests'),
  'API documentation': get_option('documentation'),
}, section: 'Features')
