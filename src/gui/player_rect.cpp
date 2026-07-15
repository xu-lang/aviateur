#include "player_rect.h"

#include "../gui_interface.h"
#include "src/player/ffmpeg/video_player.h"
#include <fmt/format.h>
#ifdef AVIATEUR_USE_GSTREAMER
    #include "src/player/gst/video_player.h"
#endif

constexpr uint32_t HUD_LABEL_FONT_SIZE = 20;

std::shared_ptr<revector::Label> MakeBarRowLabel(const std::string &text) {
    auto label = std::make_shared<revector::Label>();
    label->set_text(text);
    label->set_font_size(12);
    label->set_custom_minimum_size({44, 0});
    label->theme_override_text_style = revector::TextStyle{revector::ColorU::white()};
    label->set_horizontal_alignment(revector::Alignment::Center);
    return label;
}

class SignalBar : public revector::ProgressBar {
    void custom_ready() override {
        theme_fg = {};
        theme_bg = {};
        theme_progress->border_width = 0;
        theme_progress->corner_radius = 0;

        set_lerp_enabled(true);
        set_label_visibility(false);
    }

    void custom_update(double dt) override {
        if (value < 0.25 * max_value) {
            theme_progress->bg_color = RED;
        }
        if (value >= 0.25 * max_value && value < 0.5 * max_value) {
            theme_progress->bg_color = YELLOW;
        }
        if (value >= 0.5 * max_value) {
            theme_progress->bg_color = GREEN;
        }
    }
};

class SignalMetricsOverlay : public revector::NodeUi {
public:
    SignalMetricsOverlay() {
        set_custom_minimum_size({0, 42});

        for (const auto &text : {"LQ", "RSSI", "SNR"}) {
            auto label = MakeBarRowLabel(text);
            add_child(label);
            labels_.push_back(label);
        }
    }

    void set_metrics(const std::array<int, ANTENNA_COUNT> &lq,
                     const std::array<int, ANTENNA_COUNT> &rssi,
                     const std::array<int, ANTENNA_COUNT> &snr) {
        values_[0] = {Scale(lq[0], 1000, 2000), Scale(lq[1], 1000, 2000)};
        values_[1] = {Scale(rssi[0], 0, 120), Scale(rssi[1], 0, 120)};
        values_[2] = {Scale(snr[0], 0, 60), Scale(snr[1], 0, 60)};
        set_visibility(true);
    }

    void clear_metrics() {
        values_ = {};
    }

    void custom_update(double dt) override {
        const float label_width = 44.0f;
        const float center_x = get_size().x * 0.5f;
        for (int row = 0; row < labels_.size(); ++row) {
            labels_[row]->set_position({center_x - label_width * 0.5f, RowY(row) - 7.0f});
            labels_[row]->set_size({label_width, 16});
        }
    }

    void draw() override {
        if (!get_visibility()) {
            return;
        }

        revector::NodeUi::draw();

        auto vector_server = revector::VectorServer::get_singleton();
        const auto origin = get_global_position();
        const float center_x = get_size().x * 0.5f;
        const float label_width = 44.0f;
        const float gap = 4.0f;
        const float available_side_width = (get_size().x - label_width) * 0.5f - gap;
        const float max_side_width = available_side_width > 0.0f ? available_side_width : 0.0f;

        for (int row = 0; row < 3; ++row) {
            const float y = RowY(row);
            DrawBar(vector_server, origin, center_x - label_width * 0.5f - gap, y, max_side_width, values_[row][0], true);
            DrawBar(vector_server, origin, center_x + label_width * 0.5f + gap, y, max_side_width, values_[row][1], false);
        }
    }

private:
    static float Scale(int value, int min, int max) {
        return static_cast<float>(std::clamp(value, min, max) - min) / static_cast<float>(max - min);
    }

    static float RowY(int row) {
        return 6.0f + row * 13.0f;
    }

    static revector::ColorU BarColor(float value) {
        if (value < 0.25f) {
            return RED;
        }
        if (value < 0.5f) {
            return YELLOW;
        }
        return GREEN;
    }

    static void DrawBar(revector::VectorServer *vector_server,
                        Pathfinder::Vec2F origin,
                        float edge_x,
                        float y,
                        float max_width,
                        float value,
                        bool grow_left) {
        if (max_width <= 0) {
            return;
        }
        revector::StyleBox style;
        style.bg_color = BarColor(value);
        style.border_width = 0;
        style.corner_radius = 0;
        const float width = max_width * value;
        const float x = grow_left ? edge_x - width : edge_x;
        vector_server->draw_style_box(style, origin + Pathfinder::Vec2F{x, y}, {width, 4.0f});
    }

    std::vector<std::shared_ptr<revector::Label>> labels_;
    std::array<std::array<float, ANTENNA_COUNT>, 3> values_{};
};

void PlayerRect::show_red_tip(std::string tip) {
    tip_label_->theme_override_bg.value().bg_color = RED;
    tip_label_->show_tip(tip);
}

void PlayerRect::show_green_tip(std::string tip) {
    tip_label_->theme_override_bg.value().bg_color = GREEN;
    tip_label_->show_tip(tip);
}

void PlayerRect::custom_input(revector::InputEvent &event) {
    if (event.type == revector::InputEventType::Key) {
        auto key_args = event.args.key;

        // if (key_args.key == revector::KeyCode::F11) {
        //     if (key_args.pressed) {
        //         fullscreen_button_->set_toggled(!fullscreen_button_->get_toggled());
        //     }
        // }

        if (playing_ && key_args.key == revector::KeyCode::F10) {
            if (key_args.pressed) {
                record_button_->trigger();
            }
        }
    }
}

void PlayerRect::custom_ready() {
    auto onRtpStream = [this](std::string sdp_file) {
        play_mutex_.lock();
        play_url_ = sdp_file;
        play_mutex_.unlock();
    };
    GuiInterface::Instance().rtpStreamCallbacks.emplace_back(onRtpStream);

    collapse_panel_ = std::make_shared<revector::CollapseContainer>(revector::CollapseButtonType::Default);
    collapse_panel_->set_title(FTR("player control"));
    collapse_panel_->set_collapse(true);
    collapse_panel_->set_color(revector::ColorU(106, 171, 114));
    collapse_panel_->set_visibility(false);
    collapse_panel_->set_anchor_flag(revector::AnchorFlag::TopRight);
    add_child(collapse_panel_);

    auto vbox = std::make_shared<revector::VBoxContainer>();
    collapse_panel_->add_child(vbox);

    logo_ = std::make_shared<revector::VectorImage>(revector::get_asset_dir("openipc-logo-white.svg"));
    texture = logo_;

    render_image_ = std::make_shared<revector::RenderImage>(Pathfinder::Vec2I{1920, 1080});

    set_stretch_mode(StretchMode::KeepAspectCentered);

    tip_label_ = std::make_shared<TipLabel>();
    tip_label_->set_anchor_flag(revector::AnchorFlag::VCenterWide);
    tip_label_->set_visibility(false);
    tip_label_->set_word_wrap(true);
    tip_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    add_child(tip_label_);

    hud_container_ = std::make_shared<revector::VBoxContainer>();
    add_child(hud_container_);
    hud_container_->set_anchor_flag(revector::AnchorFlag::BottomWide);
    hud_container_->set_visibility(false);
    hud_container_->set_separation(2);

    timestamp_overlay_label_ = std::make_shared<revector::Label>();
    add_child(timestamp_overlay_label_);
    timestamp_overlay_label_->set_anchor_flag(revector::AnchorFlag::TopLeft);
    timestamp_overlay_label_->set_custom_minimum_size({520, 28});
    timestamp_overlay_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    timestamp_overlay_label_->set_text("IN: ----ms F: ----ms R: ----ms");
    timestamp_overlay_label_->set_visibility(false);

    signal_metrics_overlay_ = std::make_shared<SignalMetricsOverlay>();
    add_child(signal_metrics_overlay_);
    signal_metrics_overlay_->set_visibility(false);

    {
        auto lq_row = std::make_shared<revector::HBoxContainer>();
        hud_container_->add_child(lq_row);
        lq_row->set_visibility(false);
        lq_row->set_separation(4);
        lq_row->set_anchor_flag(revector::AnchorFlag::BottomWide);
        auto lq_left_container = std::make_shared<revector::GridContainer>();
        lq_row->add_child(lq_left_container);
        lq_left_container->set_separation(2);
        lq_left_container->set_column_limit(1);
        lq_left_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;

        lq_row->add_child(MakeBarRowLabel("LQ"));

        auto lq_right_container = std::make_shared<revector::GridContainer>();
        lq_row->add_child(lq_right_container);
        lq_right_container->set_separation(2);
        lq_right_container->set_column_limit(1);
        lq_right_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < ANTENNA_COUNT; j++) {
                auto bar = std::make_shared<SignalBar>();
                if (j == 0) {
                    bar->set_fill_mode(revector::ProgressBar::FillMode::RightToLeft);
                } else {
                    bar->set_fill_mode(revector::ProgressBar::FillMode::LeftToRight);
                }

                link_score_bars_.push_back(bar);
            }
        }

        for (int i = 0; i < link_score_bars_.size(); ++i) {
            const auto &bar = link_score_bars_[i];
            if (i % 2 == 0) {
                lq_left_container->add_child(bar);
            } else {
                lq_right_container->add_child(bar);
            }
            bar->set_value(0);
            bar->set_min_value(1000);
            bar->set_max_value(2000);
            bar->set_custom_minimum_size({0, 4});
            bar->set_size({0, 4});
            bar->set_visibility(false);
        }
    }

    {
        auto radio_container = std::make_shared<revector::VBoxContainer>();
        hud_container_->add_child(radio_container);
        radio_container->set_visibility(false);
        radio_container->set_separation(2);
        radio_container->set_anchor_flag(revector::AnchorFlag::BottomWide);

        auto rssi_row = std::make_shared<revector::HBoxContainer>();
        radio_container->add_child(rssi_row);
        rssi_row->set_separation(4);
        auto rssi_container = std::make_shared<revector::GridContainer>();
        rssi_row->add_child(rssi_container);
        rssi_container->set_separation(2);
        rssi_container->set_column_limit(1);
        rssi_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;

        rssi_row->add_child(MakeBarRowLabel("RSSI"));

        auto rssi_right_container = std::make_shared<revector::GridContainer>();
        rssi_row->add_child(rssi_right_container);
        rssi_right_container->set_separation(2);
        rssi_right_container->set_column_limit(1);
        rssi_right_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;

        for (int i = 0; i < ANTENNA_COUNT; i++) {
            auto bar = std::make_shared<SignalBar>();
            if (i == 0) {
                bar->set_fill_mode(revector::ProgressBar::FillMode::RightToLeft);
            } else {
                bar->set_fill_mode(revector::ProgressBar::FillMode::LeftToRight);
            }
            if (i == 0) {
                rssi_container->add_child(bar);
            } else {
                rssi_right_container->add_child(bar);
            }
            bar->set_value(0);
            bar->set_min_value(0);
            bar->set_max_value(120);
            bar->set_custom_minimum_size({0, 4});
            bar->set_size({0, 4});
            bar->set_visibility(false);
            rssi_bars_.push_back(bar);
        }

        auto snr_row = std::make_shared<revector::HBoxContainer>();
        radio_container->add_child(snr_row);
        snr_row->set_separation(4);
        auto snr_container = std::make_shared<revector::GridContainer>();
        snr_row->add_child(snr_container);
        snr_container->set_separation(2);
        snr_container->set_column_limit(1);
        snr_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;

        snr_row->add_child(MakeBarRowLabel("SNR"));

        auto snr_right_container = std::make_shared<revector::GridContainer>();
        snr_row->add_child(snr_right_container);
        snr_right_container->set_separation(2);
        snr_right_container->set_column_limit(1);
        snr_right_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;

        for (int i = 0; i < ANTENNA_COUNT; i++) {
            auto bar = std::make_shared<SignalBar>();
            if (i == 0) {
                bar->set_fill_mode(revector::ProgressBar::FillMode::RightToLeft);
            } else {
                bar->set_fill_mode(revector::ProgressBar::FillMode::LeftToRight);
            }
            if (i == 0) {
                snr_container->add_child(bar);
            } else {
                snr_right_container->add_child(bar);
            }
            bar->set_value(0);
            bar->set_min_value(0);
            bar->set_max_value(60);
            bar->set_custom_minimum_size({0, 4});
            bar->set_size({0, 4});
            bar->set_visibility(false);
            snr_bars_.push_back(bar);
        }
    }

    auto label_container_ = std::make_shared<revector::HBoxContainer>();
    hud_container_->add_child(label_container_);
    revector::StyleBox box;
    box.bg_color =
        GuiInterface::Instance().dark_mode_ ? revector::ColorU(27, 27, 27, 100) : revector::ColorU(228, 228, 228, 100);
    box.border_width = 0;
    box.corner_radius = 0;
    label_container_->theme_override_bg = box;
    label_container_->set_separation(16);

    rtp_ts_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(rtp_ts_label_);
    rtp_ts_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    rtp_ts_label_->set_visibility(false);

    auto on_rtp_timestamp = [this](uint64_t timestamp_ms) {
        last_rtp_timestamp_ms_.store(timestamp_ms, std::memory_order_relaxed);
    };
    GuiInterface::Instance().rtpTimestampCallbacks.emplace_back(on_rtp_timestamp);

    frame_ts_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(frame_ts_label_);
    frame_ts_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    frame_ts_label_->set_visibility(false);

    auto on_frame_decoded = [this](uint64_t timestamp_ms) {
        last_decoded_frame_timestamp_ms_.store(timestamp_ms, std::memory_order_relaxed);
        decoded_frame_count_.fetch_add(1, std::memory_order_relaxed);
    };
    GuiInterface::Instance().videoFrameDecodedCallbacks.emplace_back(on_frame_decoded);

    render_ts_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(render_ts_label_);
    render_ts_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    render_ts_label_->set_visibility(false);

    rtp_loss_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(rtp_loss_label_);
    rtp_loss_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    rtp_loss_label_->set_text("LOSS: 0 LPS: 0.0");
    rtp_loss_label_->set_visibility(false);

    {
        video_info_label_ = std::make_shared<revector::Label>();
        label_container_->add_child(video_info_label_);
        video_info_label_->set_text("");
        video_info_label_->set_font_size(HUD_LABEL_FONT_SIZE);
        video_info_label_->set_visibility(false);

        auto on_decoder_ready = [this](uint32_t width, uint32_t height, float fps, std::string decoder_name) {
            std::stringstream ss;
            ss << width << "x" << height;
            video_info_label_->set_text(ss.str());
            video_info_label_->set_visibility(true);

            decoder_label_->set_text("DEC: " + decoder_name);
            decoder_label_->set_font_size(HUD_LABEL_FONT_SIZE);
            decoder_label_->set_visibility(true);
        };
        GuiInterface::Instance().decoderReadyCallbacks.emplace_back(on_decoder_ready);
    }

    bitrate_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(bitrate_label_);
    bitrate_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    bitrate_label_->set_visibility(false);

    render_fps_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(render_fps_label_);
    render_fps_label_->set_font_size(HUD_LABEL_FONT_SIZE);

    video_fps_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(video_fps_label_);
    video_fps_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    video_fps_label_->set_text("VFPS: --");

    decoder_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(decoder_label_);
    decoder_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    decoder_label_->set_visibility(false);

#ifndef _WIN32
    pl_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(pl_label_);
    pl_label_->set_font_size(HUD_LABEL_FONT_SIZE);
    fec_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(fec_label_);
    fec_label_->set_font_size(HUD_LABEL_FONT_SIZE);
#endif

    rx_status_update_timer = std::make_shared<revector::Timer>();
    add_child(rx_status_update_timer);

    auto callback = [this] {
        for (const auto &bar : link_score_bars_) {
            bar->set_visibility(false);
        }

        for (const auto &bar : rssi_bars_) {
            if (bar) {
                bar->set_visibility(false);
            }
        }
        for (const auto &bar : snr_bars_) {
            if (bar) {
                bar->set_visibility(false);
            }
        }

        for (int i = 0; i != GuiInterface::Instance().links_.size(); ++i) {
            auto link_score = GuiInterface::Instance().links_[i]->get_link_score();

            for (int j = 0; j != ANTENNA_COUNT; ++j) {
                link_score_bars_[i * 2 + j]->set_visibility(true);
                link_score_bars_[i * 2 + j]->set_value(link_score[j]);
            }
        }

        if (!GuiInterface::Instance().links_.empty()) {
            const auto link_score = GuiInterface::Instance().links_.front()->get_link_score();
            const auto rssi = GuiInterface::Instance().links_.front()->get_rssi();
            const auto snr = GuiInterface::Instance().links_.front()->get_snr();
            signal_metrics_overlay_->set_metrics(link_score, rssi, snr);
            for (int j = 0; j < ANTENNA_COUNT; ++j) {
                rssi_bars_[j]->set_visibility(true);
                rssi_bars_[j]->set_value(rssi[j]);
                snr_bars_[j]->set_visibility(true);
                snr_bars_[j]->set_value(snr[j]);
            }
        }

#ifndef _WIN32
        if (GuiInterface::Instance().is_using_wifi) {
            pl_label_->set_visibility(true);
            fec_label_->set_visibility(false);

            int min_loss = std::numeric_limits<int>::max();
            for (const auto &link : GuiInterface::Instance().links_) {
                min_loss = std::min(min_loss, link->get_packet_loss());
            }
            pl_label_->set_text(FTR("packet loss") + ": " + std::to_string(min_loss));

            if (GuiInterface::Instance().alink_enabled_) {
                fec_label_->set_visibility(true);
                fec_label_->set_text("FEC: " + std::to_string(GuiInterface::Instance().drone_fec_level_));
            }
        } else {
            pl_label_->set_visibility(false);
            fec_label_->set_visibility(false);
        }
#endif

        rx_status_update_timer->start_timer(0.1);
    };
    rx_status_update_timer->connect_signal("timeout", callback);
    rx_status_update_timer->start_timer(0.1);

    record_status_label_ = std::make_shared<revector::Label>();
    label_container_->add_child(record_status_label_);
    record_status_label_->container_sizing.flag_h = revector::ContainerSizingFlag::ShrinkEnd;
    record_status_label_->set_text("");
    record_status_label_->set_font_size(HUD_LABEL_FONT_SIZE);

    auto capture_button = std::make_shared<revector::Button>();
    vbox->add_child(capture_button);
    capture_button->set_text(FTR("capture frame"));
    auto icon = std::make_shared<revector::VectorImage>(revector::get_asset_dir("CaptureImage.svg"), true);
    capture_button->set_icon_normal(icon);
    auto capture_callback = [this] {
        auto output_file = player_->capture_jpeg();
        if (output_file.empty()) {
            show_red_tip(FTR("capture fail"));
        } else {
            show_green_tip(FTR("frame saved") + output_file);
        }
    };
    capture_button->connect_signal("triggered", capture_callback);

    record_button_ = std::make_shared<revector::Button>();
    vbox->add_child(record_button_);
    auto icon2 = std::make_shared<revector::VectorImage>(revector::get_asset_dir("RecordVideo.svg"), true);
    record_button_->set_icon_normal(icon2);
    record_button_->set_text(FTR("record mp4") + " (F10)");

    auto record_button_raw = record_button_.get();
    auto record_callback = [record_button_raw, this] {
        if (!is_recording) {
            is_recording = player_->start_mp4_recording();

            if (is_recording) {
                record_button_raw->set_text(FTR("stop recording") + " (F10)");

                record_start_time = std::chrono::steady_clock::now();

                record_status_label_->set_text(FTR("recording") + ": 00:00");
            } else {
                record_status_label_->set_text("");
                show_red_tip(FTR("record fail"));
            }
        } else {
            is_recording = false;

            auto output_file = player_->stop_mp4_recording();

            record_button_raw->set_text(FTR("record mp4") + " (F10)");
            record_status_label_->set_text("");

            if (output_file.empty()) {
                show_red_tip(FTR("save record fail"));
            } else {
                show_green_tip(FTR("video saved") + output_file);
            }
        }
    };
    record_button_->connect_signal("triggered", record_callback);

    {
        auto button = std::make_shared<revector::CheckButton>();
        button->set_text(FTR("force sw decoding"));
        vbox->add_child(button);

        auto callback = [this](bool toggled) {
            force_software_decoding = toggled;
            if (playing_) {
                player_->stop();
                player_->play(play_url_, force_software_decoding);
            }
        };
        button->connect_signal("toggled", callback);
    }

    // {
    //     video_stabilization_button_ = std::make_shared<revector::CheckButton>();
    //     video_stabilization_button_->set_text(FTR("video stab"));
    //     vbox->add_child(video_stabilization_button_);
    //
    //     auto callback = [this](bool toggled) {
    //         player_->yuvRenderer_->mStabilize = toggled;
    //         if (toggled) {
    //             show_red_tip(FTR("video stab warning"));
    //         }
    //     };
    //     video_stabilization_button_->connect_signal("toggled", callback);
    // }
    //
    // {
    //     low_light_enhancement_button_ = std::make_shared<revector::CheckButton>();
    //     low_light_enhancement_button_->set_text(FTR("low light enhancement"));
    //     vbox->add_child(low_light_enhancement_button_);
    //
    //     auto callback = [this](bool toggled) { player_->yuvRenderer_->mLowLightEnhancement = toggled; };
    //     low_light_enhancement_button_->connect_signal("toggled", callback);
    // }

    auto onBitrateUpdate = [this](uint64_t bitrate) {
        std::string text = "BR: ";
        if (bitrate > 1024 * 1024) {
            text += fmt::format("{:.1f}", bitrate / 1024.0 / 1024.0) + " Mbps";
        } else if (bitrate > 1024) {
            text += fmt::format("{:.1f}", bitrate / 1024.0) + " Kbps";
        } else {
            text += fmt::format("{:d}", bitrate) + " bps";
        }
        bitrate_label_->set_text(text);
        bitrate_label_->show();
    };
    GuiInterface::Instance().bitrateUpdateCallbacks.emplace_back(onBitrateUpdate);

    auto onTipUpdate = [this](std::string msg) { show_red_tip(msg); };
    GuiInterface::Instance().tipCallbacks.emplace_back(onTipUpdate);

    auto onUrlStreamShouldStop = [this] { stop_playing(); };
    GuiInterface::Instance().urlStreamShouldStopCallbacks.emplace_back(onUrlStreamShouldStop);
}

void PlayerRect::custom_update(double dt) {
    if (!playing_) {
        play_mutex_.lock();
        const auto has_url = !play_url_.empty();
        play_mutex_.unlock();

        if (has_url) {
            start_playing(play_url_);
        }
    }

    if (player_) {
        player_->update(dt);
    }

    if (signal_metrics_overlay_) {
        const float overlay_y = get_size().y > 74.0f ? get_size().y - 74.0f : 0.0f;
        signal_metrics_overlay_->set_size({get_size().x, 42});
        signal_metrics_overlay_->set_position({0, overlay_y});
    }

    render_fps_label_->set_text("RFPS: " + std::to_string(revector::Engine::get_singleton()->get_fps_int()));

    const uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    const auto lost_total = GuiInterface::Instance().rtpPktLostTotal_.load(std::memory_order_relaxed);
    const auto loss_start_ms = GuiInterface::Instance().rtpLossStartTimestampMs_.load(std::memory_order_relaxed);
    double lost_per_second = 0.0;
    if (loss_start_ms != 0 && now_ms > loss_start_ms) {
        lost_per_second = lost_total * 1000.0 / static_cast<double>(now_ms - loss_start_ms);
    }
    rtp_loss_label_->set_text(fmt::format("LOSS: {} LPS: {:.1f}", lost_total, lost_per_second));
    rtp_loss_label_->set_visibility(true);

    const auto now = std::chrono::steady_clock::now();
    if (last_video_fps_update_time_ == std::chrono::steady_clock::time_point{}) {
        last_video_fps_update_time_ = now;
    }
    const std::chrono::duration<double> fps_elapsed = now - last_video_fps_update_time_;
    if (fps_elapsed.count() >= 1.0) {
        const auto frame_count = decoded_frame_count_.load(std::memory_order_relaxed);
        const auto frame_delta = frame_count - last_video_fps_frame_count_;
        const auto video_fps = frame_delta / fps_elapsed.count();
        video_fps_label_->set_text(fmt::format("VFPS: {:.0f}", video_fps));
        last_video_fps_frame_count_ = frame_count;
        last_video_fps_update_time_ = now;
    }

    if (is_recording) {
        std::chrono::duration<double> duration = std::chrono::steady_clock::now() - record_start_time;

        int total_seconds = duration.count();
        int hours = total_seconds / 3600;
        int minutes = (total_seconds % 3600) / 60;
        int seconds = total_seconds % 60;

        std::ostringstream ss;
        ss << FTR("recording") << ": ";
        if (hours > 0) {
            ss << hours << ":";
        }
        ss << std::setw(2) << std::setfill('0') << minutes << ":";
        ss << std::setw(2) << std::setfill('0') << seconds;

        record_status_label_->set_text(ss.str());
    }
}

void PlayerRect::custom_draw() {
    if (!playing_) {
        return;
    }
    if (player_) {
        auto render_image = (revector::RenderImage *)texture.get();
        player_->render(render_image->get_texture());

        const uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();

        const auto decoded_ms = last_decoded_frame_timestamp_ms_.load(std::memory_order_relaxed);
        if (decoded_ms != 0) {
            frame_ts_label_->set_text(fmt::format("F: {:04}ms", decoded_ms % 10000));
            frame_ts_label_->set_visibility(true);
        }

        const auto rtp_ms = last_rtp_timestamp_ms_.load(std::memory_order_relaxed);
        if (rtp_ms != 0) {
            rtp_ts_label_->set_text(fmt::format("IN: {:04}ms", rtp_ms % 10000));
        } else {
            rtp_ts_label_->set_text("IN: ----ms");
        }
        rtp_ts_label_->set_visibility(true);

        render_ts_label_->set_text(fmt::format("R: {:04}ms", now_ms % 10000));
        render_ts_label_->set_visibility(true);

        timestamp_overlay_label_->set_text(fmt::format("IN: {}ms F: {}ms R: {:04}ms",
                                                       rtp_ms != 0 ? fmt::format("{:04}", rtp_ms % 10000) : "----",
                                                       decoded_ms != 0 ? fmt::format("{:04}", decoded_ms % 10000)
                                                                       : "----",
                                                       now_ms % 10000));
        timestamp_overlay_label_->set_visibility(true);
    }
}

template <class DstType, class SrcType>
bool IsType(const SrcType *src) {
    return dynamic_cast<const DstType *>(src) != nullptr;
}

void PlayerRect::start_playing(const std::string &url) {
    if (playing_) {
        return;
    }

    playing_ = true;

    auto render_server = revector::RenderServer::get_singleton();

    bool recreate_player = true;
    if (player_) {
#ifdef AVIATEUR_USE_GSTREAMER
        const bool player_is_gst = dynamic_cast<const VideoPlayerGst *>(player_.get()) != nullptr;

        if (player_is_gst && GuiInterface::Instance().use_gstreamer_) {
            recreate_player = false;
        }

        if (!player_is_gst && !GuiInterface::Instance().use_gstreamer_) {
            recreate_player = false;
        }
#else
        recreate_player = false;
#endif
    }

    if (recreate_player) {
#ifdef AVIATEUR_USE_GSTREAMER
        if (GuiInterface::Instance().use_gstreamer_) {
            GuiInterface::Instance().PutLog(LogLevel::Info, "Creating video player (GStreamer)");
            player_ = std::make_shared<VideoPlayerGst>(render_server->device_, render_server->queue_);
        } else
#endif
        {
            GuiInterface::Instance().PutLog(LogLevel::Info, "Creating video player (FFmpeg)");
            player_ = std::make_shared<VideoPlayerFfmpeg>(render_server->device_, render_server->queue_);
        }
    }

    player_->play(url, force_software_decoding);

    texture = render_image_;

    collapse_panel_->set_visibility(true);

    hud_container_->set_visibility(true);
    timestamp_overlay_label_->set_visibility(true);
}

void PlayerRect::stop_playing() {
    playing_ = false;

    play_mutex_.lock();
    play_url_ = "";
    play_mutex_.unlock();

    for (const auto &bar : link_score_bars_) {
        bar->set_value(0);
    }

    if (is_recording) {
        record_button_->trigger();
    }

    // Fix crash in WfbReceiver destructor.
    if (player_) {
        player_->stop();
    }

    texture = logo_;
    collapse_panel_->set_visibility(false);
    hud_container_->set_visibility(false);
    timestamp_overlay_label_->set_visibility(false);
}
