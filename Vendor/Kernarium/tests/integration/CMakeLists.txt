# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT UML_INTEGRATION_ROOTFS)
    message(FATAL_ERROR
        "UML_BUILD_INTEGRATION_TESTS requires UML_INTEGRATION_ROOTFS")
endif()
if(NOT EXISTS "${UML_INTEGRATION_ROOTFS}")
    message(FATAL_ERROR
        "UML_INTEGRATION_ROOTFS does not exist: ${UML_INTEGRATION_ROOTFS}")
endif()

# Build a freestanding static arm64 guest payload into
# ${CMAKE_CURRENT_BINARY_DIR}/<NAME>, set OUTPUT_VARIABLE to its path, and
# make integration-test-binaries build it through the uml-<NAME> target.
# COMPILE_OPTIONS precede the common static-link flags; LINK_OPTIONS follow
# the entry-point flag.
function(uml_add_guest_payload OUTPUT_VARIABLE NAME SOURCE)
    cmake_parse_arguments(PARSE_ARGV 3 _payload ""
        "COMMENT" "COMPILE_OPTIONS;LINK_OPTIONS")
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/${NAME}")
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${CMAKE_CXX_COMPILER}"
            --target=aarch64-unknown-linux-gnu
            ${_payload_COMPILE_OPTIONS}
            -nostdlib
            -static
            -fuse-ld=lld
            -Wl,-e,_start
            ${_payload_LINK_OPTIONS}
            "${_source}"
            -o "${_output}"
        COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${_output}
            -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
        DEPENDS "${_source}"
        COMMENT "${_payload_COMMENT}"
        VERBATIM)
    add_custom_target(uml-${NAME} DEPENDS "${_output}")
    add_dependencies(integration-test-binaries uml-${NAME})
    set(${OUTPUT_VARIABLE} "${_output}" PARENT_SCOPE)
endfunction()

# uml_add_freestanding_cxx_payload(<var> <name> <source> COMMENT <text>
#     [ARCH_OPTIONS <flag>...] [NO_PIC])
#
# uml_add_guest_payload for a freestanding C++20 probe: warning-clean at -O2
# without exceptions, RTTI, a stack protector or unwind tables, and stripped.
# ARCH_OPTIONS carries the -march group the probe needs.
function(uml_add_freestanding_cxx_payload OUTPUT_VARIABLE NAME SOURCE)
    cmake_parse_arguments(PARSE_ARGV 3 _cxx "NO_PIC" "COMMENT"
        "ARCH_OPTIONS")
    set(_no_pic)
    if(_cxx_NO_PIC)
        set(_no_pic -fno-pic)
    endif()
    uml_add_guest_payload(${OUTPUT_VARIABLE} ${NAME} ${SOURCE}
        COMMENT "${_cxx_COMMENT}"
        COMPILE_OPTIONS
            -std=c++20
            -O2
            -Wall
            -Wextra
            -Werror
            ${_cxx_ARCH_OPTIONS}
            -ffreestanding
            -fno-exceptions
            -fno-rtti
            -fno-stack-protector
            -fno-asynchronous-unwind-tables
            ${_no_pic}
        LINK_OPTIONS -Wl,-s)
    set(${OUTPUT_VARIABLE} "${${OUTPUT_VARIABLE}}" PARENT_SCOPE)
endfunction()

option(UML_TEST_APPLE_AUDIO "Qualify the real Apple audio output with a short stereo tone" OFF)
if(UML_TEST_APPLE_AUDIO)
    add_subdirectory(audio)
endif()

# uml_add_guest_integration_test(NAME <test> SCRIPT <runner> [ARGS <arg>...]
#     CPUS <n> [GPUS <n>] [LABELS <label>...] [TIMEOUT <seconds>])
#
# Registers a rootfs integration test that runs the Python <runner>, relative
# to the calling directory, with uml-darwin and the integration rootfs and then
# ARGS. It reserves CPUS processors, and GPUS GPUs, from the resource spec.
function(uml_add_guest_integration_test)
    cmake_parse_arguments(_guest "" "NAME;SCRIPT;CPUS;GPUS;TIMEOUT"
        "ARGS;LABELS" ${ARGN})
    if(NOT _guest_SCRIPT OR NOT _guest_CPUS)
        message(FATAL_ERROR
            "${_guest_NAME}: guest integration tests require SCRIPT and CPUS")
    endif()
    set(_resources "cpus:${_guest_CPUS}")
    if(_guest_GPUS)
        string(APPEND _resources ",gpus:${_guest_GPUS}")
    endif()
    uml_add_integration_test(ROOTFS
        NAME "${_guest_NAME}"
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/${_guest_SCRIPT}"
            --emulator "$<TARGET_FILE:uml-darwin>"
            --rootfs "${UML_INTEGRATION_ROOTFS}"
            ${_guest_ARGS}
        LABELS ${_guest_LABELS}
        TIMEOUT "${_guest_TIMEOUT}"
        PROCESSORS "${_guest_CPUS}"
        RESOURCE_GROUPS "${_resources}")
endfunction()

set(UML_V4L2_PROBE_BINARY "" CACHE FILEPATH
    "Static guest V4L2 regression executable built from tests/integration/video/v4l2_decode_probe.cpp")
if(UML_V4L2_PROBE_BINARY)
    if(NOT EXISTS "${UML_V4L2_PROBE_BINARY}")
        message(FATAL_ERROR "V4L2 qualification requires its guest probe")
    endif()
    # The runner boots a two-vCPU guest.
    uml_add_guest_integration_test(
        NAME guest-v4l2-decode
        SCRIPT video/run_v4l2_decode.py
        ARGS --probe "${UML_V4L2_PROBE_BINARY}"
            --log "${CMAKE_CURRENT_BINARY_DIR}/v4l2-guest.log"
        LABELS guest;video
        TIMEOUT 600
        CPUS 2)
    # The same probe with the GPU: capture buffers are then host-visible blobs,
    # and a dma-buf a client imports takes the driver's other path.
    uml_add_guest_integration_test(
        NAME guest-v4l2-decode-gpu
        SCRIPT video/run_v4l2_decode.py
        ARGS --probe "${UML_V4L2_PROBE_BINARY}" --gpu
            --log "${CMAKE_CURRENT_BINARY_DIR}/v4l2-gpu-guest.log"
        LABELS guest;video;gpu
        TIMEOUT 600
        CPUS 2
        GPUS 1)
endif()

set(UML_V4L2_ENCODE_PROBE_BINARY "" CACHE FILEPATH
    "Static guest V4L2 encoder executable built from tests/integration/video/v4l2_encode_probe.cpp")
if(UML_V4L2_ENCODE_PROBE_BINARY)
    if(NOT EXISTS "${UML_V4L2_ENCODE_PROBE_BINARY}")
        message(FATAL_ERROR "V4L2 encoder qualification requires its guest probe")
    endif()
    # The runner boots a two-vCPU guest with the decoder and the encoder.
    uml_add_guest_integration_test(
        NAME guest-v4l2-encode
        SCRIPT video/run_v4l2_encode.py
        ARGS --probe "${UML_V4L2_ENCODE_PROBE_BINARY}"
            --log "${CMAKE_CURRENT_BINARY_DIR}/v4l2-encode-guest.log"
        LABELS guest;video
        TIMEOUT 720
        CPUS 2)
endif()

set(UML_V4L2_MMAP_CAPTURE_PROBE_BINARY "" CACHE FILEPATH
    "Static guest executable built from tests/integration/video/v4l2_mmap_capture_probe.cpp")
if(UML_V4L2_MMAP_CAPTURE_PROBE_BINARY)
    if(NOT EXISTS "${UML_V4L2_MMAP_CAPTURE_PROBE_BINARY}")
        message(FATAL_ERROR "V4L2 MMAP capture qualification requires its guest probe")
    endif()
    # The decoder's capture buffers are host-visible GPU blobs only in a guest
    # that has a GPU, which is why this is not part of guest-v4l2-decode.
    uml_add_guest_integration_test(
        NAME guest-v4l2-mmap-capture
        SCRIPT video/run_v4l2_mmap_capture.py
        ARGS --probe "${UML_V4L2_MMAP_CAPTURE_PROBE_BINARY}"
            --log "${CMAKE_CURRENT_BINARY_DIR}/v4l2-mmap-capture-guest.log"
        LABELS guest;video;gpu;smoke
        TIMEOUT 300
        CPUS 2
        GPUS 1)
endif()

set(UML_FUTEX_PROBE_BINARY "" CACHE FILEPATH
    "Static guest executable built from tests/integration/futex/futex_atomic_probe.cpp")
if(UML_FUTEX_PROBE_BINARY)
    if(NOT EXISTS "${UML_FUTEX_PROBE_BINARY}")
        message(FATAL_ERROR "Futex qualification requires its compiled guest probe")
    endif()
    # The runner boots a four-vCPU guest.
    uml_add_guest_integration_test(
        NAME guest-futex-atomic
        SCRIPT futex/run_futex_atomic.py
        ARGS --probe "${UML_FUTEX_PROBE_BINARY}"
            --log "${CMAKE_CURRENT_BINARY_DIR}/futex-guest.log"
        LABELS guest;futex
        TIMEOUT 320
        CPUS 4)
endif()

set(UML_GBM_DMABUF_PROBE_BINARY "" CACHE FILEPATH
    "Guest executable built from tests/integration/gpu/gbm_dmabuf_probe.cpp")
if(UML_GBM_DMABUF_PROBE_BINARY)
    if(NOT EXISTS "${UML_GBM_DMABUF_PROBE_BINARY}")
        message(FATAL_ERROR "The dma-buf visibility test requires its compiled guest probe")
    endif()
    # A four-vCPU guest with the GPU; the probe opens the render node itself.
    uml_add_guest_integration_test(
        NAME guest-gbm-dmabuf
        SCRIPT gpu/run_gpu_guest.py
        ARGS --timeout 300
            --memory 4G
            --guest-file
            /usr/local/libexec/gbm-dmabuf-probe=${UML_GBM_DMABUF_PROBE_BINARY}
            --command "timeout 60 sh -c 'until test -e /dev/dri/renderD128
do
    sleep 1
done'"
            --command "timeout 120 /usr/local/libexec/gbm-dmabuf-probe /dev/dri/renderD128"
            --
            ncpus=4 --execution-engine=jit --gpu --gpu-display=vnc
            --gpu-vnc-bind=127.0.0.1 --gpu-vnc-port=0
        LABELS guest;gpu
        TIMEOUT 420
        CPUS 4
        GPUS 1)
endif()

set(UML_DRM_FLIP_PROBE_BINARY "" CACHE FILEPATH
    "Guest executable built from tests/integration/gpu/drm_flip_probe.cpp")
if(UML_DRM_FLIP_PROBE_BINARY)
    if(NOT EXISTS "${UML_DRM_FLIP_PROBE_BINARY}")
        message(FATAL_ERROR "The vblank pacing test requires its compiled guest probe")
    endif()
    # A four-vCPU guest with the GPU; the probe takes the card's first output.
    uml_add_guest_integration_test(
        NAME guest-drm-vblank
        SCRIPT gpu/run_gpu_guest.py
        ARGS --timeout 300
            --memory 4G
            --guest-file
            /usr/local/libexec/drm-flip-probe=${UML_DRM_FLIP_PROBE_BINARY}
            --command "timeout 60 sh -c 'until test -e /dev/dri/card0
do
    sleep 1
done'"
            --command "timeout 60 /usr/local/libexec/drm-flip-probe"
            --
            ncpus=4 --execution-engine=jit --gpu --gpu-display=vnc
            --gpu-vnc-bind=127.0.0.1 --gpu-vnc-port=0
        LABELS guest;gpu
        TIMEOUT 420
        CPUS 4
        GPUS 1)
endif()

if(NOT UML_ACTIVE_KERNEL_ARTIFACT_TARGET OR
   NOT TARGET "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
    message(FATAL_ERROR "Integration tests require a selected kernel artifact")
endif()
get_target_property(_uml_integration_modules_builtin
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_MODULES_BUILTIN)
if(NOT _uml_integration_modules_builtin)
    message(FATAL_ERROR "Selected kernel artifact lacks modules.builtin evidence")
endif()

# The default is sized to the host: half its physical cores and 4 GiB of its
# memory per worker, between 1 and 16. A larger host sets UML_LTP_JOBS.
cmake_host_system_information(RESULT _uml_ltp_cores
    QUERY NUMBER_OF_PHYSICAL_CORES)
cmake_host_system_information(RESULT _uml_ltp_memory_mib
    QUERY TOTAL_PHYSICAL_MEMORY)
math(EXPR _uml_ltp_jobs_default "${_uml_ltp_cores} / 2")
math(EXPR _uml_ltp_jobs_by_memory "${_uml_ltp_memory_mib} / 4096")
if(_uml_ltp_jobs_by_memory LESS _uml_ltp_jobs_default)
    set(_uml_ltp_jobs_default ${_uml_ltp_jobs_by_memory})
endif()
if(_uml_ltp_jobs_default GREATER 16)
    set(_uml_ltp_jobs_default 16)
elseif(_uml_ltp_jobs_default LESS 1)
    set(_uml_ltp_jobs_default 1)
endif()
set(UML_LTP_JOBS ${_uml_ltp_jobs_default} CACHE STRING
    "Concurrent isolated guest workers for each LTP engine campaign")
if(NOT UML_LTP_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "UML_LTP_JOBS must be a positive integer")
endif()

# These are explicit guest campaigns, deliberately outside ordinary CTest.
add_custom_target(guest-smp-parallel
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/smp/run_guest_smp.py
        --emulator $<TARGET_FILE:uml-darwin>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    DEPENDS uml-darwin
    COMMENT "Running standalone guest SMP integration suite"
    USES_TERMINAL
    VERBATIM)

function(uml_add_ltp_engine_campaign ENGINE)
    set(_results
        "${PROJECT_BINARY_DIR}/results/refactor/ltp/${ENGINE}")
    add_custom_target(guest-ltp-${ENGINE}
        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/ltp/run_guest_ltp.py
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --manifest ${PROJECT_SOURCE_DIR}/tests/integration/ltp/verified-tests.tsv
            --modules-builtin ${_uml_integration_modules_builtin}
            --engine ${ENGINE}
            --results-dir ${_results}
            --jobs ${UML_LTP_JOBS}
        DEPENDS uml-darwin
        COMMENT "Running standalone ${ENGINE} guest LTP integration suite"
        USES_TERMINAL
        VERBATIM)
endfunction()

uml_add_ltp_engine_campaign(gadgets)
uml_add_ltp_engine_campaign(jit)
add_custom_target(guest-ltp-verified)
add_dependencies(guest-ltp-verified guest-ltp-gadgets guest-ltp-jit)

uml_add_guest_payload(UML_DOCKER_HELLO_WORLD docker-hello-world
    docker/hello_world.S
    COMMENT "Building the offline arm64 Docker hello-world payload")

uml_add_guest_payload(UML_GUEST_HWCAP_SANITY guest-hwcap-sanity
    features/guest_hwcap_sanity.S
    COMMENT "Building the arm64 guest HWCAP sanity payload")

# Guest campaigns share the EROFS image directly. The filesystem supplies the
# read-only contract; writable guest state lives in disposable overlays. The
# fast suite contains short real boots; full adds topology, MMU, and rendering.
uml_add_guest_integration_test(
    NAME guest-overlay-systemd-smoke
    SCRIPT gpu/run_gpu_guest.py
    ARGS --timeout 240
        --docker-hello-world-binary ${UML_DOCKER_HELLO_WORLD}
        --command "test -d /run/systemd/system && grep -qx systemd /proc/1/comm"
        --command "getent passwd uml >/dev/null"
        --command "test \"$(id -u uml)\" -ne 0"
        --command "test \"$(getent passwd uml | cut -d: -f6)\" = /home/uml"
        --command "runuser -u uml -- sudo -n true"
        --command "systemctl is-enabled --quiet getty@tty0.service"
        --command "timeout 30 sh -c 'until systemctl is-active --quiet getty@tty0.service
do
    sleep 1
done'"
        --command "timeout 60 sh -c 'until systemctl is-active --quiet docker.service
do
    sleep 1
done'"
        --command "docker info --format '{{.Driver}}' | grep -Eq '^overlay(fs|2)$'"
        --command "docker info --format '{{.DockerRootDir}}' | grep -qx /var/lib/docker"
        --command "test \"$(findmnt -n -o FSTYPE -T /var/lib/containerd)\" = tmpfs"
        --command "test -x /usr/bin/docker"
        --command "test -x /opt/ltp/testcases/bin/getpid01"
        --command "test -x /opt/node/bin/node"
        --command "set -- /opt/geekbench/*/geekbench6 && \"$1\" --no-upload --version >/dev/null"
        --command "test -x /usr/sbin/mkfs.ext2"
        --command "test -x /usr/sbin/mke2fs"
        --command "test -x /usr/sbin/mkfs.ext3"
        --command "test -x /usr/sbin/mkfs.ext4"
        --command "test -x /usr/sbin/debugfs"
        --command "test -x /usr/sbin/ip"
        --command "/usr/sbin/ip -Version 2>&1 | grep -q '^ip utility, iproute2-'"
        --command "test -x /usr/bin/bc"
        --command "test -x /usr/bin/quota"
        --command "test -x /usr/sbin/quotacheck"
        --command "test -x /usr/sbin/e4crypt"
        -- ncpus=10
    LABELS guest;overlay;systemd;smoke
    TIMEOUT 300
    CPUS 2)

function(uml_add_guest_engine_smoke ENGINE)
    set(_labels guest cpu ${ENGINE} dynamic-code smoke)
    uml_add_guest_integration_test(
        NAME guest-${ENGINE}-smoke
        SCRIPT jit/run_guest_jit_smoke.py
        ARGS --engine ${ENGINE}
            --hwcap-sanity-binary ${UML_GUEST_HWCAP_SANITY}
            --engine-support-tool $<TARGET_FILE:uml-sme-engine-support>
        LABELS ${_labels}
        TIMEOUT 420
        CPUS 2)
endfunction()

uml_add_guest_engine_smoke(gadgets)
uml_add_guest_engine_smoke(jit)

uml_add_guest_integration_test(
    NAME guest-slirp-network
    SCRIPT network/run_guest_network_smoke.py
    LABELS guest;network;slirp;smoke
    TIMEOUT 240
    CPUS 2)

uml_add_guest_integration_test(
    NAME guest-slirp-network-transfer
    SCRIPT network/run_guest_network_transfer.py
    LABELS guest;network;slirp
    TIMEOUT 300
    CPUS 2)

# The Virtio vsock device carries 64 MiB each way, concurrent streams, a stalled
# reader at either end, half-closes, a refused port, a suspend in the middle of
# a stream and a guest restart, all checked by SHA-256 through the launcher's
# loopback forward.
uml_add_guest_integration_test(
    NAME guest-vsock-stream
    SCRIPT vsock/run_guest_vsock_stream.py
    LABELS guest;vsock;network
    TIMEOUT 600
    CPUS 2)

add_subdirectory(aarch32)
add_subdirectory(android)
add_subdirectory(exclusive)
add_subdirectory(no_sve)
add_subdirectory(restart)
add_subdirectory(sme)

# One-vCPU guest: an input burst past the tty buffer must not end console input.
uml_add_guest_integration_test(
    NAME guest-console-input-burst
    SCRIPT console/run_guest_console_burst.py
    LABELS guest;console;smoke
    TIMEOUT 180
    CPUS 1)

# Four-vCPU guest: two CPUs draining one serial line must not split its input.
uml_add_guest_integration_test(
    NAME guest-console-concurrent-input
    SCRIPT console/run_guest_console_concurrent_input.py
    LABELS guest;console;smoke
    TIMEOUT 420
    CPUS 4)

# A terminal console leaves the emulator in its launcher's session and
# process group, so a signal to that group reaches the guest.
uml_add_guest_integration_test(
    NAME guest-console-session
    SCRIPT console/run_guest_console_session.py
    LABELS guest;console;smoke
    TIMEOUT 180
    CPUS 1)

# Two-vCPU guest: a Virtio console or UML line write allocates nothing, and
# a full line ring holds its writer back instead of losing output.
uml_add_guest_integration_test(
    NAME guest-console-write-memory
    SCRIPT console/run_guest_console_write_memory.py
    LABELS guest;console;smoke
    TIMEOUT 300
    CPUS 2)

# Two-vCPU guest: a serial line opens, and the kernel's boot log reaches the
# host, however late the host starts reading the console.
uml_add_guest_integration_test(
    NAME guest-console-late-reader
    SCRIPT console/run_guest_console_late_reader.py
    LABELS guest;console;smoke
    TIMEOUT 300
    CPUS 2)

# Two-vCPU guest: output a serial line still queues when the guest powers off
# reaches the host.
uml_add_guest_integration_test(
    NAME guest-console-queued-output
    SCRIPT console/run_guest_console_queued_output.py
    LABELS guest;console;smoke
    TIMEOUT 300
    CPUS 2)

# The interpreter runs BPF callbacks through its fixed entry-point pool.
uml_add_guest_integration_test(
    NAME guest-bpf-callbacks
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/bpf/bpf_callbacks_probe.py
        --ncpus 4 --probe-timeout 420
    LABELS guest;bpf;smoke
    TIMEOUT 600
    CPUS 4)

# The base archive and a per-run initramfs boot the EROFS image, and every
# broken initramfs stops the guest with a named reason.
if(NOT UML_BOOT_BASE_ARCHIVE OR NOT TARGET uml-initramfs)
    message(FATAL_ERROR
        "guest-boot-initramfs requires the accepted boot base and uml-initramfs")
endif()
uml_add_guest_integration_test(
    NAME guest-boot-initramfs
    SCRIPT boot/run_guest_boot_initramfs.py
    ARGS --base ${UML_BOOT_BASE_ARCHIVE}
        --initramfs-tool $<TARGET_FILE:uml-initramfs>
    LABELS guest;boot;smoke
    TIMEOUT 600
    CPUS 1)

# A guest run leaves nothing in the shared /tmp or its $TMPDIR, however it
# ends: poweroff, SIGTERM, SIGHUP, a guest panic, or SIGKILL followed by the
# next run.
uml_add_guest_integration_test(
    NAME guest-host-residue
    SCRIPT boot/run_guest_host_residue.py
    LABELS guest;boot;smoke
    TIMEOUT 600
    CPUS 1)

# A host device option the launcher cannot honour stops it with a named reason
# before any guest boots.
uml_add_integration_test(
    NAME launcher-host-device-refusals
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/boot/run_launcher_refusals.py"
        --emulator "$<TARGET_FILE:uml-darwin>"
    LABELS launcher;boot
    TIMEOUT 180)

# A faulting nofault kernel read resumes at its recovery point.
uml_add_guest_integration_test(
    NAME guest-kernel-nofault
    SCRIPT kernel/run_guest_kernel_nofault.py
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# A forced power-off with direct reads in flight on every vCPU stops the UBD
# queues before their workqueue goes, so it ends with no warning or panic.
uml_add_guest_integration_test(
    NAME guest-ubd-poweroff
    SCRIPT kernel/run_guest_ubd_poweroff.py
    LABELS guest;kernel;smoke
    TIMEOUT 600
    CPUS 4)

# A session record says how the guest ended: a kernel panic with its message,
# a power-off, the kernel's exit on SIGTERM, and "running" after a SIGKILL.
uml_add_guest_integration_test(
    NAME guest-session-record
    SCRIPT kernel/run_guest_session_record.py
    LABELS guest;kernel
    TIMEOUT 600
    CPUS 2)

# Every kind of UBD request, traced allocation by allocation, reaches the disk
# and reads back intact while the driver allocates no memory to submit or run
# it.
uml_add_guest_integration_test(
    NAME guest-ubd-request-memory
    SCRIPT kernel/run_guest_ubd_request_memory.py
    LABELS guest;kernel;smoke
    TIMEOUT 600
    CPUS 2)

# Evicting cached ext4 inodes that hold block preallocations, under memory
# pressure, releases the preallocations without a single __GFP_NOFAIL page
# allocation at reclaim time, and leaves a checked, unchanged filesystem.
uml_add_guest_integration_test(
    NAME guest-ext4-prealloc-reclaim
    SCRIPT kernel/run_guest_ext4_prealloc_reclaim.py
    LABELS guest;kernel
    TIMEOUT 900
    CPUS 4)

# Page-cache trimming is off until the host asks. The management console turns
# it on and off and runs one pass that leaves the cache at its floor and,
# compacting, hands most of what it frees to the host even when the cache lay
# between pages still in use; a trimming guest backs off while a working set it
# evicted is read back.
uml_add_guest_integration_test(
    NAME guest-page-cache-trim
    SCRIPT kernel/run_guest_page_cache_trim.py
    LABELS guest;kernel;smoke
    TIMEOUT 900
    CPUS 2)

# Removing the Virtio video decoder and encoder by unbind, management-console
# halt or forced power-off, while files are open and buffers queued, resets
# each first, returns everything it held and frees nothing an open file still
# uses.
uml_add_guest_integration_test(
    NAME guest-video-teardown
    SCRIPT kernel/run_guest_video_teardown.py
    LABELS guest;kernel;video;smoke
    TIMEOUT 600
    CPUS 4)

# Files of an LZ4 EROFS image with 4 KiB clusters, which EROFS decompresses
# through vm_map_ram() aliases of page-cache folios, read back exactly, again
# after the page cache is dropped.
uml_add_guest_integration_test(
    NAME guest-erofs-lz4
    SCRIPT kernel/run_guest_erofs_lz4.py
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# A vhost-user backend lost while its device is bound retires the device
# with no warning: removing a lost backend's virtqueues sends it nothing.
uml_add_guest_integration_test(
    NAME guest-vhost-user-backend-gone
    SCRIPT kernel/run_guest_vhost_user_backend_gone.py
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# Test support only: an external vhost-user backend that raises a config
# change every few milliseconds.
add_executable(uml-vhost-user-churn-server kernel/vhost_user_churn_server.cpp)
target_link_libraries(uml-vhost-user-churn-server PRIVATE
    UML::VhostUserTransport)
add_dependencies(integration-test-binaries uml-vhost-user-churn-server)

# On one vCPU, a device's config changes and the config reads they make the
# driver's worker send cross thousands of times, and neither side of the
# connection may wait on its channel alone. The device has the ID and the
# shape of a Virtio console, whose driver reads its size on every change.
uml_add_guest_integration_test(
    NAME guest-vhost-user-config-crossing
    SCRIPT kernel/run_guest_vhost_user_config_crossing.py
    ARGS --server $<TARGET_FILE:uml-vhost-user-churn-server>
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 1)

# A system suspend holds through network, console and vhost-user traffic
# until a wake request, and a wake sent while it is still being entered
# ends it, even while it still syncs file systems. A suspend the host asked
# for that one of the guest's own wake sources ended is entered again. A
# suspend the guest entered by itself ends for a wake, for a power key typed
# on the VNC display, which reaches the guest once, or for a network frame,
# which never ends or fails one the host asked for. A suspend whose freezer is
# held by an unanswered FUSE request gives no reply, answers a wake while it
# blocks, and then fails on its own socket, so a host can abort it at a deadline.
uml_add_guest_integration_test(
    NAME guest-suspend-holds
    SCRIPT kernel/run_guest_suspend_holds.py
    ARGS --server $<TARGET_FILE:uml-vhost-user-churn-server>
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# A Virtio sound stream starts afresh after a system suspend the guest enters
# by itself: the position of a stream suspended while playing, a prepare after
# a half-filled period, and a stream paused across a suspend, which recovers
# and opens again. The guest has the null audio output.
uml_add_guest_integration_test(
    NAME guest-virtio-sound-suspend
    SCRIPT kernel/run_guest_virtio_sound_suspend.py
    LABELS guest;kernel;audio
    TIMEOUT 300
    CPUS 2)

# The delay a playing Virtio sound stream reports, which a player keeps its
# pictures in step with, says when the frame written last is heard: the
# frames queued, and on the null audio output nothing more.
uml_add_guest_integration_test(
    NAME guest-virtio-sound-delay
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/audio/virtio_sound_delay_probe.py
        --guest-argument=--audio-output=null
        --console-failure "WARNING:|Oops|BUG:|Call Trace:"
        --ncpus 2 --probe-timeout 60
    LABELS guest;audio
    TIMEOUT 300
    CPUS 2)
if(UML_TEST_APPLE_AUDIO)
    # The same on the host's output, which adds its own latency: the same
    # for every period, and under half a second.
    uml_add_guest_integration_test(
        NAME guest-virtio-sound-delay-host
        SCRIPT support/run_guest_probe.py
        ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/audio/virtio_sound_delay_probe.py
            --guest-argument=--audio-output=host --probe-argument=500
            --console-failure "WARNING:|Oops|BUG:|Call Trace:"
            --ncpus 2 --probe-timeout 60
        LABELS guest;audio
        TIMEOUT 300
        CPUS 2)
endif()

# Path and name strings copied from user memory keep their contract.
uml_add_guest_integration_test(
    NAME guest-user-strings
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/user_strings_probe.py
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# Test support only: the Virtio touchscreen's capabilities and the reports its
# Touchscreen model sends for a few contact sequences, as JSON.
add_executable(uml-touchscreen-reports input/touchscreen_reports.cpp)
target_link_libraries(uml-touchscreen-reports PRIVATE UML::VirtioInput)
add_dependencies(integration-test-binaries uml-touchscreen-reports)

# A contact the host cancels is a palm and then a lift, which the guest's input
# core passes on as written and libinput turns into a cancelled touch with no
# touch up, for one finger of two as well, and a slot that held a palm takes a
# finger again. The guest builds an input device with the touchscreen's own
# capabilities and replays the reports of the host's model.
uml_add_guest_integration_test(
    NAME guest-touch-cancel
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/input/touch_cancel_probe.py
        --probe-input $<TARGET_FILE:uml-touchscreen-reports>
        --ncpus 2 --probe-timeout 120
    LABELS guest;input;smoke
    TIMEOUT 300
    CPUS 2)

# The kernel describes the virtual machine in SMBIOS tables that userspace
# reads from sysfs, and provides no /dev/mem.
uml_add_guest_integration_test(
    NAME guest-kernel-dmi
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/dmi_probe.py
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# The host threads the guest kernel creates carry their names.
uml_add_guest_integration_test(
    NAME guest-kernel-thread-names
    SCRIPT kernel/run_guest_thread_names.py
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# What a macOS launcher prints with --stats: the vCPU load against libproc and
# the guest's /proc/stat, the frames of an offscreen display, and the guest's
# memory, as its kernel's `mem` console request reports it, against
# /proc/meminfo, and none from a console that refuses the request.
uml_add_guest_integration_test(
    NAME guest-statistics
    SCRIPT kernel/run_guest_statistics.py
    LABELS guest;kernel;gpu
    TIMEOUT 300
    CPUS 2
    GPUS 1)

# Unmapping user pages beside the native window leaves the kernel's own
# mappings in it alone.
uml_add_guest_integration_test(
    NAME guest-native-window-unmap
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/native_window_unmap_probe.py
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# A tracer's PSTATE writes are filtered as native arm64 filters them.
uml_add_guest_integration_test(
    NAME guest-ptrace-pstate
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/ptrace_pstate_probe.py
        --ncpus 2 --probe-timeout 60
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# A tracer follows the system calls of an A64 task and, on arm64, an AArch32
# one as on native Linux: the entry stop shows the first argument in its
# register, a rewrite there is the argument the call uses, the exit stop shows
# the return value, syscall(-1) returns ENOSYS, and an interrupted call that a
# handler restarts shows its argument again. Both engines run it, because a
# register image crosses the host boundary at every system call.
foreach(_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-ptrace-syscall-args-${_engine}
        SCRIPT support/run_guest_probe.py
        ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/ptrace_syscall_args_probe.cpp
            "--cxxflags=-Wall -Wextra -Werror"
            --engine ${_engine} --ncpus 2 --probe-timeout 240
        LABELS guest;kernel;${_engine}
        TIMEOUT 480
        CPUS 2)
endforeach()

# The AArch64 tagged-address ABI (Documentation/arch/arm64/tagged-address-abi.rst)
# answers as native arm64 Linux does, row for row: PR_SET/GET_TAGGED_ADDR_CTRL,
# tagged pointers in the system calls that touch user memory, the abi sysctl,
# NT_ARM_TAGGED_ADDR_CTRL, and inheritance across fork, exec, threads and an
# AArch32 task. The engines ignore the top byte unless the host passes
# --guest-no-top-byte-ignore, so the guest runs with the default on both
# engines and the probe expects the ABI to be available. With the off switch
# enabling the ABI is refused, and the probe checks the refusal and the rows
# that need no tagged address.
foreach(_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-tagged-addr-${_engine}
        SCRIPT support/run_guest_probe.py
        ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/tagged_addr_probe.cpp
            "--cxxflags=-Wall -Wextra -Werror -pthread"
            --engine ${_engine} --ncpus 2 --probe-timeout 240
            --probe-argument=--tbi=on
        LABELS guest;kernel;${_engine}
        TIMEOUT 600
        CPUS 2)
endforeach()
uml_add_guest_integration_test(
    NAME guest-tagged-addr-no-tbi
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/tagged_addr_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror -pthread"
        --ncpus 2 --probe-timeout 240
        --guest-argument=--guest-no-top-byte-ignore --probe-argument=--tbi=off
    LABELS guest;kernel;smoke
    TIMEOUT 600
    CPUS 2)

# A signal taken between two instructions leaves x0 alone even when it holds
# an -ERESTART* code: only a system call's own return is restarted.
uml_add_guest_integration_test(
    NAME guest-signal-restart
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/signal_restart_probe.py
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;smoke
    TIMEOUT 300
    CPUS 2)

# Unwinders cross an arm64 signal frame as on native Linux: backtrace() in a
# handler and cancellation of a thread blocked in a system call.
uml_add_guest_integration_test(
    NAME guest-signal-unwind
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/signal_unwind_probe.cpp
        "--cxxflags=-g -fno-omit-frame-pointer -rdynamic -pthread"
        --ncpus 2 --probe-timeout 150
    LABELS guest;kernel;smoke
    TIMEOUT 420
    CPUS 2)

# A TLB replay that sends neighbouring user PTEs as one host call maps each
# page to its own frame and protection, across PMD and batch boundaries and
# while another thread's invalidations make it walk batches again.
uml_add_guest_integration_test(
    NAME guest-tlb-replay-runs
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/tlb_runs_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror -pthread"
        --ncpus 2 --probe-timeout 300
    LABELS guest;kernel;memory
    TIMEOUT 600
    CPUS 2)

# A kernel copy into a private page that a sibling's fork() keeps
# write-protecting succeeds: uaccess retries its copy-on-write fault until the
# fault handler itself refuses, as native uaccess re-executes a faulting access.
uml_add_guest_integration_test(
    NAME guest-uaccess-fork-cow
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/uaccess_fork_cow_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror -pthread"
        --ncpus 4 --probe-timeout 120
    LABELS guest;kernel;memory
    TIMEOUT 420
    CPUS 4)

# A parent's kernel write into its private page after fork() never reaches the
# child: a uaccess write pins its page, so fork() copies it for the child.
uml_add_guest_integration_test(
    NAME guest-uaccess-fork-isolation
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/uaccess_fork_isolation_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror -pthread"
        --ncpus 4 --probe-timeout 120
    LABELS guest;kernel;memory
    TIMEOUT 420
    CPUS 4)

# The kernel's own access to a user range reaches userfaultfd as native Linux
# delivers it: a UFFD_USER_MODE_ONLY descriptor never sees it and the system
# call fails with EFAULT, while a user-mode access and a plain descriptor's
# kernel access are delivered. Android's ART collector depends on both.
uml_add_guest_integration_test(
    NAME guest-uaccess-userfaultfd
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/uaccess_userfaultfd_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror -pthread"
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;memory
    TIMEOUT 420
    CPUS 2)

# A mapping that permits writes permits reads, as native arm64 and x86 allow
# it, for a task and for the kernel's own access: the first load from a
# PROT_WRITE mapping (anonymous or file, shared or private) is not a SIGSEGV.
# An execute-only mapping reads too, while PROT_NONE and a store to a
# read-only mapping still fault with SEGV_ACCERR.
uml_add_guest_integration_test(
    NAME guest-write-only-mapping
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/write_only_mapping_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror"
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;memory
    TIMEOUT 420
    CPUS 2)

# A blob the host refuses never becomes a handle: the ioctl that creates a
# blob waits for the host's answer and fails with EINVAL or ENOMEM, in the
# host3d and guest-blob branches alike, instead of returning a GEM handle that
# no host resource stands behind. The guest has the GPU. The probe names host
# objects that do not exist (EINVAL), and asks for a resource larger than the
# GPU budget of a 1 GiB guest, made inside the blob's command buffer as gralloc
# does (ENOMEM).
uml_add_guest_integration_test(
    NAME guest-virtgpu-blob-refusal
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/virtgpu_blob_refusal_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror"
        --guest-argument=--gpu --guest-argument=--gpu-display=vnc
        --guest-argument=--gpu-vnc-bind=127.0.0.1 --guest-argument=--gpu-vnc-port=0
        --ncpus 4 --probe-timeout 240
    LABELS guest;kernel;gpu
    TIMEOUT 600
    CPUS 4
    GPUS 1)

# A Virtio-GPU context names the process that created it: the kernel appends
# "|pid=<tgid>|comm=<comm>" to the debug name, and the host credits the memory
# a context holds to that process (docs/graphics.md). Three guest processes
# hold memory in a Virgl context, a Venus context and a Virgl context whose
# process name imitates the suffix; the runner checks the holders the launcher
# prints with --stats.
uml_add_guest_integration_test(
    NAME guest-virtgpu-context-owner
    SCRIPT kernel/run_guest_virtgpu_context_owner.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/virtgpu_context_owner_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror"
        --guest-argument=--gpu --guest-argument=--gpu-display=vnc
        --guest-argument=--gpu-vnc-bind=127.0.0.1 --guest-argument=--gpu-vnc-port=0
        --guest-argument=--stats=500
        --ncpus 4 --probe-timeout 300
    LABELS guest;kernel;gpu
    TIMEOUT 900
    CPUS 4
    GPUS 1)

# The host tells the guest how near its graphics memory budget is and which
# guest processes hold it, in the root-only file gpu_memory_report on the DRM
# device (docs/graphics.md, kernel/common/README.md). The probe reads it as
# Android's lmkd does: it arms the file, waits with poll(), fills the budget
# with a process, filters the holders, ends the process, has an allocation
# refused and disarms; the runner checks the launcher's --stats line for
# guest-reclaim=on while the file is armed.
uml_add_guest_integration_test(
    NAME guest-virtgpu-memory-report
    SCRIPT kernel/run_guest_virtgpu_memory_report.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/virtgpu_memory_report_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror"
        --guest-argument=--gpu --guest-argument=--gpu-display=vnc
        --guest-argument=--gpu-vnc-bind=127.0.0.1 --guest-argument=--gpu-vnc-port=0
        --guest-argument=--stats=500
        --ncpus 4 --probe-timeout 420
    LABELS guest;kernel;gpu
    TIMEOUT 1000
    CPUS 4
    GPUS 1)

# The settings the Android CTS and the desktop disagree on: a Linux guest has
# SysV IPC, the NFS client and SCTP, no RDS and no wakelock interface, and
# runs the helper a piped core_pattern names; every guest has IPv4 multicast,
# no listener on RDS-over-TCP's port, and the strong stack protector and
# hardened usercopy, a macvlan link that rtnetlink creates over a dummy one,
# the accept_ra_rt_table sysctl (0 until it is written, then a prefix route in
# the device's own table where the kernel has IPv6 multiple tables), the
# Virtio vsock driver registered with no balloon driver beside it and a vsock
# stream that loops back on CID 1, and one ESP SPI allocated for a second
# destination address, which a Linux guest refuses and an Android guest
# allows. A Linux guest has no Incremental file system and no 802.11 stack. An
# Android guest, which runs the probe by hand, has a static usermode helper
# that starts nothing, an Incremental file system that mounts, serves a filled
# block and times out on a missing one, and virtual Wi-Fi: a virt_wifi link
# over a macvlan connects to the access point VirtWifi through nl80211, remains
# on 2412 MHz, sends an Action frame there, and passes a multicast group it
# joins on to its macvlan.
uml_add_guest_integration_test(
    NAME guest-kernel-config
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/kernel_config_probe.cpp
        "--cxxflags=-Wall -Wextra -Werror"
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;smoke
    TIMEOUT 420
    CPUS 2)

# A system call or a fault taken with a bad stack pointer, again and again
# on one vCPU, gets native arm64's result and never kills the task, and the
# kernel prints no failed Darwin host call.
uml_add_guest_integration_test(
    NAME guest-stack-pointer
    SCRIPT support/run_guest_probe.py
    ARGS --probe ${PROJECT_SOURCE_DIR}/tests/integration/kernel/stack_pointer_probe.cpp
        --console-failure "Darwin: .*failed"
        --ncpus 2 --probe-timeout 120
    LABELS guest;kernel;smoke
    TIMEOUT 420
    CPUS 2)

# Memory the kernel shares with user space, through a vmalloc or vmap alias
# (BPF arrays and ring buffers, perf, PACKET_MMAP, io_uring), reads the same
# from both sides. An alias that is not whole groups of four pages is a
# private copy of its frames, and a ring in the application's scattered pages
# cannot be shared and is refused.
uml_add_freestanding_cxx_payload(UML_GUEST_SHARED_MEMORY_PROBE
    guest-shared-memory-probe
    kernel/shared_memory_probe.cpp
    COMMENT "Building the freestanding arm64 kernel/user shared-memory probe"
    ARCH_OPTIONS -march=armv8.1-a -mno-outline-atomics)

uml_add_guest_integration_test(
    NAME guest-kernel-user-shared-memory
    SCRIPT kernel/run_guest_shared_memory.py
    ARGS --probe ${UML_GUEST_SHARED_MEMORY_PROBE}
        --cpus 4
    LABELS guest;kernel;memory
    TIMEOUT 900
    CPUS 4)

# A stopped embedded guest returns to its host only after every kernel
# thread has left the image the loader then unmaps.
add_executable(uml_run_end_stop_tests kernel/run_end_stop_tests.cpp)
target_link_libraries(uml_run_end_stop_tests PRIVATE UML::Emulator)
# The embedded ELF resolves the host ABI with dlsym(), as in uml-darwin.
target_link_options(uml_run_end_stop_tests PRIVATE
    "LINKER:-export_dynamic"
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
add_dependencies(uml_run_end_stop_tests "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
add_dependencies(integration-test-binaries uml_run_end_stop_tests)
get_target_property(_uml_run_end_kernel
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
uml_add_integration_test(
    NAME guest-run-end-stop
    COMMAND uml_run_end_stop_tests
        ${_uml_run_end_kernel}
        ${CMAKE_CURRENT_BINARY_DIR}/run-end-stop
        20
    LABELS guest;kernel
    TIMEOUT 600
    PROCESSORS 4
    RESOURCE_GROUPS cpus:4)

# A Virtio video command that its device answers after the driver gave up on
# it reaches only memory the driver kept for it, and an open that fails after
# the device made its stream destroys that stream. The device is the
# production one behind a test-only shim that answers late.
uml_add_freestanding_cxx_payload(UML_GUEST_VIDEO_LATE_ANSWER_INIT
    guest-video-late-answer-init
    video/video_late_answer_init.cpp
    COMMENT "Building the freestanding arm64 init of the late video answer test"
    ARCH_OPTIONS -march=armv8-a)
add_executable(uml_video_late_answer_tests video/video_late_answer_tests.cpp)
target_link_libraries(uml_video_late_answer_tests PRIVATE
    UML::BootInitramfs
    UML::DelayedAnswerDevice
    UML::Emulator
    UML::VideoDevice)
target_link_options(uml_video_late_answer_tests PRIVATE
    "LINKER:-export_dynamic"
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
add_dependencies(uml_video_late_answer_tests
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
add_dependencies(integration-test-binaries uml_video_late_answer_tests)
get_target_property(_uml_video_late_answer_kernel
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
uml_add_integration_test(
    NAME guest-video-late-answer
    COMMAND uml_video_late_answer_tests
        ${_uml_video_late_answer_kernel}
        ${UML_GUEST_VIDEO_LATE_ANSWER_INIT}
        ${CMAKE_CURRENT_BINARY_DIR}/video-late-answer
    LABELS guest;kernel;video
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

# A capture buffer a Virtio video device answers as failed reaches the client
# flagged as an error, empty, and with the timestamp of the answer. A host
# drops a picture only when its codec does, so the device is the production one
# behind a test-only shim that answers the guest's capture buffer as failed.
uml_add_freestanding_cxx_payload(UML_GUEST_VIDEO_ERROR_ANSWER_INIT
    guest-video-error-answer-init
    video/video_error_answer_init.cpp
    COMMENT "Building the freestanding arm64 init of the failed video buffer test"
    ARCH_OPTIONS -march=armv8-a)
add_executable(uml_video_error_answer_tests video/video_error_answer_tests.cpp)
target_link_libraries(uml_video_error_answer_tests PRIVATE
    UML::BootInitramfs
    UML::DelayedAnswerDevice
    UML::Emulator
    UML::VideoDevice)
target_link_options(uml_video_error_answer_tests PRIVATE
    "LINKER:-export_dynamic"
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
add_dependencies(uml_video_error_answer_tests
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
add_dependencies(integration-test-binaries uml_video_error_answer_tests)
get_target_property(_uml_video_error_answer_kernel
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
uml_add_integration_test(
    NAME guest-video-error-answer
    COMMAND uml_video_error_answer_tests
        ${_uml_video_error_answer_kernel}
        ${UML_GUEST_VIDEO_ERROR_ANSWER_INIT}
        ${CMAKE_CURRENT_BINARY_DIR}/video-error-answer
    LABELS guest;kernel;video
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

# Configuration reads and in-band kicks that several vCPUs start at once on
# one vhost-user socket each keep their own reply: a console that keeps
# resizing, and answers late, stays bound while every vCPU writes to it.
uml_add_freestanding_cxx_payload(UML_GUEST_VHOST_USER_ROUND_TRIPS_INIT
    guest-vhost-user-round-trips-init
    kernel/vhost_user_round_trips_init.cpp
    COMMENT "Building the freestanding arm64 init of the vhost-user round-trip test"
    ARCH_OPTIONS -march=armv8-a)
add_executable(uml_vhost_user_round_trips_tests
    kernel/vhost_user_round_trips_tests.cpp)
target_link_libraries(uml_vhost_user_round_trips_tests PRIVATE
    UML::BootInitramfs
    UML::Emulator
    UML::VhostUserTransport)
target_link_options(uml_vhost_user_round_trips_tests PRIVATE
    "LINKER:-export_dynamic"
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
add_dependencies(uml_vhost_user_round_trips_tests
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
add_dependencies(integration-test-binaries uml_vhost_user_round_trips_tests)
get_target_property(_uml_vhost_user_round_trips_kernel
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
uml_add_integration_test(
    NAME guest-vhost-user-round-trips
    COMMAND uml_vhost_user_round_trips_tests
        ${_uml_vhost_user_round_trips_kernel}
        ${UML_GUEST_VHOST_USER_ROUND_TRIPS_INIT}
        ${CMAKE_CURRENT_BINARY_DIR}/vhost-user-round-trips
    LABELS guest;kernel
    TIMEOUT 300
    PROCESSORS 4
    RESOURCE_GROUPS cpus:4)

# A suspend that takes a vhost-user backend request inside a ring disable's
# reply wait still ends at its RTC alarm: the irq_work that delivers the
# request runs inside the suspend and must find the request socket still
# nonblocking.
uml_add_freestanding_cxx_payload(UML_GUEST_VHOST_USER_SUSPEND_DRAIN_INIT
    guest-vhost-user-suspend-drain-init
    kernel/vhost_user_suspend_drain_init.cpp
    COMMENT "Building the freestanding arm64 init of the vhost-user suspend-drain test"
    ARCH_OPTIONS -march=armv8-a)
add_executable(uml_vhost_user_suspend_drain_tests
    kernel/vhost_user_suspend_drain_tests.cpp)
target_link_libraries(uml_vhost_user_suspend_drain_tests PRIVATE
    UML::BootInitramfs
    UML::Emulator
    UML::VhostUserTransport)
target_link_options(uml_vhost_user_suspend_drain_tests PRIVATE
    "LINKER:-export_dynamic"
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
add_dependencies(uml_vhost_user_suspend_drain_tests
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
add_dependencies(integration-test-binaries uml_vhost_user_suspend_drain_tests)
uml_add_integration_test(
    NAME guest-vhost-user-suspend-drain
    COMMAND uml_vhost_user_suspend_drain_tests
        ${_uml_vhost_user_round_trips_kernel}
        ${UML_GUEST_VHOST_USER_SUSPEND_DRAIN_INIT}
        ${CMAKE_CURRENT_BINARY_DIR}/vhost-user-suspend-drain
    LABELS guest;kernel
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

uml_add_guest_payload(UML_GUEST_SAMPLER_WORKLOAD guest-sampler-workload
    profiling/guest_sampler_workload.S
    COMMENT "Building the freestanding arm64 guest sampler workload")

uml_add_guest_integration_test(
    NAME guest-code-sampler
    SCRIPT profiling/run_guest_code_sampler.py
    ARGS --workload ${UML_GUEST_SAMPLER_WORKLOAD}
    LABELS guest;profiling
    TIMEOUT 300
    CPUS 2)

uml_add_guest_payload(UML_GUEST_SHARED_MMAP_STRESS guest-shared-mmap-stress
    smp/guest_shared_mmap_stress.S
    COMMENT "Building the freestanding arm64 shared-MM guest stress binary")

uml_add_freestanding_cxx_payload(UML_GUEST_FORK_COW_COHERENCE
    guest-fork-cow-coherence
    smp/guest_fork_cow_coherence.cpp
    COMMENT "Building the freestanding arm64 fork copy-on-write guest stress binary"
    ARCH_OPTIONS -march=armv8.1-a -mno-outline-atomics)

uml_add_freestanding_cxx_payload(UML_GUEST_CODE_CHANGE guest-code-change
    smp/guest_code_change.cpp
    COMMENT "Building the freestanding arm64 guest code-change probe"
    ARCH_OPTIONS -march=armv8.1-a -mno-outline-atomics)

# Code changed by stores, protection, fork, remapping and the address space's
# other vCPUs, on each engine.
foreach(_code_change_engine IN ITEMS gadgets jit)
    uml_add_guest_integration_test(
        NAME guest-code-change-${_code_change_engine}
        SCRIPT smp/run_guest_code_change.py
        ARGS --probe ${UML_GUEST_CODE_CHANGE} --cpus 4
            --engine ${_code_change_engine}
        LABELS guest;smp;dynamic-code;${_code_change_engine}
        TIMEOUT 600
        CPUS 4)
endforeach()

uml_add_guest_integration_test(
    NAME guest-mmu-stress
    SCRIPT smp/run_guest_mmu_stress.py
    ARGS --shared-mmap-binary ${UML_GUEST_SHARED_MMAP_STRESS}
        --fork-cow-binary ${UML_GUEST_FORK_COW_COHERENCE}
        --cpus 10
    LABELS guest;memory;mmu;full;pressure
    TIMEOUT 600
    CPUS 10)
# The runner skips on a host with fewer than the ten CPUs it asks for.
set_tests_properties(guest-mmu-stress PROPERTIES SKIP_RETURN_CODE 77)

# A process with many dirty shared VMAs exits in time linear in them. The mm
# teardown flushes once per page table that holds a dirty shared page, and a
# flush that walked every VMA made the exit quadratic: tens of seconds at
# 20000 VMAs against a few tens of milliseconds.
uml_add_freestanding_cxx_payload(UML_GUEST_EXIT_SHARED_VMAS
    guest-exit-shared-vmas
    smp/guest_exit_shared_vmas.cpp
    COMMENT "Building the freestanding arm64 shared-VMA exit probe"
    ARCH_OPTIONS -march=armv8-a)

uml_add_guest_integration_test(
    NAME guest-exit-shared-vmas
    SCRIPT smp/run_guest_exit_shared_vmas.py
    ARGS --probe ${UML_GUEST_EXIT_SHARED_VMAS}
        --cpus 4
    LABELS guest;memory;mmu
    TIMEOUT 600
    CPUS 4)

if(UML_ENABLE_EXTERNAL_NETWORK_TESTS)
    uml_add_guest_integration_test(
        NAME guest-external-network
        SCRIPT network/run_guest_network_smoke.py
        ARGS --external
            --external-host ${UML_EXTERNAL_NETWORK_TEST_HOST}
        LABELS guest;network;external;full
        TIMEOUT 300
        CPUS 2)

    # guest-overlay-systemd-smoke proves Docker runs a locally imported image
    # with no network at all.  This proves the rest of the path a real user
    # takes: registry DNS and TLS over SLiRP, an image pull, and container
    # networking over the docker0 bridge.  It also exercises a large amount of
    # Go: dockerd, containerd and the client all read HWCAP-gated system
    # registers at startup, which is how the missing MRS/MSR DIT encoding was
    # found.  A benchmark cannot see that failure; nothing in Geekbench
    # executes those instructions.
    uml_add_guest_integration_test(
        NAME guest-docker-hello-world
        SCRIPT gpu/run_gpu_guest.py
        ARGS --timeout 600
            --command "timeout 180 sh -c 'until systemctl is-active --quiet docker.service
do
    sleep 1
done'"
            --command "docker run --rm hello-world | grep -Fxq 'Hello from Docker!'"
            --command "docker image rm -f hello-world >/dev/null"
            -- ncpus=10
        LABELS guest;docker;network;external;full
        TIMEOUT 900
        CPUS 4)

    uml_add_guest_integration_test(
        NAME guest-kde-vnc-webgl-aquarium
        SCRIPT gpu/run_chromium_webgl.py
        ARGS --client $<TARGET_FILE:uml-vnc-smoke-client>
        LABELS guest;gpu;vnc;webgl;external;full
        TIMEOUT 720
        CPUS 10
        GPUS 1)

    uml_add_guest_integration_test(
        NAME guest-webgl-rendered-workflows
        SCRIPT gpu/run_webgl_samples.py
        ARGS --client $<TARGET_FILE:uml-vnc-smoke-client>
        LABELS guest;gpu;webgl;external;full;pressure
        TIMEOUT 1800
        CPUS 10
        GPUS 1)
endif()

# The iOS slice renders through Mesa Zink on KosmicKrisp, which requires Metal
# 4; the iOS Simulator publishes no Metal 4 device, so the unmodified device
# build is verified by running it on this Apple silicon Mac in Designed for
# iPad mode. That needs Xcode's local install service and a sealed rendered
# image the application copies into its own container, neither of which a
# checkout provides, so the lane is opt-in: name the image and it registers.
if(APPLE AND UML_IOS_MAC_CTS_ROOTFS)
    if(NOT EXISTS "${UML_IOS_MAC_CTS_ROOTFS}")
        message(FATAL_ERROR
            "UML_IOS_MAC_CTS_ROOTFS does not exist: ${UML_IOS_MAC_CTS_ROOTFS}")
    endif()
    if(NOT UML_IOS_MAC_CTS_ROOTFS_RECEIPT)
        message(FATAL_ERROR
            "The iOS-on-Mac CTS lane needs the image's identity receipt in "
            "UML_IOS_MAC_CTS_ROOTFS_RECEIPT")
    endif()
    if(NOT UML_IOS_MAC_CTS_CAMPAIGN)
        set(UML_IOS_MAC_CTS_CAMPAIGN "gles3-identity-sentinel-v1")
    endif()
    uml_add_integration_test(
        NAME apple-ios-mac-vk-gl-cts
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_ios_mac_vk_gl_cts.py
            --campaign ${UML_IOS_MAC_CTS_CAMPAIGN}
            --rootfs ${UML_IOS_MAC_CTS_ROOTFS}
            --rootfs-receipt ${UML_IOS_MAC_CTS_ROOTFS_RECEIPT}
            --initramfs-tool $<TARGET_FILE:uml-initramfs>
            --artifact-root ${CMAKE_CURRENT_BINARY_DIR}/ios-mac-cts
        LABELS gpu;apple;ios;external;full
        TIMEOUT 5400
        PROCESSORS 8
        RESOURCE_GROUPS cpus:8,gpus:1)
endif()

# The device counterpart drives a connected iOS device through devicectl. The
# guest runs on the device, so the host only holds a console; what a checkout
# cannot provide is the paired device with the automation application and the
# image installed, so this lane is opt-in too: name the device and it
# registers.
if(APPLE AND UML_IOS_DEVICE_VK_CTS_DEVICE)
    if(NOT UML_IOS_DEVICE_VK_CTS_CAMPAIGN)
        set(UML_IOS_DEVICE_VK_CTS_CAMPAIGN "vulkan-info-receipt-smoke-v1")
    endif()
    uml_add_integration_test(
        NAME apple-ios-device-vk-cts
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_ios_device_vk_cts.py
            --device ${UML_IOS_DEVICE_VK_CTS_DEVICE}
            --campaign ${UML_IOS_DEVICE_VK_CTS_CAMPAIGN}
            --initramfs-tool $<TARGET_FILE:uml-initramfs>
            --artifact-root ${CMAKE_CURRENT_BINARY_DIR}/ios-device-vk-cts
        LABELS gpu;apple;ios;external;full
        TIMEOUT 5400
        PROCESSORS 1)
endif()
