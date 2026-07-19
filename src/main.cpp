#include <glad/gl.h>
#include <nodes/scene_tree.h>
#include <resources/default_resource.h>

#include "app.h"
#include "gui/control_panel.h"
#include "gui/player_rect.h"
#include "gui_interface.h"
#include "wifi/wfbng_link.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace {

constexpr uint16_t TSYNC_PORT = 5602;

uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class TimeSyncServer {
public:
    void start() {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this] { run(); });
        led_thread_ = std::thread([this] { run_led_control(); });
    }

    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (led_thread_.joinable()) {
            led_thread_.join();
        }
    }

private:
    struct Response {
        char magic[4];
        uint64_t t1;
        uint64_t t2;
        uint64_t t3;
    };

    void run() {
#ifdef _WIN32
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "Time sync WSAStartup failed");
            return;
        }
#endif

        const auto sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "Time sync socket creation failed");
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(TSYNC_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "Time sync bind failed on UDP port {}", TSYNC_PORT);
            close_socket(sock);
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }

        GuiInterface::Instance().PutLog(LogLevel::Info, "Time sync server listening on UDP port {}", TSYNC_PORT);

        while (running_.load(std::memory_order_relaxed)) {
            char buffer[1024]{};
            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            const int received = recvfrom(sock, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (received < 12 || std::memcmp(buffer, "PSYN", 4) != 0) {
                continue;
            }
            GuiInterface::Instance().timeSyncRequestCount_.fetch_add(1, std::memory_order_relaxed);

            {
                std::lock_guard lock(peer_mutex_);
                last_peer_ = peer;
                has_peer_ = true;
            }

            Response response{};
            std::memcpy(response.magic, "PSYN", 4);
            std::memcpy(&response.t1, buffer + 4, sizeof(response.t1));
            response.t2 = NowMs();
            response.t3 = NowMs();
            sendto(sock,
                   reinterpret_cast<const char *>(&response),
                   sizeof(response),
                   0,
                   reinterpret_cast<sockaddr *>(&peer),
                   peer_len);
        }

        close_socket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    static void close_socket(int sock) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

    void run_led_control() {
#ifdef _WIN32
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "LED control WSAStartup failed");
            return;
        }
#endif

        const auto sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "LED control socket creation failed");
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }

        GuiInterface::Instance().PutLog(LogLevel::Info, "LED control sending to last time sync source endpoint");

        bool led_on = false;
        auto next_tick = std::chrono::steady_clock::now();
        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in peer{};
            bool has_peer = false;
            {
                std::lock_guard lock(peer_mutex_);
                has_peer = has_peer_;
                peer = last_peer_;
            }

            if (has_peer) {
                const uint8_t value = led_on ? 1 : 0;
                GuiInterface::Instance().led_on_.store(led_on, std::memory_order_relaxed);
                sendto(sock,
                       reinterpret_cast<const char *>(&value),
                       sizeof(value),
                       0,
                       reinterpret_cast<sockaddr *>(&peer),
                       sizeof(peer));
                led_on = !led_on;
            }

            next_tick += std::chrono::milliseconds(500);
            std::this_thread::sleep_until(next_tick);
            if (std::chrono::steady_clock::now() > next_tick + std::chrono::milliseconds(500)) {
                next_tick = std::chrono::steady_clock::now();
            }
        }

        close_socket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    std::atomic<bool> running_ = false;
    std::thread thread_;
    std::thread led_thread_;
    std::mutex peer_mutex_;
    sockaddr_in last_peer_{};
    bool has_peer_ = false;
};

} // namespace

int main() {
    GuiInterface::Instance().init();
    GuiInterface::Instance().PutLog(LogLevel::Info, "App started");

    TimeSyncServer time_sync_server;
    time_sync_server.start();

    auto app = std::make_shared<revector::App>(revector::Vec2I{1280, 720},
                                               GuiInterface::Instance().dark_mode_,
                                               GuiInterface::Instance().use_vulkan_);
    app->set_window_title("Aviateur - OpenIPC FPV Ground Station");

    revector::TranslationServer::get_singleton()->load_translations(revector::get_asset_dir("translations.csv"));

    // Initialize the default libusb context.
    int rc = libusb_init(nullptr);

    {
        auto hbox_container = std::make_shared<revector::HBoxContainer>();
        hbox_container->set_anchor_flag(revector::AnchorFlag::FullRect);
        hbox_container->set_separation(0);
        app->get_tree_root()->add_child(hbox_container);

        auto player_rect = std::make_shared<PlayerRect>();
        player_rect->container_sizing.flag_h = revector::ContainerSizingFlag::Fill;
        player_rect->container_sizing.flag_v = revector::ContainerSizingFlag::Fill;
        hbox_container->add_child(player_rect);

        auto control_panel = std::make_shared<ControlPanel>();
        control_panel->container_sizing.flag_v = revector::ContainerSizingFlag::Fill;
        control_panel->set_custom_minimum_size({340, 0});
        hbox_container->add_child(control_panel);

        std::weak_ptr control_panel_weak = control_panel;
        std::weak_ptr player_rect_weak = player_rect;

        auto on_wifi_stopped = [control_panel_weak, player_rect_weak] {
            if (!control_panel_weak.expired() && !player_rect_weak.expired()) {
                player_rect_weak.lock()->stop_playing();
                player_rect_weak.lock()->show_red_tip(FTR("wi-fi stopped msg"));
                control_panel_weak.lock()->update_adapter_start_button_looking(true);
            }
        };
        GuiInterface::Instance().wifiStopCallbacks.emplace_back(on_wifi_stopped);

        {
            player_rect->control_panel_button_ = std::make_shared<revector::Button>();
            player_rect->add_child(player_rect->control_panel_button_);
            player_rect->control_panel_button_->set_text(">");
            player_rect->control_panel_button_->set_size({24, 64});
            player_rect->control_panel_button_->set_anchor_flag(revector::AnchorFlag::CenterRight);
            player_rect->control_panel_button_->theme_override_normal = revector::StyleBox::from_empty();
            player_rect->control_panel_button_->theme_override_normal->bg_color = revector::ColorU{30, 30, 30, 100};
            player_rect->control_panel_button_->theme_override_hovered = revector::StyleBox::from_empty();
            player_rect->control_panel_button_->theme_override_hovered->bg_color = revector::ColorU{30, 30, 30, 150};
            player_rect->control_panel_button_->theme_override_pressed = revector::StyleBox::from_empty();
            player_rect->control_panel_button_->theme_override_pressed->bg_color = revector::ColorU{30, 30, 30, 200};

            auto on_control_panel_triggered = [player_rect_weak, control_panel_weak]() {
                bool visible = false;
                if (!control_panel_weak.expired()) {
                    visible = control_panel_weak.lock()->get_visibility();
                }
                if (visible) {
                    player_rect_weak.lock()->control_panel_button_->set_text("<");
                } else {
                    player_rect_weak.lock()->control_panel_button_->set_text(">");
                }
                control_panel_weak.lock()->set_visibility(!visible);
            };
            player_rect->control_panel_button_->connect_signal("triggered", on_control_panel_triggered);
        }
    }

    GuiInterface::Instance().PutLog(LogLevel::Info, "Entering app main loop");

    app->main_loop();

    time_sync_server.stop();

    GuiInterface::SaveConfig();

    app.reset();

    libusb_exit(nullptr);

    return EXIT_SUCCESS;
}
