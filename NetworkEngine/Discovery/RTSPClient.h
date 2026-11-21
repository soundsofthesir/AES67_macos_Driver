//
// RTSPClient.h
// AES67 macOS Driver - Build #1
// RTSP client for stream control (DESCRIBE, SETUP, PLAY, TEARDOWN)
//

#pragma once

#include "../../Shared/Types.h"
#include "../../Driver/SDPParser.h"
#include <string>
#include <optional>
#include <map>

namespace AES67 {

//
// RTSP Response
//
struct RTSPResponse {
    int statusCode{0};
    std::string statusMessage;
    std::map<std::string, std::string> headers;
    std::string body;

    bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
};

//
// RTSP Client
//
// Simple RTSP client for AES67 stream control
// Implements subset of RFC 2326 needed for AES67
//
class RTSPClient {
public:
    explicit RTSPClient(const std::string& url);
    ~RTSPClient();

    // Prevent copy/move
    RTSPClient(const RTSPClient&) = delete;
    RTSPClient& operator=(const RTSPClient&) = delete;

    //
    // RTSP Methods
    //

    // DESCRIBE: Get SDP description of stream
    std::optional<SDPSession> describe(const std::string& path = "/");

    // SETUP: Setup stream transport
    bool setup(const std::string& path, uint16_t clientPort);

    // PLAY: Start stream playback
    bool play(const std::string& path = "/");

    // PAUSE: Pause stream playback
    bool pause(const std::string& path = "/");

    // TEARDOWN: Tear down stream
    bool teardown(const std::string& path = "/");

    //
    // Configuration
    //

    // Set timeout for requests (milliseconds)
    void setTimeout(int timeoutMs) { timeoutMs_ = timeoutMs; }

    // Set user agent string
    void setUserAgent(const std::string& userAgent) { userAgent_ = userAgent; }

    //
    // Status
    //

    // Get last response
    const RTSPResponse& getLastResponse() const { return lastResponse_; }

    // Get session ID (from SETUP response)
    std::string getSessionID() const { return sessionID_; }

    // Check if connected
    bool isConnected() const { return socket_ >= 0; }

    // Parse an RTSP response from the server (RFC 2326 section 7.1). Static and
    // public so tests and the fuzzer can feed it bytes without a socket; a
    // malformed response is rejected rather than throwing.
    static std::optional<RTSPResponse> parseResponse(const std::string& response);

private:
    // Send RTSP request
    std::optional<RTSPResponse> sendRequest(
        const std::string& method,
        const std::string& path,
        const std::map<std::string, std::string>& headers = {},
        const std::string& body = ""
    );

    // Parse URL
    bool parseURL(const std::string& url, std::string& host, uint16_t& port, std::string& path);

    // Connect to server
    bool connect();

    // Disconnect from server
    void disconnect();

    // Read response from socket
    std::optional<std::string> readResponse();

    // URL components
    std::string url_;
    std::string host_;
    uint16_t port_{554};  // Default RTSP port
    std::string basePath_;

    // Connection
    int socket_{-1};
    int timeoutMs_{5000};

    // RTSP state
    int cseq_{1};
    std::string sessionID_;
    std::string userAgent_{"AES67Driver/1.0"};

    // Last response
    RTSPResponse lastResponse_;
};

} // namespace AES67
