#pragma once

#include "../player/video_player.h"
#include "app.h"
#include "tip_label.h"

#include <atomic>
#include <mutex>
#include <queue>

class SignalBar;
class SignalMetricsOverlay;
class GstDecoder;

class PlayerRect final : public revector::TextureRect {
public:
    std::shared_ptr<VideoPlayer> player_;

    // Preserved for reuse when restarting decoder.
    std::string play_url_;
    std::mutex play_mutex_;

    bool playing_ = false;

    bool force_software_decoding = false;

    std::shared_ptr<revector::VectorImage> logo_;
    std::shared_ptr<revector::RenderImage> render_image_;

    std::shared_ptr<TipLabel> tip_label_;

    bool is_recording = false;

    std::chrono::time_point<std::chrono::steady_clock> record_start_time;

    std::shared_ptr<revector::Timer> rx_status_update_timer;

    std::shared_ptr<revector::CollapseContainer> collapse_panel_;

    std::shared_ptr<revector::VBoxContainer> hud_container_;

    std::shared_ptr<revector::Label> timestamp_overlay_label_;

    std::shared_ptr<revector::Label> clock_label_;

    std::shared_ptr<SignalMetricsOverlay> signal_metrics_overlay_;

    std::shared_ptr<revector::Label> record_status_label_;

    std::shared_ptr<revector::Label> bitrate_label_;

    std::shared_ptr<revector::Label> decoder_label_;

    std::shared_ptr<revector::Label> frame_ts_label_;

    std::shared_ptr<revector::Label> render_ts_label_;

    std::shared_ptr<revector::Label> rtp_loss_label_;

    std::shared_ptr<revector::Label> rtp_ts_label_;

    std::shared_ptr<revector::Label> pl_label_;

    std::shared_ptr<revector::Label> fec_label_;

    std::vector<std::shared_ptr<SignalBar>> link_score_bars_;

    std::vector<std::shared_ptr<SignalBar>> rssi_bars_;

    std::vector<std::shared_ptr<SignalBar>> snr_bars_;

    std::shared_ptr<revector::Label> video_info_label_;

    std::shared_ptr<revector::Label> render_fps_label_;

    std::shared_ptr<revector::Label> video_fps_label_;

    std::shared_ptr<revector::Button> video_stabilization_button_;
    std::shared_ptr<revector::Button> low_light_enhancement_button_;

    std::shared_ptr<revector::Button> control_panel_button_;

    std::shared_ptr<revector::Button> record_button_;

    // Record when the signal had been lost.
    std::chrono::time_point<std::chrono::steady_clock> signal_lost_time_;

    std::atomic<uint64_t> last_decoded_frame_timestamp_ms_ = 0;
    std::atomic<uint64_t> last_rtp_timestamp_ms_ = 0;
    std::atomic<uint64_t> decode_latency_total_ms_ = 0;
    std::atomic<uint64_t> decode_latency_count_ = 0;
    std::atomic<uint64_t> decoded_frame_count_ = 0;
    std::mutex received_frame_timestamps_mutex_;
    std::queue<uint64_t> received_frame_timestamps_ms_;
    uint64_t last_video_fps_frame_count_ = 0;
    std::chrono::steady_clock::time_point last_video_fps_update_time_{};
    double display_refresh_rate_hz_ = 60.0;
    std::chrono::steady_clock::time_point next_clock_update_time_{};

    void show_red_tip(std::string tip);

    void show_green_tip(std::string tip);

    void custom_input(revector::InputEvent &event) override;

    void custom_ready() override;

    void custom_update(double dt) override;

    void custom_draw() override;

    void start_playing(const std::string &url);

    void stop_playing();
};
