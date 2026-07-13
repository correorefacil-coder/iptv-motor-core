#pragma once

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include "third_party/json.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

using json = nlohmann::json;

struct ProgramInfo {
    int program_number;
    std::string name;
    std::vector<int> pids;
    double bitrate_kbps = 0.0;
    uint64_t bytes_accumulator = 0;
};

struct PreparedStream {
    int input_stream_idx;
    AVCodecParameters* codecpar = nullptr;
    AVRational time_base;

    PreparedStream() = default;
    PreparedStream(int idx, const AVCodecParameters* cp, AVRational tb)
        : input_stream_idx(idx), time_base(tb) {
        if (cp) {
            codecpar = avcodec_parameters_alloc();
            avcodec_parameters_copy(codecpar, cp);
        }
    }
    
    PreparedStream(const PreparedStream& other) {
        input_stream_idx = other.input_stream_idx;
        time_base = other.time_base;
        if (other.codecpar) {
            codecpar = avcodec_parameters_alloc();
            avcodec_parameters_copy(codecpar, other.codecpar);
        } else {
            codecpar = nullptr;
        }
    }

    PreparedStream& operator=(const PreparedStream& other) {
        if (this != &other) {
            if (codecpar) {
                avcodec_parameters_free(&codecpar);
            }
            input_stream_idx = other.input_stream_idx;
            time_base = other.time_base;
            if (other.codecpar) {
                codecpar = avcodec_parameters_alloc();
                avcodec_parameters_copy(codecpar, other.codecpar);
            } else {
                codecpar = nullptr;
            }
        }
        return *this;
    }

    PreparedStream(PreparedStream&& other) noexcept {
        input_stream_idx = other.input_stream_idx;
        time_base = other.time_base;
        codecpar = other.codecpar;
        other.codecpar = nullptr;
    }

    PreparedStream& operator=(PreparedStream&& other) noexcept {
        if (this != &other) {
            if (codecpar) {
                avcodec_parameters_free(&codecpar);
            }
            input_stream_idx = other.input_stream_idx;
            time_base = other.time_base;
            codecpar = other.codecpar;
            other.codecpar = nullptr;
        }
        return *this;
    }

    ~PreparedStream() {
        if (codecpar) {
            avcodec_parameters_free(&codecpar);
        }
    }
};

struct InputStats {
    std::string id;
    std::string name;
    std::string url;
    bool connected = false;
    double total_bitrate_kbps = 0.0;
    std::vector<ProgramInfo> programs;
};

struct OutputStats {
    std::string id;
    std::string name;
    std::string input_id;
    int program_number;
    std::string output_url;
    bool active = false;
    double bitrate_kbps = 0.0;
    uint64_t packets_sent = 0;
    uint64_t packets_dropped = 0;
    std::string error_message = "";
    std::string output_interface;
    std::string video_filename;
};

struct ScheduledMessage {
    std::string id;
    std::string text;
    std::string start_time;
    std::string end_time;
    bool all_channels = false;
    std::vector<std::string> channel_ids;
};

struct HLSSession {
    std::string ip;
    std::string stream_id;
    std::string user_agent;
    std::chrono::system_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_request_time;
};

struct OutputDestination {
    std::string url;
    std::string output_interface;
    std::string type; // "udp", "srt", "rtp", "hls"
};

// Represents a single channel output stream (SPTS) demuxed from an input source (MPTS)
class OutputStream {
public:
    OutputStream(const std::string& id, const std::string& name, const std::string& input_id,
                 int program_number, const std::vector<OutputDestination>& outputs, bool enabled,
                 const std::string& input_url, bool is_video_pack,
                 bool transcode_enabled = false, bool transcode_video = false,
                 const std::string& video_input_format = "", const std::string& video_output_format = "",
                 bool transcode_audio = false, const std::string& audio_input_format = "",
                 const std::string& audio_output_format = "", int limit_bitrate = 0,
                 const std::string& video_filename = "", const std::string& transcode_preset = "");
    ~OutputStream();

    bool IsTranscodeEnabled() const { return transcode_enabled_; }
    bool IsTranscodeVideo() const { return transcode_video_; }
    std::string GetVideoInputFormat() const { return video_input_format_; }
    std::string GetVideoOutputFormat() const { return video_output_format_; }
    bool IsTranscodeAudio() const { return transcode_audio_; }
    std::string GetAudioInputFormat() const { return audio_input_format_; }
    std::string GetAudioOutputFormat() const { return audio_output_format_; }
    int GetLimitBitrate() const { return limit_bitrate_; }
    std::string GetTranscodePreset() const { return transcode_preset_; }
    std::string GetDetectedVideoCodec() const { return detected_video_codec_; }
    std::string GetDetectedAudioCodec() const { return detected_audio_codec_; }

    void Start();
    void Stop();
    void PushPacket(AVPacket* pkt, int input_stream_idx, AVRational time_base);
    void PrepareStreams(const std::vector<PreparedStream>& streams);

    OutputStats GetStats();
    std::string GetId() const { return id_; }
    std::string GetName() const { return name_; }
    std::string GetInputId() const { return input_id_; }
    int GetProgramNumber() const { return program_number_; }
    std::string GetOutputUrl() const { return output_url_; }
    bool IsEnabled() const { return enabled_; }
    void SetEnabled(bool enabled);
    bool IsRunning() const { return running_; }
    std::string GetOutputInterface() const { return output_interface_; }
    std::string GetVideoFilename() const { return video_filename_; }
    const std::vector<OutputDestination>& GetOutputs() const { return outputs_; }

private:
    void OutputLoop();
    void OutputLoopVideoPack();
    void ClearQueue();
    void CalculateBitrates();

    std::string id_;
    std::string name_;
    std::string input_id_;
    int program_number_;
    std::string output_url_;
    std::string output_interface_;
    std::vector<OutputDestination> outputs_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};

    std::thread thread_;
    std::thread bitrate_thread_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    
    struct QueuedPacket {
        AVPacket* pkt;
        int input_stream_idx;
        AVRational time_base;
    };
    std::queue<QueuedPacket> packet_queue_;
    const size_t max_queue_size_ = 500; // Prevent out-of-memory if network is slow

    // Stats
    std::atomic<uint64_t> packets_sent_{0};
    std::atomic<uint64_t> packets_dropped_{0};
    std::atomic<uint64_t> bytes_accumulator_{0};
    std::atomic<double> bitrate_kbps_{0.0};
    std::string error_message_ = "";
    std::mutex stats_mutex_;

    std::vector<PreparedStream> prepared_streams_;
    bool needs_reinit_{false};

    std::chrono::steady_clock::time_point last_bitrate_calc_;

    bool transcode_enabled_ = false;
    bool transcode_video_ = false;
    std::string video_input_format_;
    std::string video_output_format_;
    bool transcode_audio_ = false;
    std::string audio_input_format_;
    std::string audio_output_format_;
    int limit_bitrate_ = 0;
    std::string detected_video_codec_ = "Auto";
    std::string detected_audio_codec_ = "Auto";
    FILE* ffmpeg_pipe_ = nullptr;
    std::string initialized_message_ = "";
    std::string video_filename_;
    std::string input_url_;
    bool is_video_pack_ = false;
    std::string transcode_preset_;
};

// Represents an input connection (Pack) which may contain multiple programs
class InputSource {
public:
    InputSource(const std::string& id, const std::string& name, const std::string& url, bool enabled, bool is_video_pack = false);
    ~InputSource();

    void Start();
    void Stop();

    void RegisterOutputStream(OutputStream* out_stream);
    void UnregisterOutputStream(OutputStream* out_stream);

    InputStats GetStats();
    std::string GetId() const { return id_; }
    std::string GetName() const { return name_; }
    std::string GetUrl() const { return url_; }
    bool IsEnabled() const { return enabled_; }
    bool IsVideoPack() const { return is_video_pack_; }
    void SetEnabled(bool enabled);
    bool IsRunning() const { return running_; }

    // Static helper to probe a URL for programs list
    static std::vector<ProgramInfo> ProbeURL(const std::string& url, std::string& error_out);

private:
    void InputLoop();
    void AnalyzePrograms(AVFormatContext* fmt_ctx);
    void CalculateBitrates();

    struct SourceStreamInfo {
        int program_number;
        int input_stream_idx;
        AVCodecParameters* codecpar = nullptr;
        AVRational time_base;
    };
    std::vector<SourceStreamInfo> source_streams_;
    void ClearSourceStreams();
    void PrepareAllListeners();

    std::string id_;
    std::string name_;
    std::string url_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    bool is_video_pack_ = false;

    std::thread thread_;
    std::thread bitrate_thread_;
    std::mutex listeners_mutex_;
    std::vector<OutputStream*> listeners_;

    std::mutex stats_mutex_;
    std::vector<ProgramInfo> programs_;
    std::atomic<double> total_bitrate_kbps_{0.0};
    std::atomic<uint64_t> total_bytes_accumulator_{0};
    
    std::chrono::steady_clock::time_point last_bitrate_calc_;
    std::condition_variable sleep_cv_;
    std::mutex sleep_mutex_;
};

// Orchestrator for all input and output streams
class StreamerEngine {
public:
    static StreamerEngine& GetInstance();

    void Init(const std::string& config_path);
    void Shutdown();

    // Configuration Management
    bool LoadConfig();
    bool SaveConfig();

    // Inputs Management
    bool AddInput(const std::string& name, const std::string& url, bool enabled, bool is_video_pack, std::string& id_out);
    bool UpdateInput(const std::string& id, const std::string& name, const std::string& url, bool enabled, bool is_video_pack);
    bool DeleteInput(const std::string& id);

    // Streams (Outputs) Management
    bool AddStream(const std::string& name, const std::string& input_id, int program_number,
                   const std::vector<OutputDestination>& outputs, bool enabled,
                   bool transcode_enabled, bool transcode_video, const std::string& video_input_format,
                   const std::string& video_output_format, bool transcode_audio, const std::string& audio_input_format,
                   const std::string& audio_output_format, int limit_bitrate, const std::string& video_filename, std::string& id_out,
                   const std::string& transcode_preset = "");
    bool UpdateStream(const std::string& id, const std::string& name, const std::string& input_id,
                      int program_number, const std::vector<OutputDestination>& outputs, bool enabled,
                      bool transcode_enabled, bool transcode_video, const std::string& video_input_format,
                      const std::string& video_output_format, bool transcode_audio, const std::string& audio_input_format,
                      const std::string& audio_output_format, int limit_bitrate, const std::string& video_filename,
                      const std::string& transcode_preset = "");
    bool DeleteStream(const std::string& id);

    // API Helpers
    json GetStatusJSON();
    json GetInputsJSON();
    json GetStreamsJSON();
    json ProbeInputURL(const std::string& url);

    bool IsInputVideoPack(const std::string& input_id);
    std::string GetInputUrl(const std::string& input_id);

    // Global Settings
    std::string GetOutputInterface();
    void SetOutputInterface(const std::string& iface);
    std::string GetNVENCPreset();
    void SetNVENCPreset(const std::string& preset);
    std::string GetCPUPreset();
    void SetCPUPreset(const std::string& preset);

    // Messages Settings
    std::vector<ScheduledMessage> GetMessages();
    bool AddMessage(const ScheduledMessage& msg);
    bool UpdateMessage(const std::string& id, const ScheduledMessage& msg);
    bool DeleteMessage(const std::string& id);
    std::string GetActiveMessageForStream(const std::string& stream_id);

    // HLS Sessions & IP Blocking
    void RecordHLSAccess(const std::string& ip, const std::string& stream_id, const std::string& user_agent);
    void PruneExpiredSessions();
    json GetActiveSessionsJSON();
    bool IsIPBlocked(const std::string& ip);
    void BlockIP(const std::string& ip);
    void UnblockIP(const std::string& ip);
    json GetBlockedIPsJSON();

private:
    StreamerEngine() = default;
    ~StreamerEngine() = default;
    StreamerEngine(const StreamerEngine&) = delete;
    StreamerEngine& operator=(const StreamerEngine&) = delete;

    std::string config_path_ = "config.json";
    std::mutex engine_mutex_;
    std::mutex messages_mutex_;
    std::mutex sessions_mutex_;
    std::mutex blocked_ips_mutex_;

    std::string output_interface_;
    std::string nvenc_preset_ = "p4";
    std::string cpu_preset_ = "ultrafast";

    std::map<std::string, std::unique_ptr<InputSource>> inputs_;
    std::map<std::string, std::unique_ptr<OutputStream>> streams_;
    std::vector<ScheduledMessage> messages_;
    std::vector<HLSSession> hls_sessions_;
    std::vector<std::string> blocked_ips_;
};
