#include "http_server.h"
#include "engine.h"
#include "logger.h"
#include "system_stats.h"
#include "network_utils.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <limits>

using json = nlohmann::json;

struct User {
    std::string username;
    std::string password;
    std::string role; // "SuperAdmin", "Admin", "Consulta", "Programadores"
    std::vector<std::string> allowed_streams;
};

class UserManager {
public:
    static UserManager& GetInstance() {
        static UserManager instance;
        return instance;
    }

    void Init(const std::string& path = "users.json") {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        Load();
    }

    bool Authenticate(const std::string& username, const std::string& password, std::string& role_out) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it != users_.end() && it->second.password == password) {
            role_out = it->second.role;
            return true;
        }
        return false;
    }

    bool UserExists(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        return users_.find(username) != users_.end();
    }

    std::string GetRole(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it != users_.end()) {
            return it->second.role;
        }
        return "";
    }

    std::vector<std::string> GetAllowedStreams(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it != users_.end()) {
            return it->second.allowed_streams;
        }
        return {};
    }

    json GetUsersJSON(const std::string& requesting_user) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string req_role = "";
        auto req_it = users_.find(requesting_user);
        if (req_it != users_.end()) {
            req_role = req_it->second.role;
        }

        json arr = json::array();
        for (const auto& pair : users_) {
            const auto& u = pair.second;
            bool allowed = false;
            if (req_role == "SuperAdmin") {
                allowed = true;
            } else if (req_role == "Admin") {
                if (u.role != "SuperAdmin" && u.username != "Cristian") {
                    allowed = true;
                }
            } else if (req_role == "Consulta" || req_role == "Programadores") {
                if (u.username == requesting_user) {
                    allowed = true;
                }
            }
            if (allowed) {
                json item;
                item["username"] = u.username;
                item["password"] = u.password;
                item["role"] = u.role;
                item["allowed_streams"] = json::array();
                for (const auto& p : u.allowed_streams) {
                    item["allowed_streams"].push_back(p);
                }
                arr.push_back(item);
            }
        }
        return arr;
    }

    bool CreateUser(const std::string& requesting_user, const std::string& username, const std::string& password, const std::string& role, const std::vector<std::string>& allowed_streams, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string req_role = "";
        auto req_it = users_.find(requesting_user);
        if (req_it != users_.end()) {
            req_role = req_it->second.role;
        }

        if (req_role != "SuperAdmin" && req_role != "Admin") {
            err = "No autorizado para crear usuarios";
            return false;
        }

        if (role == "SuperAdmin" && req_role != "SuperAdmin") {
            err = "No autorizado para asignar el rol SuperAdmin";
            return false;
        }

        if (users_.find(username) != users_.end()) {
            err = "El usuario ya existe";
            return false;
        }

        if (username.empty() || password.empty() || role.empty()) {
            err = "Campos vacíos";
            return false;
        }

        User u{username, password, role, allowed_streams};
        users_[username] = u;
        Save();
        return true;
    }

    bool UpdateUser(const std::string& requesting_user, const std::string& target_username, const std::string& new_password, const std::string& new_role, const std::vector<std::string>& allowed_streams, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto req_it = users_.find(requesting_user);
        if (req_it == users_.end()) {
            err = "Usuario solicitante no existe";
            return false;
        }
        std::string req_role = req_it->second.role;

        auto target_it = users_.find(target_username);
        if (target_it == users_.end()) {
            err = "Usuario objetivo no existe";
            return false;
        }

        if (req_role == "Consulta" || req_role == "Programadores") {
            if (requesting_user != target_username) {
                err = "No autorizado para modificar otros usuarios";
                return false;
            }
            if (new_role != target_it->second.role) {
                err = "No autorizado para cambiar su propio rol";
                return false;
            }
            if (new_password.empty()) {
                err = "La contraseña no puede estar vacía";
                return false;
            }
            target_it->second.password = new_password;
            Save();
            return true;
        }

        if (req_role == "Admin") {
            if (target_it->second.role == "SuperAdmin" || target_it->second.username == "Cristian") {
                err = "No autorizado para modificar a este usuario";
                return false;
            }
            if (new_role == "SuperAdmin") {
                err = "No autorizado para asignar el rol SuperAdmin";
                return false;
            }
            if (!new_password.empty()) {
                target_it->second.password = new_password;
            }
            target_it->second.role = new_role;
            target_it->second.allowed_streams = allowed_streams;
            Save();
            return true;
        }

        if (req_role == "SuperAdmin") {
            if (!new_password.empty()) {
                target_it->second.password = new_password;
            }
            target_it->second.role = new_role;
            target_it->second.allowed_streams = allowed_streams;
            Save();
            return true;
        }

        err = "Rol desconocido";
        return false;
    }

    bool DeleteUser(const std::string& requesting_user, const std::string& target_username, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto req_it = users_.find(requesting_user);
        if (req_it == users_.end()) {
            err = "Usuario solicitante no existe";
            return false;
        }
        std::string req_role = req_it->second.role;

        auto target_it = users_.find(target_username);
        if (target_it == users_.end()) {
            err = "Usuario objetivo no existe";
            return false;
        }

        if (requesting_user == target_username) {
            err = "No puedes eliminarte a ti mismo";
            return false;
        }

        if (req_role == "Consulta" || req_role == "Programadores") {
            err = "No autorizado para eliminar usuarios";
            return false;
        }

        if (req_role == "Admin") {
            if (target_it->second.role == "SuperAdmin" || target_it->second.username == "Cristian") {
                err = "No autorizado para eliminar a este usuario";
                return false;
            }
            users_.erase(target_it);
            Save();
            return true;
        }

        if (req_role == "SuperAdmin") {
            users_.erase(target_it);
            Save();
            return true;
        }

        err = "Rol desconocido";
        return false;
    }

private:
    std::string path_;
    std::map<std::string, User> users_;
    std::mutex mutex_;

    void Load() {
        std::ifstream f(path_);
        if (!f.is_open()) {
            users_["IngCristian"] = User{"IngCristian", "CARE90po#", "SuperAdmin", {}};
            users_["Cristian"] = User{"Cristian", "cristian123", "SuperAdmin", {}};
            users_["admin"] = User{"admin", "admin123", "Admin", {}};
            users_["consultas"] = User{"consultas", "consultas123", "Consulta", {}};
            users_["cafeteria"] = User{"cafeteria", "cafeteria", "Programadores", {"stream_1780604845893654388"}};
            users_["comunicaciones"] = User{"comunicaciones", "comunicaciones", "Programadores", {
                "stream_inst_1",
                "stream_inst_2",
                "stream_inst_3",
                "stream_inst_4",
                "stream_inst_5"
            }};
            Save();
            return;
        }
        try {
            json j = json::parse(f);
            for (const auto& item : j) {
                std::string username = item.value("username", "");
                std::string password = item.value("password", "");
                std::string role = item.value("role", "");
                std::vector<std::string> allowed_streams;
                if (item.contains("allowed_streams") && item["allowed_streams"].is_array()) {
                    for (const auto& p : item["allowed_streams"]) {
                        allowed_streams.push_back(p.get<std::string>());
                    }
                }
                if (!username.empty() && !password.empty() && !role.empty()) {
                    users_[username] = User{username, password, role, allowed_streams};
                }
            }
        } catch (...) {
            std::cerr << "Error parsing users.json" << std::endl;
        }
    }

    void Save() {
        std::ofstream f(path_);
        if (!f.is_open()) return;
        json arr = json::array();
        for (const auto& pair : users_) {
            json item;
            item["username"] = pair.second.username;
            item["password"] = pair.second.password;
            item["role"] = pair.second.role;
            item["allowed_streams"] = json::array();
            for (const auto& p : pair.second.allowed_streams) {
                item["allowed_streams"].push_back(p);
            }
            arr.push_back(item);
        }
        f << arr.dump(4);
    }
};

struct UserLogEntry {
    std::string timestamp;
    std::string username;
    std::string action;
};

class UserLogger {
public:
    static UserLogger& GetInstance() {
        static UserLogger instance;
        return instance;
    }

    void Init(const std::string& path = "user_logs.json") {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        Load();
    }

    void LogAction(const std::string& username, const std::string& action) {
        std::lock_guard<std::mutex> lock(mutex_);
        UserLogEntry entry;
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
        entry.timestamp = ss.str();
        entry.username = username;
        entry.action = action;
        logs_.push_back(entry);
        Save();
    }

    json GetLogsJSON() {
        std::lock_guard<std::mutex> lock(mutex_);
        json arr = json::array();
        for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
            json item;
            item["timestamp"] = it->timestamp;
            item["username"] = it->username;
            item["action"] = it->action;
            arr.push_back(item);
        }
        return arr;
    }

private:
    std::string path_;
    std::vector<UserLogEntry> logs_;
    std::mutex mutex_;

    void Load() {
        std::ifstream f(path_);
        if (!f.is_open()) return;
        try {
            json j = json::parse(f);
            for (const auto& item : j) {
                UserLogEntry entry;
                entry.timestamp = item.value("timestamp", "");
                entry.username = item.value("username", "");
                entry.action = item.value("action", "");
                logs_.push_back(entry);
            }
        } catch (...) {
            std::cerr << "Error parsing user_logs.json" << std::endl;
        }
    }

    void Save() {
        std::ofstream f(path_);
        if (!f.is_open()) return;
        json arr = json::array();
        for (const auto& log : logs_) {
            json item;
            item["timestamp"] = log.timestamp;
            item["username"] = log.username;
            item["action"] = log.action;
            arr.push_back(item);
        }
        f << arr.dump(4);
    }
};

static std::string GetSessionUser(const httplib::Request& req) {
    std::string param_user = req.get_param_value("session");
    if (!param_user.empty()) return param_user;

    std::string cookie = req.get_header_value("Cookie");
    size_t pos = cookie.find("session=");
    if (pos == std::string::npos) return "";
    pos += 8;
    size_t end = cookie.find(";", pos);
    std::string user = (end == std::string::npos) ? cookie.substr(pos) : cookie.substr(pos, end - pos);
    if (user == "deleted") return "";
    return user;
}

static bool IsVideoFileUrl(const std::string& url) {
    std::string lower_url = url;
    std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    if (lower_url.rfind("srt://", 0) == 0 ||
        lower_url.rfind("udp://", 0) == 0 ||
        lower_url.rfind("rtmp://", 0) == 0 ||
        lower_url.rfind("rtsp://", 0) == 0 ||
        lower_url.rfind("rtp://", 0) == 0 ||
        lower_url.rfind("http://", 0) == 0 ||
        lower_url.rfind("https://", 0) == 0) {
        return false;
    }
    return true;
}

static bool IsStreamAllowed(const std::string& username, const std::string& stream_id) {
    std::string role = UserManager::GetInstance().GetRole(username);
    if (role == "SuperAdmin" || role == "Admin") return true;
    if (role == "Consulta") return true;
    if (role != "Programadores") return false;
    
    auto allowed = UserManager::GetInstance().GetAllowedStreams(username);
    return std::find(allowed.begin(), allowed.end(), stream_id) != allowed.end();
}

static bool IsInputAllowed(const std::string& username, const std::string& input_id) {
    std::string role = UserManager::GetInstance().GetRole(username);
    if (role == "SuperAdmin" || role == "Admin") return true;
    if (role == "Consulta") return true;
    if (role != "Programadores") return false;

    auto allowed_streams = UserManager::GetInstance().GetAllowedStreams(username);
    json streams_list = StreamerEngine::GetInstance().GetStreamsJSON();
    for (const auto& item : streams_list) {
        std::string stream_id = item.value("id", "");
        if (std::find(allowed_streams.begin(), allowed_streams.end(), stream_id) != allowed_streams.end()) {
            if (item.value("input_id", "") == input_id) {
                return true;
            }
        }
    }
    return false;
}

static bool CheckProgramadorBlock(const httplib::Request& req, httplib::Response& res) {
    std::string user = GetSessionUser(req);
    std::string role = UserManager::GetInstance().GetRole(user);
    if (role == "Programadores") {
        res.status = 403;
        res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Los programadores no tienen acceso a este recurso.\"}", "application/json");
        return false;
    }
    return true;
}

static bool CheckWritePermission(const httplib::Request& req, httplib::Response& res) {
    std::string user = GetSessionUser(req);
    std::string role = UserManager::GetInstance().GetRole(user);
    if (role == "Consulta") {
        res.status = 403;
        res.set_content("{\"success\":false,\"error\":\"Acceso denegado: El rol Consulta no tiene permisos de edición.\"}", "application/json");
        return false;
    }
    return true;
}

HTTPServer::HTTPServer(int port, const std::string& www_dir)
    : port_(port), www_dir_(www_dir) {}

HTTPServer::~HTTPServer() {
    Stop();
}

void HTTPServer::Start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&HTTPServer::ServerLoop, this);
    LOG_INFO("Servidor HTTP iniciado en el puerto " + std::to_string(port_));
}

void HTTPServer::Stop() {
    if (!running_) return;
    running_ = false;
    svr_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG_INFO("Servidor HTTP detenido.");
}

void HTTPServer::ServerLoop() {
    SystemStats sys_stats;
    UserManager::GetInstance().Init();
    UserLogger::GetInstance().Init();

    // Permitir subir archivos de cualquier tamaño sin límites (ej. videos de gran tamaño)
    svr_.set_payload_max_length((std::numeric_limits<size_t>::max)());

    if (!svr_.set_mount_point("/", www_dir_)) {
        LOG_WARN("No se pudo montar la carpeta web: " + www_dir_ + ". Solo funcionará la API REST.");
    }

    svr_.set_post_routing_handler([](const auto& req, auto& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });

    svr_.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.path == "/login.html" || req.path == "/login" || req.path == "/logo.png" || req.path == "/Logo.PNG" || req.path == "/style.css" || req.path == "/favicon.ico" || req.path == "/app.js") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        std::string user = GetSessionUser(req);
        if (!user.empty() && UserManager::GetInstance().UserExists(user)) {
            if (!req.get_param_value("session").empty()) {
                res.set_header("Set-Cookie", "session=" + user + "; Path=/; HttpOnly; Max-Age=3600");
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        if (req.path.rfind("/api/", 0) == 0) {
            res.status = 401;
            res.set_content("{\"success\":false,\"error\":\"No autorizado\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        res.set_redirect("/login.html");
        return httplib::Server::HandlerResponse::Handled;
    });

    svr_.Post("/login", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");

            std::string role;
            if (UserManager::GetInstance().Authenticate(username, password, role)) {
                res.set_header("Set-Cookie", "session=" + username + "; Path=/; HttpOnly; Max-Age=3600");
                json r;
                r["success"] = true;
                r["redirect"] = "/";
                res.set_content(r.dump(), "application/json");
                UserLogger::GetInstance().LogAction(username, "Inició sesión");
            } else {
                res.status = 401;
                res.set_content("{\"success\":false,\"message\":\"Usuario o contraseña incorrectos.\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"message\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Get("/logout", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = GetSessionUser(req);
        if (!user.empty()) {
            UserLogger::GetInstance().LogAction(user, "Cerró sesión");
        }
        res.set_header("Set-Cookie", "session=deleted; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; HttpOnly");
        res.set_redirect("/login.html");
    });

    svr_.Options(R"(/api/.*)", [](const httplib::Request& req, httplib::Response& res) {
        res.status = 204;
    });

    svr_.Get("/api/status", [&](const httplib::Request& req, httplib::Response& res) {
        json j = StreamerEngine::GetInstance().GetStatusJSON();
        j["cpu_usage"] = sys_stats.GetCPUUsage();
        j["mem_usage"] = sys_stats.GetMemoryUsage();
        
        auto gpu = sys_stats.GetGPUStats();
        j["gpu_usage"] = gpu.gpu_usage;
        j["gpu_mem_usage"] = gpu.gpu_mem_usage;
        j["gpu_mem_used"] = gpu.gpu_mem_used;
        j["gpu_mem_total"] = gpu.gpu_mem_total;
        j["gpu_available"] = gpu.available;

        res.set_content(j.dump(), "application/json");
    });

    svr_.Get("/api/me", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        json r;
        r["username"] = user;
        r["role"] = role;
        res.set_content(r.dump(), "application/json");
    });

    svr_.Get("/api/users", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = GetSessionUser(req);
        json arr = UserManager::GetInstance().GetUsersJSON(user);
        res.set_content(arr.dump(), "application/json");
    });

    svr_.Post("/api/users", [](const httplib::Request& req, httplib::Response& res) {
        std::string requesting_user = GetSessionUser(req);
        try {
            auto body = json::parse(req.body);
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            std::string role = body.value("role", "");
            std::vector<std::string> allowed_streams;
            if (body.contains("allowed_streams") && body["allowed_streams"].is_array()) {
                for (const auto& p : body["allowed_streams"]) {
                    allowed_streams.push_back(p.get<std::string>());
                }
            }
            
            std::string err;
            if (UserManager::GetInstance().CreateUser(requesting_user, username, password, role, allowed_streams, err)) {
                json r;
                r["success"] = true;
                res.set_content(r.dump(), "application/json");
                UserLogger::GetInstance().LogAction(requesting_user, "Creó el usuario '" + username + "' con rol '" + role + "'");
            } else {
                res.status = 400;
                res.set_content("{\"success\":false,\"error\":\"" + err + "\"}", "application/json");
            }
        } catch (...) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Put(R"(/api/users/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string requesting_user = GetSessionUser(req);
        std::string target_user = req.matches[1];
        try {
            auto body = json::parse(req.body);
            std::string password = body.value("password", "");
            std::string role = body.value("role", "");
            std::vector<std::string> allowed_streams;
            if (body.contains("allowed_streams") && body["allowed_streams"].is_array()) {
                for (const auto& p : body["allowed_streams"]) {
                    allowed_streams.push_back(p.get<std::string>());
                }
            }
            
            std::string err;
            if (UserManager::GetInstance().UpdateUser(requesting_user, target_user, password, role, allowed_streams, err)) {
                json r;
                r["success"] = true;
                res.set_content(r.dump(), "application/json");
                std::string details = "Modificó al usuario '" + target_user + "'";
                if (!password.empty()) details += " (cambió contraseña)";
                if (!role.empty()) details += " (rol: " + role + ")";
                UserLogger::GetInstance().LogAction(requesting_user, details);
            } else {
                res.status = 400;
                res.set_content("{\"success\":false,\"error\":\"" + err + "\"}", "application/json");
            }
        } catch (...) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Delete(R"(/api/users/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string requesting_user = GetSessionUser(req);
        std::string target_user = req.matches[1];
        std::string err;
        if (UserManager::GetInstance().DeleteUser(requesting_user, target_user, err)) {
            json r;
            r["success"] = true;
            res.set_content(r.dump(), "application/json");
            UserLogger::GetInstance().LogAction(requesting_user, "Eliminó al usuario '" + target_user + "'");
        } else {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"" + err + "\"}", "application/json");
        }
    });

    svr_.Get("/api/user_logs", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role != "SuperAdmin" && role != "Admin") {
            res.status = 403;
            res.set_content("{\"success\":false,\"error\":\"Acceso denegado a los logs de actividad\"}", "application/json");
            return;
        }
        json logs = UserLogger::GetInstance().GetLogsJSON();
        res.set_content(logs.dump(), "application/json");
    });

    svr_.Get("/api/inputs", [](const httplib::Request& req, httplib::Response& res) {
        json j = StreamerEngine::GetInstance().GetInputsJSON();
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role == "Programadores") {
            json filtered = json::array();
            for (auto& item : j) {
                std::string id = item.value("id", "");
                bool is_video_pack = item.value("is_video_pack", false);
                if (IsInputAllowed(user, id) && is_video_pack) {
                    filtered.push_back(item);
                }
            }
            j = filtered;
        } else if (role == "Consulta") {
            for (auto& item : j) {
                item["url"] = "********";
            }
        }
        res.set_content(j.dump(), "application/json");
    });

    svr_.Post("/api/inputs", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role == "Programadores") {
            res.status = 403;
            res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Los programadores no pueden crear paquetes de canales.\"}", "application/json");
            return;
        }
        try {
            auto body = json::parse(req.body);
            std::string name = body["name"];
            std::string url = body["url"];
            bool enabled = body.value("enabled", true);
            bool is_video_pack = body.value("is_video_pack", false);
            
            std::string id_out;
            if (StreamerEngine::GetInstance().AddInput(name, url, enabled, is_video_pack, id_out)) {
                json r;
                r["success"] = true;
                r["id"] = id_out;
                res.set_content(r.dump(), "application/json");
                UserLogger::GetInstance().LogAction(user, "Agregó entrada Pack '" + name + "'");
            } else {
                res.status = 500;
                res.set_content("{\"success\":false,\"error\":\"Error interno al agregar entrada\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Put(R"(/api/inputs/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role == "Programadores") {
            res.status = 403;
            res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Los programadores no pueden editar paquetes de canales.\"}", "application/json");
            return;
        }
        std::string id = req.matches[1];
        try {
            auto body = json::parse(req.body);
            std::string name = body["name"];
            std::string url = body["url"];
            bool enabled = body.value("enabled", true);
            bool is_video_pack = body.value("is_video_pack", false);

            if (StreamerEngine::GetInstance().UpdateInput(id, name, url, enabled, is_video_pack)) {
                res.set_content("{\"success\":true}", "application/json");
                UserLogger::GetInstance().LogAction(user, "Modificó entrada Pack '" + name + "'");
            } else {
                res.status = 404;
                res.set_content("{\"success\":false,\"error\":\"Entrada no encontrada\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Delete(R"(/api/inputs/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role == "Programadores") {
            res.status = 403;
            res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Los programadores no pueden eliminar paquetes.\"}", "application/json");
            return;
        }
        std::string id = req.matches[1];
        
        std::string name = id;
        json inputs_list = StreamerEngine::GetInstance().GetInputsJSON();
        for (const auto& item : inputs_list) {
            if (item.value("id", "") == id) {
                name = item.value("name", "");
                break;
            }
        }

        if (StreamerEngine::GetInstance().DeleteInput(id)) {
            res.set_content("{\"success\":true}", "application/json");
            UserLogger::GetInstance().LogAction(user, "Eliminó entrada Pack '" + name + "'");
        } else {
            res.status = 404;
            res.set_content("{\"success\":false,\"error\":\"Entrada no encontrada\"}", "application/json");
        }
    });

    svr_.Post("/api/inputs/probe", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        try {
            auto body = json::parse(req.body);
            std::string url = body["url"];
            
            if (role == "Programadores") {
                bool is_allowed_dir = false;
                json inputs_list = StreamerEngine::GetInstance().GetInputsJSON();
                for (const auto& item : inputs_list) {
                    std::string input_id = item.value("id", "");
                    std::string input_url = item.value("url", "");
                    if (IsInputAllowed(user, input_id) && url.rfind(input_url, 0) == 0) {
                        is_allowed_dir = true;
                        break;
                    }
                }
                if (!is_allowed_dir) {
                    res.status = 403;
                    res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Solo puede analizar carpetas y archivos de video de sus paquetes permitidos.\"}", "application/json");
                    return;
                }
            }

            json result = StreamerEngine::GetInstance().ProbeInputURL(url);
            res.set_content(result.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido o URL faltante\"}", "application/json");
        }
    });

    svr_.Get("/api/streams", [](const httplib::Request& req, httplib::Response& res) {
        json j = StreamerEngine::GetInstance().GetStreamsJSON();
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role == "Programadores") {
            json filtered = json::array();
            for (auto& item : j) {
                std::string stream_id = item.value("id", "");
                std::string input_id = item.value("input_id", "");
                bool is_video_pack = StreamerEngine::GetInstance().IsInputVideoPack(input_id);
                if (IsStreamAllowed(user, stream_id) && is_video_pack) {
                    filtered.push_back(item);
                }
            }
            j = filtered;
        }
        res.set_content(j.dump(), "application/json");
    });

    svr_.Post("/api/streams", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role == "Programadores") {
            res.status = 403;
            res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Los programadores no pueden crear canales.\"}", "application/json");
            return;
        }
        try {
            auto body = json::parse(req.body);
            std::string name = body["name"];
            std::string input_id = body["input_id"];
            int program_number = body["program_number"];
            bool enabled = body.value("enabled", true);

            std::vector<OutputDestination> outputs;
            if (body.contains("outputs") && body["outputs"].is_array()) {
                for (const auto& out_item : body["outputs"]) {
                    OutputDestination dest;
                    dest.url = out_item.value("url", "");
                    dest.output_interface = out_item.value("output_interface", "");
                    dest.type = out_item.value("type", "");
                    outputs.push_back(dest);
                }
            } else {
                OutputDestination dest;
                dest.url = body.value("output_url", "");
                dest.output_interface = body.value("output_interface", "");
                if (dest.url.rfind("udp://", 0) == 0) dest.type = "udp";
                else if (dest.url.rfind("srt://", 0) == 0) dest.type = "srt";
                else if (dest.url.rfind("rtp://", 0) == 0) dest.type = "rtp";
                else dest.type = "hls";
                outputs.push_back(dest);
            }

            bool transcode_enabled = body.value("transcode_enabled", false);
            bool transcode_video = body.value("transcode_video", false);
            std::string video_input_format = body.value("video_input_format", "");
            std::string video_output_format = body.value("video_output_format", "");
            bool transcode_audio = body.value("transcode_audio", false);
            std::string audio_input_format = body.value("audio_input_format", "");
            std::string audio_output_format = body.value("audio_output_format", "");
            int limit_bitrate = body.value("limit_bitrate", 0);
            std::string video_filename = body.value("video_filename", "");

            std::string id_out;
            if (StreamerEngine::GetInstance().AddStream(name, input_id, program_number, outputs, enabled,
                                                       transcode_enabled, transcode_video, video_input_format, video_output_format,
                                                       transcode_audio, audio_input_format, audio_output_format, limit_bitrate, video_filename, id_out)) {
                json r;
                r["success"] = true;
                r["id"] = id_out;
                res.set_content(r.dump(), "application/json");
                std::string log_url = outputs.empty() ? "" : outputs[0].url;
                UserLogger::GetInstance().LogAction(user, "Agregó canal '" + name + "' (Salida: " + log_url + ")");
            } else {
                res.status = 500;
                res.set_content("{\"success\":false,\"error\":\"Error interno al agregar stream\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Put(R"(/api/streams/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        std::string id = req.matches[1];
        try {
            auto body = json::parse(req.body);
            std::string name = body["name"];
            std::string input_id = body["input_id"];
            int program_number = body["program_number"];
            bool enabled = body.value("enabled", true);

            std::vector<OutputDestination> outputs;
            if (body.contains("outputs") && body["outputs"].is_array()) {
                for (const auto& out_item : body["outputs"]) {
                    OutputDestination dest;
                    dest.url = out_item.value("url", "");
                    dest.output_interface = out_item.value("output_interface", "");
                    dest.type = out_item.value("type", "");
                    outputs.push_back(dest);
                }
            } else {
                OutputDestination dest;
                dest.url = body.value("output_url", "");
                dest.output_interface = body.value("output_interface", "");
                if (dest.url.rfind("udp://", 0) == 0) dest.type = "udp";
                else if (dest.url.rfind("srt://", 0) == 0) dest.type = "srt";
                else if (dest.url.rfind("rtp://", 0) == 0) dest.type = "rtp";
                else dest.type = "hls";
                outputs.push_back(dest);
            }

            bool transcode_enabled = body.value("transcode_enabled", false);
            bool transcode_video = body.value("transcode_video", false);
            std::string video_input_format = body.value("video_input_format", "");
            std::string video_output_format = body.value("video_output_format", "");
            bool transcode_audio = body.value("transcode_audio", false);
            std::string audio_input_format = body.value("audio_input_format", "");
            std::string audio_output_format = body.value("audio_output_format", "");
            int limit_bitrate = body.value("limit_bitrate", 0);
            std::string video_filename = body.value("video_filename", "");

            if (role == "Programadores") {
                if (!IsStreamAllowed(user, id)) {
                    res.status = 403;
                    res.set_content("{\"success\":false,\"error\":\"Acceso denegado: No tiene permisos para modificar este canal.\"}", "application/json");
                    return;
                }

                json streams_list = StreamerEngine::GetInstance().GetStreamsJSON();
                json existing_stream;
                bool found = false;
                for (const auto& item : streams_list) {
                    if (item.value("id", "") == id) {
                        existing_stream = item;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    res.status = 404;
                    res.set_content("{\"success\":false,\"error\":\"Stream no encontrado\"}", "application/json");
                    return;
                }

                bool outputs_changed = false;
                if (body.contains("outputs") && body["outputs"].is_array() && existing_stream.contains("outputs")) {
                    if (body["outputs"] != existing_stream["outputs"]) {
                        outputs_changed = true;
                    }
                } else if (body.value("output_url", "") != existing_stream.value("output_url", "") ||
                           body.value("output_interface", "") != existing_stream.value("output_interface", "")) {
                    outputs_changed = true;
                }

                if (body.value("name", "") != existing_stream.value("name", "") ||
                    body.value("input_id", "") != existing_stream.value("input_id", "") ||
                    body.value("program_number", 0) != existing_stream.value("program_number", 0) ||
                    outputs_changed ||
                    body.value("transcode_enabled", false) != existing_stream.value("transcode_enabled", false) ||
                    body.value("transcode_video", false) != existing_stream.value("transcode_video", false) ||
                    body.value("transcode_audio", false) != existing_stream.value("transcode_audio", false) ||
                    body.value("limit_bitrate", 0) != existing_stream.value("limit_bitrate", 0)) {
                    
                    res.status = 403;
                    res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Los programadores solo pueden cambiar el archivo de video y activar/desactivar el canal.\"}", "application/json");
                    return;
                }
            }

            if (StreamerEngine::GetInstance().UpdateStream(id, name, input_id, program_number, outputs, enabled,
                                                          transcode_enabled, transcode_video, video_input_format, video_output_format,
                                                          transcode_audio, audio_input_format, audio_output_format, limit_bitrate, video_filename)) {
                res.set_content("{\"success\":true}", "application/json");
                std::string log_url = outputs.empty() ? "" : outputs[0].url;
                UserLogger::GetInstance().LogAction(user, "Modificó canal '" + name + "' (Salida: " + log_url + ", Estado: " + (enabled ? "Activado" : "Pausado") + ")");
            } else {
                res.status = 404;
                res.set_content("{\"success\":false,\"error\":\"Stream no encontrado\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Delete(R"(/api/streams/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string role = UserManager::GetInstance().GetRole(user);
        if (role == "Programadores") {
            res.status = 403;
            res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Los programadores no pueden eliminar canales.\"}", "application/json");
            return;
        }
        std::string id = req.matches[1];

        std::string name = id;
        json streams_list = StreamerEngine::GetInstance().GetStreamsJSON();
        for (const auto& item : streams_list) {
            if (item.value("id", "") == id) {
                name = item.value("name", "");
                break;
            }
        }

        if (StreamerEngine::GetInstance().DeleteStream(id)) {
            res.set_content("{\"success\":true}", "application/json");
            UserLogger::GetInstance().LogAction(user, "Eliminó canal '" + name + "'");
        } else {
            res.status = 404;
            res.set_content("{\"success\":false,\"error\":\"Stream no encontrado\"}", "application/json");
        }
    });

    svr_.Get("/api/logs", [](const httplib::Request& req, httplib::Response& res) {
        auto logs = Logger::GetInstance().GetLogs(100);
        json j = json::array();
        for (const auto& log : logs) {
            j.push_back(log);
        }
        res.set_content(j.dump(), "application/json");
    });

    svr_.Get("/api/interfaces", [](const httplib::Request& req, httplib::Response& res) {
        auto list = GetNetworkInterfaces();
        json arr = json::array();
        for (const auto& iface : list) {
            json item;
            item["name"] = iface.name;
            item["ip"] = iface.ip;
            item["is_up"] = iface.is_up;
            item["is_loopback"] = iface.is_loopback;
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    svr_.Get("/api/settings", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckProgramadorBlock(req, res)) return;
        json j;
        j["output_interface"] = StreamerEngine::GetInstance().GetOutputInterface();
        res.set_content(j.dump(), "application/json");
    });

    svr_.Post("/api/settings", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckProgramadorBlock(req, res)) return;
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        try {
            auto body = json::parse(req.body);
            std::string iface = body.value("output_interface", "");
            StreamerEngine::GetInstance().SetOutputInterface(iface);
            res.set_content("{\"success\":true}", "application/json");
            UserLogger::GetInstance().LogAction(user, "Modificó interfaz de red de salida global a '" + iface + "'");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Get("/api/messages", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckProgramadorBlock(req, res)) return;
        auto msgs = StreamerEngine::GetInstance().GetMessages();
        json j = json::array();
        for (const auto& msg : msgs) {
            json item;
            item["id"] = msg.id;
            item["text"] = msg.text;
            item["start_time"] = msg.start_time;
            item["end_time"] = msg.end_time;
            item["all_channels"] = msg.all_channels;
            item["channel_ids"] = json::array();
            for (const auto& ch : msg.channel_ids) {
                item["channel_ids"].push_back(ch);
            }
            j.push_back(item);
        }
        res.set_content(j.dump(), "application/json");
    });

    svr_.Post("/api/messages", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckProgramadorBlock(req, res)) return;
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        try {
            auto body = json::parse(req.body);
            ScheduledMessage msg;
            msg.id = "msg_" + std::to_string(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) + "_" + std::to_string(rand() % 1000);
            msg.text = body.value("text", "");
            msg.start_time = body.value("start_time", "");
            msg.end_time = body.value("end_time", "");
            msg.all_channels = body.value("all_channels", false);
            if (body.contains("channel_ids") && body["channel_ids"].is_array()) {
                for (const auto& ch : body["channel_ids"]) {
                    msg.channel_ids.push_back(ch.get<std::string>());
                }
            }
            
            if (StreamerEngine::GetInstance().AddMessage(msg)) {
                UserLogger::GetInstance().LogAction(user, "Creó mensaje programado: " + msg.text);
                res.status = 201;
                res.set_content("{\"success\":true}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"success\":false,\"error\":\"Failed to save message\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"" + std::string(e.what()) + "\"}", "application/json");
        }
    });

    svr_.Put(R"(/api/messages/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckProgramadorBlock(req, res)) return;
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string msg_id = req.matches[1];
        try {
            auto body = json::parse(req.body);
            ScheduledMessage msg;
            msg.id = msg_id;
            msg.text = body.value("text", "");
            msg.start_time = body.value("start_time", "");
            msg.end_time = body.value("end_time", "");
            msg.all_channels = body.value("all_channels", false);
            if (body.contains("channel_ids") && body["channel_ids"].is_array()) {
                for (const auto& ch : body["channel_ids"]) {
                    msg.channel_ids.push_back(ch.get<std::string>());
                }
            }
            
            if (StreamerEngine::GetInstance().UpdateMessage(msg_id, msg)) {
                UserLogger::GetInstance().LogAction(user, "Editó mensaje programado: " + msg.text);
                res.status = 200;
                res.set_content("{\"success\":true}", "application/json");
            } else {
                res.status = 404;
                res.set_content("{\"success\":false,\"error\":\"Message not found\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"" + std::string(e.what()) + "\"}", "application/json");
        }
    });

    svr_.Delete(R"(/api/messages/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckProgramadorBlock(req, res)) return;
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        std::string msg_id = req.matches[1];
        if (StreamerEngine::GetInstance().DeleteMessage(msg_id)) {
            UserLogger::GetInstance().LogAction(user, "Eliminó mensaje programado ID: " + msg_id);
            res.status = 200;
            res.set_content("{\"success\":true}", "application/json");
        } else {
            res.status = 404;
            res.set_content("{\"success\":false,\"error\":\"Message not found\"}", "application/json");
        }
    });

    svr_.Post("/api/fs/mkdir", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);
        
        try {
            auto body = json::parse(req.body);
            std::string parent_path = body.value("path", "");
            std::string name = body.value("name", "");
            
            if (name.empty()) {
                res.status = 400;
                res.set_content("{\"success\":false,\"error\":\"El nombre del directorio no puede estar vacío\"}", "application/json");
                return;
            }

            namespace fs = std::filesystem;
            fs::path target_dir;
            if (parent_path.empty()) {
                target_dir = fs::current_path() / "uploads" / name;
            } else {
                target_dir = fs::path(parent_path) / name;
            }

            std::error_code ec;
            if (fs::exists(target_dir, ec)) {
                res.status = 400;
                res.set_content("{\"success\":false,\"error\":\"El directorio ya existe\"}", "application/json");
                return;
            }

            if (fs::create_directories(target_dir, ec)) {
                json r;
                r["success"] = true;
                r["path"] = target_dir.string();
                res.set_content(r.dump(), "application/json");
                UserLogger::GetInstance().LogAction(user, "Creó la carpeta '" + name + "' en '" + parent_path + "'");
            } else {
                res.status = 500;
                res.set_content("{\"success\":false,\"error\":\"No se pudo crear el directorio\"}", "application/json");
            }
        } catch (...) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"JSON inválido\"}", "application/json");
        }
    });

    svr_.Get("/api/fs/list", [](const httplib::Request& req, httplib::Response& res) {
        std::string path_param = req.get_param_value("path");
        namespace fs = std::filesystem;
        
        fs::path target_path;
        if (path_param.empty()) {
            target_path = fs::current_path();
        } else {
            target_path = fs::path(path_param);
        }

        std::error_code ec;
        if (!fs::exists(target_path, ec) || !fs::is_directory(target_path, ec)) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"La ruta especificada no existe o no es una carpeta.\"}", "application/json");
            return;
        }

        try {
            std::string canonical_path = fs::weakly_canonical(target_path, ec).string();
            json arr = json::array();
            
            if (target_path.has_parent_path() && target_path != target_path.root_path()) {
                json item;
                item["name"] = "..";
                item["path"] = fs::weakly_canonical(target_path.parent_path(), ec).string();
                item["is_directory"] = true;
                item["size"] = 0;
                arr.push_back(item);
            }

            for (const auto& entry : fs::directory_iterator(target_path, fs::directory_options::skip_permission_denied, ec)) {
                json item;
                item["name"] = entry.path().filename().string();
                item["path"] = entry.path().string();
                
                std::error_code entry_ec;
                bool is_dir = entry.is_directory(entry_ec);
                item["is_directory"] = is_dir;
                if (is_dir) {
                    item["size"] = 0;
                } else {
                    uint64_t size = entry.file_size(entry_ec);
                    item["size"] = entry_ec ? 0 : size;
                }
                arr.push_back(item);
            }

            std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
                bool a_dir = a["is_directory"];
                bool b_dir = b["is_directory"];
                if (a_dir != b_dir) return a_dir > b_dir;
                std::string a_name = a["name"];
                std::string b_name = b["name"];
                if (a_name == "..") return true;
                if (b_name == "..") return false;
                return a_name < b_name;
            });

            json result;
            result["success"] = true;
            result["current_path"] = canonical_path;
            result["items"] = arr;
            res.set_content(result.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"success\":false,\"error\":\"") + e.what() + "\"}", "application/json");
        }
    });

    svr_.Post("/api/fs/upload", [](const httplib::Request& req, httplib::Response& res) {
        if (!CheckWritePermission(req, res)) return;
        std::string user = GetSessionUser(req);

        if (!req.form.has_file("file")) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"No se recibió ningún archivo.\"}", "application/json");
            return;
        }

        const auto& file = req.form.get_file("file");
        
        namespace fs = std::filesystem;
        std::string path_param = req.get_param_value("path");
        fs::path target_dir;
        if (path_param.empty()) {
            target_dir = fs::current_path() / "uploads";
        } else {
            target_dir = fs::path(path_param);
        }

        // Security restriction: Only allow uploads to directories defined as a video pack input source url
        std::string role = UserManager::GetInstance().GetRole(user);
        bool is_valid_video_pack_dir = false;
        json inputs_list = StreamerEngine::GetInstance().GetInputsJSON();
        for (const auto& item : inputs_list) {
            std::string input_url = item.value("url", "");
            bool is_video_pack = item.value("is_video_pack", false);
            std::string input_id = item.value("id", "");
            
            if (is_video_pack) {
                if (role != "Programadores" || IsInputAllowed(user, input_id)) {
                    std::error_code equiv_ec;
                    if (fs::equivalent(fs::path(input_url), target_dir, equiv_ec)) {
                        is_valid_video_pack_dir = true;
                        break;
                    }
                    if (equiv_ec && input_url == target_dir.string()) {
                        is_valid_video_pack_dir = true;
                        break;
                    }
                }
            }
        }

        if (!is_valid_video_pack_dir) {
            res.status = 403;
            res.set_content("{\"success\":false,\"error\":\"Acceso denegado: Solo se permite subir archivos a las carpetas de video packs autorizadas por el administrador.\"}", "application/json");
            return;
        }

        std::error_code ec;
        if (!fs::exists(target_dir, ec)) {
            fs::create_directories(target_dir, ec);
        }

        if (!fs::is_directory(target_dir, ec)) {
            res.status = 400;
            res.set_content("{\"success\":false,\"error\":\"La ruta de destino no es un directorio válido.\"}", "application/json");
            return;
        }

        fs::path target_file = target_dir / file.filename;
        
        std::ofstream ofs(target_file, std::ios::binary);
        if (!ofs) {
            res.status = 500;
            res.set_content("{\"success\":false,\"error\":\"No se pudo abrir el archivo de destino para escribir.\"}", "application/json");
            return;
        }

        ofs.write(file.content.data(), file.content.size());
        ofs.close();

        UserLogger::GetInstance().LogAction(user, "Subió el archivo '" + file.filename + "' a '" + target_dir.string() + "'");

        json r;
        r["success"] = true;
        r["path"] = target_file.string();
        res.set_content(r.dump(), "application/json");
    });

    svr_.listen("0.0.0.0", port_);
}
