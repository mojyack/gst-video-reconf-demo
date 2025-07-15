project('gst-video-reconf-demo', 'cpp', version: '1.0.0', default_options : ['warning_level=3', 'werror=false', 'cpp_std=c++23'])
add_project_arguments('-Wfatal-errors', language: 'cpp')
add_project_arguments('-Wno-missing-field-initializers', language: 'cpp')

subdir('src/net')
subdir('src/net/tcp')

deps = [
  dependency('gstreamer-1.0', version : ['>=1.20'])
]

executable('camera',
  files(
    'src/camera.cpp',
    'src/gstutil/caps.cpp',
    'src/gstutil/pipeline-helper.cpp',
  ) + netprotocol_files + netprotocol_tcp_server_files,
  dependencies : deps + netprotocol_deps + netprotocol_tcp_deps,
)

executable('display',
  files(
    'src/display.cpp',
    'src/gstutil/caps.cpp',
    'src/gstutil/pipeline-helper.cpp',
  ) + netprotocol_files + netprotocol_tcp_client_files, 
  dependencies : deps + netprotocol_deps + netprotocol_tcp_deps,
)
