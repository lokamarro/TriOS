# ARM64 VDSO Build Configuration
#
# The VDSO gets inserted into the address space of emulated processes, so it
# needs to be compiled as an aarch64 ELF shared library. This requires a cross
# compiler and linker.
#
# On macOS, you need an aarch64 cross-compiler toolchain.
# On Linux, you can use the native compiler if running on arm64, or a cross compiler.

# Try to find an aarch64 compiler
clang = find_program('/usr/local/opt/llvm/bin/clang', '/opt/homebrew/opt/llvm/bin/clang', '/opt/local/bin/clang', 'clang', required: false)
has_vdso_compiler = false

if clang.found()
    # Check if the compiler supports aarch64 target
    check_cc = find_program('./check-cc-arm64.sh', required: false)
    if check_cc.found()
        result = run_command(check_cc, clang, check: false)
        if result.returncode() == 0
            has_vdso_compiler = true
        else
            message('ARM64 VDSO: Cross-compiler check failed')
            message(result.stdout() + result.stderr())
        endif
    endif
endif

if has_vdso_compiler
    vdso_compiler = [clang, '-target', 'aarch64-linux-gnu', '-fuse-ld=lld']

    vdso = custom_target('vdso-arm64',
        input: ['vdso.S', 'vdso.c', 'vdso.lds'],
        output: 'libvdso.so.elf',
        command: vdso_compiler + [
            '-o', '@OUTPUT@', '@INPUT0@', '@INPUT1@',
            '-nostdlib', '-Wl,-T,@INPUT2@', '-Wl,--hash-style,sysv',
            '-shared', '-fPIC', '-ffreestanding'
        ] + get_option('vdso_c_args').split())
else
    # Without clang + lld only an empty placeholder can be embedded: signal
    # handlers then return through a stack trampoline instead of the vDSO's
    # __kernel_rt_sigreturn. Release builds (app/xcode-meson.sh) require the
    # real vDSO so every IPA behaves the same (GitHub issue #3).
    if get_option('vdso_required')
        error('ARM64 vDSO required but no aarch64 clang + lld found. Install lld (brew install lld) and reconfigure.')
    endif
    message('ARM64 VDSO: No suitable cross-compiler/linker found, VDSO will not be built')
    # Create an empty file as placeholder
    vdso = custom_target('vdso-arm64-stub',
        output: 'libvdso.so.elf',
        command: ['touch', '@OUTPUT@'])
endif

# vdso_blob.c: the vDSO as a C array (a tracked dependency, unlike .incbin).
vdso_blob = custom_target('vdso-blob',
    input: vdso,
    output: 'vdso_blob.c',
    command: [find_program('python3'), files('embed.py'), '@INPUT@', '@OUTPUT@', '2'])
