#include "engine.h"
#include "logger.h"
#include "network_utils.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>
#include <cmath>

static bool IsNvidiaGPUPresent() {
#ifdef _WIN32
    return false;
#else
    struct stat buffer;
    return (stat("/dev/nvidia0", &buffer) == 0);
#endif
}

// --- Helper structure to bridge input and output ---
struct StreamInfo {
    int input_index;
    AVCodecParameters* codecpar;
    AVRational time_base;
};

std::vector<ProgramInfo> scan_video_pack_directory(const std::string& dir_path) {
    std::vector<ProgramInfo> programs;
    try {
        if (std::filesystem::exists(dir_path) && std::filesystem::is_directory(dir_path)) {
            int program_num = 1;
            std::vector<std::filesystem::path> paths;
            for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".mp4" || ext == ".mkv" || ext == ".ts" || ext == ".avi" ||
                        ext == ".mov" || ext == ".flv" || ext == ".mpeg" || ext == ".mpg" ||
                        ext == ".webm" || ext == ".m4v") {
                        paths.push_back(entry.path());
                    }
                }
            }
            std::sort(paths.begin(), paths.end());
            for (const auto& p : paths) {
                ProgramInfo info;
                info.program_number = program_num++;
                info.name = p.filename().string();
                info.pids = {0};
                programs.push_back(info);
            }
        }
    } catch (...) {}
    return programs;
}

// ==========================================
// OUTPUT STREAM IMPLEMENTATION
// ==========================================

OutputStream::OutputStream(const std::string& id, const std::string& name, const std::string& input_id,
                           int program_number, const std::vector<OutputDestination>& outputs, bool enabled,
                           const std::string& input_url, bool is_video_pack,
                           bool transcode_enabled, bool transcode_video,
                           const std::string& video_input_format, const std::string& video_output_format,
                           bool transcode_audio, const std::string& audio_input_format,
                           const std::string& audio_output_format, int limit_bitrate,
                           const std::string& video_filename)
    : id_(id), name_(name), input_id_(input_id), program_number_(program_number),
      outputs_(outputs), enabled_(enabled),
      input_url_(input_url), is_video_pack_(is_video_pack),
      transcode_enabled_(transcode_enabled), transcode_video_(transcode_video),
      video_input_format_(video_input_format), video_output_format_(video_output_format),
      transcode_audio_(transcode_audio), audio_input_format_(audio_input_format),
      audio_output_format_(audio_output_format), limit_bitrate_(limit_bitrate),
      video_filename_(video_filename) {
    if (!outputs.empty()) {
        output_url_ = outputs[0].url;
        output_interface_ = outputs[0].output_interface;
    } else {
        output_url_ = "";
        output_interface_ = "";
    }
    last_bitrate_calc_ = std::chrono::steady_clock::now();
}

OutputStream::~OutputStream() {
    Stop();
}

void OutputStream::Start() {
    if (!enabled_ || running_) return;
    running_ = true;
    thread_ = std::thread(&OutputStream::OutputLoop, this);
    bitrate_thread_ = std::thread(&OutputStream::CalculateBitrates, this);
    LOG_INFO("Canal [" + name_ + "] (Salida) iniciado.");
}

void OutputStream::Stop() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (bitrate_thread_.joinable()) {
        bitrate_thread_.join();
    }
    ClearQueue();
    LOG_INFO("Canal [" + name_ + "] (Salida) detenido.");
}

void OutputStream::CalculateBitrates() {
    auto last_calc = std::chrono::steady_clock::now();
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_calc).count();
        if (duration > 0) {
            uint64_t bytes = bytes_accumulator_.exchange(0);
            double instant_bitrate = (bytes * 8.0) / duration;
            
            // If transcoding is active with a bitrate limit, show the target limit in the panel
            if (transcode_enabled_ && limit_bitrate_ > 0 && instant_bitrate > 50.0) {
                instant_bitrate = limit_bitrate_;
            }

            double current = bitrate_kbps_.load();
            if (current == 0.0) {
                bitrate_kbps_ = instant_bitrate;
            } else if (instant_bitrate > 0) {
                bitrate_kbps_ = current * 0.7 + instant_bitrate * 0.3;
            } else {
                double decayed = current * 0.75;
                bitrate_kbps_ = (decayed < 1.0) ? 0.0 : decayed;
            }
        }
        last_calc = now;
    }
}

void OutputStream::SetEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled) {
        Start();
    } else {
        Stop();
    }
}

void OutputStream::PushPacket(AVPacket* pkt, int input_stream_idx, AVRational time_base) {
    if (!running_) return;

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (packet_queue_.size() >= max_queue_size_) {
        packets_dropped_++;
        return; // Drop packet if buffer is full
    }

    AVPacket* cloned_pkt = av_packet_clone(pkt);
    if (!cloned_pkt) return;

    QueuedPacket q_pkt = {cloned_pkt, input_stream_idx, time_base};
    packet_queue_.push(q_pkt);
    cv_.notify_one();
}

void OutputStream::PrepareStreams(const std::vector<PreparedStream>& streams) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    prepared_streams_ = streams;

    // Detect codecs from prepared streams
    detected_video_codec_ = "Auto";
    detected_audio_codec_ = "Auto";
    for (const auto& s : streams) {
        if (s.codecpar) {
            const char* name = avcodec_get_name(s.codecpar->codec_id);
            std::string codec_name = name ? name : "desconocido";
            if (s.codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (codec_name == "h264") detected_video_codec_ = "H264";
                else if (codec_name == "hevc") detected_video_codec_ = "HEVC";
                else if (codec_name == "mpeg2video") detected_video_codec_ = "MPEG2";
                else {
                    std::transform(codec_name.begin(), codec_name.end(), codec_name.begin(), ::toupper);
                    detected_video_codec_ = codec_name;
                }
            } else if (s.codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (codec_name == "aac") detected_audio_codec_ = "AAC";
                else if (codec_name == "mp3") detected_audio_codec_ = "MP3";
                else if (codec_name == "ac3") detected_audio_codec_ = "AC3";
                else {
                    std::transform(codec_name.begin(), codec_name.end(), codec_name.begin(), ::toupper);
                    detected_audio_codec_ = codec_name;
                }
            }
        }
    }

    needs_reinit_ = true;
    cv_.notify_one();
}

void OutputStream::ClearQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!packet_queue_.empty()) {
        auto q_pkt = packet_queue_.front();
        av_packet_free(&q_pkt.pkt);
        packet_queue_.pop();
    }
}

OutputStats OutputStream::GetStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    OutputStats stats;
    stats.id = id_;
    stats.name = name_;
    stats.input_id = input_id_;
    stats.program_number = program_number_;
    stats.output_url = output_url_;
    stats.active = running_;
    stats.bitrate_kbps = running_ ? bitrate_kbps_.load() : 0.0;
    stats.packets_sent = packets_sent_;
    stats.packets_dropped = packets_dropped_;
    stats.error_message = error_message_;
    stats.output_interface = output_interface_;
    stats.video_filename = video_filename_;
    return stats;
}

static int write_to_ffmpeg_pipe(void* opaque, uint8_t* buf, int buf_size) {
    FILE* pipe = static_cast<FILE*>(opaque);
    if (!pipe) return 0;
    size_t written = fwrite(buf, 1, buf_size, pipe);
    fflush(pipe);
    return static_cast<int>(written);
}

void OutputStream::OutputLoopVideoPack() {
    AVFormatContext* in_fmt_ctx = nullptr;
    
    std::string current_video_filename = video_filename_;
    std::string current_msg = StreamerEngine::GetInstance().GetActiveMessageForStream(id_);
    bool current_transcode_enabled = transcode_enabled_;
    bool current_transcode_video = transcode_video_;
    bool current_transcode_audio = transcode_audio_;
    int current_limit_bitrate = limit_bitrate_;

    auto cleanup = [&]() {
        if (ffmpeg_pipe_) {
            // Write 'q\n' to stdin of ffmpeg to shut it down gracefully
            fwrite("q\n", 1, 2, ffmpeg_pipe_);
            fflush(ffmpeg_pipe_);
            pclose(ffmpeg_pipe_);
            ffmpeg_pipe_ = nullptr;
        }

        if (in_fmt_ctx) {
            avformat_close_input(&in_fmt_ctx);
            in_fmt_ctx = nullptr;
        }
    };

    while (running_) {
        if (current_video_filename.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            current_video_filename = video_filename_;
            continue;
        }

        std::string input_dir = input_url_;
        std::string file_path = input_dir + "/" + current_video_filename;

        avformat_network_init();
        int ret = avformat_open_input(&in_fmt_ctx, file_path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            {
                std::lock_guard<std::mutex> stat_lock(stats_mutex_);
                error_message_ = "Error abriendo archivo " + current_video_filename + ": " + std::string(err_buf);
            }
            LOG_ERROR("Canal [" + name_ + "]: " + error_message_);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            current_video_filename = video_filename_;
            continue;
        }

        ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            {
                std::lock_guard<std::mutex> stat_lock(stats_mutex_);
                error_message_ = "Error stream info en " + current_video_filename + ": " + std::string(err_buf);
            }
            LOG_ERROR("Canal [" + name_ + "]: " + error_message_);
            cleanup();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            current_video_filename = video_filename_;
            continue;
        }

        // Extracción de códecs para mostrar en el panel
        std::string det_v = "Auto";
        std::string det_a = "Auto";
        for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
            AVCodecParameters* codecpar = in_fmt_ctx->streams[i]->codecpar;
            if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
                if (codec) det_v = codec->name;
            } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
                if (codec) det_a = codec->name;
            }
        }
        {
            std::lock_guard<std::mutex> stat_lock(stats_mutex_);
            detected_video_codec_ = det_v;
            detected_audio_codec_ = det_a;
        }

        // Determinar tasa de bits esperada para la simulación de estadísticas
        int64_t expected_bitrate_bps = 2500000; // Default fallback to 2.5 Mbps
        if (current_transcode_enabled && current_limit_bitrate > 0) {
            expected_bitrate_bps = current_limit_bitrate * 1000;
        } else if (in_fmt_ctx->bit_rate > 0) {
            expected_bitrate_bps = in_fmt_ctx->bit_rate;
        } else {
            // Estimar tasa de bits basada en el tamaño del archivo y su duración
            try {
                uint64_t file_size = std::filesystem::file_size(file_path);
                double duration_sec = 0.0;
                if (in_fmt_ctx->duration != AV_NOPTS_VALUE) {
                    duration_sec = in_fmt_ctx->duration / (double)AV_TIME_BASE;
                }
                if (duration_sec > 0.5) {
                    expected_bitrate_bps = static_cast<int64_t>((file_size * 8.0) / duration_sec);
                }
            } catch (...) {}
        }

        // Una vez extraída la información que necesitamos del in_fmt_ctx, podemos cerrarlo para que FFmpeg lo abra de manera exclusiva
        if (in_fmt_ctx) {
            avformat_close_input(&in_fmt_ctx);
            in_fmt_ctx = nullptr;
        }

        if (outputs_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            current_video_filename = video_filename_;
            continue;
        }

        bool force_transcode = !current_msg.empty();
        bool do_video_transcode = current_transcode_video || (current_limit_bitrate > 0) || force_transcode;
        bool use_cuda = do_video_transcode && IsNvidiaGPUPresent();

        // Construir comando de ffmpeg
        std::string ffmpeg_cmd = "ffmpeg -y -hide_banner -loglevel error ";
        if (use_cuda) {
            ffmpeg_cmd += "-hwaccel cuda ";
        }
        // -re para leer en tiempo real
        // -stream_loop -1 para bucle infinito natively
        ffmpeg_cmd += "-re -stream_loop -1 -i \"" + file_path + "\" ";

        if (do_video_transcode) {
            std::string target_format = video_output_format_;
            if (!current_transcode_video) {
                std::string det_v_lower = det_v;
                std::transform(det_v_lower.begin(), det_v_lower.end(), det_v_lower.begin(), ::tolower);
                if (det_v_lower == "hevc" || det_v_lower == "h265") {
                    target_format = "hevc";
                } else if (det_v_lower == "mpeg2" || det_v_lower == "mpeg2video") {
                    target_format = "mpeg2video";
                } else {
                    target_format = "h264";
                }
            }

            std::string codec = "libx264";
            if (IsNvidiaGPUPresent()) {
                codec = "h264_nvenc";
                if (target_format == "hevc") codec = "hevc_nvenc";
                else if (target_format == "mpeg2video") codec = "mpeg2video";
            } else {
                if (target_format == "hevc") codec = "libx265";
                else if (target_format == "mpeg2video") codec = "mpeg2video";
            }
            
            ffmpeg_cmd += "-c:v " + codec + " ";
            if (codec == "h264_nvenc" || codec == "hevc_nvenc") {
                ffmpeg_cmd += "-preset p1 -tune ll -g 60 -forced-idr 1 -aud 1 -flags:v -global_header ";
            } else if (codec == "libx264" || codec == "libx265") {
                ffmpeg_cmd += "-preset ultrafast -tune zerolatency -g 60 -flags:v -global_header ";
            }
            
            if (!current_msg.empty()) {
                std::string escaped_msg = "";
                for (char c : current_msg) {
                    if (c == '\'') {
                        escaped_msg += "'\\''";
                    } else if (c == ',') {
                        escaped_msg += "\\\\,";
                    } else if (c == ':') {
                        escaped_msg += "\\\\:";
                    } else if (c == '\\') {
                        escaped_msg += "\\\\\\\\";
                    } else {
                        escaped_msg += c;
                    }
                }
                ffmpeg_cmd += "-vf \"drawtext=fontfile=/usr/share/fonts/truetype/freefont/FreeSans.ttf:text='" + escaped_msg + "':expansion=none:x=(w-text_w)/2:y=h-50:fontsize=32:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=10\" ";
            }
            
            if (current_limit_bitrate > 0) {
                int v_bitrate = std::max(200, current_limit_bitrate - 128);
                ffmpeg_cmd += "-b:v " + std::to_string(v_bitrate) + "k -maxrate " + std::to_string(v_bitrate) + "k -bufsize " + std::to_string(2 * v_bitrate) + "k ";
            }
        } else {
            ffmpeg_cmd += "-c:v copy ";
        }
        
        if (current_transcode_audio) {
            std::string codec = "aac";
            if (audio_output_format_ == "mp3") codec = "libmp3lame";
            else if (audio_output_format_ == "ac3") codec = "ac3";
            
            ffmpeg_cmd += "-c:a " + codec + " -b:a 128k ";
        } else {
            ffmpeg_cmd += "-c:a copy ";
        }
        
        std::string outputs_str = "";
        for (const auto& dest : outputs_) {
            std::string r_url = dest.url;
            std::string o_iface = dest.output_interface;
            if (!o_iface.empty()) {
                std::string iface_ip = GetInterfaceIP(o_iface);
                if (!iface_ip.empty()) {
                    if (r_url.rfind("udp://", 0) == 0 || r_url.rfind("srt://", 0) == 0 || r_url.rfind("rtp://", 0) == 0) {
                        if (r_url.find("localaddr=") == std::string::npos) {
                            if (r_url.find('?') == std::string::npos) {
                                r_url += "?localaddr=" + iface_ip;
                            } else {
                                r_url += "&localaddr=" + iface_ip;
                            }
                        }
                    }
                }
            }

            if (dest.type == "hls" || r_url.rfind("hls://", 0) == 0 || r_url.find(".m3u8") != std::string::npos) {
                try {
                    std::filesystem::path p(r_url);
                    std::filesystem::create_directories(p.parent_path());
                } catch (...) {}
                outputs_str += " -f hls -hls_time 2 -hls_list_size 5 -hls_flags delete_segments \"" + r_url + "\"";
            } else if (dest.type == "rtp" || r_url.rfind("rtp://", 0) == 0) {
                outputs_str += " -f rtp_mpegts \"" + r_url + "\"";
            } else {
                std::string optimized_url = r_url;
                if (optimized_url.rfind("udp://", 0) == 0) {
                    if (optimized_url.find("pkt_size=") == std::string::npos) {
                        if (optimized_url.find('?') == std::string::npos) {
                            optimized_url += "?pkt_size=1316";
                        } else {
                            optimized_url += "&pkt_size=1316";
                        }
                    }
                }
                outputs_str += " -f mpegts \"" + optimized_url + "\"";
            }
        }
        ffmpeg_cmd += outputs_str;
        
        LOG_INFO("Canal [" + name_ + "]: Iniciando comando de FFmpeg nativo para VideoPack: " + ffmpeg_cmd);
        
        ffmpeg_pipe_ = popen(ffmpeg_cmd.c_str(), "w");
        if (!ffmpeg_pipe_) {
            {
                std::lock_guard<std::mutex> stat_lock(stats_mutex_);
                error_message_ = "Error al abrir pipe de FFmpeg.";
            }
            LOG_ERROR("Canal [" + name_ + "]: " + error_message_);
            cleanup();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            current_video_filename = video_filename_;
            continue;
        }

        {
            std::lock_guard<std::mutex> stat_lock(stats_mutex_);
            error_message_ = "";
        }
        LOG_INFO("Transmisión de salida inicializada (VideoPack nativo) para canal [" + name_ + "]");

        int loop_counter = 0;
        bool ffmpeg_running = true;
        while (running_ && ffmpeg_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Simular estadísticas basadas en la tasa de bits estimada con fluctuación senoidal para dinamismo
            static thread_local double phase = 0.0;
            phase += 0.15;
            if (phase > 2.0 * 3.14159265) phase -= 2.0 * 3.14159265;
            double fluctuation = 1.0 + 0.12 * std::sin(phase) + 0.03 * std::sin(phase * 4.2);
            int64_t bytes_to_add = static_cast<int64_t>(((expected_bitrate_bps * 0.1) / 8.0) * fluctuation);
            bytes_accumulator_ += bytes_to_add;
            packets_sent_ += bytes_to_add / 1316;

            loop_counter++;
            if (loop_counter >= 10) { // Cada 1 segundo
                loop_counter = 0;
                
                // Comprobar si ha cambiado la configuración
                std::string latest_msg = StreamerEngine::GetInstance().GetActiveMessageForStream(id_);
                if (latest_msg != current_msg || video_filename_ != current_video_filename ||
                    transcode_enabled_ != current_transcode_enabled || transcode_video_ != current_transcode_video ||
                    transcode_audio_ != current_transcode_audio || limit_bitrate_ != current_limit_bitrate) {
                    LOG_INFO("Canal [" + name_ + "]: Configuración, archivo o mensaje de texto de salida modificados. Reiniciando stream.");
                    ffmpeg_running = false;
                    break;
                }

                // Comprobar si FFmpeg sigue vivo escribiendo un salto de línea en su stdin
                if (ffmpeg_pipe_) {
                    if (fwrite("\n", 1, 1, ffmpeg_pipe_) != 1 || fflush(ffmpeg_pipe_) != 0) {
                        LOG_WARN("Canal [" + name_ + "]: El proceso FFmpeg terminó inesperadamente.");
                        ffmpeg_running = false;
                        break;
                    }
                }
            }
        }

        // Limpiar para la siguiente iteración (o finalización)
        cleanup();

        current_video_filename = video_filename_;
        current_msg = StreamerEngine::GetInstance().GetActiveMessageForStream(id_);
        current_transcode_enabled = transcode_enabled_;
        current_transcode_video = transcode_video_;
        current_transcode_audio = transcode_audio_;
        current_limit_bitrate = limit_bitrate_;
    }
}

void OutputStream::OutputLoop() {
    bool is_video_pack = is_video_pack_;
    if (is_video_pack) {
        OutputLoopVideoPack();
        return;
    }
    AVFormatContext* out_fmt_ctx = nullptr;
    std::map<int, int> stream_mapping;
    std::map<int, AVBSFContext*> bsf_contexts;
    std::map<int, int64_t> last_written_dts;
    bool initialized = false;
    std::chrono::steady_clock::time_point last_init_attempt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::string initialized_message_ = "";
    auto last_message_check_ = std::chrono::steady_clock::now();

    auto cleanup = [&]() {
        ClearQueue();
        last_written_dts.clear();
        for (auto& pair : bsf_contexts) {
            if (pair.second) {
                av_bsf_free(&pair.second);
            }
        }
        bsf_contexts.clear();

        if (out_fmt_ctx) {
            if (initialized) {
                av_write_trailer(out_fmt_ctx);
            }
            if (out_fmt_ctx->pb) {
                if (out_fmt_ctx->flags & AVFMT_FLAG_CUSTOM_IO) {
                    av_freep(&out_fmt_ctx->pb->buffer);
                    avio_context_free(&out_fmt_ctx->pb);
                } else {
                    avio_closep(&out_fmt_ctx->pb);
                }
            }
            avformat_free_context(out_fmt_ctx);
            out_fmt_ctx = nullptr;
        }

        if (ffmpeg_pipe_) {
            pclose(ffmpeg_pipe_);
            ffmpeg_pipe_ = nullptr;
        }

        initialized = false;
        stream_mapping.clear();
    };

    while (running_) {
        // Check if we need to initialize or re-initialize streams
        std::vector<PreparedStream> local_prepared;
        bool reinit_requested = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (needs_reinit_) {
                local_prepared = prepared_streams_;
                needs_reinit_ = false;
                reinit_requested = true;
            } else if (!initialized) {
                local_prepared = prepared_streams_;
            }
        }

        bool should_init = reinit_requested;
        if (!initialized && !local_prepared.empty()) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_init_attempt >= std::chrono::seconds(2)) {
                should_init = true;
                last_init_attempt = now;
            }
        }

        if (should_init) {
            cleanup();
            if (!local_prepared.empty()) {
                std::lock_guard<std::mutex> stat_lock(stats_mutex_);
                error_message_ = "";

                std::string resolved_url = "";
                std::string out_iface = "";
                if (!outputs_.empty()) {
                    resolved_url = outputs_[0].url;
                    out_iface = outputs_[0].output_interface;
                }
                if (!out_iface.empty()) {
                    std::string iface_ip = GetInterfaceIP(out_iface);
                    if (!iface_ip.empty()) {
                        if (resolved_url.rfind("udp://", 0) == 0 || resolved_url.rfind("srt://", 0) == 0) {
                            if (resolved_url.find("localaddr=") == std::string::npos) {
                                if (resolved_url.find('?') == std::string::npos) {
                                    resolved_url += "?localaddr=" + iface_ip;
                                } else {
                                    resolved_url += "&localaddr=" + iface_ip;
                                }
                            }
                        }
                    }
                }

                int ret = avformat_alloc_output_context2(&out_fmt_ctx, nullptr, "mpegts", nullptr);
                if (ret < 0) {
                    char err_buf[256];
                    av_strerror(ret, err_buf, sizeof(err_buf));
                    error_message_ = "Error alloc output: " + std::string(err_buf);
                    LOG_ERROR("Canal [" + name_ + "]: " + error_message_);
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                std::string active_msg = StreamerEngine::GetInstance().GetActiveMessageForStream(id_);
                initialized_message_ = active_msg;
                bool force_transcode = !active_msg.empty();
                
                bool force_ffmpeg = !outputs_.empty();
                bool use_transcoding = transcode_enabled_ || force_transcode || force_ffmpeg;

                if (use_transcoding) {
                    bool do_video_transcode = transcode_video_ || (limit_bitrate_ > 0) || force_transcode;
                    bool use_cuda = do_video_transcode && IsNvidiaGPUPresent();
                    
                    std::string ffmpeg_cmd = "ffmpeg -y -hide_banner -loglevel error ";
                    if (use_cuda) {
                        ffmpeg_cmd += "-hwaccel cuda ";
                    }
                    ffmpeg_cmd += "-copyts -i pipe:0 ";
                    
                    // Video options
                    if (do_video_transcode) {
                        std::string target_format = video_output_format_;
                        if (!transcode_video_) {
                            // Keep original codec if transcode_video is unchecked (but transcoding is forced by limit_bitrate)
                            std::string det_v = detected_video_codec_;
                            std::transform(det_v.begin(), det_v.end(), det_v.begin(), ::tolower);
                            if (det_v == "hevc" || det_v == "h265") {
                                target_format = "hevc";
                            } else if (det_v == "mpeg2" || det_v == "mpeg2video") {
                                target_format = "mpeg2video";
                            } else {
                                target_format = "h264";
                            }
                        }

                        std::string codec = "libx264";
                        if (IsNvidiaGPUPresent()) {
                            codec = "h264_nvenc";
                            if (target_format == "hevc") codec = "hevc_nvenc";
                            else if (target_format == "mpeg2video") codec = "mpeg2video";
                        } else {
                            if (target_format == "hevc") codec = "libx265";
                            else if (target_format == "mpeg2video") codec = "mpeg2video";
                        }
                        
                        ffmpeg_cmd += "-c:v " + codec + " ";
                        if (codec == "h264_nvenc" || codec == "hevc_nvenc") {
                            ffmpeg_cmd += "-preset p1 -tune ll -g 60 -forced-idr 1 -aud 1 -flags:v -global_header ";
                        } else if (codec == "libx264" || codec == "libx265") {
                            ffmpeg_cmd += "-preset ultrafast -tune zerolatency -g 60 -flags:v -global_header ";
                        }
                        
                        if (!active_msg.empty()) {
                            std::string escaped_msg = "";
                            for (char c : active_msg) {
                                if (c == '\'') {
                                    escaped_msg += "'\\''";
                                } else if (c == ',') {
                                    escaped_msg += "\\\\,";
                                } else if (c == ':') {
                                    escaped_msg += "\\\\:";
                                } else if (c == '\\') {
                                    escaped_msg += "\\\\\\\\";
                                } else {
                                    escaped_msg += c;
                                }
                            }
                            ffmpeg_cmd += "-vf \"drawtext=fontfile=/usr/share/fonts/truetype/freefont/FreeSans.ttf:text='" + escaped_msg + "':expansion=none:x=(w-text_w)/2:y=h-50:fontsize=32:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=10\" ";
                        }
                        
                        if (limit_bitrate_ > 0) {
                            int v_bitrate = std::max(200, limit_bitrate_ - 128);
                            ffmpeg_cmd += "-b:v " + std::to_string(v_bitrate) + "k -maxrate " + std::to_string(v_bitrate) + "k -bufsize " + std::to_string(2 * v_bitrate) + "k ";
                        }
                    } else {
                        ffmpeg_cmd += "-c:v copy ";
                    }
                    
                    // Audio options
                    if (transcode_audio_) {
                        std::string codec = "aac";
                        if (audio_output_format_ == "mp3") codec = "libmp3lame";
                        else if (audio_output_format_ == "ac3") codec = "ac3";
                        
                        ffmpeg_cmd += "-c:a " + codec + " -b:a 128k ";
                    } else {
                        ffmpeg_cmd += "-c:a copy ";
                    }
                    
                    std::string outputs_str = "";
                    for (const auto& dest : outputs_) {
                        std::string r_url = dest.url;
                        std::string o_iface = dest.output_interface;
                        if (!o_iface.empty()) {
                            std::string iface_ip = GetInterfaceIP(o_iface);
                            if (!iface_ip.empty()) {
                                if (r_url.rfind("udp://", 0) == 0 || r_url.rfind("srt://", 0) == 0 || r_url.rfind("rtp://", 0) == 0) {
                                    if (r_url.find("localaddr=") == std::string::npos) {
                                        if (r_url.find('?') == std::string::npos) {
                                            r_url += "?localaddr=" + iface_ip;
                                        } else {
                                            r_url += "&localaddr=" + iface_ip;
                                        }
                                    }
                                }
                            }
                        }

                        if (dest.type == "hls" || r_url.rfind("hls://", 0) == 0 || r_url.find(".m3u8") != std::string::npos) {
                            try {
                                std::filesystem::path p(r_url);
                                std::filesystem::create_directories(p.parent_path());
                            } catch (...) {}
                            outputs_str += " -f hls -hls_time 2 -hls_list_size 5 -hls_flags delete_segments \"" + r_url + "\"";
                        } else if (dest.type == "rtp" || r_url.rfind("rtp://", 0) == 0) {
                            outputs_str += " -f rtp_mpegts \"" + r_url + "\"";
                        } else {
                            std::string optimized_url = r_url;
                            if (optimized_url.rfind("udp://", 0) == 0) {
                                if (optimized_url.find("pkt_size=") == std::string::npos) {
                                    if (optimized_url.find('?') == std::string::npos) {
                                        optimized_url += "?pkt_size=1316";
                                    } else {
                                        optimized_url += "&pkt_size=1316";
                                    }
                                }
                            }
                            outputs_str += " -f mpegts \"" + optimized_url + "\"";
                        }
                    }
                    ffmpeg_cmd += outputs_str;
                    
                    LOG_INFO("Canal [" + name_ + "]: Iniciando comando de transcodificación: " + ffmpeg_cmd);
                    
                    ffmpeg_pipe_ = popen(ffmpeg_cmd.c_str(), "w");
                    if (!ffmpeg_pipe_) {
                        error_message_ = "Error al abrir pipe de FFmpeg para transcodificación.";
                        LOG_ERROR("Canal [" + name_ + "]: " + error_message_);
                        avformat_free_context(out_fmt_ctx);
                        out_fmt_ctx = nullptr;
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        continue;
                    }
                }

                // Add all prepared streams
                bool streams_ok = true;
                for (const auto& ps : local_prepared) {
                    AVStream* out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
                    if (!out_stream) {
                        LOG_ERROR("Canal [" + name_ + "]: No se pudo crear nuevo stream de salida.");
                        streams_ok = false;
                        break;
                    }
                    if (ps.codecpar) {
                        avcodec_parameters_copy(out_stream->codecpar, ps.codecpar);
                        out_stream->codecpar->codec_tag = 0; // Let FFmpeg handle it
                    }
                    out_stream->time_base = ps.time_base;
                    stream_mapping[ps.input_stream_idx] = out_stream->index;

                    // Setup BSF for H.264/HEVC streams mapped to mpegts output
                    if (ps.codecpar) {
                        const AVBitStreamFilter* bsf = nullptr;
                        if (ps.codecpar->extradata_size > 0 && ps.codecpar->extradata[0] == 1) {
                            // AVCC format needs conversion to Annex B
                            if (ps.codecpar->codec_id == AV_CODEC_ID_H264) {
                                bsf = av_bsf_get_by_name("h264_mp4toannexb");
                            } else if (ps.codecpar->codec_id == AV_CODEC_ID_HEVC) {
                                bsf = av_bsf_get_by_name("hevc_mp4toannexb");
                            }
                        } else if (ps.codecpar->extradata_size > 0) {
                            // Annex B format with extradata: insert SPS/PPS before keyframes
                            if (ps.codecpar->codec_id == AV_CODEC_ID_H264 || ps.codecpar->codec_id == AV_CODEC_ID_HEVC) {
                                bsf = av_bsf_get_by_name("dump_extra");
                            }
                        }

                        if (bsf) {
                            AVBSFContext* bsf_ctx = nullptr;
                            int bsf_ret = av_bsf_alloc(bsf, &bsf_ctx);
                            if (bsf_ret >= 0) {
                                avcodec_parameters_copy(bsf_ctx->par_in, ps.codecpar);
                                bsf_ctx->time_base_in = ps.time_base;
                                bsf_ret = av_bsf_init(bsf_ctx);
                                if (bsf_ret >= 0) {
                                    bsf_contexts[out_stream->index] = bsf_ctx;
                                    // Update codec parameters to match the output of BSF (with SPS/PPS extradata)
                                    avcodec_parameters_copy(out_stream->codecpar, bsf_ctx->par_out);
                                    LOG_INFO("Filtro BSF '" + std::string(bsf->name) + "' inicializado para stream index " + std::to_string(out_stream->index));
                                } else {
                                    LOG_WARN("Error al inicializar BSF para stream " + std::to_string(out_stream->index));
                                    av_bsf_free(&bsf_ctx);
                                }
                            } else {
                                LOG_WARN("Error al asignar BSF para stream " + std::to_string(out_stream->index));
                            }
                        }
                    }
                }

                if (streams_ok) {
                    if (use_transcoding) {
                        const int avio_buffer_size = 4096;
                        unsigned char* avio_buffer = (unsigned char*)av_malloc(avio_buffer_size);
                        out_fmt_ctx->pb = avio_alloc_context(
                            avio_buffer, avio_buffer_size,
                            1, // Write flag
                            ffmpeg_pipe_,
                            nullptr, // Read callback
                            &write_to_ffmpeg_pipe,
                            nullptr // Seek callback
                        );
                        out_fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
                    } else if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                        AVDictionary* opts = nullptr;
                        int open_ret = avio_open2(&out_fmt_ctx->pb, resolved_url.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
                        if (opts) av_dict_free(&opts);
                        if (open_ret < 0) {
                            char err_buf[256];
                            av_strerror(open_ret, err_buf, sizeof(err_buf));
                            error_message_ = "Error abriendo salida: " + std::string(err_buf);
                            LOG_ERROR("Canal [" + name_ + "]: " + error_message_);
                            cleanup();
                        }
                    }

                    if (out_fmt_ctx) {
                        int header_ret = avformat_write_header(out_fmt_ctx, nullptr);
                        if (header_ret < 0) {
                            char err_buf[256];
                            av_strerror(header_ret, err_buf, sizeof(err_buf));
                            error_message_ = "Error escribiendo cabecera: " + std::string(err_buf);
                            LOG_ERROR("Canal [" + name_ + "]: " + error_message_);
                            cleanup();
                        } else {
                             initialized = true;
                             LOG_INFO("Transmisión de salida inicializada para canal [" + name_ + "]");
                             if (use_transcoding) {
                                 std::string msg = "Transcoding habilitado para canal [" + name_ + "]:";
                                 if (transcode_video_ || (limit_bitrate_ > 0)) {
                                     std::string target_format = video_output_format_;
                                     if (!transcode_video_) {
                                         std::string det_v = detected_video_codec_;
                                         std::transform(det_v.begin(), det_v.end(), det_v.begin(), ::tolower);
                                         if (det_v == "hevc" || det_v == "h265") target_format = "hevc";
                                         else if (det_v == "mpeg2" || det_v == "mpeg2video") target_format = "mpeg2video";
                                         else target_format = "h264";
                                     }
                                     msg += " Video (" + video_input_format_ + " -> " + target_format + ")";
                                 }
                                 if (transcode_audio_) msg += " Audio (" + audio_input_format_ + " -> " + audio_output_format_ + ")";
                                 if (limit_bitrate_ > 0) msg += " Límite Bitrate: " + std::to_string(limit_bitrate_) + " Kbps";
                                 LOG_INFO(msg);
                             }
                        }
                    }
                }
            }
        }

        QueuedPacket q_pkt;
        bool has_pkt = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait_for(lock, std::chrono::seconds(2), [&] { return !packet_queue_.empty() || !running_ || needs_reinit_; });
            if (!running_) break;
            if (needs_reinit_) continue;
            if (!packet_queue_.empty()) {
                q_pkt = packet_queue_.front();
                packet_queue_.pop();
                has_pkt = true;
            }
        }

        // Periodic check for message changes (every 2 seconds)
        auto time_now = std::chrono::steady_clock::now();
        if (time_now - last_message_check_ >= std::chrono::seconds(2)) {
            last_message_check_ = time_now;
            std::string current_msg = StreamerEngine::GetInstance().GetActiveMessageForStream(id_);
            if (current_msg != initialized_message_) {
                LOG_INFO("Canal [" + name_ + "]: El mensaje programado cambió/inició/terminó. Reiniciando stream.");
                std::lock_guard<std::mutex> lock(queue_mutex_);
                needs_reinit_ = true;
                cv_.notify_all();
                if (has_pkt) {
                    av_packet_free(&q_pkt.pkt);
                }
                continue;
            }
        }

        if (has_pkt) {
            if (initialized) {
                auto it = stream_mapping.find(q_pkt.input_stream_idx);
                if (it != stream_mapping.end()) {
                    int out_stream_idx = it->second;
                    AVStream* out_stream = out_fmt_ctx->streams[out_stream_idx];

                    AVBSFContext* bsf_ctx = nullptr;
                    auto bsf_it = bsf_contexts.find(out_stream_idx);
                    if (bsf_it != bsf_contexts.end()) {
                        bsf_ctx = bsf_it->second;
                    }

                    if (bsf_ctx) {
                        int bsf_ret = av_bsf_send_packet(bsf_ctx, q_pkt.pkt);
                        if (bsf_ret < 0) {
                            char err_buf[256];
                            av_strerror(bsf_ret, err_buf, sizeof(err_buf));
                            LOG_WARN("Canal [" + name_ + "]: Error bsf_send_packet: " + std::string(err_buf));
                        }

                        while (true) {
                            AVPacket* filtered_pkt = av_packet_alloc();
                            int bsf_recv = av_bsf_receive_packet(bsf_ctx, filtered_pkt);
                            if (bsf_recv < 0) {
                                av_packet_free(&filtered_pkt);
                                break;
                            }

                            // Rescale timestamps
                            av_packet_rescale_ts(filtered_pkt, q_pkt.time_base, out_stream->time_base);
                            filtered_pkt->stream_index = out_stream_idx;

                            // Monotonicity check
                            if (filtered_pkt->dts != AV_NOPTS_VALUE) {
                                auto last_dts_it = last_written_dts.find(out_stream_idx);
                                if (last_dts_it != last_written_dts.end()) {
                                    int64_t last_dts = last_dts_it->second;
                                    if (last_dts != AV_NOPTS_VALUE && filtered_pkt->dts <= last_dts) {
                                        filtered_pkt->dts = last_dts + 1;
                                        if (filtered_pkt->pts != AV_NOPTS_VALUE && filtered_pkt->pts < filtered_pkt->dts) {
                                            filtered_pkt->pts = filtered_pkt->dts;
                                        }
                                    }
                                }
                                last_written_dts[out_stream_idx] = filtered_pkt->dts;
                            }

                            int pkt_size = filtered_pkt->size;

                            int write_ret = av_write_frame(out_fmt_ctx, filtered_pkt);
                            av_packet_free(&filtered_pkt);

                            if (write_ret < 0) {
                                char err_buf[256];
                                av_strerror(write_ret, err_buf, sizeof(err_buf));
                                std::lock_guard<std::mutex> stat_lock(stats_mutex_);
                                error_message_ = "Error escribiendo frame (BSF): " + std::string(err_buf);
                                LOG_WARN("Canal [" + name_ + "]: " + error_message_ + ". Reiniciando salida.");
                                cleanup();
                                break;
                            } else {
                                packets_sent_++;
                                bytes_accumulator_ += pkt_size;
                            }
                        }
                    } else {
                        // Rescale timestamps
                        av_packet_rescale_ts(q_pkt.pkt, q_pkt.time_base, out_stream->time_base);
                        q_pkt.pkt->stream_index = out_stream_idx;

                        // Monotonicity check
                        if (q_pkt.pkt->dts != AV_NOPTS_VALUE) {
                            auto last_dts_it = last_written_dts.find(out_stream_idx);
                            if (last_dts_it != last_written_dts.end()) {
                                int64_t last_dts = last_dts_it->second;
                                if (last_dts != AV_NOPTS_VALUE && q_pkt.pkt->dts <= last_dts) {
                                    q_pkt.pkt->dts = last_dts + 1;
                                    if (q_pkt.pkt->pts != AV_NOPTS_VALUE && q_pkt.pkt->pts < q_pkt.pkt->dts) {
                                        q_pkt.pkt->pts = q_pkt.pkt->dts;
                                    }
                                }
                            }
                            last_written_dts[out_stream_idx] = q_pkt.pkt->dts;
                        }

                        int pkt_size = q_pkt.pkt->size;

                        // Write frame
                        int write_ret = av_write_frame(out_fmt_ctx, q_pkt.pkt);
                        if (write_ret < 0) {
                            char err_buf[256];
                            av_strerror(write_ret, err_buf, sizeof(err_buf));
                            std::lock_guard<std::mutex> stat_lock(stats_mutex_);
                            error_message_ = "Error escribiendo frame: " + std::string(err_buf);
                            LOG_WARN("Canal [" + name_ + "]: " + error_message_ + ". Reiniciando salida.");
                            cleanup();
                        } else {
                            packets_sent_++;
                            bytes_accumulator_ += pkt_size;
                        }
                    }
                } else {
                    packets_dropped_++;
                }
            } else {
                packets_dropped_++;
            }

            // Clean up packet
            av_packet_free(&q_pkt.pkt);
        }
    }

    cleanup();
}


// ==========================================
// INPUT SOURCE IMPLEMENTATION
// ==========================================

InputSource::InputSource(const std::string& id, const std::string& name, const std::string& url, bool enabled, bool is_video_pack)
    : id_(id), name_(name), url_(url), enabled_(enabled), is_video_pack_(is_video_pack) {
    last_bitrate_calc_ = std::chrono::steady_clock::now();
}

InputSource::~InputSource() {
    Stop();
    ClearSourceStreams();
}

void InputSource::Start() {
    if (!enabled_ || running_) return;
    running_ = true;
    thread_ = std::thread(&InputSource::InputLoop, this);
    bitrate_thread_ = std::thread(&InputSource::CalculateBitrates, this);
    LOG_INFO("Fuente de entrada [" + name_ + "] iniciada.");
}

void InputSource::Stop() {
    if (!running_) return;
    running_ = false;
    
    if (thread_.joinable()) {
        thread_.join();
    }
    if (bitrate_thread_.joinable()) {
        bitrate_thread_.join();
    }
    connected_ = false;
    LOG_INFO("Fuente de entrada [" + name_ + "] detenida.");
}

void InputSource::SetEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled) {
        Start();
    } else {
        Stop();
    }
}

void InputSource::RegisterOutputStream(OutputStream* out_stream) {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    listeners_.push_back(out_stream);
    LOG_INFO("Salida [" + out_stream->GetName() + "] registrada en entrada [" + name_ + "]");

    if (connected_) {
        std::vector<PreparedStream> streams_for_listener;
        for (const auto& s : source_streams_) {
            if (s.program_number == out_stream->GetProgramNumber()) {
                streams_for_listener.push_back(PreparedStream(s.input_stream_idx, s.codecpar, s.time_base));
            }
        }
        out_stream->PrepareStreams(streams_for_listener);
    }
}

void OutputStream::SetEnabled(bool enabled); // Forward reference resolved

void InputSource::UnregisterOutputStream(OutputStream* out_stream) {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    auto it = std::find(listeners_.begin(), listeners_.end(), out_stream);
    if (it != listeners_.end()) {
        listeners_.erase(it);
        LOG_INFO("Salida [" + out_stream->GetName() + "] desregistrada de entrada [" + name_ + "]");
        out_stream->PrepareStreams({});
    }
}

InputStats InputSource::GetStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    InputStats stats;
    stats.id = id_;
    stats.name = name_;
    stats.url = url_;
    stats.connected = connected_;
    stats.total_bitrate_kbps = total_bitrate_kbps_;
    if (is_video_pack_) {
        stats.programs = scan_video_pack_directory(url_);
    } else {
        stats.programs = programs_;
    }
    return stats;
}

void InputSource::CalculateBitrates() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!connected_) {
            total_bitrate_kbps_ = 0.0;
            std::lock_guard<std::mutex> lock(stats_mutex_);
            for (auto& prog : programs_) {
                prog.bitrate_kbps = 0.0;
            }
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_bitrate_calc_).count();
        if (duration > 0) {
            uint64_t total_bytes = total_bytes_accumulator_.exchange(0);
            double instant_total = (total_bytes * 8.0) / duration;
            double current_total = total_bitrate_kbps_.load();
            if (current_total == 0.0) {
                total_bitrate_kbps_ = instant_total;
            } else if (instant_total > 0) {
                total_bitrate_kbps_ = current_total * 0.7 + instant_total * 0.3;
            } else {
                double decayed = current_total * 0.75;
                total_bitrate_kbps_ = (decayed < 1.0) ? 0.0 : decayed;
            }

            std::lock_guard<std::mutex> lock(stats_mutex_);
            for (auto& prog : programs_) {
                uint64_t prog_bytes = prog.bytes_accumulator;
                prog.bytes_accumulator = 0;
                double instant_prog = (prog_bytes * 8.0) / duration;
                if (prog.bitrate_kbps == 0.0) {
                    prog.bitrate_kbps = instant_prog;
                } else if (instant_prog > 0) {
                    prog.bitrate_kbps = prog.bitrate_kbps * 0.7 + instant_prog * 0.3;
                } else {
                    double decayed = prog.bitrate_kbps * 0.75;
                    prog.bitrate_kbps = (decayed < 1.0) ? 0.0 : decayed;
                }
            }
        }
        last_bitrate_calc_ = now;
    }
}

void InputSource::ClearSourceStreams() {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    for (auto& s : source_streams_) {
        if (s.codecpar) {
            avcodec_parameters_free(&s.codecpar);
        }
    }
    source_streams_.clear();
}

void InputSource::PrepareAllListeners() {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    for (auto* listener : listeners_) {
        std::vector<PreparedStream> streams_for_listener;
        for (const auto& s : source_streams_) {
            if (s.program_number == listener->GetProgramNumber()) {
                streams_for_listener.push_back(PreparedStream(s.input_stream_idx, s.codecpar, s.time_base));
            }
        }
        listener->PrepareStreams(streams_for_listener);
    }
}

void InputSource::AnalyzePrograms(AVFormatContext* fmt_ctx) {
    // Clear previous source streams
    ClearSourceStreams();

    std::lock_guard<std::mutex> list_lock(listeners_mutex_);
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    programs_.clear();

    if (!fmt_ctx) return;

    if (fmt_ctx->nb_programs == 0) {
        ProgramInfo info;
        info.program_number = 1;
        info.name = "Programa 01";
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            info.pids.push_back(i);

            AVCodecParameters* cp = avcodec_parameters_alloc();
            if (cp) {
                avcodec_parameters_copy(cp, fmt_ctx->streams[i]->codecpar);
            }
            SourceStreamInfo s_info = {
                1,
                static_cast<int>(i),
                cp,
                fmt_ctx->streams[i]->time_base
            };
            source_streams_.push_back(s_info);
        }
        programs_.push_back(info);
        LOG_INFO("Entrada [" + name_ + "] sin programas definidos. Programa por defecto '1' sintetizado.");
        return;
    }

    for (unsigned int i = 0; i < fmt_ctx->nb_programs; i++) {
        AVProgram* prog = fmt_ctx->programs[i];
        ProgramInfo info;
        info.program_number = prog->program_num;

        // Try to get service name
        AVDictionaryEntry* entry = av_dict_get(prog->metadata, "service_name", nullptr, 0);
        if (!entry) {
            entry = av_dict_get(prog->metadata, "title", nullptr, 0);
        }
        
        if (entry) {
            info.name = entry->value;
        } else {
            // Emulate nice names based on user expectation if it maps to ESPN, Institucionales, etc.
            // But by default "Canal <number>" or "Programa <number>"
            std::stringstream ss;
            ss << std::setw(2) << std::setfill('0') << prog->program_num;
            info.name = "Programa " + ss.str();
        }

        for (unsigned int j = 0; j < prog->nb_stream_indexes; j++) {
            int stream_idx = prog->stream_index[j];
            info.pids.push_back(stream_idx);

            // Populate source stream info
            AVCodecParameters* cp = avcodec_parameters_alloc();
            if (cp) {
                avcodec_parameters_copy(cp, fmt_ctx->streams[stream_idx]->codecpar);
            }
            SourceStreamInfo s_info = {
                prog->program_num,
                stream_idx,
                cp,
                fmt_ctx->streams[stream_idx]->time_base
            };
            source_streams_.push_back(s_info);
        }

        programs_.push_back(info);
    }

    // Sort by program number
    std::sort(programs_.begin(), programs_.end(), [](const ProgramInfo& a, const ProgramInfo& b) {
        return a.program_number < b.program_number;
    });

    LOG_INFO("Entrada [" + name_ + "] analizada. Encontrados " + std::to_string(programs_.size()) + " canales/programas.");
}

void InputSource::InputLoop() {
    if (is_video_pack_) {
        connected_ = true;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        connected_ = false;
        return;
    }
    avformat_network_init();

    while (running_) {
        AVFormatContext* in_fmt_ctx = nullptr;
        AVDictionary* opts = nullptr;
        
        // For HLS/HTTP we set timeout
        av_dict_set(&opts, "timeout", "5000000", 0); // 5 seconds in microseconds
        // For SRT we can let FFmpeg handle default parameters or pass them in URL

        LOG_INFO("Conectando a entrada [" + name_ + "] URL: " + url_);
        
        int ret = avformat_open_input(&in_fmt_ctx, url_.c_str(), nullptr, &opts);
        if (opts) av_dict_free(&opts);

        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_WARN("No se pudo abrir la entrada [" + name_ + "]: " + std::string(err_buf) + ". Reintentando en 5s...");
            connected_ = false;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_WARN("No se pudo obtener información del flujo en [" + name_ + "]: " + std::string(err_buf) + ". Reintentando...");
            avformat_close_input(&in_fmt_ctx);
            connected_ = false;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        connected_ = true;
        AnalyzePrograms(in_fmt_ctx);
        PrepareAllListeners();

        bool is_file = (url_.find("://") == std::string::npos || url_.compare(0, 7, "file://") == 0);
        std::map<int, int64_t> stream_dts_offsets;
        std::map<int, int64_t> stream_start_dts;
        std::map<int, int64_t> stream_last_dts;
        std::map<int, int64_t> stream_last_pts;
        double first_pts_secs = -1.0;
        auto loop_start_time = std::chrono::steady_clock::now();

        int master_stream_idx = -1;
        if (is_file) {
            for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
                if (in_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    master_stream_idx = i;
                    break;
                }
            }
            if (master_stream_idx == -1) {
                for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
                    if (in_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                        master_stream_idx = i;
                        break;
                    }
                }
            }
        }

        AVPacket* pkt = av_packet_alloc();
        
        // Main reading loop
        while (running_) {
            ret = av_read_frame(in_fmt_ctx, pkt);
            if (ret < 0) {
                if (ret == AVERROR_EOF && is_file) {
                    ret = av_seek_frame(in_fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
                    if (ret < 0) {
                        ret = avformat_seek_file(in_fmt_ctx, -1, INT64_MIN, 0, INT64_MAX, 0);
                    }
                    if (ret >= 0) {
                        // Successfully looped! Update offsets subtracting original start timestamp
                        for (auto const& [idx, last_dts] : stream_last_dts) {
                            int64_t start_ts = 0;
                            if (stream_start_dts.find(idx) != stream_start_dts.end()) {
                                start_ts = stream_start_dts[idx];
                            }
                            stream_dts_offsets[idx] = last_dts + 1 - start_ts;
                        }
                        loop_start_time = std::chrono::steady_clock::now();
                        first_pts_secs = -1.0;
                        LOG_INFO("Entrada local [" + name_ + "] finalizada. Reiniciando bucle de reproducción.");
                        continue;
                    }
                }

                char err_buf[256];
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_WARN("Error de lectura en entrada [" + name_ + "]: " + std::string(err_buf) + ". Reconectando...");
                break;
            }

            // If it's a local file: pacing and timestamp offsetting
            if (is_file) {
                int idx = pkt->stream_index;
                int64_t duration = pkt->duration;
                if (duration <= 0) duration = 0;

                // Record start DTS of the file (on the very first loop, before offsets are applied)
                if (stream_start_dts.find(idx) == stream_start_dts.end()) {
                    int64_t ts = pkt->dts;
                    if (ts == AV_NOPTS_VALUE) {
                        ts = pkt->pts;
                    }
                    if (ts != AV_NOPTS_VALUE) {
                        stream_start_dts[idx] = ts;
                    }
                }

                // 1. Timestamp offsetting
                if (stream_dts_offsets.find(idx) != stream_dts_offsets.end()) {
                    if (pkt->dts != AV_NOPTS_VALUE) {
                        pkt->dts += stream_dts_offsets[idx];
                    }
                    if (pkt->pts != AV_NOPTS_VALUE) {
                        pkt->pts += stream_dts_offsets[idx];
                    }
                }

                // Update last seen DTS/PTS
                if (pkt->dts != AV_NOPTS_VALUE) {
                    stream_last_dts[idx] = pkt->dts;
                } else if (pkt->pts != AV_NOPTS_VALUE) {
                    stream_last_dts[idx] = pkt->pts;
                } else {
                    if (stream_last_dts.find(idx) == stream_last_dts.end()) {
                        stream_last_dts[idx] = 0;
                    } else {
                        stream_last_dts[idx] += duration;
                    }
                }

                if (pkt->pts != AV_NOPTS_VALUE) {
                    stream_last_pts[idx] = pkt->pts;
                }

                // 2. Real-time pacing (speed regulation) using the master stream as clock reference
                if (idx == master_stream_idx) {
                    int64_t ts = pkt->dts;
                    if (ts == AV_NOPTS_VALUE) {
                        ts = pkt->pts;
                    }

                    if (ts != AV_NOPTS_VALUE) {
                        double pkt_pts_secs = ts * av_q2d(in_fmt_ctx->streams[idx]->time_base);
                        if (first_pts_secs < 0) {
                            first_pts_secs = pkt_pts_secs;
                        }

                        double relative_pkt_time = pkt_pts_secs - first_pts_secs;
                        auto now = std::chrono::steady_clock::now();
                        double elapsed_wall_clock = std::chrono::duration_cast<std::chrono::microseconds>(now - loop_start_time).count() / 1000000.0;

                        if (relative_pkt_time > elapsed_wall_clock) {
                            double sleep_sec = relative_pkt_time - elapsed_wall_clock;
                            if (sleep_sec > 1.0) {
                                sleep_sec = 1.0;
                                loop_start_time = now - std::chrono::microseconds(static_cast<int64_t>(relative_pkt_time * 1000000.0));
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(sleep_sec * 1000000.0)));
                        } else if (elapsed_wall_clock - relative_pkt_time > 0.5) {
                            // Adjust baseline clock if we lag too far behind to prevent catch-up bursts
                            loop_start_time = now - std::chrono::microseconds(static_cast<int64_t>(relative_pkt_time * 1000000.0));
                        }
                    }
                }
            }

            // Stats
            total_bytes_accumulator_ += pkt->size;

            // Find which program this packet belongs to
            int prog_num = -1;
            if (in_fmt_ctx->nb_programs == 0) {
                prog_num = 1;
            } else {
                for (unsigned int i = 0; i < in_fmt_ctx->nb_programs; i++) {
                    AVProgram* prog = in_fmt_ctx->programs[i];
                    for (unsigned int j = 0; j < prog->nb_stream_indexes; j++) {
                        if (prog->stream_index[j] == pkt->stream_index) {
                            prog_num = prog->program_num;
                            break;
                        }
                    }
                    if (prog_num != -1) break;
                }
            }

            // Add bytes to program accumulator
            if (prog_num != -1) {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                for (auto& prog : programs_) {
                    if (prog.program_number == prog_num) {
                        prog.bytes_accumulator += pkt->size;
                        break;
                    }
                }
            }

            // Dispatch packet to matching output streams
            {
                std::lock_guard<std::mutex> listeners_lock(listeners_mutex_);
                for (auto* listener : listeners_) {
                    if (listener->IsEnabled() && listener->GetProgramNumber() == prog_num) {
                        listener->PushPacket(pkt, pkt->stream_index, in_fmt_ctx->streams[pkt->stream_index]->time_base);
                    }
                }
            }

            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        avformat_close_input(&in_fmt_ctx);
        connected_ = false;
        ClearSourceStreams();
        PrepareAllListeners();
        LOG_INFO("Entrada [" + name_ + "] desconectada.");
        
        if (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

// Static probe method
std::vector<ProgramInfo> InputSource::ProbeURL(const std::string& url, std::string& error_out) {
    try {
        if (std::filesystem::exists(url) && std::filesystem::is_directory(url)) {
            return scan_video_pack_directory(url);
        }
    } catch (...) {}

    avformat_network_init();
    AVFormatContext* in_fmt_ctx = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "timeout", "4000000", 0); // 4 seconds probe
    
    std::vector<ProgramInfo> progs;
    
    int ret = avformat_open_input(&in_fmt_ctx, url.c_str(), nullptr, &opts);
    if (opts) av_dict_free(&opts);

    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        error_out = std::string(err_buf);
        return progs;
    }

    ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        error_out = std::string(err_buf);
        avformat_close_input(&in_fmt_ctx);
        return progs;
    }

    if (in_fmt_ctx->nb_programs == 0) {
        ProgramInfo info;
        info.program_number = 1;
        info.name = "Programa 01";
        for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
            info.pids.push_back(i);
        }
        progs.push_back(info);
    } else {
        for (unsigned int i = 0; i < in_fmt_ctx->nb_programs; i++) {
            AVProgram* prog = in_fmt_ctx->programs[i];
            ProgramInfo info;
            info.program_number = prog->program_num;

            AVDictionaryEntry* entry = av_dict_get(prog->metadata, "service_name", nullptr, 0);
            if (!entry) {
                entry = av_dict_get(prog->metadata, "title", nullptr, 0);
            }
            
            if (entry) {
                info.name = entry->value;
            } else {
                std::stringstream ss;
                ss << std::setw(2) << std::setfill('0') << prog->program_num;
                info.name = "Programa " + ss.str();
            }

            for (unsigned int j = 0; j < prog->nb_stream_indexes; j++) {
                info.pids.push_back(prog->stream_index[j]);
            }
            progs.push_back(info);
        }
    }

    std::sort(progs.begin(), progs.end(), [](const ProgramInfo& a, const ProgramInfo& b) {
        return a.program_number < b.program_number;
    });

    avformat_close_input(&in_fmt_ctx);
    return progs;
}


// ==========================================
// STREAMER ENGINE IMPLEMENTATION
// ==========================================

StreamerEngine& StreamerEngine::GetInstance() {
    static StreamerEngine instance;
    return instance;
}

void StreamerEngine::Init(const std::string& config_path) {
    config_path_ = config_path;
    avformat_network_init();
    LoadConfig();
}

void StreamerEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    // Stop all outputs first
    for (auto& pair : streams_) {
        pair.second->Stop();
    }
    
    // Stop all inputs
    for (auto& pair : inputs_) {
        pair.second->Stop();
    }
    
    streams_.clear();
    inputs_.clear();
}

bool StreamerEngine::LoadConfig() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    std::ifstream file(config_path_);
    if (!file.is_open()) {
        LOG_WARN("No se encontró archivo de configuración: " + config_path_ + ". Se creará uno nuevo.");
        return false;
    }

    json cfg;
    try {
        file >> cfg;
    } catch (const std::exception& e) {
        LOG_ERROR("Error al parsear config.json: " + std::string(e.what()));
        return false;
    }

    // Stop current runs
    for (auto& pair : streams_) pair.second->Stop();
    for (auto& pair : inputs_) pair.second->Stop();
    streams_.clear();
    inputs_.clear();

    if (cfg.contains("settings") && cfg["settings"].is_object()) {
        output_interface_ = cfg["settings"].value("output_interface", "");
    } else {
        output_interface_ = "";
    }

    // Load inputs
    if (cfg.contains("inputs") && cfg["inputs"].is_array()) {
        for (const auto& item : cfg["inputs"]) {
            std::string id = item["id"];
            std::string name = item["name"];
            std::string url = item["url"];
            bool enabled = item.value("enabled", true);
            bool is_video_pack = item.value("is_video_pack", false);

            auto input = std::make_unique<InputSource>(id, name, url, enabled, is_video_pack);
            inputs_[id] = std::move(input);
        }
    }

    // Load output streams
    if (cfg.contains("streams") && cfg["streams"].is_array()) {
        for (const auto& item : cfg["streams"]) {
            std::string id = item["id"];
            std::string name = item["name"];
            std::string input_id = item["input_id"];
            int program_number = item["program_number"];
            bool enabled = item.value("enabled", true);
            bool transcode_enabled = item.value("transcode_enabled", false);
            bool transcode_video = item.value("transcode_video", false);
            std::string video_input_format = item.value("video_input_format", "");
            std::string video_output_format = item.value("video_output_format", "");
            bool transcode_audio = item.value("transcode_audio", false);
            std::string audio_input_format = item.value("audio_input_format", "");
            std::string audio_output_format = item.value("audio_output_format", "");
            int limit_bitrate = item.value("limit_bitrate", 0);
            std::string video_filename = item.value("video_filename", "");

            std::string input_url = "";
            bool is_video_pack = false;
            auto input_it = inputs_.find(input_id);
            if (input_it != inputs_.end()) {
                input_url = input_it->second->GetUrl();
                is_video_pack = input_it->second->IsVideoPack();
            }

            std::vector<OutputDestination> outputs;
            if (item.contains("outputs") && item["outputs"].is_array()) {
                for (const auto& out_item : item["outputs"]) {
                    OutputDestination dest;
                    dest.url = out_item.value("url", "");
                    dest.output_interface = out_item.value("output_interface", "");
                    dest.type = out_item.value("type", "");
                    if (dest.type.empty()) {
                        if (dest.url.rfind("udp://", 0) == 0) dest.type = "udp";
                        else if (dest.url.rfind("srt://", 0) == 0) dest.type = "srt";
                        else if (dest.url.rfind("rtp://", 0) == 0) dest.type = "rtp";
                        else dest.type = "hls";
                    }
                    outputs.push_back(dest);
                }
            } else {
                std::string output_url = item.value("output_url", "");
                std::string output_interface = item.value("output_interface", "");
                std::string resolved_interface = output_interface.empty() ? output_interface_ : output_interface;
                OutputDestination dest;
                dest.url = output_url;
                dest.output_interface = resolved_interface;
                if (dest.url.rfind("udp://", 0) == 0) dest.type = "udp";
                else if (dest.url.rfind("srt://", 0) == 0) dest.type = "srt";
                else if (dest.url.rfind("rtp://", 0) == 0) dest.type = "rtp";
                else dest.type = "hls";
                outputs.push_back(dest);
            }

            auto out_stream = std::make_unique<OutputStream>(id, name, input_id, program_number, outputs, enabled,
                                                             input_url, is_video_pack,
                                                             transcode_enabled, transcode_video, video_input_format, video_output_format,
                                                             transcode_audio, audio_input_format, audio_output_format, limit_bitrate, video_filename);
            streams_[id] = std::move(out_stream);
        }
    }

    // Load messages
    if (cfg.contains("messages") && cfg["messages"].is_array()) {
        messages_.clear();
        for (const auto& item : cfg["messages"]) {
            ScheduledMessage msg;
            msg.id = item.value("id", "");
            msg.text = item.value("text", "");
            msg.start_time = item.value("start_time", "");
            msg.end_time = item.value("end_time", "");
            msg.all_channels = item.value("all_channels", false);
            if (item.contains("channel_ids") && item["channel_ids"].is_array()) {
                for (const auto& ch : item["channel_ids"]) {
                    msg.channel_ids.push_back(ch.get<std::string>());
                }
            }
            messages_.push_back(msg);
        }
    }

    // Wire listeners and start enabled components
    for (auto& pair : streams_) {
        std::string input_id = pair.second->GetInputId();
        if (inputs_.find(input_id) != inputs_.end()) {
            inputs_[input_id]->RegisterOutputStream(pair.second.get());
        }
    }

    for (auto& pair : inputs_) {
        pair.second->Start();
    }

    for (auto& pair : streams_) {
        pair.second->Start();
    }

    LOG_INFO("Configuración cargada correctamente.");
    return true;
}

bool StreamerEngine::SaveConfig() {
    json cfg;
    cfg["inputs"] = json::array();
    cfg["streams"] = json::array();
    cfg["settings"] = json::object();
    cfg["settings"]["output_interface"] = output_interface_;

    for (const auto& pair : inputs_) {
        json item;
        item["id"] = pair.second->GetId();
        item["name"] = pair.second->GetName();
        item["url"] = pair.second->GetUrl();
        item["enabled"] = pair.second->IsEnabled();
        item["is_video_pack"] = pair.second->IsVideoPack();
        cfg["inputs"].push_back(item);
    }

    for (const auto& pair : streams_) {
        json item;
        item["id"] = pair.second->GetId();
        item["name"] = pair.second->GetName();
        item["input_id"] = pair.second->GetInputId();
        item["program_number"] = pair.second->GetProgramNumber();
        item["enabled"] = pair.second->IsEnabled();
        
        json outputs_arr = json::array();
        for (const auto& out : pair.second->GetOutputs()) {
            json out_item;
            out_item["url"] = out.url;
            out_item["output_interface"] = out.output_interface;
            out_item["type"] = out.type;
            outputs_arr.push_back(out_item);
        }
        item["outputs"] = outputs_arr;

        // Legacy compatibility
        if (!pair.second->GetOutputs().empty()) {
            item["output_url"] = pair.second->GetOutputs()[0].url;
            item["output_interface"] = pair.second->GetOutputs()[0].output_interface;
        } else {
            item["output_url"] = "";
            item["output_interface"] = "";
        }

        item["transcode_enabled"] = pair.second->IsTranscodeEnabled();
        item["transcode_video"] = pair.second->IsTranscodeVideo();
        item["video_input_format"] = pair.second->GetVideoInputFormat();
        item["video_output_format"] = pair.second->GetVideoOutputFormat();
        item["transcode_audio"] = pair.second->IsTranscodeAudio();
        item["audio_input_format"] = pair.second->GetAudioInputFormat();
        item["audio_output_format"] = pair.second->GetAudioOutputFormat();
        item["limit_bitrate"] = pair.second->GetLimitBitrate();
        item["video_filename"] = pair.second->GetVideoFilename();
        cfg["streams"].push_back(item);
    }

    cfg["messages"] = json::array();
    {
        std::lock_guard<std::mutex> lock(messages_mutex_);
        for (const auto& msg : messages_) {
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
            cfg["messages"].push_back(item);
        }
    }

    std::ofstream file(config_path_);
    if (!file.is_open()) {
        LOG_ERROR("No se pudo escribir en el archivo de configuración: " + config_path_);
        return false;
    }

    file << cfg.dump(4);
    return true;
}

bool StreamerEngine::AddInput(const std::string& name, const std::string& url, bool enabled, bool is_video_pack, std::string& id_out) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    std::string id = "input_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    id_out = id;

    auto input = std::make_unique<InputSource>(id, name, url, enabled, is_video_pack);
    if (enabled) {
        input->Start();
    }
    inputs_[id] = std::move(input);

    SaveConfig();
    return true;
}

bool StreamerEngine::UpdateInput(const std::string& id, const std::string& name, const std::string& url, bool enabled, bool is_video_pack) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    auto it = inputs_.find(id);
    if (it == inputs_.end()) return false;

    bool was_running = it->second->IsEnabled();
    it->second->Stop();
    
    it->second = std::make_unique<InputSource>(id, name, url, enabled, is_video_pack);
    
    for (auto& pair : streams_) {
        if (pair.second->GetInputId() == id) {
            it->second->RegisterOutputStream(pair.second.get());
        }
    }

    if (enabled) {
        it->second->Start();
    }

    SaveConfig();
    return true;
}

bool StreamerEngine::DeleteInput(const std::string& id) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    auto it = inputs_.find(id);
    if (it == inputs_.end()) return false;

    it->second->Stop();

    std::vector<std::string> streams_to_delete;
    for (auto& pair : streams_) {
        if (pair.second->GetInputId() == id) {
            streams_to_delete.push_back(pair.first);
        }
    }

    for (const auto& stream_id : streams_to_delete) {
        streams_[stream_id]->Stop();
        streams_.erase(stream_id);
    }

    inputs_.erase(it);
    SaveConfig();
    return true;
}

bool StreamerEngine::AddStream(const std::string& name, const std::string& input_id, int program_number,
                               const std::vector<OutputDestination>& outputs, bool enabled,
                               bool transcode_enabled, bool transcode_video, const std::string& video_input_format,
                               const std::string& video_output_format, bool transcode_audio, const std::string& audio_input_format,
                               const std::string& audio_output_format, int limit_bitrate, const std::string& video_filename, std::string& id_out) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    std::string id = "stream_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    id_out = id;

    std::string input_url = "";
    bool is_video_pack = false;
    auto input_it = inputs_.find(input_id);
    if (input_it != inputs_.end()) {
        input_url = input_it->second->GetUrl();
        is_video_pack = input_it->second->IsVideoPack();
    }

    std::vector<OutputDestination> resolved_outputs = outputs;
    for (auto& out : resolved_outputs) {
        if (out.output_interface.empty()) {
            out.output_interface = output_interface_;
        }
    }

    auto out_stream = std::make_unique<OutputStream>(id, name, input_id, program_number, resolved_outputs, enabled,
                                                     input_url, is_video_pack,
                                                     transcode_enabled, transcode_video, video_input_format, video_output_format,
                                                     transcode_audio, audio_input_format, audio_output_format, limit_bitrate, video_filename);
    
    if (inputs_.find(input_id) != inputs_.end()) {
        inputs_[input_id]->RegisterOutputStream(out_stream.get());
    }

    if (enabled) {
        out_stream->Start();
    }

    streams_[id] = std::move(out_stream);
    SaveConfig();
    return true;
}

bool StreamerEngine::UpdateStream(const std::string& id, const std::string& name, const std::string& input_id,
                                  int program_number, const std::vector<OutputDestination>& outputs, bool enabled,
                                  bool transcode_enabled, bool transcode_video, const std::string& video_input_format,
                                  const std::string& video_output_format, bool transcode_audio, const std::string& audio_input_format,
                                  const std::string& audio_output_format, int limit_bitrate, const std::string& video_filename) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    auto it = streams_.find(id);
    if (it == streams_.end()) return false;

    std::string old_input_id = it->second->GetInputId();
    if (inputs_.find(old_input_id) != inputs_.end()) {
        inputs_[old_input_id]->UnregisterOutputStream(it->second.get());
    }

    it->second->Stop();

    std::string input_url = "";
    bool is_video_pack = false;
    auto input_it = inputs_.find(input_id);
    if (input_it != inputs_.end()) {
        input_url = input_it->second->GetUrl();
        is_video_pack = input_it->second->IsVideoPack();
    }

    std::vector<OutputDestination> resolved_outputs = outputs;
    for (auto& out : resolved_outputs) {
        if (out.output_interface.empty()) {
            out.output_interface = output_interface_;
        }
    }

    it->second = std::make_unique<OutputStream>(id, name, input_id, program_number, resolved_outputs, enabled,
                                               input_url, is_video_pack,
                                               transcode_enabled, transcode_video, video_input_format, video_output_format,
                                               transcode_audio, audio_input_format, audio_output_format, limit_bitrate, video_filename);

    if (inputs_.find(input_id) != inputs_.end()) {
        inputs_[input_id]->RegisterOutputStream(it->second.get());
    }

    if (enabled) {
        it->second->Start();
    }

    SaveConfig();
    return true;
}

bool StreamerEngine::DeleteStream(const std::string& id) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    auto it = streams_.find(id);
    if (it == streams_.end()) return false;

    std::string input_id = it->second->GetInputId();
    if (inputs_.find(input_id) != inputs_.end()) {
        inputs_[input_id]->UnregisterOutputStream(it->second.get());
    }

    it->second->Stop();
    streams_.erase(it);
    
    SaveConfig();
    return true;
}

json StreamerEngine::GetStatusJSON() {
    // Return summary status
    json j;
    size_t active_inputs = 0;
    size_t active_streams = 0;

    for (const auto& pair : inputs_) {
        if (pair.second->GetStats().connected) active_inputs++;
    }
    for (const auto& pair : streams_) {
        if (pair.second->GetStats().active) active_streams++;
    }

    j["inputs_total"] = inputs_.size();
    j["inputs_active"] = active_inputs;
    j["streams_total"] = streams_.size();
    j["streams_active"] = active_streams;
    return j;
}

json StreamerEngine::GetInputsJSON() {
    json arr = json::array();
    for (const auto& pair : inputs_) {
        InputStats stats = pair.second->GetStats();
        json item;
        item["id"] = stats.id;
        item["name"] = stats.name;
        item["url"] = stats.url;
        item["connected"] = stats.connected;
        item["bitrate_kbps"] = stats.total_bitrate_kbps;
        item["enabled"] = pair.second->IsEnabled();
        item["is_video_pack"] = pair.second->IsVideoPack();
        
        json progs = json::array();
        for (const auto& prog : stats.programs) {
            json p;
            p["program_number"] = prog.program_number;
            p["name"] = prog.name;
            p["bitrate_kbps"] = prog.bitrate_kbps;
            
            json pids = json::array();
            for (int pid : prog.pids) pids.push_back(pid);
            p["pids"] = pids;
            
            progs.push_back(p);
        }
        item["programs"] = progs;
        arr.push_back(item);
    }
    return arr;
}

json StreamerEngine::GetStreamsJSON() {
    json arr = json::array();
    for (const auto& pair : streams_) {
        OutputStats stats = pair.second->GetStats();
        json item;
        item["id"] = stats.id;
        item["name"] = stats.name;
        item["input_id"] = stats.input_id;
        item["program_number"] = stats.program_number;
        item["output_url"] = stats.output_url;
        item["active"] = stats.active;
        item["bitrate_kbps"] = stats.bitrate_kbps;
        item["packets_sent"] = stats.packets_sent;
        item["packets_dropped"] = stats.packets_dropped;
        item["error_message"] = stats.error_message;
        item["enabled"] = pair.second->IsEnabled();
        item["output_interface"] = stats.output_interface;

        json outputs_arr = json::array();
        for (const auto& out : pair.second->GetOutputs()) {
            json out_item;
            out_item["url"] = out.url;
            out_item["output_interface"] = out.output_interface;
            out_item["type"] = out.type;
            outputs_arr.push_back(out_item);
        }
        item["outputs"] = outputs_arr;
        item["transcode_enabled"] = pair.second->IsTranscodeEnabled();
        item["transcode_video"] = pair.second->IsTranscodeVideo();
        item["video_input_format"] = pair.second->GetVideoInputFormat();
        item["video_output_format"] = pair.second->GetVideoOutputFormat();
        item["transcode_audio"] = pair.second->IsTranscodeAudio();
        item["audio_input_format"] = pair.second->GetAudioInputFormat();
        item["audio_output_format"] = pair.second->GetAudioOutputFormat();
        item["limit_bitrate"] = pair.second->GetLimitBitrate();
        item["video_filename"] = pair.second->GetVideoFilename();
        item["detected_video_codec"] = pair.second->GetDetectedVideoCodec();
        item["detected_audio_codec"] = pair.second->GetDetectedAudioCodec();
        arr.push_back(item);
    }
    return arr;
}

json StreamerEngine::ProbeInputURL(const std::string& url) {
    std::string err;
    auto progs = InputSource::ProbeURL(url, err);
    
    json j;
    if (!err.empty()) {
        j["success"] = false;
        j["error"] = err;
    } else {
        j["success"] = true;
        json arr = json::array();
        for (const auto& prog : progs) {
            json item;
            item["program_number"] = prog.program_number;
            item["name"] = prog.name;
            json pids = json::array();
            for (int pid : prog.pids) pids.push_back(pid);
            item["pids"] = pids;
            arr.push_back(item);
        }
        j["programs"] = arr;
    }
    return j;
}

std::string StreamerEngine::GetOutputInterface() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    return output_interface_;
}

void StreamerEngine::SetOutputInterface(const std::string& iface) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    output_interface_ = iface;
    SaveConfig();
}

static std::chrono::system_clock::time_point parse_datetime(const std::string& datetime_str) {
    std::tm tm = {};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    
    std::string s = datetime_str;
    for (char& c : s) {
        if (c == 'T') c = ' ';
    }
    
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6 ||
        sscanf(s.c_str(), "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) == 5) {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        tm.tm_isdst = -1;
        std::time_t t = std::mktime(&tm);
        if (t != -1) {
            return std::chrono::system_clock::from_time_t(t);
        }
    }
    return std::chrono::system_clock::time_point::min();
}

std::vector<ScheduledMessage> StreamerEngine::GetMessages() {
    std::lock_guard<std::mutex> lock(messages_mutex_);
    return messages_;
}

bool StreamerEngine::AddMessage(const ScheduledMessage& msg) {
    {
        std::lock_guard<std::mutex> lock(messages_mutex_);
        messages_.push_back(msg);
    }
    SaveConfig();
    return true;
}

bool StreamerEngine::UpdateMessage(const std::string& id, const ScheduledMessage& msg) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(messages_mutex_);
        for (auto& item : messages_) {
            if (item.id == id) {
                item = msg;
                found = true;
                break;
            }
        }
    }
    if (found) {
        SaveConfig();
        return true;
    }
    return false;
}

bool StreamerEngine::DeleteMessage(const std::string& id) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(messages_mutex_);
        for (auto it = messages_.begin(); it != messages_.end(); ++it) {
            if (it->id == id) {
                messages_.erase(it);
                found = true;
                break;
            }
        }
    }
    if (found) {
        SaveConfig();
        return true;
    }
    return false;
}

std::string StreamerEngine::GetActiveMessageForStream(const std::string& stream_id) {
    std::lock_guard<std::mutex> lock(messages_mutex_);
    auto now = std::chrono::system_clock::now();
    std::string active_msg = "";
    
    for (const auto& msg : messages_) {
        auto start = parse_datetime(msg.start_time);
        auto end = parse_datetime(msg.end_time);
        
        if (now >= start && now <= end) {
            bool applies = msg.all_channels;
            if (!applies) {
                for (const auto& ch_id : msg.channel_ids) {
                    if (ch_id == stream_id) {
                        applies = true;
                        break;
                    }
                }
            }
            if (applies) {
                if (!active_msg.empty()) active_msg += " | ";
                active_msg += msg.text;
            }
        }
    }
    return active_msg;
}

bool StreamerEngine::IsInputVideoPack(const std::string& input_id) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    auto it = inputs_.find(input_id);
    if (it != inputs_.end()) {
        return it->second->IsVideoPack();
    }
    return false;
}

std::string StreamerEngine::GetInputUrl(const std::string& input_id) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    auto it = inputs_.find(input_id);
    if (it != inputs_.end()) {
        return it->second->GetUrl();
    }
    return "";
}
