/// @file TestStreamSetup.cpp
/// @brief streams.json entries built from a few choices (aes67ctl setup).

#include "../NetworkEngine/StreamSetup.h"
#include "../NetworkEngine/PTP/PtpSettings.h"
#include <iostream>
#include <string>

using namespace AES67;
using namespace AES67::Setup;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

constexpr const char* kAddress = "192.0.2.10";

StreamRequest sendRequest(const std::string& group = "239.69.1.2") {
    StreamRequest request;
    request.direction = Direction::Send;
    request.group = group;
    request.networkInterface = "en9";
    return request;
}

StreamRequest receiveRequest(const std::string& group = "239.69.1.1") {
    StreamRequest request = sendRequest(group);
    request.direction = Direction::Receive;
    return request;
}

PersistedStreamConfig build(const StreamRequest& request) {
    std::string error;
    auto config = buildStream(request, kAddress, &error);
    if (!config) std::cerr << "  unexpected error: " << error << std::endl;
    return config.value_or(PersistedStreamConfig{});
}

/// What the driver reads back from the file text
std::vector<SavedStreamEntry> reread(const std::vector<SavedStreamEntry>& entries) {
    return StreamConfigManager::fromJSON(toFileText(entries)).value_or(std::vector<SavedStreamEntry>{});
}

void testSendStreamDefaults() {
    std::cout << "A send stream defaults to the Mac's stereo output" << std::endl;
    const auto added = addStream({}, build(sendRequest()), nullptr);
    CHECK(added && added->size() == 1, "one entry is added");
    const auto read = reread(*added);
    CHECK(read.size() == 1 && read[0].config, "the driver reads the entry back");
    if (read.size() != 1 || !read[0].config) return;
    const PersistedStreamConfig& c = *read[0].config;
    CHECK(c.sdp.direction == "sendonly", "the driver transmits it");
    CHECK(c.sdp.numChannels == 2 && c.mapping.deviceChannelStart == 0 && c.mapping.deviceChannelCount == 2,
          "device outputs 1-2 (the Mac's stereo) go out");
    CHECK(c.sdp.encoding == "L24" && c.sdp.sampleRate == 48000 && c.sdp.ptime == 1 && c.sdp.framecount == 48,
          "L24, 48 kHz, 1 ms packets");
    CHECK(c.sdp.sourceAddress == kAddress && c.sdp.originAddress == kAddress, "the source is the interface's address");
    CHECK(c.sdp.sessionID != 0, "it has a session ID");
    CHECK(!c.mapping.streamID.isNull(), "it has a stream ID");
    CHECK(c.networkInterface == "en9", "it is bound to the interface");
    CHECK(c.sdp.port == 5004 && c.sdp.ttl == 32, "port 5004, TTL 32");
    CHECK(c.sdp.sessionName == "239.69.1.2", "it is named after its group by default");
}

void testEachStreamGetsItsOwnIDs() {
    std::cout << "Each built stream has its own stream and session ID" << std::endl;
    const auto a = build(sendRequest());
    const auto b = build(sendRequest());
    CHECK(a.mapping.streamID != b.mapping.streamID, "stream IDs differ");
    CHECK(a.sdp.sessionID != b.sdp.sessionID, "session IDs differ");
}

void testReceiveStream() {
    std::cout << "A receive stream needs no address and lands on device inputs" << std::endl;
    StreamRequest request = receiveRequest();
    request.channels = 1;
    std::string error;
    const auto config = buildStream(request, "", &error);
    CHECK(config.has_value(), "an interface without an address is fine for receiving: " << error);
    if (!config) return;
    CHECK(config->sdp.direction == "recvonly", "the driver receives it");
    CHECK(config->mapping.deviceChannelStart == 0 && config->sdp.numChannels == 1, "one channel on input 1");
}

void testInvalidRequests() {
    std::cout << "Requests that can't be a stream are refused with a reason" << std::endl;
    const auto refused = [](StreamRequest request, const std::string& address, const std::string& expected) {
        std::string error;
        const bool built = buildStream(request, address, &error).has_value();
        CHECK(!built, "refused: " << expected);
        CHECK(error.find(expected) != std::string::npos, "error names the problem (got '" << error << "')");
    };
    refused(sendRequest("192.168.1.1"), kAddress, "not an IPv4 multicast");
    refused(sendRequest("239.1"), kAddress, "not an IPv4 multicast");
    StreamRequest wide = sendRequest();
    wide.channels = 9;
    refused(wide, kAddress, "1 to 8 channels");
    StreamRequest none = sendRequest();
    none.channels = 0;
    refused(none, kAddress, "1 to 8 channels");
    StreamRequest past = sendRequest();
    past.firstDeviceChannel = 127;
    refused(past, kAddress, "128 channels");
    StreamRequest noInterface = sendRequest();
    noInterface.networkInterface.clear();
    refused(noInterface, kAddress, "interface is required");
    refused(sendRequest(), "", "has no IPv4 address");
    StreamRequest noPort = sendRequest();
    noPort.port = 0;
    refused(noPort, kAddress, "port");
}

void testAddReplacesTheSameStream() {
    std::cout << "Adding a stream on the same group, port and direction replaces it" << std::endl;
    auto entries = addStream({}, build(sendRequest()), nullptr);
    entries = addStream(*entries, build(receiveRequest("239.69.1.2")), nullptr);  // same group, other direction
    CHECK(entries && entries->size() == 2, "a receive stream on the same group is a different stream");
    StreamRequest mono = sendRequest();
    mono.channels = 1;
    entries = addStream(*entries, build(mono), nullptr);
    CHECK(entries && entries->size() == 2, "the send stream is replaced, not duplicated");
    const auto read = reread(*entries);
    CHECK(read.size() == 2 && read[1].config && read[1].config->sdp.numChannels == 1, "the new one is written");
}

void testAddRefusesOverlappingChannels() {
    std::cout << "Adding a stream whose device channels another stream uses is refused" << std::endl;
    auto entries = addStream({}, build(sendRequest("239.69.1.2")), nullptr);
    std::string error;
    const auto clash = addStream(*entries, build(sendRequest("239.69.1.3")), &error);
    CHECK(!clash.has_value(), "outputs 1-2 are taken");
    CHECK(error.find("1-2") != std::string::npos, "the error names the channels (got '" << error << "')");
    StreamRequest next = sendRequest("239.69.1.3");
    next.firstDeviceChannel = 2;
    CHECK(addStream(*entries, build(next), nullptr).has_value(), "outputs 3-4 are free");
    CHECK(addStream(*entries, build(receiveRequest()), nullptr).has_value(), "inputs are separate from outputs");
}

void testUnreadableEntriesAreKept() {
    std::cout << "Entries this version can't read are kept as written" << std::endl;
    const std::string file = R"({"version": "1.0", "streams": [
        {"enabled": true, "future": "field", "sdp": {"sessionName": "Odd", "port": "not a number",
         "connectionAddress": "239.69.9.9", "direction": "sendonly"}, "mapping": {}}
    ]})";
    const auto existing = StreamConfigManager::fromJSON(file);
    CHECK(existing && existing->size() == 1 && !(*existing)[0].config, "the entry is unreadable");
    if (!existing) return;
    CHECK(directionOf((*existing)[0]) == Direction::Send, "its direction is read from the raw entry");
    const auto entries = addStream(*existing, build(sendRequest()), nullptr);
    CHECK(entries && entries->size() == 2, "it stays");
    const std::string text = toFileText(*entries);
    CHECK(text.find("\"future\"") != std::string::npos && text.find("not a number") != std::string::npos,
          "it is written back unchanged");

    size_t removed = 0;
    const auto left = removeStreams(*entries, "239.69.9.9", &removed);
    CHECK(removed == 1 && left.size() == 1, "an unreadable entry can still be removed by group");
}

void testRemoveByGroup() {
    std::cout << "Removing a group removes every stream on it" << std::endl;
    auto entries = addStream({}, build(sendRequest("239.69.1.2")), nullptr);
    entries = addStream(*entries, build(receiveRequest("239.69.1.2")), nullptr);
    StreamRequest other = receiveRequest("239.69.1.1");
    other.firstDeviceChannel = 2;
    entries = addStream(*entries, build(other), nullptr);
    CHECK(entries && entries->size() == 3, "three streams are set up");
    if (!entries) return;
    size_t removed = 0;
    const auto left = removeStreams(*entries, "239.69.1.2", &removed);
    CHECK(removed == 2 && left.size() == 1, "both directions on the group go (removed " << removed << ")");
    removeStreams(left, "239.69.7.7", &removed);
    CHECK(removed == 0, "an unknown group removes nothing");
}

void testPtpFile() {
    std::cout << "ptp.json text is what the driver's PTP settings read" << std::endl;
    std::string error;
    const auto settings = Ptp::Settings::parse(ptpFileText("en9", 3, true), &error);
    CHECK(error.empty(), "it parses: " << error);
    CHECK(settings.enabled && settings.networkInterface == "en9" && settings.domain == 3 && settings.unicastDelayRequests,
          "enabled on en9, domain 3, hybrid");
}

}  // namespace

int main() {
    testSendStreamDefaults();
    testEachStreamGetsItsOwnIDs();
    testReceiveStream();
    testInvalidRequests();
    testAddReplacesTheSameStream();
    testAddRefusesOverlappingChannels();
    testUnreadableEntriesAreKept();
    testRemoveByGroup();
    testPtpFile();
    std::cout << "Stream setup: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
