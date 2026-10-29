# Tests CMakeLists.txt
# AES67 macOS Driver - Build #4
# Test suite compilation

cmake_minimum_required(VERSION 3.15)

# Tests keep assert() in every build type. Release defines NDEBUG, which would
# compile the assert-based suites' checks away; -UNDEBUG comes after the
# configuration flags on the command line, so it wins.
add_compile_options(-UNDEBUG)

# Test: SDP Parser
add_executable(TestSDPParser
    TestSDPParser.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestSDPParser PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestSDPParser PRIVATE cxx_std_17)

# Test: Channel Mapper
add_executable(TestChannelMapper
    TestChannelMapper.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestChannelMapper PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestChannelMapper PRIVATE cxx_std_17)

# Test: Ring Buffer
add_executable(TestRingBuffer
    TestRingBuffer.cpp
)

target_include_directories(TestRingBuffer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRingBuffer PRIVATE cxx_std_17)

# Test: RTP Receiver
add_executable(TestRTPReceiver
    TestRTPReceiver.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/RtpPlacement.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
    ../NetworkEngine/Clock/RecoveredClockSource.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRTPReceiver PRIVATE cxx_std_17)

# Test: RTP Transmitter
add_executable(TestRTPTransmitter
    TestRTPTransmitter.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPTransmitter PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRTPTransmitter PRIVATE cxx_std_17)

# Test: Stream Manager
add_executable(TestStreamManager
    TestStreamManager.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestStreamManager PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestStreamManager PRIVATE cxx_std_17)

# Test: JSON reader/writer and streams.json save/load
add_executable(TestStreamConfig
    TestStreamConfig.cpp
    ../NetworkEngine/Json.cpp
    ../NetworkEngine/StreamConfig.cpp
    ../NetworkEngine/NetworkUtils.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestStreamConfig PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestStreamConfig PRIVATE cxx_std_17)

# Integration Test: Multi-Stream Scenarios
add_executable(TestMultiStream
    TestMultiStream.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestMultiStream PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestMultiStream PRIVATE cxx_std_17)

# Benchmark: I/O Handler Performance
add_executable(BenchmarkIOHandler
    BenchmarkIOHandler.cpp
    ../Driver/AES67IOHandler.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
    ../Shared/Types.cpp
)

target_include_directories(BenchmarkIOHandler PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ASPL_INCLUDE_DIR}
)

target_compile_features(BenchmarkIOHandler PRIVATE cxx_std_17)

# Link libASPL for benchmark
if(ASPL_LIBRARY)
    target_link_libraries(BenchmarkIOHandler PRIVATE ${ASPL_LIBRARY})
endif()

# Integration Test: Full Audio Path
add_executable(TestIntegrationAudioPath
    TestIntegrationAudioPath.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/RtpPlacement.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
    ../NetworkEngine/Clock/RecoveredClockSource.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestIntegrationAudioPath PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestIntegrationAudioPath PRIVATE cxx_std_17)

# Regression Test: Critical path defects (links the full driver)
set(DRIVER_TEST_SOURCES ${ALL_CPP_SOURCES})
list(TRANSFORM DRIVER_TEST_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/")

add_executable(TestCriticalPathRegressions
    TestCriticalPathRegressions.cpp
    ${DRIVER_TEST_SOURCES}
)

target_include_directories(TestCriticalPathRegressions PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ASPL_INCLUDE_DIR}
)

target_compile_features(TestCriticalPathRegressions PRIVATE cxx_std_17)

target_link_libraries(TestCriticalPathRegressions PRIVATE
    ${ASPL_LIBRARY}
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioToolbox"
)

# Test: Step 2 clock foundations (HostTime, MediaClock, TimestampedAudioBuffer)
add_executable(TestClockFoundations
    TestClockFoundations.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
)

target_include_directories(TestClockFoundations PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestClockFoundations PRIVATE cxx_std_17)

# Test: Step 2 RX placement by timestamp and IO routing
add_executable(TestRxTimestamp
    TestRxTimestamp.cpp
    ../NetworkEngine/RTP/RtpPlacement.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
)

target_include_directories(TestRxTimestamp PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRxTimestamp PRIVATE cxx_std_17)

# Test: Step 2 phase 3 clock recovery from a received stream
add_executable(TestClockRecovery
    TestClockRecovery.cpp
    ../NetworkEngine/Clock/RecoveredClockSource.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
    ../NetworkEngine/RTP/RtpPlacement.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestClockRecovery PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestClockRecovery PRIVATE cxx_std_17)

# Test: SAP announcement parsing (RFC 2974)
add_executable(TestSAPListener
    TestSAPListener.cpp
    ../NetworkEngine/Discovery/SAPListener.cpp
)

target_include_directories(TestSAPListener PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestSAPListener PRIVATE cxx_std_17)

# Test: Transmit on the media clock (step 2 phase 5 core)
add_executable(TestTxMediaClock
    TestTxMediaClock.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/RtpPlacement.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
    ../NetworkEngine/Clock/RecoveredClockSource.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestTxMediaClock PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestTxMediaClock PRIVATE cxx_std_17)

# Test: SAP announcements (RFC 2974)
add_executable(TestSAPAnnouncer
    TestSAPAnnouncer.cpp
    ../NetworkEngine/Discovery/SAPAnnouncer.cpp
    ../NetworkEngine/Discovery/SAPListener.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestSAPAnnouncer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestSAPAnnouncer PRIVATE cxx_std_17)

# Test: network interface state and change monitoring
add_executable(TestNetworkMonitor
    TestNetworkMonitor.cpp
    ../NetworkEngine/NetworkMonitor.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
)

target_include_directories(TestNetworkMonitor PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestNetworkMonitor PRIVATE cxx_std_17)

# Test: PTP message parsing and building (step 2 phase 4)
add_executable(TestPtpMessages
    TestPtpMessages.cpp
    ../NetworkEngine/PTP/PtpMessages.cpp
)

target_include_directories(TestPtpMessages PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpMessages PRIVATE cxx_std_17)

# Test: choosing the PTP master (best master algorithm)
add_executable(TestPtpBestMaster
    TestPtpBestMaster.cpp
    ../NetworkEngine/PTP/PtpBestMaster.cpp
    ../NetworkEngine/PTP/PtpMessages.cpp
)

target_include_directories(TestPtpBestMaster PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpBestMaster PRIVATE cxx_std_17)

# Test: pairing a two-step master's Syncs with their Follow_Ups
add_executable(TestPtpSyncPairer
    TestPtpSyncPairer.cpp
)

target_include_directories(TestPtpSyncPairer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpSyncPairer PRIVATE cxx_std_17)

# Test: the PTP servo, in simulation
add_executable(TestPtpServo
    TestPtpServo.cpp
    ../NetworkEngine/PTP/PtpServo.cpp
)

target_include_directories(TestPtpServo PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpServo PRIVATE cxx_std_17)

add_executable(TestPtpTimeReceiver
    TestPtpTimeReceiver.cpp
    ../NetworkEngine/PTP/PtpTimeReceiver.cpp
    ../NetworkEngine/PTP/PtpSocket.cpp
    ../NetworkEngine/PTP/PtpServo.cpp
    ../NetworkEngine/PTP/PtpBestMaster.cpp
    ../NetworkEngine/PTP/PtpMessages.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
)

target_include_directories(TestPtpTimeReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpTimeReceiver PRIVATE cxx_std_17)

add_executable(TestPtpClockControl
    TestPtpClockControl.cpp
    ../NetworkEngine/Clock/PtpClockControl.cpp
    ../NetworkEngine/PTP/PtpServo.cpp
)

target_include_directories(TestPtpClockControl PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpClockControl PRIVATE cxx_std_17)

add_executable(TestPtpSettings
    TestPtpSettings.cpp
    ../NetworkEngine/PTP/PtpSettings.cpp
)

target_include_directories(TestPtpSettings PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpSettings PRIVATE cxx_std_17)

add_executable(TestPtpClockSource
    TestPtpClockSource.cpp
    ../NetworkEngine/Clock/PtpClockSource.cpp
    ../NetworkEngine/NetworkMonitor.cpp
    ../NetworkEngine/Clock/PtpClockControl.cpp
    ../NetworkEngine/PTP/PtpTimeReceiver.cpp
    ../NetworkEngine/PTP/PtpSocket.cpp
    ../NetworkEngine/PTP/PtpServo.cpp
    ../NetworkEngine/PTP/PtpBestMaster.cpp
    ../NetworkEngine/PTP/PtpMessages.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
)

target_include_directories(TestPtpClockSource PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPtpClockSource PRIVATE cxx_std_17)

# PTP test ports on lo0, one pair per binary so ctest -j can run them side by
# side (Tests/PtpTestMaster.h). CriticalPathRegressions uses 319/320.
target_compile_definitions(TestPtpTimeReceiver PRIVATE AES67_PTP_TEST_EVENT_PORT=31319)
target_compile_definitions(TestPtpClockSource PRIVATE AES67_PTP_TEST_EVENT_PORT=31329)

# SAP test ports, one per binary that announces (Tests/SapTestSupport.h)
target_compile_definitions(TestCriticalPathRegressions PRIVATE AES67_SAP_TEST_PORT=19875)
target_compile_definitions(TestSAPAnnouncer PRIVATE AES67_SAP_TEST_PORT=19885)

# Add tests to CTest
enable_testing()
add_test(NAME ClockRecovery COMMAND TestClockRecovery)
add_test(NAME RxTimestamp COMMAND TestRxTimestamp)
add_test(NAME ClockFoundations COMMAND TestClockFoundations)
add_test(NAME CriticalPathRegressions COMMAND TestCriticalPathRegressions)
add_test(NAME SDPParser COMMAND TestSDPParser)
add_test(NAME ChannelMapper COMMAND TestChannelMapper)
add_test(NAME RingBuffer COMMAND TestRingBuffer)
add_test(NAME RTPReceiver COMMAND TestRTPReceiver)
add_test(NAME RTPTransmitter COMMAND TestRTPTransmitter)
add_test(NAME StreamManager COMMAND TestStreamManager)
add_test(NAME StreamConfig COMMAND TestStreamConfig)
add_test(NAME MultiStream COMMAND TestMultiStream)
add_test(NAME IntegrationAudioPath COMMAND TestIntegrationAudioPath)
add_test(NAME SAPListener COMMAND TestSAPListener)
add_test(NAME SAPAnnouncer COMMAND TestSAPAnnouncer)
add_test(NAME NetworkMonitor COMMAND TestNetworkMonitor)
add_test(NAME PtpMessages COMMAND TestPtpMessages)
add_test(NAME PtpBestMaster COMMAND TestPtpBestMaster)
add_test(NAME PtpSyncPairer COMMAND TestPtpSyncPairer)
add_test(NAME PtpServo COMMAND TestPtpServo)
add_test(NAME PtpTimeReceiver COMMAND TestPtpTimeReceiver)
add_test(NAME PtpClockControl COMMAND TestPtpClockControl)
add_test(NAME PtpSettings COMMAND TestPtpSettings)
add_test(NAME PtpClockSource COMMAND TestPtpClockSource)
add_test(NAME TxMediaClock COMMAND TestTxMediaClock)

# The driver bundle must carry a valid signature over its resources: Core
# Audio loads plug-ins from /Library/Audio/Plug-Ins/HAL, and the linker's
# ad-hoc signature alone leaves Info.plist unsealed
if(TARGET AES67Driver)
    add_test(NAME DriverBundleSignature
             COMMAND codesign --verify --strict $<TARGET_BUNDLE_DIR:AES67Driver>)
endif()

message(STATUS "Tests configured:")
message(STATUS "  - TestSDPParser")
message(STATUS "  - TestChannelMapper")
message(STATUS "  - TestRingBuffer")
message(STATUS "  - TestRTPReceiver")
message(STATUS "  - TestRTPTransmitter")
message(STATUS "  - TestStreamManager")
message(STATUS "  - TestStreamConfig")
message(STATUS "  - TestMultiStream (Integration)")
message(STATUS "  - TestIntegrationAudioPath (Integration)")
message(STATUS "")
message(STATUS "Benchmarks configured:")
message(STATUS "  - BenchmarkIOHandler")
message(STATUS "")
message(STATUS "Run tests with: ctest or make test")
message(STATUS "Run benchmarks manually: ./BenchmarkIOHandler")
