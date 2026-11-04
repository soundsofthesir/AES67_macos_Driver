#include "NetworkUtils.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <cstring>
#include <unistd.h>
#include <iostream>

namespace AES67 {

bool NetworkUtils::joinMulticastGroup(int sockfd, const std::string& multicastAddr, 
                                      const std::string& interfaceName) {
    struct ip_mreqn mreq;
    memset(&mreq, 0, sizeof(mreq));
    
    // Set multicast group address
    mreq.imr_multiaddr.s_addr = inet_addr(multicastAddr.c_str());
    if (mreq.imr_multiaddr.s_addr == INADDR_NONE) {
        return false; // Invalid address
    }
    
    // Set interface for multicast traffic
    mreq.imr_ifindex = if_nametoindex(interfaceName.c_str());
    if (mreq.imr_ifindex == 0) {
        // If if_nametoindex fails, try using the legacy approach
        struct in_addr interfaceAddr;
        if (inet_aton(interfaceName.c_str(), &interfaceAddr) == 0) {
            // Try to get the interface address
            struct ifaddrs *ifaddrs_ptr, *ifa;
            if (getifaddrs(&ifaddrs_ptr) == 0) {
                for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
                    if (ifa->ifa_addr == nullptr) continue;
                    if (ifa->ifa_addr->sa_family == AF_INET && 
                        interfaceName == ifa->ifa_name) {
                        mreq.imr_address = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                        break;
                    }
                }
                freeifaddrs(ifaddrs_ptr);
            }
        } else {
            mreq.imr_address = interfaceAddr;
        }
    }
    
    // Join multicast group
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("Failed to join multicast group");
        return false;
    }
    
    return true;
}

bool NetworkUtils::leaveMulticastGroup(int sockfd, const std::string& multicastAddr, 
                                       const std::string& interfaceName) {
    struct ip_mreqn mreq;
    memset(&mreq, 0, sizeof(mreq));
    
    // Set multicast group address
    mreq.imr_multiaddr.s_addr = inet_addr(multicastAddr.c_str());
    if (mreq.imr_multiaddr.s_addr == INADDR_NONE) {
        return false; // Invalid address
    }
    
    // Set interface
    mreq.imr_ifindex = if_nametoindex(interfaceName.c_str());
    if (mreq.imr_ifindex == 0) {
        struct in_addr interfaceAddr;
        if (inet_aton(interfaceName.c_str(), &interfaceAddr) == 0) {
            // Try to get the interface address
            struct ifaddrs *ifaddrs_ptr, *ifa;
            if (getifaddrs(&ifaddrs_ptr) == 0) {
                for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
                    if (ifa->ifa_addr == nullptr) continue;
                    if (ifa->ifa_addr->sa_family == AF_INET && 
                        interfaceName == ifa->ifa_name) {
                        mreq.imr_address = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                        break;
                    }
                }
                freeifaddrs(ifaddrs_ptr);
            }
        } else {
            mreq.imr_address = interfaceAddr;
        }
    }
    
    // Leave multicast group
    if (setsockopt(sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("Failed to leave multicast group");
        return false;
    }
    
    return true;
}

bool NetworkUtils::setQoSTrafficClass(int sockfd, int dscpValue) {
    // Convert DSCP value to the format expected by IP_TOS
    // DSCP is upper 6 bits of TOS field
    int tosValue = (dscpValue & 0x3F) << 2;
    
    if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tosValue, sizeof(tosValue)) < 0) {
        perror("Failed to set IP_TOS for QoS");
        return false;
    }
    
    return true;
}

bool NetworkUtils::bindToInterface(int sockfd, const std::string& interfaceName) {
#ifdef __APPLE__
    // macOS binds by interface index. SO_BINDTODEVICE exists only in newer
    // SDKs, and the driver still builds against older ones.
    unsigned int if_index = if_nametoindex(interfaceName.c_str());
    if (if_index == 0) {
        return false;
    }
    if (setsockopt(sockfd, IPPROTO_IP, IP_BOUND_IF, &if_index, sizeof(if_index)) < 0) {
        perror("Failed to bind to interface");
        return false;
    }
    return true;
#elif defined(SO_BINDTODEVICE)
    // Linux; may require root privileges
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE,
                   interfaceName.c_str(), interfaceName.length()) < 0) {
        perror("Failed to bind to interface");
        return false;
    }
    return true;
#else
    (void)sockfd;
    (void)interfaceName;
    return false;
#endif
}

std::vector<std::string> NetworkUtils::getNetworkInterfaces() {
    std::vector<std::string> interfaces;
    struct ifaddrs *ifaddrs_ptr, *ifa;
    
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {  // IPv4 interfaces
                interfaces.push_back(ifa->ifa_name);
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }
    
    return interfaces;
}

std::string NetworkUtils::getPrimaryEthernetInterface() {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    std::string primaryInterface;
    
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {  // IPv4 interfaces
                std::string name(ifa->ifa_name);
                
                // Look for ethernet interfaces (typically named en0, en1, etc.)
                if (name.substr(0, 2) == "en" && isdigit(name[2])) {
                    // Prefer interfaces that are up and running
                    if ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING)) {
                        primaryInterface = name;
                        break;  // Return first active ethernet interface
                    }
                    // If we haven't found any active interface yet, keep this one as backup
                    if (primaryInterface.empty()) {
                        primaryInterface = name;
                    }
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }
    
    return primaryInterface;
}

bool NetworkUtils::isValidMulticastAddress(const std::string& addr) {
    struct in_addr in_addr_var;
    if (inet_aton(addr.c_str(), &in_addr_var) == 0) {
        return false; // Not a valid IP address
    }

    // Check if it's in the multicast range (224.0.0.0 to 239.255.255.255)
    uint32_t ip = ntohl(in_addr_var.s_addr);
    return (ip >= 0xE0000000 && ip <= 0xEFFFFFFF); // 224.0.0.0 to 239.255.255.255
}

bool NetworkUtils::hasMulticastRoute(const std::string& interfaceName) {
    // On macOS, check routing table for 239.0.0.0/8
    // Use netstat -rn to list routes, grep for 239 range
    FILE* fp = popen("netstat -rn 2>/dev/null | grep -E '^239'", "r");
    if (!fp) {
        return false;
    }

    char buffer[256];
    bool hasRoute = false;

    // Read output line by line
    while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
        // If interfaceName is specified, check if it matches
        if (!interfaceName.empty()) {
            // netstat output format: destination gateway flags refs use if
            // Example: 239.0.0.0/8     192.168.1.1    UGSc  0  0  en0
            std::string line(buffer);

            // Simple check: does the line contain the interface name?
            if (line.find(interfaceName) != std::string::npos) {
                hasRoute = true;
                break;
            }
        } else {
            // Any route to 239.x.x.x is acceptable
            hasRoute = true;
            break;
        }
    }

    pclose(fp);
    return hasRoute;
}

std::string NetworkUtils::getMulticastRouteCommand(const std::string& interfaceName) {
    std::string iface = interfaceName.empty() ? "en0" : interfaceName;
    return "sudo route add -net 239.0.0.0/8 -interface " + iface;
}

bool NetworkUtils::isIPv4Address(const std::string& str) {
    if (str.empty()) return false;

    struct in_addr addr;
    return inet_pton(AF_INET, str.c_str(), &addr) == 1;
}

std::string NetworkUtils::getInterfaceIP(const std::string& interfaceName) {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    std::string result;

    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET &&
                interfaceName == ifa->ifa_name) {
                char ip[INET_ADDRSTRLEN];
                struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                result = ip;
                break;
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }

    return result;
}

std::string NetworkUtils::resolveInterfaceToIP(const std::string& interfaceSpec) {
    // Empty string -> auto-detect best interface
    if (interfaceSpec.empty()) {
        std::string primaryIface = getPrimaryEthernetInterface();
        if (!primaryIface.empty()) {
            return getInterfaceIP(primaryIface);
        }
        return "";
    }

    // Already an IP address -> return as-is
    if (isIPv4Address(interfaceSpec)) {
        return interfaceSpec;
    }

    // Interface name -> resolve to IP
    return getInterfaceIP(interfaceSpec);
}

std::vector<std::pair<std::string, std::string>> NetworkUtils::getActiveInterfacesWithIPs() {
    std::vector<std::pair<std::string, std::string>> result;
    struct ifaddrs *ifaddrs_ptr, *ifa;

    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;

            // Skip loopback
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;

            // Only include interfaces that are up
            if (!(ifa->ifa_flags & IFF_UP)) continue;

            char ip[INET_ADDRSTRLEN];
            struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));

            result.emplace_back(ifa->ifa_name, ip);
        }
        freeifaddrs(ifaddrs_ptr);
    }

    return result;
}

} // namespace AES67