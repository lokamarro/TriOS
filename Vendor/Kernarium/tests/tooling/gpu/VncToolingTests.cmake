# SPDX-License-Identifier: GPL-2.0-or-later

if(TARGET uml-virtio-gpu AND TARGET uml-vnc-smoke-client)
    # The runner waits for the server's announcement and for each client's
    # exit, so host load only makes it slower. Its wedge bound is for a
    # process that never answers, and the CTest bound sits a minute above it
    # so the runner reports the wedge and stops the server itself.
    set(_uml_vnc_smoke_wedge_timeout 60)
    set(_uml_vnc_smoke_test_timeout 120)
    if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=")
        # Host GL provider initialization is intentionally exercised by this
        # test
        # and is substantially slower when the host executable is instrumented.
        set(_uml_vnc_smoke_wedge_timeout 180)
        set(_uml_vnc_smoke_test_timeout 240)
        if(TARGET uml_third_party_kosmickrisp)
            # The Venus product's larger statically linked renderer takes
            # about four minutes to initialize in an unoptimized ASan+UBSan
            # build on the supported Apple Silicon baseline.
            set(_uml_vnc_smoke_wedge_timeout 360)
            set(_uml_vnc_smoke_test_timeout 450)
        endif()
    endif()
    uml_add_tooling_test(
        NAME virtio-gpu-vnc-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/run_vnc_smoke.py
            --server $<TARGET_FILE:uml-virtio-gpu>
            --client $<TARGET_FILE:uml-vnc-smoke-client>
            --wedge-timeout ${_uml_vnc_smoke_wedge_timeout}
        LABELS gpu;vnc
        TIMEOUT ${_uml_vnc_smoke_test_timeout}
        PROCESSORS 1
        RESOURCE_GROUPS gpus:1)
    unset(_uml_vnc_smoke_wedge_timeout)
    unset(_uml_vnc_smoke_test_timeout)
endif()
