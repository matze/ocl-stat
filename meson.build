project('ocl-stat', 'c')

cc = meson.get_compiler('c')

glib_dep = dependency ('glib-2.0')
dl_dep = declare_dependency(dependencies: cc.find_library('dl'))
unwind_dep = dependency('libunwind', required: false)
opencl_dep = dependency('OpenCL', required: false)

if not opencl_dep.found()
    if not cc.has_header('CL/cl.h')
        error('Cannot find CL/cl.h')
    endif

    opencl_dep = declare_dependency(dependencies: cc.find_library('OpenCL'))
endif

c_args = []

lib_deps = [glib_dep, dl_dep]

if unwind_dep.found()
  c_args += ['-DHAVE_LIBUNWIND']
  lib_deps += [unwind_dep]
endif

library('ocl-stat', 
  sources: ['ocl-stat.c'],
  dependencies: lib_deps,
  c_args: c_args,
)

executable('stat-test', 
  sources: ['stat-test.c'],
  dependencies: [opencl_dep]
)
