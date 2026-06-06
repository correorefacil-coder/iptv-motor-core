#pragma once

#include <string>
#include <vector>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include "logger.h"

struct NetworkInterfaceInfo {
    std::string name;
    std::string ip;
    bool is_up;
    bool is_loopback;
};

inline std::vector<NetworkInterfaceInfo> GetNetworkInterfaces() {
    std::vector<NetworkInterfaceInfo> list;
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        LOG_ERROR("Error getting network interfaces: getifaddrs failed");
        return list;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        
        // We only care about IPv4 interfaces for streaming
        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            char host[256] = {0};
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                               host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
            if (s == 0) {
                NetworkInterfaceInfo info;
                info.name = ifa->ifa_name;
                info.ip = host;
                info.is_up = (ifa->ifa_flags & IFF_UP) != 0;
                info.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
                list.push_back(info);
            }
        }
    }
    freeifaddrs(ifaddr);
    return list;
}

inline std::string GetInterfaceIP(const std::string& name) {
    if (name.empty()) return "";
    auto list = GetNetworkInterfaces();
    for (const auto& info : list) {
        if (info.name == name) {
            return info.ip;
        }
    }
    return "";
}
