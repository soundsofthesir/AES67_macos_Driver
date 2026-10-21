# Tools CMakeLists.txt
# AES67 macOS Driver - Test and debugging tools

cmake_minimum_required(VERSION 3.15)

# AES67 Test Sender: generates sine wave RTP multicast stream with SAP
add_executable(AES67TestSender
    AES67TestSender.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
)

target_include_directories(AES67TestSender PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(AES67TestSender PRIVATE cxx_std_17)

# AES67 Test Receiver: joins multicast group and reports received RTP packets
add_executable(AES67TestReceiver
    AES67TestReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
)

target_include_directories(AES67TestReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(AES67TestReceiver PRIVATE cxx_std_17)

# Spike S1: throwaway HAL plug-in (no device) that probes whether PTP can run
# inside the Core Audio driver host. Off by default; see Docs/Spikes/.
option(BUILD_SPIKES "Build investigation spikes" OFF)

if(APPLE AND BUILD_SPIKES)
    add_library(AES67SandboxProbe MODULE SandboxProbe/SandboxProbe.cpp)
    target_include_directories(AES67SandboxProbe PRIVATE ${ASPL_INCLUDE_DIR})
    target_compile_features(AES67SandboxProbe PRIVATE cxx_std_17)
    target_link_libraries(AES67SandboxProbe PRIVATE
        ${ASPL_LIBRARY}
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    set_target_properties(AES67SandboxProbe PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "driver"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/SandboxProbe/Info.plist.in"
    )
    message(STATUS "  - AES67SandboxProbe (spike S1)")

    # Issue #42: where the driver host lets the driver keep its config (no device, no network)
    add_library(AES67StorageProbe MODULE StorageProbe/StorageProbe.cpp)
    target_include_directories(AES67StorageProbe PRIVATE ${ASPL_INCLUDE_DIR})
    target_compile_features(AES67StorageProbe PRIVATE cxx_std_17)
    target_link_libraries(AES67StorageProbe PRIVATE
        ${ASPL_LIBRARY}
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    set_target_properties(AES67StorageProbe PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "driver"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/StorageProbe/Info.plist.in"
    )
    message(STATUS "  - AES67StorageProbe (config storage, #42)")

    # Spike S2: HAL plug-in whose device clock follows a scheduled clock
    # model, plus a client that measures whether Core Audio obeys it
    add_library(AES67ClockProbe MODULE ClockProbe/ClockProbe.cpp)
    target_include_directories(AES67ClockProbe PRIVATE ${ASPL_INCLUDE_DIR})
    target_compile_features(AES67ClockProbe PRIVATE cxx_std_17)
    target_link_libraries(AES67ClockProbe PRIVATE
        ${ASPL_LIBRARY}
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    set_target_properties(AES67ClockProbe PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "driver"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ClockProbe/Info.plist.in"
    )

    add_executable(AES67ClockProbeClient ClockProbe/ClockProbeClient.cpp)
    target_compile_features(AES67ClockProbeClient PRIVATE cxx_std_17)
    target_link_libraries(AES67ClockProbeClient PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    message(STATUS "  - AES67ClockProbe + AES67ClockProbeClient (spike S2)")
endif()

message(STATUS "Tools configured:")
message(STATUS "  - AES67TestSender")
message(STATUS "  - AES67TestReceiver")
message(STATUS "")
message(STATUS "Run test sender:")
message(STATUS "  ./Tools/AES67TestSender --ip 239.1.1.1 --port 5004 --channels 8 --freq 1000")
message(STATUS "Run test receiver:")
message(STATUS "  ./Tools/AES67TestReceiver --ip 239.1.1.2 --port 5004 --duration 10")
