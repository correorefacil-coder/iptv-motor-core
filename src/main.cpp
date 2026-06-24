#include <iostream>
#include <csignal>
#include "logger.h"
#include "engine.h"
#include "http_server.h"

std::unique_ptr<HTTPServer> g_http_server;

void SignalHandler(int signum) {
    LOG_INFO("Se recibió señal de detención (" + std::to_string(signum) + "). Apagando el sistema...");
    
    if (g_http_server) {
        g_http_server->Stop();
    }
    
    StreamerEngine::GetInstance().Shutdown();
    
    LOG_INFO("Sistema apagado correctamente. ¡Adiós!");
    exit(signum);
}

int main(int argc, char* argv[]) {
    // Register signal handlers for clean exit
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    LOG_INFO("Iniciando Antigravity IPTV Gateway...");

    int port = 45020;
    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
        } catch (...) {
            LOG_WARN("Puerto inválido proporcionado en argumentos. Usando puerto por defecto: 45020");
        }
    }

    std::string config_path = "config.json";
    if (argc > 2) {
        config_path = argv[2];
    }

    // Initialize Streamer Engine
    StreamerEngine::GetInstance().Init(config_path);

    // Start HTTPServer
    g_http_server = std::make_unique<HTTPServer>(port, "./www");
    g_http_server->Start();

    // Main thread wait loop
    LOG_INFO("Antigravity IPTV Gateway listo. Presiona Ctrl+C para salir.");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
