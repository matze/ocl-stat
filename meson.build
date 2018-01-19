project('ocl-stat', 'c')

cc = meson.get_compiler('c')

glib_dep = dependency ('glib-2.0')
opencl_dep = dependency('OpenCL', required: false)

if not opencl_dep.found()
    if not cc.has_header('CL/cl.h')
        error('Cannot find CL/cl.h')
    endif

    opencl_dep = declare_dependency(dependencies: cc.find_library('OpenCL'))
endif

dl_dep = declare_dependency(dependencies: cc.find_library('dl'))

library('ocl-stat', 
  sources: ['ocl-stat.c'],
  dependencies: [glib_dep, dl_dep]
)

executable('stat-test', 
  sources: ['stat-test.c'],
  dependencies: [opencl_dep]
)
