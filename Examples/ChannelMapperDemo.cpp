//
// ChannelMapperDemo.cpp
// AES67 macOS Driver - Build #4
// Demonstration of Stream-to-Channel Mapping
//

#include "../NetworkEngine/StreamChannelMapper.h"
#include "../Driver/SDPParser.h"
#include <iostream>
#include <iomanip>

using namespace AES67;

void printSeparator(char c = '=') {
    std::cout << std::string(70, c) << "\n";
}

void printChannelGrid(const StreamChannelMapper& mapper) {
    std::cout << "\nDevice Channel Layout (128 channels):\n\n";

    // Print header
    std::cout << "     ";
    for (int col = 0; col < 16; col++) {
        std::cout << std::setw(4) << col;
    }
    std::cout << "\n";

    printSeparator('-');

    // Print 8 rows of 16 channels each
    for (int row = 0; row < 8; row++) {
        std::cout << std::setw(3) << (row * 16) << " |";

        for (int col = 0; col < 16; col++) {
            int channel = row * 16 + col;

            // Check if this channel is assigned
            bool assigned = false;
            auto mappings = mapper.getAllMappings();

            for (const auto& mapping : mappings) {
                int start = mapping.deviceChannelStart;
                int end = start + mapping.deviceChannelCount;

                if (channel >= start && channel < end) {
                    // Show stream number (first digit of stream name)
                    std::string name = mapping.streamName;
                    size_t pos = name.find_last_of(' ');
                    if (pos != std::string::npos && pos + 1 < name.length()) {
                        std::cout << " " << name[pos + 1] << "  ";
                    } else {
                        std::cout << " *  ";
                    }
                    assigned = true;
                    break;
                }
            }

            if (!assigned) {
                std::cout << " .  ";  // Unassigned
            }
        }
        std::cout << "\n";
    }

    std::cout << "\nLegend: [.] = Unassigned  [1-9] = Stream number  [*] = Assigned\n";
}

void printStreamList(const StreamChannelMapper& mapper) {
    std::cout << "\nActive Streams:\n";
    printSeparator('-');

    auto mappings = mapper.getAllMappings();

    if (mappings.empty()) {
        std::cout << "  (no active streams)\n";
        return;
    }

    std::cout << std::left;
    std::cout << std::setw(25) << "  Name"
              << std::setw(15) << "Device Chs"
              << std::setw(15) << "Stream Chs"
              << "UUID\n";

    printSeparator('-');

    for (const auto& mapping : mappings) {
        std::string devRange = std::to_string(mapping.deviceChannelStart) + "-" +
                              std::to_string(mapping.deviceChannelStart +
                                           mapping.deviceChannelCount - 1);

        std::string streamRange = "0-" + std::to_string(mapping.streamChannelCount - 1);

        std::cout << "  " << std::setw(23) << mapping.streamName
                  << std::setw(15) << devRange
                  << std::setw(15) << streamRange
                  << mapping.streamID.toString().substr(0, 8) << "...\n";
    }

    std::cout << std::right;
}

int main() {
    std::cout << "\n=== AES67 Channel Mapper - Demo ===\n";

    StreamChannelMapper mapper;

    // Scenario 1: Basic mapping
    printSeparator();
    std::cout << "Scenario 1: Adding a single 8-channel stream\n";
    printSeparator();

    StreamID stream1 = StreamID::generate();
    auto mapping1 = mapper.createDefaultMapping(stream1, "Microphones 1-8", 8);

    if (mapping1) {
        std::cout << "\n✓ Created mapping: " << mapping1->streamName << "\n";
        std::cout << "  Device channels: " << mapping1->deviceChannelStart
                  << "-" << (mapping1->deviceChannelStart + mapping1->deviceChannelCount - 1)
                  << "\n";

        mapper.addMapping(*mapping1);
        printStreamList(mapper);
        printChannelGrid(mapper);
    }

    // Scenario 2: Multiple streams
    std::cout << "\n\n";
    printSeparator();
    std::cout << "Scenario 2: Adding multiple streams (Riedel Artist setup)\n";
    printSeparator();

    for (int i = 2; i <= 8; i++) {
        StreamID streamID = StreamID::generate();
        std::string name = "Riedel Panel " + std::to_string(i);

        auto mapping = mapper.createDefaultMapping(streamID, name, 8);
        if (mapping) {
            mapper.addMapping(*mapping);
            std::cout << "\n✓ Added: " << name << " → channels "
                      << mapping->deviceChannelStart << "-"
                      << (mapping->deviceChannelStart + 7) << "\n";
        }
    }

    printStreamList(mapper);
    printChannelGrid(mapper);

    // Scenario 3: Statistics
    std::cout << "\n\n";
    printSeparator();
    std::cout << "Channel Statistics\n";
    printSeparator();

    auto unassigned = mapper.getUnassignedDeviceChannels();
    size_t assignedCount = 128 - unassigned.size();

    std::cout << "\n  Total device channels:    128\n";
    std::cout << "  Assigned channels:        " << assignedCount << "\n";
    std::cout << "  Unassigned channels:      " << unassigned.size() << "\n";
    std::cout << "  Active streams:           " << mapper.getAllMappings().size() << "\n";
    std::cout << "  Channel utilization:      "
              << std::fixed << std::setprecision(1)
              << (assignedCount * 100.0 / 128.0) << "%\n\n";

    // Scenario 4: Remove a stream
    std::cout << "\n";
    printSeparator();
    std::cout << "Scenario 3: Removing a stream\n";
    printSeparator();

    // Get second stream (stream1 was the first)
    auto allMappings = mapper.getAllMappings();
    if (allMappings.size() > 1) {
        StreamID toRemove = allMappings[1].streamID;

        std::cout << "\nRemoving: " << allMappings[1].streamName << "\n";
        mapper.removeMapping(toRemove);
        std::cout << "✓ Removed successfully\n";

        printStreamList(mapper);
        printChannelGrid(mapper);
    }

    // Scenario 5: Load from SDP
    std::cout << "\n\n";
    printSeparator();
    std::cout << "Scenario 4: Creating mapping from SDP file\n";
    printSeparator();

    std::string sdpPath = "Docs/Examples/riedel_artist_8ch.sdp";
    auto session = SDPParser::parseFile(sdpPath);

    if (session) {
        std::cout << "\n✓ Loaded SDP: " << session->sessionName << "\n";
        std::cout << "  Channels: " << session->numChannels << "\n";
        std::cout << "  Sample Rate: " << session->sampleRate << " Hz\n";
        std::cout << "  Encoding: " << session->encoding << "\n\n";

        // Create a new mapper for this scenario
        StreamChannelMapper sdpMapper;
        StreamID sdpStream = StreamID::generate();

        auto sdpMapping = sdpMapper.createDefaultMapping(
            sdpStream,
            session->sessionName,
            session->numChannels
        );

        if (sdpMapping) {
            sdpMapper.addMapping(*sdpMapping);
            std::cout << "✓ Created mapping from SDP\n";
            std::cout << "  Device channels: " << sdpMapping->deviceChannelStart
                      << "-" << (sdpMapping->deviceChannelStart + sdpMapping->deviceChannelCount - 1)
                      << "\n";
        }
    } else {
        std::cout << "\n⚠ Could not load SDP file (may not exist yet)\n";
        std::cout << "  This is normal if building on Linux\n";
        std::cout << "  SDP files will be available after transfer to macOS\n";
    }

    // Summary
    std::cout << "\n\n";
    printSeparator();
    std::cout << "Summary\n";
    printSeparator();

    std::cout << "\nThe Stream-to-Channel Mapper provides:\n";
    std::cout << "  ✓ Automatic channel allocation\n";
    std::cout << "  ✓ Support for 128 device channels\n";
    std::cout << "  ✓ Multiple simultaneous streams\n";
    std::cout << "  ✓ Overlap detection and validation\n";
    std::cout << "  ✓ Integration with SDP parsing\n";
    std::cout << "  ✓ Real-time stream addition/removal\n\n";

    std::cout << "Use cases:\n";
    std::cout << "  • Riedel Artist intercom (8x 8-channel streams)\n";
    std::cout << "  • Dante/RAVENNA routing (flexible channel counts)\n";
    std::cout << "  • Multi-stream DAW recording (route to specific tracks)\n";
    std::cout << "  • Broadcast mixing (aggregate multiple sources)\n\n";

    return 0;
}
