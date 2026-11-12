# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_file_sync_tests guest_file_sync_tests.cpp)
target_link_libraries(uml_guest_file_sync_tests PRIVATE UML::GuestFiles)
uml_add_unit_test(NAME platform-guest-file-sync
    COMMAND uml_guest_file_sync_tests)

# The iOS family's disk locks: the kernel's locks stay in the registry and no
# host file is locked at any step of a guest's life.
add_executable(uml_guest_file_process_lock_tests
    guest_file_process_lock_tests.cpp)
target_link_libraries(uml_guest_file_process_lock_tests PRIVATE
    uml_guest_files_process_locks)
uml_add_unit_test(NAME platform-guest-file-process-locks
    COMMAND uml_guest_file_process_lock_tests)

add_executable(uml_video_decoder_tests video_decoder_tests.cpp)
target_link_libraries(uml_video_decoder_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-video-decoder
    COMMAND uml_video_decoder_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 120)

# The decoder's HEVC header readers and output ordering, which need no hardware.
add_executable(uml_hevc_stream_tests hevc_stream_tests.cpp)
target_link_libraries(uml_hevc_stream_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-hevc-stream
    COMMAND uml_hevc_stream_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video)

# The decoder's H.264 header readers and output ordering, which need no hardware.
add_executable(uml_h264_stream_tests h264_stream_tests.cpp)
target_link_libraries(uml_h264_stream_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-h264-stream
    COMMAND uml_h264_stream_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video)

# The video encoder backend's Annex B and parameter-set readers, which need no
# hardware.
add_executable(uml_video_encoder_bitstream_tests video_encoder_bitstream_tests.cpp)
target_include_directories(uml_video_encoder_bitstream_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(NAME platform-video-encoder-bitstream
    COMMAND uml_video_encoder_bitstream_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video)

# The hardware video encoder on this host: discovery, every advertised profile
# and mode, and the stream checked against the decoder.
add_executable(uml_video_encoder_tests video_encoder_tests.cpp)
target_link_libraries(uml_video_encoder_tests PRIVATE
    UML::VideoEncoder UML::VideoDecoder)
uml_add_unit_test(NAME platform-video-encoder
    COMMAND uml_video_encoder_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 600)

# Losing the hardware session: the encoder's test variant fails sessions on
# demand, as the system does when it revokes the hardware.
add_executable(uml_video_encoder_recovery_tests video_encoder_recovery_tests.cpp)
target_link_libraries(uml_video_encoder_recovery_tests PRIVATE
    uml_video_encoder_test_variant UML::VideoDecoder)
uml_add_unit_test(NAME platform-video-encoder-recovery
    COMMAND uml_video_encoder_recovery_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 300)

# The capability cache, through child processes that find a valid file, no
# file, or a file they cannot vouch for.
add_executable(uml_video_encoder_cache_tests video_encoder_cache_tests.cpp)
target_link_libraries(uml_video_encoder_cache_tests PRIVATE UML::VideoEncoder)
uml_add_unit_test(NAME platform-video-encoder-cache
    COMMAND uml_video_encoder_cache_tests
    LABELS video TIMEOUT 900)

# The system limits hardware encoder sessions across every process, so the tests
# that hold them (one holds all it can get) run one at a time.
set_tests_properties(platform-video-encoder platform-video-encoder-recovery
    platform-video-encoder-cache PROPERTIES RESOURCE_LOCK video-encoder-hardware)

# The platform's run routing; the transaction tests use its test variant.
add_executable(uml_run_routing_tests
    run_routing_tests.cpp
    $<TARGET_OBJECTS:uml_run_routing_test_variant>)
target_link_libraries(uml_run_routing_tests PRIVATE
    uml_test_allocation_failure)
target_include_directories(uml_run_routing_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
target_compile_definitions(uml_run_routing_tests PRIVATE
    UML_RUN_ROUTING_TESTING=1)
uml_add_unit_test(
    NAME runtime-route-transaction
    COMMAND uml_run_routing_tests
    LABELS tsan)

add_executable(uml_run_routing_stress_tests
    run_routing_stress_tests.cpp
    $<TARGET_OBJECTS:uml_run_routing>)
target_include_directories(uml_run_routing_stress_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME runtime-route-unbound-reads
    COMMAND uml_run_routing_stress_tests)

# The process's memory limit: the host's own, else the one a launch states, so a
# Mac measurement sees the budgets that derive from the limit as the iPad does.
add_executable(uml_host_environment_tests
    host_environment_tests.cpp
    $<TARGET_OBJECTS:uml_host_environment>)
target_link_libraries(uml_host_environment_tests PRIVATE
    UML::PlatformContract
    "-framework Foundation")
target_include_directories(uml_host_environment_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME platform-host-environment
    COMMAND uml_host_environment_tests)

# The host GPU's fixed budget: the clamp, the guest's half as the maximum, no
# limit, and when the process is short of the floor.
add_executable(uml_gpu_budget_tests gpu_budget_tests.cpp)
target_link_libraries(uml_gpu_budget_tests PRIVATE UML::GpuBudget)
uml_add_unit_test(
    NAME platform-gpu-budget
    COMMAND uml_gpu_budget_tests)
