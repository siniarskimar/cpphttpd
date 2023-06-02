project('cpphttpd', ['c', 'cpp'],
  default_options: [
    'cpp_std=c++20'
  ],
  meson_version: '>=1.1.0'
)

libfmt = dependency('fmt', required: true)
libBoost = dependency('boost')
libBoostURL = dependency('boost', modules: ['url', 'system'])
src_inc = include_directories('./src')

subdir('src')
subdir('test')
