# aes67ctl: sets up streams.json and ptp.json, restarts coreaudiod, checks.
# Part of the product (the .pkg installs it), so not behind BUILD_TOOLS.

add_executable(aes67ctl
    main.cpp
    Checks.cpp
    System.cpp
    PacketProbe.cpp
    KeepAlive.cpp
    ${CMAKE_SOURCE_DIR}/NetworkEngine/StreamSetup.cpp
    ${CMAKE_SOURCE_DIR}/NetworkEngine/StreamConfig.cpp
    ${CMAKE_SOURCE_DIR}/NetworkEngine/Json.cpp
    ${CMAKE_SOURCE_DIR}/NetworkEngine/NetworkUtils.cpp
    ${CMAKE_SOURCE_DIR}/NetworkEngine/StreamChannelMapper.cpp
    ${CMAKE_SOURCE_DIR}/Driver/SDPParser.cpp
    ${CMAKE_SOURCE_DIR}/Shared/Types.cpp
)
target_include_directories(aes67ctl PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(aes67ctl PRIVATE cxx_std_17)
target_link_libraries(aes67ctl PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
set_target_properties(aes67ctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

install(TARGETS aes67ctl RUNTIME DESTINATION "/usr/local/bin")
