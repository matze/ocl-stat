### OpenCL resource allocation tracker

An `LD_PRELOAD` library that intercepts calls to `libOpenCL.so` and tracks
references to OpenCL resources during the run-time of an application. It prints
out allocation information at the end of the execution of the application as
well as when receiving signals such as `SIGUSR1`.

Most of the code is a direct translation of the
[gobject-list](https://github.com/danni/gobject-list) library.

#### Build and usage

Install development files for glib-2.0 and libunwind if you want to see
backtraces in case of problems. Run

    make

or

    meson build && cd build && ninja

to build the library and

    LD_PRELOAD=libocl-stat.so /path/to/application

to examine the lifetime of OpenCL resources.
