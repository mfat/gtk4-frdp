# GTK4 port (sshPilot embedded-RDP spike). Differences from upstream:
#  * gtk4 instead of gtk+-3.0; FreeRDP 3 only (no FreeRDP 2 fallback)
#  * clipboard channel optional (it hard-requires FUSE3)
#  * GIR namespace GtkFrdp-4.0 so it never clashes with the GTK3 GtkFrdp-0.2
#  * no Vala bindings
api_version = '4.0'

gtk_frdp_public_sources = [
  'frdp-display.c',
  'frdp-session.c'
]

gtk_frdp_private_sources = [
  'frdp-channel.c',
  'frdp-channel-display-control.c',
]

gtk_frdp_public_headers = [
  'frdp-display.h',
  'frdp-session.h',
  'gtk-frdp.h'
]

version_split = meson.project_version().split('.')
version_conf = configuration_data()
version_conf.set('VERSION', meson.project_version())
version_conf.set('MAJOR_VERSION', version_split[0])
version_conf.set('MINOR_VERSION', version_split[1])
version_conf.set('MICRO_VERSION', version_split[2])

gtk_frdp_header_subdir = join_paths(package_subdir, 'gtk4-frdp')
gtk_frdp_header_dir = join_paths(get_option('includedir'), gtk_frdp_header_subdir)

configure_file(
  input: 'gtk-frdp-version.h.in',
  output: 'gtk-frdp-version.h',
  configuration: version_conf,
  install: true,
  install_dir: gtk_frdp_header_dir
)

cc = meson.get_compiler('c')

freerdp3_dep = dependency('freerdp3', version: '>= 3.11.0')
add_project_arguments('-DHAVE_FREERDP3', '-DHAVE_FREERDP_3_11_0', language: 'c')

gtk_frdp_deps = [
  dependency('gio-2.0', version: '>= 2.66'),
  dependency('gtk4', version: '>= 4.14'),
  freerdp3_dep,
  dependency('winpr3', version: '>= 3.0.0'),
  dependency('freerdp-client3'),
  cc.find_library('m', required: false),
]

if get_option('clipboard')
  gtk_frdp_deps += dependency('fuse3', version: '>= 3.9.1')
  gtk_frdp_private_sources += 'frdp-channel-clipboard.c'
  add_project_arguments('-DFRDP_WITH_CLIPBOARD', language: 'c')
endif

gtk_frdp_lib = shared_library('gtk4-frdp-' + api_version,
  gtk_frdp_public_sources + gtk_frdp_private_sources,
  dependencies: gtk_frdp_deps,
  install: true,
  install_dir: libdir
)

gtk_frdp_dep = declare_dependency(
  sources: gtk_frdp_public_headers,
  dependencies: gtk_frdp_deps,
  link_with: gtk_frdp_lib,
  include_directories: include_directories('.'),
)

install_headers(gtk_frdp_public_headers, subdir: gtk_frdp_header_subdir)

gtk_frdp_gir = gnome.generate_gir(gtk_frdp_lib,
  sources: gtk_frdp_public_sources + gtk_frdp_public_headers,
  nsversion: api_version,
  namespace: 'GtkFrdp',
  symbol_prefix: 'frdp',
  identifier_prefix: 'Frdp',
  link_with: gtk_frdp_lib,
  includes: ['Gio-2.0', 'Gtk-4.0'],
  install: true,
  install_dir_gir: girdir,
  install_dir_typelib: typelibdir,
  extra_args: [ '--c-include=gtk-frdp.h', '--quiet' ],
)
