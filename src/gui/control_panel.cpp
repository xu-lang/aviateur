#include "control_panel.h"

#include <cstdlib>
#include <thread>

#include <resources/default_resource.h>

#include "settings_tab.h"

namespace {

constexpr auto REMOTE_CAPTURE_TARGET = "root@192.168.1.10";
constexpr auto REMOTE_CAPTURE_PASSWORD = "12345";

std::string ShellQuote(const std::string &value) {
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

void RunRemoteCaptureCommand(bool start) {
    std::thread([start] {
        const std::string remote_command = start
            ? "pkill -f '[s]igmastar_venc_poc' >/dev/null 2>&1 || true; "
              "nohup /mnt/mmcblk0p1/sigmastar_venc_poc --server 192.168.1.3 --tsync 5602 venc-dump "
              "-r 1280x720 -f 120 --sensor-config /etc/sensors/imx415_fpv.bin -x 1 -n 0 "
              "--led-active-high --rtp 5600 --bitrate 4096 >/tmp/aviateur-venc.log 2>&1 < /dev/null & "
              "echo $! >/tmp/aviateur-venc.pid"
            : "if [ -f /tmp/aviateur-venc.pid ]; then kill $(cat /tmp/aviateur-venc.pid) >/dev/null 2>&1 || true; "
              "rm -f /tmp/aviateur-venc.pid; fi; pkill -f '[s]igmastar_venc_poc' >/dev/null 2>&1 || true";

        const std::string command = fmt::format(
            "SSHPASS={} sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
            "-o ConnectTimeout=5 {} {}",
            ShellQuote(REMOTE_CAPTURE_PASSWORD),
            REMOTE_CAPTURE_TARGET,
            ShellQuote("sh -c " + ShellQuote(remote_command)));

        GuiInterface::Instance().PutLog(LogLevel::Info,
                                        start ? "Starting remote device capture" : "Stopping remote device capture");
        const int result = std::system(command.c_str());
        if (result == 0) {
            GuiInterface::Instance().PutLog(LogLevel::Info,
                                            start ? "Remote device capture started" : "Remote device capture stopped");
        } else {
            GuiInterface::Instance().PutLog(LogLevel::Error,
                                            "Remote device capture command failed with exit code {}",
                                            result);
        }
    }).detach();
}

} // namespace

void ControlPanel::update_dongle_list(const std::shared_ptr<revector::MenuButton> &menu_button,
                                      std::string &dongle_name) {
    auto menu = menu_button->get_popup_menu().lock();

    devices_ = GuiInterface::GetDeviceList();

    menu->clear_items();

    bool previous_device_exists = false;
    for (const auto &d : devices_) {
        if (dongle_name == d.display_name) {
            previous_device_exists = true;
        }
        menu->create_item(d.display_name);
    }

    if (!previous_device_exists) {
        dongle_name = "";
    }
}

void ControlPanel::update_adapter_start_button_looking(bool start_status) const {
    tab_container_->set_tab_disabled(!start_status);

    play_button_->theme_override_normal = revector::StyleBox();
    play_button_->theme_override_pressed = revector::StyleBox();

    if (!start_status) {
        play_button_->theme_override_normal.value().bg_color = RED;
        play_button_->theme_override_pressed.value().bg_color = RED;
        play_button_->set_text(FTR("stop") + " (F5)");
        adapter_prop_block_->set_visibility(true);
    } else {
        play_button_->theme_override_normal.value().bg_color = GREEN;
        play_button_->theme_override_pressed.value().bg_color = GREEN;
        play_button_->set_text(FTR("start") + " (F5)");
        adapter_prop_block_->set_visibility(false);
    }
}

void ControlPanel::update_url_start_button_looking(bool start_status) const {
    tab_container_->set_tab_disabled(!start_status);

    play_port_button_->theme_override_normal = revector::StyleBox();
    play_port_button_->theme_override_pressed = revector::StyleBox();

    if (!start_status) {
        play_port_button_->theme_override_normal.value().bg_color = RED;
        play_port_button_->theme_override_pressed.value().bg_color = RED;
        play_port_button_->set_text(FTR("stop") + " (F5)");
    } else {
        play_port_button_->theme_override_normal.value().bg_color = GREEN;
        play_port_button_->theme_override_pressed.value().bg_color = GREEN;
        play_port_button_->set_text(FTR("start") + " (F5)");
    }
}

void ControlPanel::custom_ready() {
    auto &ini = GuiInterface::Instance().ini_;
    dongle_names.resize(2);
    dongle_names[0] = ini[CONFIG_WIFI][WIFI_DEVICE];
    dongle_names[1] = {};
    channel = std::stoi(ini[CONFIG_WIFI][WIFI_CHANNEL]);
    channelWidthMode = std::stoi(ini[CONFIG_WIFI][WIFI_CHANNEL_WIDTH_MODE]);
    keyPath = ini[CONFIG_WIFI][WIFI_GS_KEY];

    set_anchor_flag(revector::AnchorFlag::RightWide);

    tab_container_ = std::make_shared<revector::TabContainer>();
    add_child(tab_container_);
    tab_container_->set_anchor_flag(revector::AnchorFlag::FullRect);

    // Wi-Fi adapter tab
    {
        auto margin_container = std::make_shared<revector::MarginContainer>();
        margin_container->set_margin_all(8);
        margin_container->name = "Wi-Fi";
        tab_container_->add_child(margin_container);

        auto vbox = std::make_shared<revector::VBoxContainer>();
        vbox->set_separation(8);
        margin_container->add_child(vbox);

        auto con = std::make_shared<revector::Container>();
        vbox->add_child(con);

        auto vbox_blockable = std::make_shared<revector::VBoxContainer>();
        con->add_child(vbox_blockable);

        adapter_prop_block_ = std::make_shared<revector::Panel>();
        revector::StyleBox new_theme;
        new_theme.bg_color = revector::ColorU(0, 0, 0, 150);
        new_theme.border_width = 0;
        new_theme.corner_radius = 0;
        new_theme.border_color = revector::ColorU(0, 0, 0);
        adapter_prop_block_->theme_override_bg_ = new_theme;
        con->add_child(adapter_prop_block_);

        auto vbox_unblockable = std::make_shared<revector::VBoxContainer>();
        vbox->add_child(vbox_unblockable);

        {
            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            hbox_container->set_separation(8);
            vbox_blockable->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("device"));
            hbox_container->add_child(label);

            dongle_menu_button_ = std::make_shared<revector::MenuButton>();
            dongle_menu_button_->set_custom_minimum_size({0, 32});
            dongle_menu_button_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            hbox_container->add_child(dongle_menu_button_);

            // Do this before setting dongle button text.
            update_dongle_list(dongle_menu_button_, dongle_names[0].value());
            dongle_menu_button_->set_text(dongle_names[0].value());

            auto callback = [this](uint32_t) {
                dongle_names[0] = dongle_menu_button_->get_selected_item_text();
                GuiInterface::Instance().ini_[CONFIG_WIFI][WIFI_DEVICE] = *dongle_names[0];
            };
            dongle_menu_button_->connect_signal("item_selected", callback);

            refresh_dongle_button_ = std::make_shared<revector::Button>();
            auto icon = std::make_shared<revector::VectorImage>(revector::get_asset_dir("Refresh.svg"), true);
            refresh_dongle_button_->set_icon_normal(icon);
            refresh_dongle_button_->set_text("");
            hbox_container->add_child(refresh_dongle_button_);

            auto callback2 = [this] {
                update_dongle_list(dongle_menu_button_, dongle_names[0].value());

                if (dongle_names[1].has_value()) {
                    update_dongle_list(dongle_menu_button_b_, dongle_names[1].value());
                }
            };
            refresh_dongle_button_->connect_signal("triggered", callback2);
        }

        {
            device_b_con = std::make_shared<revector::CollapseContainer>(revector::CollapseButtonType::Check);
            device_b_con->set_title(FTR("dual adapter"));
            device_b_con->set_collapse(true);
            device_b_con->set_color(revector::ColorU(110, 137, 94));
            vbox_blockable->add_child(device_b_con);

            auto callback2 = [this](bool collapsed) {
                if (collapsed) {
                    // GuiInterface::Instance().links_.resize(1);
                    dongle_names[1] = {};
                } else {
                    // GuiInterface::Instance().links_.resize(2);
                    // GuiInterface::Instance().links_.back() = std::make_shared<WfbngLink>();
                    dongle_names[1] = "";

                    update_dongle_list(dongle_menu_button_b_, dongle_names[1].value());
                }
            };
            device_b_con->connect_signal("collapsed", callback2);

            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            hbox_container->set_separation(8);
            device_b_con->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("device"));
            hbox_container->add_child(label);

            dongle_menu_button_b_ = std::make_shared<revector::MenuButton>();
            dongle_menu_button_b_->set_custom_minimum_size({0, 32});
            dongle_menu_button_b_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            dongle_menu_button_b_->set_text("");
            hbox_container->add_child(dongle_menu_button_b_);

            // Do this before setting dongle button text.
            if (dongle_names[1].has_value()) {
                update_dongle_list(dongle_menu_button_b_, dongle_names[1].value());
                dongle_menu_button_b_->set_text(dongle_names[1].value());
            }

            auto callback = [this](uint32_t) { dongle_names[1] = dongle_menu_button_b_->get_selected_item_text(); };
            dongle_menu_button_b_->connect_signal("item_selected", callback);
        }

        {
            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            vbox_blockable->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("channel"));
            hbox_container->add_child(label);

            channel_button_ = std::make_shared<revector::MenuButton>();
            channel_button_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            hbox_container->add_child(channel_button_);

            {
                auto channel_menu = channel_button_->get_popup_menu();

                auto callback = [this](uint32_t) {
                    const auto meta = channel_button_->get_selected_item_meta();
                    channel = std::stoi(meta);
                    GuiInterface::Instance().ini_[CONFIG_WIFI][WIFI_CHANNEL] = meta;
                };
                channel_button_->connect_signal("item_selected", callback);

                uint32_t selected = 0;
                for (const auto &pair : CHANNELS) {
                    channel_menu.lock()->create_item(pair.second);
                    int item_index = channel_menu.lock()->get_item_count() - 1;
                    channel_menu.lock()->set_item_meta(item_index, std::to_string(pair.first));
                    if (channel == pair.first) {
                        selected = item_index;
                    }
                }

                channel_button_->select_item(selected);
            }
        }

        {
            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            vbox_blockable->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("channel width"));
            hbox_container->add_child(label);

            channel_width_button_ = std::make_shared<revector::MenuButton>();
            channel_width_button_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            hbox_container->add_child(channel_width_button_);

            {
                auto channel_width_menu = channel_width_button_->get_popup_menu();

                auto callback = [this](uint32_t) {
                    auto selected = channel_width_button_->get_selected_item_index();
                    if (selected.has_value()) {
                        channelWidthMode = selected.value();

                        GuiInterface::Instance().ini_[CONFIG_WIFI][WIFI_CHANNEL_WIDTH_MODE] =
                            std::to_string(channelWidthMode);
                    }
                };
                channel_width_button_->connect_signal("item_selected", callback);

                uint32_t selected = 0;
                for (auto width : CHANNEL_WIDTHS) {
                    channel_width_menu.lock()->create_item(width);
                    int current_index = channel_width_menu.lock()->get_item_count() - 1;
                    if (channelWidthMode == current_index) {
                        selected = current_index;
                    }
                }
                channel_width_button_->select_item(selected);
            }
        }

        {
            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            vbox_blockable->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("key"));
            hbox_container->add_child(label);

            auto text_edit = std::make_shared<revector::TextEdit>();
            text_edit->set_editable(false);
            if (keyPath.empty()) {
                text_edit->set_text(FTR("default"));
            } else {
                text_edit->set_text(std::filesystem::path(keyPath).filename().string());
            }
            text_edit->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            hbox_container->add_child(text_edit);

            auto file_dialog = std::make_shared<revector::FileDialog>();
            add_child(file_dialog);

            if (!keyPath.empty()) {
                auto defaultKeyPath = std::filesystem::absolute(keyPath).string();
                file_dialog->set_default_path(defaultKeyPath);
            }

            auto select_button = std::make_shared<revector::Button>();
            select_button->set_text(FTR("open"));

            std::weak_ptr file_dialog_weak = file_dialog;
            std::weak_ptr text_edit_weak = text_edit;
            auto callback = [this, file_dialog_weak, text_edit_weak] {
                auto path = file_dialog_weak.lock()->show();
                if (path.has_value()) {
                    std::filesystem::path p(path.value());
                    text_edit_weak.lock()->set_text(p.filename().string());
                    keyPath = path.value();
                    GuiInterface::Instance().ini_[CONFIG_WIFI][WIFI_GS_KEY] = keyPath;
                }
            };
            select_button->connect_signal("triggered", callback);
            hbox_container->add_child(select_button);
        }

#ifndef _WIN32
        {
            auto alink_con = std::make_shared<revector::CollapseContainer>(revector::CollapseButtonType::Check);
            alink_con->set_title(FTR("alink"));
            alink_con->set_collapse(false);
            alink_con->set_color(revector::ColorU(210, 137, 94));
            vbox_unblockable->add_child(alink_con);

            auto callback2 = [](bool collapsed) { GuiInterface::EnableAlink(!collapsed); };
            alink_con->connect_signal("collapsed", callback2);

            auto vbox_container2 = std::make_shared<revector::HBoxContainer>();
            alink_con->add_child(vbox_container2);

            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            hbox_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            vbox_container2->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("tx power"));
            hbox_container->add_child(label);

            tx_pwr_label_ = std::make_shared<revector::Label>();
            tx_pwr_label_->set_custom_minimum_size({64, 0});
            hbox_container->add_child(tx_pwr_label_);

            tx_pwr_slider_ = std::make_shared<revector::Slider>();
            tx_pwr_slider_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            tx_pwr_slider_->set_integer_mode(true);
            tx_pwr_slider_->set_range(1, 40);
            hbox_container->add_child(tx_pwr_slider_);

            auto callback = [this](float new_value) {
                GuiInterface::SetAlinkTxPower(new_value);
                tx_pwr_label_->set_text(std::to_string(int(round(new_value))) + " mW");
            };
            tx_pwr_slider_->connect_signal("value_changed", callback);

            // Set UI according to config
            {
                bool enabled = GuiInterface::Instance().ini_[CONFIG_WIFI][WIFI_ALINK_ENABLED] == "true";
                GuiInterface::EnableAlink(enabled);
                alink_con->set_collapse(!enabled);

                std::string tx_power = GuiInterface::Instance().ini_[CONFIG_WIFI][WIFI_ALINK_TX_POWER];
                tx_pwr_slider_->set_value(std::stoi(tx_power));
            }
        }
#endif

        {
            forward_con = std::make_shared<revector::CollapseContainer>(revector::CollapseButtonType::Check);
            forward_con->set_title(FTR("forward"));
            forward_con->set_collapse(true);
            forward_con->set_color(revector::ColorU(147, 115, 165));
            vbox_blockable->add_child(forward_con);

            auto on_collapsed = [](bool collapsed) {
                // if (collapsed) {
                //     GuiInterface::Instance().forward_port_.reset();
                // }
            };
            forward_con->connect_signal("collapsed", on_collapsed);

            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            hbox_container->set_separation(8);
            forward_con->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("target port"));
            hbox_container->add_child(label);

            forward_port_edit = std::make_shared<revector::TextEdit>();
            forward_port_edit->set_custom_minimum_size({0, 32});
            forward_port_edit->set_numbers_only(true);
            forward_port_edit->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            forward_port_edit->set_text("");
            hbox_container->add_child(forward_port_edit);

            // auto callback = [this](uint32_t) { dongle_names[1] = dongle_menu_button_b_->get_selected_item_text(); };
            // dongle_menu_button_b_->connect_signal("item_selected", callback);
        }

        {
            play_button_ = std::make_shared<revector::Button>();
            play_button_->set_custom_minimum_size({0, 48});
            play_button_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            update_adapter_start_button_looking(true);

            auto callback1 = [this] {
                bool start = play_button_->get_text() == FTR("start") + " (F5)";

                GuiInterface::Instance().is_using_wifi = true;
                GuiInterface::Instance().links_.clear();

                if (start) {
                    bool all_started = true;

                    if (dongle_names[1].has_value() && dongle_names[1] == dongle_names[0]) {
                        GuiInterface::Instance().ShowTip("Same device for dual adapter mode");
                        all_started = false;
                    } else {
                        for (const auto &dongle_name : dongle_names) {
                            if (!dongle_name.has_value()) {
                                continue;
                            }

                            // Check if the device is available.
                            std::optional<DeviceId> target_device_id;
                            for (auto &d : devices_) {
                                if (dongle_name == d.display_name) {
                                    target_device_id = d;
                                }
                            }

                            if (target_device_id.has_value()) {
                                bool res = false;

                                std::optional<std::string> forward_port;
                                if (!forward_con->get_collapse()) {
                                    if (forward_port_edit->get_text().empty()) {
                                        GuiInterface::Instance().ShowTip("Invalid port for RTP forwarding");
                                        all_started = false;
                                        break;
                                    }
                                    forward_port = forward_port_edit->get_text();
                                }

                                res = GuiInterface::Start(target_device_id.value(),
                                                          channel,
                                                          channelWidthMode,
                                                          keyPath,
                                                          forward_port);

                                if (!res) {
                                    GuiInterface::Instance().ShowTip("Device failed to start");
                                }

                                all_started &= res;
                            } else {
                                all_started = false;
                                GuiInterface::Instance().ShowTip("Null device");
                            }
                        }
                    }

                    if (!all_started) {
                        start = false;
                        GuiInterface::Stop();
                    }
                } else {
                    GuiInterface::Stop();
                }

                update_adapter_start_button_looking(!start);
            };
            play_button_->connect_signal("triggered", callback1);
            vbox_unblockable->add_child(play_button_);
        }
    }

    // Local tab
    {
        auto margin_container = std::make_shared<revector::MarginContainer>();
        margin_container->set_margin_all(8);
        margin_container->name = FTR("local");
        tab_container_->add_child(margin_container);

        auto vbox = std::make_shared<revector::VBoxContainer>();
        vbox->set_separation(8);
        margin_container->add_child(vbox);

        auto con = std::make_shared<revector::Container>();
        vbox->add_child(con);

        auto vbox_blockable = std::make_shared<revector::VBoxContainer>();
        con->add_child(vbox_blockable);

        udp_prop_block_ = std::make_shared<revector::Panel>();
        revector::StyleBox new_theme;
        new_theme.bg_color = revector::ColorU(0, 0, 0, 150);
        new_theme.border_width = 0;
        new_theme.corner_radius = 0;
        new_theme.border_color = revector::ColorU(0, 0, 0);
        udp_prop_block_->theme_override_bg_ = new_theme;
        udp_prop_block_->set_visibility(false);
        con->add_child(udp_prop_block_);

        auto hbox_container = std::make_shared<revector::HBoxContainer>();
        vbox_blockable->add_child(hbox_container);

        auto label = std::make_shared<revector::Label>();
        label->set_text(FTR("port"));
        hbox_container->add_child(label);

        local_listener_port_edit_ = std::make_shared<revector::TextEdit>();
        local_listener_port_edit_->set_editable(true);
        local_listener_port_edit_->set_numbers_only(true);
        local_listener_port_edit_->set_text(GuiInterface::Instance().ini_[CONFIG_LOCALHOST][CONFIG_LOCALHOST_PORT]);
        local_listener_port_edit_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
        hbox_container->add_child(local_listener_port_edit_);

        {
            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            hbox_container->set_separation(8);
            vbox_blockable->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text(FTR("codec"));
            hbox_container->add_child(label);

            auto codec_menu_button = std::make_shared<revector::MenuButton>();
            codec_menu_button->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            codec_menu_button->set_text(GuiInterface::Instance().rtp_codec_);
            hbox_container->add_child(codec_menu_button);

            auto menu = codec_menu_button->get_popup_menu().lock();

            menu->create_item("H264");
            menu->create_item("H265");

            auto callback = [this](uint32_t item_index) {
                if (item_index == 0) {
                    GuiInterface::Instance().rtp_codec_ = "H264";
                }
                if (item_index == 1) {
                    GuiInterface::Instance().rtp_codec_ = "H265";
                }
            };
            codec_menu_button->connect_signal("item_selected", callback);

            if (GuiInterface::Instance().rtp_codec_ == "H264") {
                codec_menu_button->select_item(0);
            } else {
                codec_menu_button->select_item(1);
            }
        }

        {
            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            hbox_container->set_separation(8);
            vbox_blockable->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text("LED control");
            hbox_container->add_child(label);

            auto vbox_container = std::make_shared<revector::VBoxContainer>();
            vbox_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            hbox_container->add_child(vbox_container);

            led_control_button_group_ = std::make_shared<revector::ToggleButtonGroup>();

            {
                auto udp_btn = std::make_shared<revector::RadioButton>();
                udp_btn->set_text("UDP");
                udp_btn->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
                vbox_container->add_child(udp_btn);
                udp_btn->set_toggled_no_signal(GuiInterface::Instance().led_control_mode_ == LedControlMode::Udp);
                udp_btn->connect_signal("toggled", [](bool toggled) {
                    if (toggled) {
                        GuiInterface::Instance().led_control_mode_ = LedControlMode::Udp;
                    }
                });
                led_control_button_group_->add_button(udp_btn);
            }

            {
                auto serial_btn = std::make_shared<revector::RadioButton>();
                serial_btn->set_text("Serial TX");
                serial_btn->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
                vbox_container->add_child(serial_btn);
                serial_btn->set_toggled_no_signal(GuiInterface::Instance().led_control_mode_ == LedControlMode::Serial);
                serial_btn->connect_signal("toggled", [](bool toggled) {
                    if (toggled) {
                        GuiInterface::Instance().led_control_mode_ = LedControlMode::Serial;
                    }
                });
                led_control_button_group_->add_button(serial_btn);
            }
        }

        {
            remote_capture_button_ = std::make_shared<revector::CheckButton>();
            remote_capture_button_->set_text("start device capture");
            remote_capture_button_->connect_signal("toggled", [](bool toggled) {
                GuiInterface::Instance().local_remote_capture_enabled_ = toggled;
            });
            vbox_blockable->add_child(remote_capture_button_);
        }

        {
            record_raw_rtp_button_ = std::make_shared<revector::CheckButton>();
            record_raw_rtp_button_->set_text("record raw RTP");
            record_raw_rtp_button_->connect_signal("toggled", [](bool toggled) {
                GuiInterface::Instance().local_rtp_record_raw_ = toggled;
            });
            vbox_blockable->add_child(record_raw_rtp_button_);
        }

        {
            auto hbox_container = std::make_shared<revector::HBoxContainer>();
            hbox_container->set_separation(8);
            vbox_blockable->add_child(hbox_container);

            auto label = std::make_shared<revector::Label>();
            label->set_text("frame index");
            hbox_container->add_child(label);

            auto vbox_container = std::make_shared<revector::VBoxContainer>();
            vbox_container->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            hbox_container->add_child(vbox_container);

            frame_index_source_button_group_ = std::make_shared<revector::ToggleButtonGroup>();

            {
                auto rtp_btn = std::make_shared<revector::RadioButton>();
                rtp_btn->set_text("RTP received");
                rtp_btn->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
                vbox_container->add_child(rtp_btn);
                rtp_btn->set_toggled_no_signal(GuiInterface::Instance().local_rtp_frame_index_source_ ==
                                               LocalRtpFrameIndexSource::RtpFrame);
                rtp_btn->connect_signal("toggled", [](bool toggled) {
                    if (toggled) {
                        GuiInterface::Instance().local_rtp_frame_index_source_ = LocalRtpFrameIndexSource::RtpFrame;
                    }
                });
                frame_index_source_button_group_->add_button(rtp_btn);
            }

            {
                auto decoded_btn = std::make_shared<revector::RadioButton>();
                decoded_btn->set_text("frame decoded");
                decoded_btn->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
                vbox_container->add_child(decoded_btn);
                decoded_btn->set_toggled_no_signal(GuiInterface::Instance().local_rtp_frame_index_source_ ==
                                                   LocalRtpFrameIndexSource::DecodedFrame);
                decoded_btn->connect_signal("toggled", [](bool toggled) {
                    if (toggled) {
                        GuiInterface::Instance().local_rtp_frame_index_source_ = LocalRtpFrameIndexSource::DecodedFrame;
                    }
                });
                frame_index_source_button_group_->add_button(decoded_btn);
            }

            {
                auto rendered_btn = std::make_shared<revector::RadioButton>();
                rendered_btn->set_text("frame rendered");
                rendered_btn->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
                vbox_container->add_child(rendered_btn);
                rendered_btn->set_toggled_no_signal(GuiInterface::Instance().local_rtp_frame_index_source_ ==
                                                     LocalRtpFrameIndexSource::RenderedFrame);
                rendered_btn->connect_signal("toggled", [](bool toggled) {
                    if (toggled) {
                        GuiInterface::Instance().local_rtp_frame_index_source_ = LocalRtpFrameIndexSource::RenderedFrame;
                    }
                });
                frame_index_source_button_group_->add_button(rendered_btn);
            }
        }

        {
            play_port_button_ = std::make_shared<revector::Button>();
            play_port_button_->set_custom_minimum_size({0, 48});
            play_port_button_->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
            update_url_start_button_looking(true);

            auto callback1 = [this] {
                bool start = play_port_button_->get_text() == FTR("start") + " (F5)";

                GuiInterface::Instance().is_using_wifi = false;

                if (start) {
                    std::string port = local_listener_port_edit_->get_text();
                    int play_port = std::stoi(port);
                    GuiInterface::Instance().decodedFrameCount_.store(0, std::memory_order_relaxed);
                    GuiInterface::Instance().renderedFrameCount_.store(0, std::memory_order_relaxed);
                    if (GuiInterface::Instance().local_remote_capture_enabled_) {
                        RunRemoteCaptureCommand(true);
                    }
                    if (GuiInterface::Instance().local_rtp_record_raw_) {
                        const bool forward_rtp = GuiInterface::Instance().local_rtp_frame_index_source_ ==
                            LocalRtpFrameIndexSource::RtpFrame;
                        if (forward_rtp) {
                            const int forward_port = 15600;
                            GuiInterface::Instance().local_rtp_recorder_ = std::make_unique<LocalRtpRecorder>();
                            if (GuiInterface::Instance().local_rtp_recorder_->start(
                                    play_port, forward_port, GuiInterface::Instance().rtp_codec_)) {
                                play_port = forward_port;
                            } else {
                                GuiInterface::Instance().local_rtp_recorder_.reset();
                            }
                        }
                    }

                    if (GuiInterface::Instance().use_gstreamer_) {
                        GuiInterface::Instance().EmitRtpStream("udp://0.0.0.0:" + std::to_string(play_port));
                    } else {
                        GuiInterface::Instance().NotifyRtpStream(97,
                                                                   0,
                                                                   play_port,
                                                                   GuiInterface::Instance().rtp_codec_,
                                                                   "0.0.0.0");
                    }

                    GuiInterface::Instance().ini_[CONFIG_LOCALHOST][CONFIG_LOCALHOST_PORT] = port;

                    udp_prop_block_->set_visibility(true);
                } else {
                    if (GuiInterface::Instance().local_rtp_recorder_) {
                        GuiInterface::Instance().local_rtp_recorder_->stop();
                        GuiInterface::Instance().local_rtp_recorder_.reset();
                    }
                    GuiInterface::Instance().EmitUrlStreamShouldStop();
                    if (GuiInterface::Instance().local_remote_capture_enabled_) {
                        RunRemoteCaptureCommand(false);
                    }

                    udp_prop_block_->set_visibility(false);
                }

                update_url_start_button_looking(!start);
            };

            play_port_button_->connect_signal("triggered", callback1);
            vbox->add_child(play_port_button_);
        }
    }

    // Settings tab
    {
        auto margin_container = std::make_shared<SettingsContainer>();
        margin_container->name = FTR("settings");
        tab_container_->add_child(margin_container);
    }
}

void ControlPanel::custom_input(revector::InputEvent &event) {
    if (event.type == revector::InputEventType::Key) {
        auto key_args = event.args.key;

        if (key_args.key == revector::KeyCode::F5) {
            if (key_args.pressed) {
                if (tab_container_->get_current_tab().has_value()) {
                    if (tab_container_->get_current_tab().value() == 0) {
                        play_button_->trigger();
                    } else if (tab_container_->get_current_tab().value() == 1) {
                        play_port_button_->trigger();
                    }
                }
            }
        }
    }
}

void ControlPanel::custom_update(double dt) {
    if (device_b_con) {
        if (GuiInterface::Instance().use_gstreamer_) {
            if (!device_b_con->get_visibility()) {
                device_b_con->show();
            }
        } else {
            if (device_b_con->get_visibility()) {
                device_b_con->set_collapse(true);
                device_b_con->hide();
            }
        }
    }
}
