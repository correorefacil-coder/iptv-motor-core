#pragma once

#include <string>
#include <thread>
#include <atomic>
#include "third_party/httplib.h"

class HTTPServer {
public:
    HTTPServer(int port, const std::string& www_dir);
    ~HTTPServer();

    void Start();
    void Stop();

private:
    void ServerLoop();

    int port_;
    std::string www_dir_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    httplib::Server svr_;
};
