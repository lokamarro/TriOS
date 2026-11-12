These five files are byte-for-byte copies from the Mesa demos upstream mirror
https://github.com/mirror/mesa-demos, paths src/egl/opengles1/gears.c and
src/egl/eglut/{eglut.c,eglut.h,eglutint.h,eglut_x11.c}, retrieved 2026-10-09.
SHA256SUMS pins their exact contents. Copyright and permission notices remain
inside every source. No application or EGLUT source modifications were made.

Build with real Debian ARM64 GCC and system Khronos headers:
 gcc -O2 gears.c eglut.c eglut_x11.c -lEGL -lGLESv1_CM -lX11 -lm -o linuxrunner-mesa-es1-gears

The managed gpu-gears.sh scopes library selection to the limited LinuxRunner
EGL/GLES1 backend, leaving system Mesa untouched. This is conventional API
integration for this supported application, NOT a Mesa Gallium driver, GLX,
GLES1 conformance claim, or support for all existing applications.

GLES2 es2gears.c is also byte-for-byte upstream src/egl/opengles2/es2gears.c, retrieved 2026-10-10 and pinned in SHA256SUMS. It uses GLSL ES 100 shaders, uniform matrices, lighting and VBOs, executed by the experimental Metal shader provider via gpu-gears2.sh. No application source changes. The 855 provider adds a limited sampler2D/texture path; GLX and full GLES conformance remain unsupported.

The GLX application glxheads.c is also unchanged upstream src/xdemos/glxheads.c,
retrieved 2026-10-11. It links conventionally against system libGL.so.1; the
explicit gpu-glxheads.sh launcher scopes that name to the experimental client
provider. It does not change system Mesa, advertise desktop GL conformance or
provide Blender's 4.3 core context. Copyright/permission are retained in source.
