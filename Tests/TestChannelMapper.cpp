//
// TestChannelMapper.cpp
// AES67 macOS Driver - Build #4
// Unit tests for Stream-to-Channel Mapper
//

#include "../NetworkEngine/StreamChannelMapper.h"
#include <iostream>
#include <cassert>

// These tests check with assert(). Release builds define NDEBUG, which would
// compile every check away and let the suite pass without testing anything.
#ifdef NDEBUG
#error "TestChannelMapper needs assert(): build tests without NDEBUG (see Tests/CMakeLists.txt)"
#endif
#include <algorithm>

namespace AES67 {
namespace Tests {

void testBasicMapping() {
    std::cout << "Test: Basic Channel Mapping... ";

    StreamChannelMapper mapper;
    StreamID stream1 = StreamID::generate();

    // Create 8-channel mapping
    auto mapping = mapper.createDefaultMapping(stream1, "Test Stream 1", 8);
    assert(mapping.has_value());
    assert(mapping->deviceChannelStart == 0);
    assert(mapping->deviceChannelCount == 8);
    assert(mapping->streamChannelCount == 8);

    // Add to mapper
    bool added = mapper.addMapping(*mapping);
    assert(added);

    // Verify retrieval
    auto retrieved = mapper.getMapping(stream1);
    assert(retrieved.has_value());
    assert(retrieved->streamName == "Test Stream 1");

    std::cout << "✓ PASSED\n";
}

void testMultipleStreams() {
    std::cout << "Test: Multiple Stream Mapping... ";

    StreamChannelMapper mapper;

    // Add 8-channel stream
    StreamID stream1 = StreamID::generate();
    auto mapping1 = mapper.createDefaultMapping(stream1, "Stream 1", 8);
    assert(mapping1.has_value());
    assert(mapping1->deviceChannelStart == 0);
    mapper.addMapping(*mapping1);

    // Add another 8-channel stream
    StreamID stream2 = StreamID::generate();
    auto mapping2 = mapper.createDefaultMapping(stream2, "Stream 2", 8);
    assert(mapping2.has_value());
    assert(mapping2->deviceChannelStart == 8);  // Should start after first stream
    mapper.addMapping(*mapping2);

    // Add 16-channel stream
    StreamID stream3 = StreamID::generate();
    auto mapping3 = mapper.createDefaultMapping(stream3, "Stream 3", 16);
    assert(mapping3.has_value());
    assert(mapping3->deviceChannelStart == 16);
    mapper.addMapping(*mapping3);

    // Verify all mappings
    auto allMappings = mapper.getAllMappings();
    assert(allMappings.size() == 3);

    std::cout << "✓ PASSED\n";
}

void testChannelExhaustion() {
    std::cout << "Test: Channel Exhaustion Handling... ";

    StreamChannelMapper mapper;

    // Fill most channels (120 out of 128)
    StreamID stream1 = StreamID::generate();
    auto mapping1 = mapper.createDefaultMapping(stream1, "Big Stream", 120);
    assert(mapping1.has_value());
    mapper.addMapping(*mapping1);

    // Try to add 16 channels (should fail - not enough space)
    StreamID stream2 = StreamID::generate();
    auto mapping2 = mapper.createDefaultMapping(stream2, "Too Big", 16);
    assert(!mapping2.has_value());  // Should fail

    // Add 8 channels (should succeed)
    StreamID stream3 = StreamID::generate();
    auto mapping3 = mapper.createDefaultMapping(stream3, "Fits", 8);
    assert(mapping3.has_value());
    assert(mapping3->deviceChannelStart == 120);

    std::cout << "✓ PASSED\n";
}

void testMappingRemoval() {
    std::cout << "Test: Mapping Removal... ";

    StreamChannelMapper mapper;

    // Add three streams — must addMapping before next createDefaultMapping
    // so the mapper knows which channels are already taken
    StreamID stream1 = StreamID::generate();
    StreamID stream2 = StreamID::generate();
    StreamID stream3 = StreamID::generate();

    auto m1 = mapper.createDefaultMapping(stream1, "Stream 1", 16);
    mapper.addMapping(*m1);

    auto m2 = mapper.createDefaultMapping(stream2, "Stream 2", 16);
    mapper.addMapping(*m2);

    auto m3 = mapper.createDefaultMapping(stream3, "Stream 3", 16);
    mapper.addMapping(*m3);

    assert(mapper.getAllMappings().size() == 3);

    // Remove middle stream
    bool removed = mapper.removeMapping(stream2);
    assert(removed);
    assert(mapper.getAllMappings().size() == 2);

    // Verify channels 16-31 are now available
    auto unassigned = mapper.getUnassignedDeviceChannels();
    assert(std::find(unassigned.begin(), unassigned.end(), 16) != unassigned.end());

    // Should be able to add new stream in freed space
    StreamID stream4 = StreamID::generate();
    auto m4 = mapper.createDefaultMapping(stream4, "Stream 4", 16);
    assert(m4.has_value());
    assert(m4->deviceChannelStart == 16);  // Reuses freed space

    std::cout << "✓ PASSED\n";
}

void testMappingValidation() {
    std::cout << "Test: Mapping Validation... ";

    StreamChannelMapper mapper;

    ChannelMapping invalid;
    invalid.streamID = StreamID::generate();
    invalid.streamName = "Invalid";
    invalid.streamChannelCount = 8;
    invalid.deviceChannelStart = 125;  // Would extend to channel 132 (>127)
    invalid.deviceChannelCount = 8;

    std::string error;
    bool valid = mapper.validateMapping(invalid, &error);
    assert(!valid);
    assert(!error.empty());

    std::cout << "✓ PASSED\n";
}

void testMappingOverlap() {
    std::cout << "Test: Overlap Detection... ";

    StreamChannelMapper mapper;

    // Add first stream at channels 10-17
    StreamID stream1 = StreamID::generate();
    ChannelMapping mapping1;
    mapping1.streamID = stream1;
    mapping1.streamName = "Stream 1";
    mapping1.streamChannelCount = 8;
    mapping1.deviceChannelStart = 10;
    mapping1.deviceChannelCount = 8;

    bool added1 = mapper.addMapping(mapping1);
    assert(added1);

    // Try to add overlapping stream at channels 15-22 (should fail)
    StreamID stream2 = StreamID::generate();
    ChannelMapping mapping2;
    mapping2.streamID = stream2;
    mapping2.streamName = "Stream 2";
    mapping2.streamChannelCount = 8;
    mapping2.deviceChannelStart = 15;  // Overlaps with stream1
    mapping2.deviceChannelCount = 8;

    bool added2 = mapper.addMapping(mapping2);
    assert(!added2);  // Should be rejected

    std::cout << "✓ PASSED\n";
}

void testGetUnassignedChannels() {
    std::cout << "Test: Unassigned Channels Query... ";

    StreamChannelMapper mapper;

    // Initially all 128 channels should be unassigned
    auto unassigned = mapper.getUnassignedDeviceChannels();
    assert(unassigned.size() == 128);

    // Add stream at channels 0-7
    StreamID stream1 = StreamID::generate();
    auto m1 = mapper.createDefaultMapping(stream1, "Stream 1", 8);
    mapper.addMapping(*m1);

    // Now 120 channels should be unassigned
    unassigned = mapper.getUnassignedDeviceChannels();
    assert(unassigned.size() == 120);

    // Verify channels 0-7 are NOT in unassigned list
    for (int ch = 0; ch < 8; ch++) {
        assert(std::find(unassigned.begin(), unassigned.end(), ch) == unassigned.end());
    }

    // Verify channels 8-127 ARE in unassigned list
    for (int ch = 8; ch < 128; ch++) {
        assert(std::find(unassigned.begin(), unassigned.end(), ch) != unassigned.end());
    }

    std::cout << "✓ PASSED\n";
}

void testRiedelScenario() {
    std::cout << "Test: Riedel Artist Scenario (8x8-channel streams)... ";

    StreamChannelMapper mapper;

    // Simulate 8 Riedel Artist streams, each with 8 channels
    std::vector<StreamID> streams;
    for (int i = 0; i < 8; i++) {
        StreamID streamID = StreamID::generate();
        streams.push_back(streamID);

        std::string name = "Riedel Panel " + std::to_string(i + 1);
        auto mapping = mapper.createDefaultMapping(streamID, name, 8);
        assert(mapping.has_value());
        assert(mapping->deviceChannelStart == i * 8);

        bool added = mapper.addMapping(*mapping);
        assert(added);
    }

    // All 64 channels should be assigned
    auto unassigned = mapper.getUnassignedDeviceChannels();
    assert(unassigned.size() == 64);  // 128 - 64 = 64 remaining

    // Verify all streams are active
    assert(mapper.getAllMappings().size() == 8);

    std::cout << "✓ PASSED\n";
}

void testLargeScaleScenario() {
    std::cout << "Test: Large Scale Scenario (16x8-channel streams)... ";

    StreamChannelMapper mapper;

    // Add 16 streams of 8 channels each (full 128 channels)
    for (int i = 0; i < 16; i++) {
        StreamID streamID = StreamID::generate();
        std::string name = "Stream " + std::to_string(i + 1);

        auto mapping = mapper.createDefaultMapping(streamID, name, 8);
        assert(mapping.has_value());

        bool added = mapper.addMapping(*mapping);
        assert(added);
    }

    // All channels should be assigned
    auto unassigned = mapper.getUnassignedDeviceChannels();
    assert(unassigned.empty());

    // No more streams should fit
    StreamID extraStream = StreamID::generate();
    auto extraMapping = mapper.createDefaultMapping(extraStream, "Extra", 1);
    assert(!extraMapping.has_value());

    std::cout << "✓ PASSED\n";
}

void runAllTests() {
    std::cout << "\n=== AES67 Channel Mapper Test Suite ===\n\n";

    testBasicMapping();
    testMultipleStreams();
    testChannelExhaustion();
    testMappingRemoval();
    testMappingValidation();
    testMappingOverlap();
    testGetUnassignedChannels();
    testRiedelScenario();
    testLargeScaleScenario();

    std::cout << "\n✅ All Channel Mapper tests passed!\n\n";
}

} // namespace Tests
} // namespace AES67

int main() {
    try {
        AES67::Tests::runAllTests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
