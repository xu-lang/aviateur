#include <glad/gl.h>
#include <nodes/scene_tree.h>
#include <resources/default_resource.h>

#include "app.h"
#include "gui/control_panel.h"
#include "gui/player_rect.h"
#include "gui_interface.h"
#include "wifi/wfbng_link.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <sys/ioctl.h>
    #include <sys/socket.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace {

constexpr uint16_t TSYNC_PORT = 5602;

uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class SerialLedController {
public:
    ~SerialLedController() {
        close_port();
    }

    bool set_led(bool on) {
        if (!is_open()) {
            if (std::chrono::steady_clock::now() < next_probe_) {
                return false;
            }
            if (!probe()) {
                next_probe_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                return false;
            }
        }

#ifdef _WIN32
        const bool success = on ? SetCommBreak(handle_) : ClearCommBreak(handle_);
#else
        const bool success = ioctl(fd_, on ? TIOCSBRK : TIOCCBRK) == 0;
#endif
        if (!success) {
            GuiInterface::Instance().PutLog(LogLevel::Warn, "Serial LED control failed on {}", port_name_);
            close_port();
        }
        return success;
    }

    void reset() {
        if (is_open()) {
            set_led(false);
            close_port();
        }
    }

private:
    bool probe() {
        close_port();

        for (const auto &port : list_ports()) {
            if (open_port(port)) {
                port_name_ = port;
                GuiInterface::Instance().PutLog(LogLevel::Info, "Serial LED control using {} at 115200 baud", port_name_);
                return true;
            }
        }

        GuiInterface::Instance().PutLog(LogLevel::Warn, "No usable serial port found for LED control");
        return false;
    }

    std::vector<std::string> list_ports() const {
        std::vector<std::string> ports;
#ifdef _WIN32
        for (int i = 1; i <= 256; ++i) {
            ports.push_back("\\\\.\\COM" + std::to_string(i));
        }
#else
        const std::vector<std::string> prefixes = {
#ifdef __APPLE__
            "cu.",
            "tty.",
#else
            "ttyUSB",
            "ttyACM",
            "ttyS",
#endif
        };

        std::error_code ec;
        std::vector<std::vector<std::string>> ports_by_priority(prefixes.size());
        for (const auto &entry : std::filesystem::directory_iterator("/dev", ec)) {
            if (ec) {
                break;
            }
            const auto name = entry.path().filename().string();
            for (size_t i = 0; i < prefixes.size(); ++i) {
                const auto &prefix = prefixes[i];
                if (name.rfind(prefix, 0) == 0) {
                    ports_by_priority[i].push_back(entry.path().string());
                    break;
                }
            }
        }
        for (auto &group : ports_by_priority) {
            std::sort(group.begin(), group.end());
            ports.insert(ports.end(), group.begin(), group.end());
        }
#endif
        return ports;
    }

    bool open_port(const std::string &port) {
#ifdef _WIN32
        handle_ = CreateFileA(port.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) {
            close_port();
            return false;
        }
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        if (!SetCommState(handle_, &dcb)) {
            close_port();
            return false;
        }
        return true;
#else
        fd_ = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            return false;
        }

        termios tty{};
        if (tcgetattr(fd_, &tty) != 0) {
            close_port();
            return false;
        }
        cfmakeraw(&tty);
        cfsetispeed(&tty, B115200);
        cfsetospeed(&tty, B115200);
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~CRTSCTS;
        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            close_port();
            return false;
        }
        return true;
#endif
    }

    bool is_open() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

    void close_port() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            ClearCommBreak(handle_);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ioctl(fd_, TIOCCBRK);
            close(fd_);
            fd_ = -1;
        }
#endif
    }

    std::string port_name_;
    std::chrono::steady_clock::time_point next_probe_{};
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

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

        SerialLedController serial_led;
        auto last_mode = GuiInterface::Instance().led_control_mode_;
        bool led_on = false;
        auto next_tick = std::chrono::steady_clock::now();
        while (running_.load(std::memory_order_relaxed)) {
            const auto mode = GuiInterface::Instance().led_control_mode_;
            if (mode != last_mode) {
                if (last_mode == LedControlMode::Serial) {
                    serial_led.reset();
                }
                last_mode = mode;
            }

            if (mode == LedControlMode::Serial) {
                if (serial_led.set_led(led_on)) {
                    GuiInterface::Instance().led_on_.store(led_on, std::memory_order_relaxed);
                    led_on = !led_on;
                }
            } else {
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
            }

            next_tick += std::chrono::milliseconds(500);
            std::this_thread::sleep_until(next_tick);
            if (std::chrono::steady_clock::now() > next_tick + std::chrono::milliseconds(500)) {
                next_tick = std::chrono::steady_clock::now();
            }
        }

        serial_led.reset();
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
