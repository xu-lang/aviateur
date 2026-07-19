#include "local_rtp_recorder.h"

#include "gui_interface.h"

#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <mutex>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
using NativeSocket = SOCKET;
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <unistd.h>
using NativeSocket = int;
#endif

namespace {

constexpr uint8_t StartCode[] = {0x00, 0x00, 0x00, 0x01};

uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void CloseSocket(NativeSocket sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

bool IsInvalidSocket(NativeSocket sock) {
#ifdef _WIN32
    return sock == INVALID_SOCKET;
#else
    return sock < 0;
#endif
}

bool EnsureWinsockStarted() {
#ifdef _WIN32
    static std::once_flag once;
    static bool started = false;
    std::call_once(once, [] {
        WSADATA wsa_data;
        started = WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    });
    return started;
#else
    return true;
#endif
}

void AppendStartCode(std::vector<uint8_t> &data) {
    data.insert(data.end(), std::begin(StartCode), std::end(StartCode));
}

void AppendBytes(std::vector<uint8_t> &data, const uint8_t *bytes, size_t size) {
    data.insert(data.end(), bytes, bytes + size);
}

size_t RtpPayloadOffset(const uint8_t *packet, int packet_size) {
    if (packet_size < 12 || (packet[0] >> 6) != 2) {
        return 0;
    }

    const uint8_t csrc_count = packet[0] & 0x0F;
    size_t offset = 12 + csrc_count * 4;
    if (offset > static_cast<size_t>(packet_size)) {
        return 0;
    }

    const bool has_extension = (packet[0] & 0x10) != 0;
    if (has_extension) {
        if (offset + 4 > static_cast<size_t>(packet_size)) {
            return 0;
        }
        const uint16_t extension_words = (static_cast<uint16_t>(packet[offset + 2]) << 8) | packet[offset + 3];
        offset += 4 + extension_words * 4;
        if (offset > static_cast<size_t>(packet_size)) {
            return 0;
        }
    }

    return offset;
}

} // namespace

LocalRtpRecorder::~LocalRtpRecorder() {
    stop();
}

bool LocalRtpRecorder::start(int listen_port, int forward_port, const std::string &codec) {
    stop();
    running_.store(true, std::memory_order_relaxed);
    started_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(start_mutex_);
        start_finished_ = false;
    }
    thread_ = std::thread([this, listen_port, forward_port, codec] { run(listen_port, forward_port, codec); });

    {
        std::unique_lock lock(start_mutex_);
        start_cv_.wait(lock, [this] { return start_finished_; });
    }

    if (!started_.load(std::memory_order_relaxed) && thread_.joinable()) {
        thread_.join();
        return false;
    }

    return true;
}

void LocalRtpRecorder::stop() {
    running_.store(false, std::memory_order_relaxed);
    const auto recv_socket = recv_socket_.exchange(0, std::memory_order_relaxed);
    if (recv_socket != 0) {
#ifdef _WIN32
        shutdown(static_cast<NativeSocket>(recv_socket), SD_BOTH);
#endif
        CloseSocket(static_cast<NativeSocket>(recv_socket));
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (!frame_data_.empty()) {
        flush_frame(0, 0, NowMs());
    }
    if (tsv_.is_open()) {
        tsv_.close();
    }
    if (stream_.is_open()) {
        stream_.close();
    }
}

void LocalRtpRecorder::run(int listen_port, int forward_port, std::string codec) {
    if (!EnsureWinsockStarted()) {
        GuiInterface::Instance().PutLog(LogLevel::Error, "Raw RTP recorder WSAStartup failed");
        running_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard lock(start_mutex_);
            start_finished_ = true;
        }
        start_cv_.notify_one();
        return;
    }

    const auto timestamp = NowMs();
    output_dir_ = std::filesystem::current_path().string() + "/";
    codec_ = std::move(codec);
    stream_file_name_ = codec_ == "H265" ? "frames.h265" : "frames.h264";
    std::filesystem::create_directories(output_dir_);
    tsv_.open(output_dir_ + "frames.tsv", std::ios::out | std::ios::trunc);
    stream_.open(output_dir_ + stream_file_name_, std::ios::binary | std::ios::trunc);
    output_offset_ = 0;
    tsv_ << "frame_index\treceived_ms\trtp_timestamp\tlast_seq\tpacket_count\tled_on\toffset\tbytes\n";

    const bool forwarding_enabled = forward_port > 0;
    const NativeSocket recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    const NativeSocket send_sock = forwarding_enabled ? socket(AF_INET, SOCK_DGRAM, 0) : NativeSocket{};
    if (IsInvalidSocket(recv_sock) || (forwarding_enabled && IsInvalidSocket(send_sock))) {
        GuiInterface::Instance().PutLog(LogLevel::Error, "Raw RTP recorder socket creation failed");
        if (!IsInvalidSocket(recv_sock)) {
            CloseSocket(recv_sock);
        }
        if (forwarding_enabled && !IsInvalidSocket(send_sock)) {
            CloseSocket(send_sock);
        }
        running_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard lock(start_mutex_);
            start_finished_ = true;
        }
        start_cv_.notify_one();
        return;
    }

    int reuse = 1;
    setsockopt(recv_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#ifdef _WIN32
    DWORD timeout_ms = 500;
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#endif

    sockaddr_in listen_addr{};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(listen_port);
    if (bind(recv_sock, reinterpret_cast<sockaddr *>(&listen_addr), sizeof(listen_addr)) < 0) {
        GuiInterface::Instance().PutLog(LogLevel::Error, "Raw RTP recorder bind failed on UDP port {}", listen_port);
        CloseSocket(recv_sock);
        CloseSocket(send_sock);
        running_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard lock(start_mutex_);
            start_finished_ = true;
        }
        start_cv_.notify_one();
        return;
    }

    recv_socket_.store(static_cast<uintptr_t>(recv_sock), std::memory_order_relaxed);
    started_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard lock(start_mutex_);
        start_finished_ = true;
    }
    start_cv_.notify_one();

    sockaddr_in forward_addr{};
    if (forwarding_enabled) {
        forward_addr.sin_family = AF_INET;
        forward_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        forward_addr.sin_port = htons(forward_port);

        GuiInterface::Instance().PutLog(LogLevel::Info,
                                        "Raw RTP recorder listening on {}, forwarding to {}",
                                        listen_port,
                                        forward_port);
    } else {
        GuiInterface::Instance().PutLog(LogLevel::Info, "Raw RTP recorder listening on {} without forwarding", listen_port);
    }

    while (running_.load(std::memory_order_relaxed)) {
        uint8_t buffer[65536]{};
        const int received = recvfrom(recv_sock, reinterpret_cast<char *>(buffer), sizeof(buffer), 0, nullptr, nullptr);
        if (received <= 0) {
            continue;
        }

        if (forwarding_enabled) {
            sendto(send_sock,
                   reinterpret_cast<const char *>(buffer),
                   received,
                   0,
                   reinterpret_cast<sockaddr *>(&forward_addr),
                   sizeof(forward_addr));
        }

        if (received < 12) {
            continue;
        }

        const bool marker = (buffer[1] & 0x80) != 0;
        const uint16_t seq = (static_cast<uint16_t>(buffer[2]) << 8) | buffer[3];
        const uint32_t rtp_timestamp = (static_cast<uint32_t>(buffer[4]) << 24) |
                                       (static_cast<uint32_t>(buffer[5]) << 16) |
                                       (static_cast<uint32_t>(buffer[6]) << 8) | buffer[7];
        process_rtp_packet(buffer, received);
        frame_packet_count_++;
        if (marker) {
            flush_frame(rtp_timestamp, seq, NowMs());
        }
    }

    const auto still_owned_recv_socket = recv_socket_.exchange(0, std::memory_order_relaxed);
    if (still_owned_recv_socket != 0) {
        CloseSocket(static_cast<NativeSocket>(still_owned_recv_socket));
    }
    if (forwarding_enabled) {
        CloseSocket(send_sock);
    }
}

void LocalRtpRecorder::process_rtp_packet(const uint8_t *packet, int packet_size) {
    size_t payload_offset = RtpPayloadOffset(packet, packet_size);
    if (payload_offset == 0 || payload_offset >= static_cast<size_t>(packet_size)) {
        return;
    }

    size_t payload_size = static_cast<size_t>(packet_size) - payload_offset;
    if ((packet[0] & 0x20) != 0 && payload_size > 0) {
        const uint8_t padding = packet[packet_size - 1];
        if (padding <= payload_size) {
            payload_size -= padding;
        }
    }

    const uint8_t *payload = packet + payload_offset;
    if (codec_ == "H265") {
        process_h265_payload(payload, payload_size);
    } else {
        process_h264_payload(payload, payload_size);
    }
}

void LocalRtpRecorder::process_h264_payload(const uint8_t *payload, size_t payload_size) {
    if (payload_size == 0) {
        return;
    }

    const uint8_t nal_type = payload[0] & 0x1F;
    if (nal_type >= 1 && nal_type <= 23) {
        AppendStartCode(frame_data_);
        AppendBytes(frame_data_, payload, payload_size);
        return;
    }

    if (nal_type == 24) {
        size_t offset = 1;
        while (offset + 2 <= payload_size) {
            const uint16_t nal_size = (static_cast<uint16_t>(payload[offset]) << 8) | payload[offset + 1];
            offset += 2;
            if (offset + nal_size > payload_size) {
                break;
            }
            AppendStartCode(frame_data_);
            AppendBytes(frame_data_, payload + offset, nal_size);
            offset += nal_size;
        }
        return;
    }

    if (nal_type == 28 && payload_size >= 2) {
        const uint8_t fu_indicator = payload[0];
        const uint8_t fu_header = payload[1];
        const bool start = (fu_header & 0x80) != 0;
        const bool end = (fu_header & 0x40) != 0;
        const uint8_t reconstructed_header = (fu_indicator & 0xE0) | (fu_header & 0x1F);

        if (start) {
            fragmented_nal_.clear();
            AppendStartCode(fragmented_nal_);
            fragmented_nal_.push_back(reconstructed_header);
            AppendBytes(fragmented_nal_, payload + 2, payload_size - 2);
            fragmented_nal_active_ = true;
        } else if (fragmented_nal_active_) {
            AppendBytes(fragmented_nal_, payload + 2, payload_size - 2);
        }

        if (end && fragmented_nal_active_) {
            AppendBytes(frame_data_, fragmented_nal_.data(), fragmented_nal_.size());
            fragmented_nal_.clear();
            fragmented_nal_active_ = false;
        }
    }
}

void LocalRtpRecorder::process_h265_payload(const uint8_t *payload, size_t payload_size) {
    if (payload_size < 2) {
        return;
    }

    const uint8_t nal_type = (payload[0] >> 1) & 0x3F;
    if (nal_type < 48) {
        AppendStartCode(frame_data_);
        AppendBytes(frame_data_, payload, payload_size);
        return;
    }

    if (nal_type == 48) {
        size_t offset = 2;
        while (offset + 2 <= payload_size) {
            const uint16_t nal_size = (static_cast<uint16_t>(payload[offset]) << 8) | payload[offset + 1];
            offset += 2;
            if (offset + nal_size > payload_size) {
                break;
            }
            AppendStartCode(frame_data_);
            AppendBytes(frame_data_, payload + offset, nal_size);
            offset += nal_size;
        }
        return;
    }

    if (nal_type == 49 && payload_size >= 3) {
        const uint8_t fu_header = payload[2];
        const bool start = (fu_header & 0x80) != 0;
        const bool end = (fu_header & 0x40) != 0;
        const uint8_t original_type = fu_header & 0x3F;
        const uint8_t reconstructed_header[2] = {
            static_cast<uint8_t>((payload[0] & 0x81) | (original_type << 1)),
            payload[1],
        };

        if (start) {
            fragmented_nal_.clear();
            AppendStartCode(fragmented_nal_);
            AppendBytes(fragmented_nal_, reconstructed_header, sizeof(reconstructed_header));
            AppendBytes(fragmented_nal_, payload + 3, payload_size - 3);
            fragmented_nal_active_ = true;
        } else if (fragmented_nal_active_) {
            AppendBytes(fragmented_nal_, payload + 3, payload_size - 3);
        }

        if (end && fragmented_nal_active_) {
            AppendBytes(frame_data_, fragmented_nal_.data(), fragmented_nal_.size());
            fragmented_nal_.clear();
            fragmented_nal_active_ = false;
        }
    }
}

void LocalRtpRecorder::flush_frame(uint32_t rtp_timestamp, uint16_t last_seq, uint64_t received_ms) {
    if (frame_data_.empty()) {
        return;
    }

    const auto frame_offset = output_offset_;
    stream_.write(reinterpret_cast<const char *>(frame_data_.data()), frame_data_.size());
    stream_.flush();
    output_offset_ += frame_data_.size();

    const int led_on = GuiInterface::Instance().led_on_.load(std::memory_order_relaxed) ? 1 : 0;
    const uint64_t frame_index = GuiInterface::Instance().local_rtp_frame_index_source_ ==
            LocalRtpFrameIndexSource::DecodedFrame
        ? GuiInterface::Instance().decodedFrameCount_.load(std::memory_order_relaxed)
        : frame_index_;
    tsv_ << frame_index << '\t' << received_ms << '\t' << rtp_timestamp << '\t' << last_seq << '\t'
         << frame_packet_count_ << '\t' << led_on << '\t' << frame_offset << '\t' << frame_data_.size() << '\n';
    tsv_.flush();

    frame_index_++;
    frame_packet_count_ = 0;
    frame_data_.clear();
}
