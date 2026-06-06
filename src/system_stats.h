#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

class SystemStats {
public:
    SystemStats() {
        ReadCPUTicks(prev_user_, prev_nice_, prev_system_, prev_idle_, prev_iowait_, prev_irq_, prev_softirq_, prev_steal_);
    }

    double GetCPUUsage() {
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
        if (!ReadCPUTicks(user, nice, system, idle, iowait, irq, softirq, steal)) {
            return 0.0;
        }

        unsigned long long prev_idle_all = prev_idle_ + prev_iowait_;
        unsigned long long idle_all = idle + iowait;

        unsigned long long prev_non_idle = prev_user_ + prev_nice_ + prev_system_ + prev_irq_ + prev_softirq_ + prev_steal_;
        unsigned long long non_idle = user + nice + system + irq + softirq + steal;

        unsigned long long prev_total = prev_idle_all + prev_non_idle;
        unsigned long long total = idle_all + non_idle;

        unsigned long long total_diff = total - prev_total;
        unsigned long long idle_diff = idle_all - prev_idle_all;

        double usage = 0.0;
        if (total_diff > 0) {
            usage = 100.0 * (total_diff - idle_diff) / total_diff;
        }

        prev_user_ = user;
        prev_nice_ = nice;
        prev_system_ = system;
        prev_idle_ = idle;
        prev_iowait_ = iowait;
        prev_irq_ = irq;
        prev_softirq_ = softirq;
        prev_steal_ = steal;

        return usage;
    }

    double GetMemoryUsage() {
        std::ifstream file("/proc/meminfo");
        if (!file.is_open()) return 0.0;

        std::string line;
        unsigned long long total_mem = 0;
        unsigned long long free_mem = 0;
        unsigned long long buffers = 0;
        unsigned long long cached = 0;
        unsigned long long reclaimable = 0;

        while (std::getline(file, line)) {
            std::stringstream ss(line);
            std::string key;
            unsigned long long val;
            ss >> key >> val;
            if (key == "MemTotal:") {
                total_mem = val;
            } else if (key == "MemFree:") {
                free_mem = val;
            } else if (key == "Buffers:") {
                buffers = val;
            } else if (key == "Cached:") {
                cached = val;
            } else if (key == "SReclaimable:") {
                reclaimable = val;
            }
        }

        if (total_mem == 0) return 0.0;

        // Actual used memory is total - free - buffers - cached - SReclaimable
        unsigned long long used_mem = total_mem - free_mem - buffers - cached - reclaimable;
        return 100.0 * used_mem / total_mem;
    }

    struct GPUStats {
        double gpu_usage = 0.0;
        double gpu_mem_usage = 0.0;
        double gpu_mem_used = 0.0;
        double gpu_mem_total = 0.0;
        bool available = false;
    };

    GPUStats GetGPUStats() {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_gpu_query_).count() < 2) {
            return cached_gpu_stats_;
        }
        last_gpu_query_ = now;

        GPUStats stats;
        FILE* fp = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
        if (fp) {
            char buffer[128];
            if (fgets(buffer, sizeof(buffer), fp) != nullptr) {
                std::string line(buffer);
                std::stringstream ss(line);
                std::string val_gpu, val_mem_used, val_mem_total;
                if (std::getline(ss, val_gpu, ',') && 
                    std::getline(ss, val_mem_used, ',') && 
                    std::getline(ss, val_mem_total, ',')) {
                    try {
                        stats.gpu_usage = std::stod(val_gpu);
                        stats.gpu_mem_used = std::stod(val_mem_used);
                        stats.gpu_mem_total = std::stod(val_mem_total);
                        if (stats.gpu_mem_total > 0) {
                            stats.gpu_mem_usage = (stats.gpu_mem_used / stats.gpu_mem_total) * 100.0;
                        }
                        stats.available = true;
                    } catch (...) {
                        stats.available = false;
                    }
                }
            }
            pclose(fp);
        }
        cached_gpu_stats_ = stats;
        return stats;
    }

private:
    bool ReadCPUTicks(unsigned long long& user, unsigned long long& nice, unsigned long long& system,
                      unsigned long long& idle, unsigned long long& iowait, unsigned long long& irq,
                      unsigned long long& softirq, unsigned long long& steal) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return false;

        std::string line;
        std::getline(file, line);
        std::stringstream ss(line);
        std::string cpu;
        ss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        return cpu == "cpu";
    }

    unsigned long long prev_user_ = 0;
    unsigned long long prev_nice_ = 0;
    unsigned long long prev_system_ = 0;
    unsigned long long prev_idle_ = 0;
    unsigned long long prev_iowait_ = 0;
    unsigned long long prev_irq_ = 0;
    unsigned long long prev_softirq_ = 0;
    unsigned long long prev_steal_ = 0;

    std::chrono::steady_clock::time_point last_gpu_query_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    GPUStats cached_gpu_stats_;
};
