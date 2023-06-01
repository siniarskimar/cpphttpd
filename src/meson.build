
# cpphttpd_lib = static_library('cpphttpd_lib',
#   'func.cpp',
#   include_directories: src_inc
# )

executable('cpphttpd',
  'main.cpp',
  include_directories: src_inc,
  dependencies: [
    libfmt,
    libBoost,
  ],
  cpp_pch: 'pch/pch.hpp'
  # link_with: cpphttpd_lib
)

