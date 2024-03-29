project(
	'egl_wayland', 'c',
	default_options: ['warning_level=3',
		'werror=true',
		'buildtype=debugoptimized',
		'default_library=static']
)

wl_client_dep = dependency('wayland-client')
wl_protocol_dep = dependency('wayland-protocols')
wl_egl_dep = dependency('wayland-egl')
epoxy_dep = dependency('epoxy')
threads_dep = dependency('threads')

wl_scanner = find_program('wayland-scanner')

wl_sources = [
    'hello_egl_wayland.c',
    'init_window.c',
]

wl_headers = [
]

protocols_datadir = wl_protocol_dep.get_variable(pkgconfig: 'pkgdatadir', internal: 'pkgdatadir')

protocol_defs = [
    ['/stable/viewporter/viewporter.xml', 'viewporter-protocol.c', 'viewporter-client-protocol.h'],
    ['/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml',
     'linux-dmabuf-unstable-v1-protocol.c', 'linux-dmabuf-unstable-v1-client-protocol.h'],
    ['/unstable/fullscreen-shell/fullscreen-shell-unstable-v1.xml',
     'fullscreen-shell-unstable-v1-protocol.c', 'fullscreen-shell-unstable-v1-client-protocol.h'],
    ['/stable/xdg-shell/xdg-shell.xml', 'xdg-shell-protocol.c', 'xdg-shell-client-protocol.h'],
    ['/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml', 'xdg-decoration-unstable-v1-protocol.c', 'xdg-decoration-unstable-v1-client-protocol.h'],
]
protocols_files = []

foreach protodef: protocol_defs
    xmlfile = protocols_datadir + protodef.get(0)

    protocols_files += [custom_target(protodef.get(1),
      output : protodef.get(1),
      input : xmlfile,
      command : [wl_scanner, 'code', '@INPUT@', '@OUTPUT@'])]

    protocols_files += [custom_target(protodef.get(2),
      output : protodef.get(2),
      input : xmlfile,
      command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])]
endforeach

extra_c_args = [
]

dep_rt = meson.get_compiler('c').find_library('rt')

executable('hello_egl_wayland',
  wl_sources + protocols_files,
  install : true,
  dependencies : [wl_client_dep, wl_protocol_dep, wl_egl_dep, epoxy_dep,
    threads_dep,
    dep_rt,
    dependency('libavcodec'),
    dependency('libavfilter'),
    dependency('libavformat'),
    dependency('libavutil'),
  ]
)
