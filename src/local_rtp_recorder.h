#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LocalRtpRecorder {
public:
    ~LocalRtpRecorder();

    bool start(int listen_port, int forward_port, const std::string &codec);
    void stop();

private:
    void run(int listen_port, int forward_port, std::string codec);
    void process_rtp_packet(const uint8_t *packet, int packet_size);
    void process_h264_payload(const uint8_t *payload, size_t payload_size);
    void process_h265_payload(const uint8_t *payload, size_t payload_size);
    void flush_frame(uint32_t rtp_timestamp, uint16_t last_seq, uint64_t received_ms);

    std::atomic<bool> running_ = false;
    std::atomic<bool> started_ = false;
    std::atomic<uintptr_t> recv_socket_ = 0;
    std::thread thread_;
    std::mutex start_mutex_;
    std::condition_variable start_cv_;
    bool start_finished_ = false;
    std::string output_dir_;
    std::string codec_;
    std::string stream_file_name_;
    std::ofstream tsv_;
    std::ofstream stream_;
    std::vector<uint8_t> frame_data_;
    std::vector<uint8_t> fragmented_nal_;
    bool fragmented_nal_active_ = false;
    uint64_t frame_index_ = 0;
    uint64_t output_offset_ = 0;
    uint32_t frame_packet_count_ = 0;
};
