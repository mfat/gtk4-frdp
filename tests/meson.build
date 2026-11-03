# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests need a display. Run headless with:
#   xvfb-run -a meson test -C _build
# Tests exit 77 (reported as SKIP) when no display or no server is available.

test_env = environment()
test_env.prepend('LD_LIBRARY_PATH', meson.project_build_root() / 'src')
test_env.prepend('GI_TYPELIB_PATH', meson.project_build_root() / 'src')
test_env.set('GDK_BACKEND', 'x11')
test_env.set('GSK_RENDERER', 'cairo')

test_header = executable('test-header', 'test-header.c',
  dependencies: gtk4_frdp_dep,
)
test('header', test_header, env: test_env)

# Internal module, compiled directly (not part of the public API).
test_keyboard_layout = executable('test-keyboard-layout',
  'test-keyboard-layout.c',
  '../src/frdp-keyboard-layout.c',
  '../src/frdp-xkb-layout-ids.c',
  include_directories: include_directories('../src'),
  dependencies: deps,
)
test('keyboard-layout', test_keyboard_layout)

test_dib = executable('test-dib', 'test-dib.c', '../src/frdp-dib.c',
  include_directories: include_directories('../src'),
  dependencies: [ dependency('gtk4') ],
)
test('dib', test_dib)

python = find_program('python3', required: false)
if get_option('introspection') and python.found()
  test('widget', python,
    args: [ files('test-widget.py') ],
    env: test_env,
    depends: gtk4_frdp_gir,
    timeout: 60,
  )
  test('live-session', python,
    args: [ files('live-session.py') ],
    env: test_env,
    depends: gtk4_frdp_gir,
    timeout: 120,
    suite: 'live',
    is_parallel: false,  # one server account: concurrent logins evict each other
  )
  test('live-clipboard', python,
    args: [ files('live-clipboard.py') ],
    env: test_env,
    depends: gtk4_frdp_gir,
    timeout: 120,
    suite: 'live',
    is_parallel: false,  # one server account: concurrent logins evict each other
  )
  test('live-cursor', python,
    args: [ files('live-cursor.py') ],
    env: test_env,
    depends: gtk4_frdp_gir,
    timeout: 60,
    suite: 'live',
    is_parallel: false,  # one server account: concurrent logins evict each other
  )
endif
