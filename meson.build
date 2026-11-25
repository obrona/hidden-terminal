project('hidden-terminal', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release'])

gtk = dependency('gtk4', version: '>= 4.10')
vte = dependency('vte-2.91-gtk4', version: '>= 0.78')
pcre2 = dependency('libpcre2-8')

executable('hidden-terminal', 'src/main.cpp',
  dependencies: [gtk, vte, pcre2],
  install: true)

install_data('data/dev.hidden.Terminal.desktop',
  install_dir: get_option('datadir') / 'applications')
