# SPDX-License-Identifier: GPL-3.0-or-later

# Public API: the FrdpDisplay widget only. FrdpSession, the channel objects
# and the FreeRDP context are implementation details and are neither
# installed nor introspected, so they can change without an API break.
public_headers = [
  'frdp-display.h',
  'gtk4-frdp.h',
]

public_sources = [
  'frdp-display.c',
]

private_sources = [
  'frdp-session.c',
  'frdp-channel.c',
  'frdp-channel-display-control.c',
]

header_subdir = 'gtk4-frdp'

version_split = meson.project_version().split('.')
version_conf = configuration_data()
version_conf.set('VERSION', meson.project_version())
version_conf.set('MAJOR_VERSION', version_split[0])
version_conf.set('MINOR_VERSION', version_split[1])
version_conf.set('MICRO_VERSION', version_split[2])

version_header = configure_file(
  input: 'gtk4-frdp-version.h.in',
  output: 'gtk4-frdp-version.h',
  configuration: version_conf,
  install: true,
  install_dir: get_option('includedir') / header_subdir,
)

freerdp_dep = dependency('freerdp3', version: '>= 3.11.0')

deps = [
  dependency('gio-2.0', version: '>= 2.66'),
  dependency('gtk4', version: '>= 4.14'),
  freerdp_dep,
  dependency('winpr3', version: '>= 3.11.0'),
  dependency('freerdp-client3', version: '>= 3.11.0'),
  cc.find_library('m', required: false),
]

c_args = [
  '-DGTK4_FRDP_COMPILATION',
  '-DG_LOG_DOMAIN="Gtk4Frdp"',
]

# frdp-channel-clipboard.c is still the GTK 3 implementation from gtk-frdp
# and is not built: it needs porting to GdkClipboard (asynchronous) and to
# the session thread. FRDP_WITH_CLIPBOARD marks the hooks in frdp-session.c.

gtk4_frdp_lib = shared_library(package_api_name,
  public_sources + private_sources + [version_header],
  dependencies: deps,
  c_args: c_args,
  version: '0.0.0',
  soversion: '0',
  gnu_symbol_visibility: 'default',
  install: true,
)

install_headers(public_headers, subdir: header_subdir)

gtk4_frdp_dep = declare_dependency(
  link_with: gtk4_frdp_lib,
  include_directories: include_directories('.'),
  sources: [version_header],
  dependencies: [ dependency('gtk4') ],
)
meson.override_dependency(package_api_name, gtk4_frdp_dep)

pkg.generate(gtk4_frdp_lib,
  name: 'gtk4-frdp',
  filebase: package_api_name,
  description: 'GTK 4 widget for embedding FreeRDP 3 remote desktops',
  subdirs: header_subdir,
  requires: [ 'gtk4' ],
  requires_private: [ 'freerdp3', 'winpr3', 'freerdp-client3' ],
)

if get_option('introspection')
  gtk4_frdp_gir = gnome.generate_gir(gtk4_frdp_lib,
    sources: public_sources + public_headers + [version_header],
    nsversion: api_version,
    namespace: 'GtkFrdp',
    # gtk4_frdp: the GTK4_FRDP_* version macros become GtkFrdp constants.
    symbol_prefix: [ 'frdp', 'gtk4_frdp' ],
    identifier_prefix: 'Frdp',
    includes: [ 'Gio-2.0', 'Gtk-4.0' ],
    header: 'gtk4-frdp.h',
    export_packages: package_api_name,
    install: true,
    extra_args: [ '--quiet', '-DGTK4_FRDP_COMPILATION' ],
  )
endif
