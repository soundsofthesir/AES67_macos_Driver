//
// RTSPClient.cpp
// AES67 macOS Driver - Build #16
// RTSP client implementation for stream control
// RFC 2326 - Real Time Streaming Protocol
//

#include "RTSPClient.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace AES67 {

//
// Constructor
//
RTSPClient::RTSPClient(const std::string& url)
    : url_(url)
{
    parseURL(url, host_, port_, basePath_);
}

//
// Destructor
//
RTSPClient::~RTSPClient() {
    disconnect();
}

//
// DESCRIBE: Get SDP description
//
std::optional<SDPSession> RTSPClient::describe(const std::string& path) {
    if (!connect()) {
        return std::nullopt;
    }

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/sdp";

    auto response = sendRequest("DESCRIBE", path, headers);
    if (!response || !response->isSuccess()) {
        return std::nullopt;
    }

    // Parse SDP from body
    return SDPParser::parseString(response->body);
}

//
// SETUP: Setup stream transport
//
bool RTSPClient::setup(const std::string& path, uint16_t clientPort) {
    if (!connect()) {
        return false;
    }

    std::map<std::string, std::string> headers;
    std::ostringstream transport;
    transport << "RTP/AVP;unicast;client_port=" << clientPort << "-" << (clientPort + 1);
    headers["Transport"] = transport.str();

    auto response = sendRequest("SETUP", path, headers);
    if (!response || !response->isSuccess()) {
        return false;
    }

    // Extract session ID from response
    auto sessionIt = response->headers.find("Session");
    if (sessionIt != response->headers.end()) {
        sessionID_ = sessionIt->second;
        // Remove timeout parameter if present
        size_t semicolon = sessionID_.find(';');
        if (semicolon != std::string::npos) {
            sessionID_ = sessionID_.substr(0, semicolon);
        }
    }

    return true;
}

//
// PLAY: Start playback
//
bool RTSPClient::play(const std::string& path) {
    if (!connect() || sessionID_.empty()) {
        return false;
    }

    std::map<std::string, std::string> headers;
    headers["Session"] = sessionID_;
    headers["Range"] = "npt=0.000-";  // Play from beginning

    auto response = sendRequest("PLAY", path, headers);
    return response && response->isSuccess();
}

//
// PAUSE: Pause playback
//
bool RTSPClient::pause(const std::string& path) {
    if (!connect() || sessionID_.empty()) {
        return false;
    }

    std::map<std::string, std::string> headers;
    headers["Session"] = sessionID_;

    auto response = sendRequest("PAUSE", path, headers);
    return response && response->isSuccess();
}

//
// TEARDOWN: Tear down stream
//
bool RTSPClient::teardown(const std::string& path) {
    if (!connect() || sessionID_.empty()) {
        return false;
    }

    std::map<std::string, std::string> headers;
    headers["Session"] = sessionID_;

    auto response = sendRequest("TEARDOWN", path, headers);
    sessionID_.clear();

    return response && response->isSuccess();
}

//
// Send RTSP request
//
std::optional<RTSPResponse> RTSPClient::sendRequest(
    const std::string& method,
    const std::string& path,
    const std::map<std::string, std::string>& headers,
    const std::string& body)
{
    if (socket_ < 0) {
        return std::nullopt;
    }

    // Build request
    std::ostringstream request;
    request << method << " ";

    // Use full URL for DESCRIBE, path for others
    if (method == "DESCRIBE") {
        request << url_;
    } else {
        request << path;
    }

    request << " RTSP/1.0\r\n";
    request << "CSeq: " << cseq_++ << "\r\n";
    request << "User-Agent: " << userAgent_ << "\r\n";

    // Add custom headers
    for (const auto& [key, value] : headers) {
        request << key << ": " << value << "\r\n";
    }

    // Add content length if body present
    if (!body.empty()) {
        request << "Content-Length: " << body.length() << "\r\n";
        request << "Content-Type: application/sdp\r\n";
    }

    request << "\r\n";

    // Add body
    if (!body.empty()) {
        request << body;
    }

    std::string requestStr = request.str();

    // Send request
    ssize_t sent = send(socket_, requestStr.c_str(), requestStr.length(), 0);
    if (sent < 0) {
        std::cerr << "RTSPClient: Failed to send request" << std::endl;
        return std::nullopt;
    }

    // Read response
    auto responseStr = readResponse();
    if (!responseStr) {
        return std::nullopt;
    }

    // Parse response
    auto response = parseResponse(*responseStr);
    if (response) {
        lastResponse_ = *response;
    }

    return response;
}

//
// Parse RTSP response
//
std::optional<RTSPResponse> RTSPClient::parseResponse(const std::string& responseStr) {
    RTSPResponse response;

    std::istringstream stream(responseStr);
    std::string line;

    // Parse status line: RTSP/1.0 200 OK
    if (!std::getline(stream, line)) {
        return std::nullopt;
    }

    // Remove \r if present
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream statusLine(line);
    std::string protocol;
    if (!(statusLine >> protocol >> response.statusCode) ||
        protocol.rfind("RTSP/", 0) != 0 ||
        response.statusCode < 100 || response.statusCode > 999) {
        return std::nullopt;
    }
    std::getline(statusLine, response.statusMessage);

    // Trim leading space from message
    if (!response.statusMessage.empty() && response.statusMessage[0] == ' ') {
        response.statusMessage = response.statusMessage.substr(1);
    }

    // Parse headers
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);

            // Trim leading/trailing whitespace from value
            size_t start = value.find_first_not_of(" \t");
            size_t end = value.find_last_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start, end - start + 1);
            }

            response.headers[key] = value;
        }
    }

    // Read body if Content-Length is present. The value comes from the other
    // end of the connection: a malformed or absurd length must fail the
    // response, not throw out of the caller or allocate what it asks for.
    auto contentLengthIt = response.headers.find("Content-Length");
    if (contentLengthIt != response.headers.end()) {
        const std::string& lengthText = contentLengthIt->second;
        const bool isNumber = !lengthText.empty() && lengthText.size() <= 9 &&
            std::all_of(lengthText.begin(), lengthText.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; });
        constexpr size_t kMaxBodyBytes = 1u << 20;  // SDP descriptions are kilobytes
        if (!isNumber) {
            return std::nullopt;
        }
        const size_t contentLength = static_cast<size_t>(std::stoul(lengthText));
        if (contentLength > kMaxBodyBytes) {
            return std::nullopt;
        }
        std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (body.size() < contentLength) {
            return std::nullopt;  // truncated: fewer bytes than the header promised
        }
        body.resize(contentLength);
        response.body = std::move(body);
    } else {
        // Read remaining as body
        std::string remaining((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());
        response.body = remaining;
    }

    return response;
}

//
// Parse URL
//
bool RTSPClient::parseURL(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
    // Expected format: rtsp://host:port/path or rtsp://host/path

    // Check for rtsp:// prefix
    const std::string prefix = "rtsp://";
    if (url.find(prefix) != 0) {
        return false;
    }

    size_t hostStart = prefix.length();
    size_t pathStart = url.find('/', hostStart);

    std::string hostPort;
    if (pathStart != std::string::npos) {
        hostPort = url.substr(hostStart, pathStart - hostStart);
        path = url.substr(pathStart);
    } else {
        hostPort = url.substr(hostStart);
        path = "/";
    }

    // Parse host:port
    size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        const std::string portText = hostPort.substr(colon + 1);
        const bool isNumber = !portText.empty() && portText.size() <= 5 &&
            std::all_of(portText.begin(), portText.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; });
        if (!isNumber) {
            return false;
        }
        host = hostPort.substr(0, colon);
        port = static_cast<uint16_t>(std::stoi(portText));
    } else {
        host = hostPort;
        port = 554;  // Default RTSP port
    }

    return true;
}

//
// Connect to server
//
bool RTSPClient::connect() {
    if (socket_ >= 0) {
        return true;  // Already connected
    }

    // Resolve hostname
    struct hostent* he = gethostbyname(host_.c_str());
    if (!he) {
        std::cerr << "RTSPClient: Failed to resolve host " << host_ << std::endl;
        return false;
    }

    // Create socket
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        std::cerr << "RTSPClient: Failed to create socket" << std::endl;
        return false;
    }

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = timeoutMs_ / 1000;
    timeout.tv_usec = (timeoutMs_ % 1000) * 1000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Connect
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port_);
    memcpy(&serverAddr.sin_addr, he->h_addr_list[0], he->h_length);

    if (::connect(socket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "RTSPClient: Failed to connect to " << host_ << ":" << port_ << std::endl;
        close(socket_);
        socket_ = -1;
        return false;
    }

    std::cout << "RTSPClient: Connected to " << host_ << ":" << port_ << std::endl;
    return true;
}

//
// Disconnect from server
//
void RTSPClient::disconnect() {
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    sessionID_.clear();
}

//
// Read response from socket
//
std::optional<std::string> RTSPClient::readResponse() {
    if (socket_ < 0) {
        return std::nullopt;
    }

    std::string response;
    char buffer[4096];
    ssize_t bytesRead;

    // Read until we get end of headers (\r\n\r\n)
    bool headersComplete = false;
    size_t contentLength = 0;

    while (!headersComplete) {
        bytesRead = recv(socket_, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) {
            return std::nullopt;
        }

        buffer[bytesRead] = '\0';
        response.append(buffer, bytesRead);

        // Check for end of headers
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            headersComplete = true;

            // Extract Content-Length if present
            size_t clPos = response.find("Content-Length:");
            if (clPos != std::string::npos && clPos < headerEnd) {
                size_t clStart = clPos + 15;  // Length of "Content-Length:"
                size_t clEnd = response.find("\r\n", clStart);
                if (clEnd != std::string::npos) {
                    std::string clStr = response.substr(clStart, clEnd - clStart);
                    // Trim whitespace
                    size_t start = clStr.find_first_not_of(" \t");
                    if (start != std::string::npos) {
                        contentLength = std::stoul(clStr.substr(start));
                    }
                }
            }
        }
    }

    // Read body if Content-Length specified
    if (contentLength > 0) {
        size_t bodyStart = response.find("\r\n\r\n") + 4;
        size_t currentBodySize = response.size() - bodyStart;

        while (currentBodySize < contentLength) {
            bytesRead = recv(socket_, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) {
                break;
            }
            buffer[bytesRead] = '\0';
            response.append(buffer, bytesRead);
            currentBodySize += bytesRead;
        }
    }

    return response;
}

} // namespace AES67
