#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>

std::string socket_path;
int runner_sock = -1;

void connect_socket() {
    if (runner_sock != -1) {
        close(runner_sock);
        runner_sock = -1;
    }
    runner_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (runner_sock < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(runner_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(runner_sock);
        runner_sock = -1;
    }
}

void send_cmd(const std::string& cmd) {
    if (runner_sock == -1) connect_socket();
    if (runner_sock != -1) {
        if (write(runner_sock, cmd.c_str(), cmd.length()) < 0) {
            connect_socket();
            if (runner_sock != -1) write(runner_sock, cmd.c_str(), cmd.length());
        }
    }
}

long long current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int main() {
    const char* home = getenv("HOME");
    if (!home) return 1;
    socket_path = std::string(home) + "/.config/SYSui/runner.sock";
    
    int js_fd = -1;
    
    long long last_start_click = 0;
    int start_click_count = 0;
    const int TRIPLE_CLICK_TIMEOUT_MS = 700;

    long long last_nav_time = 0;
    const int NAV_COOLDOWN_MS = 200;

    while (true) {
        if (js_fd < 0) {
            js_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
            if (js_fd < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            } else {
                std::cout << "Gamepad connected: /dev/input/js0" << std::endl;
            }
        }
        
        struct js_event e;
        int bytes = read(js_fd, &e, sizeof(e));
        
        if (bytes == sizeof(e)) {
            // BUTTONS
            if (e.type & JS_EVENT_BUTTON) {
                if (e.value == 0) { // RELEASED
                    if (e.number == 7) { // Start
                        long long now = current_time_ms();
                        if (now - last_start_click < TRIPLE_CLICK_TIMEOUT_MS) {
                            start_click_count++;
                        } else {
                            start_click_count = 1;
                        }
                        last_start_click = now;
                        
                        if (start_click_count >= 3) {
                            send_cmd("tv_toggle");
                            start_click_count = 0;
                        }
                    }
                } else if (e.value == 1) { // PRESSED
                    if (e.number == 0) { // A
                        send_cmd("enter");
                    } else if (e.number == 1) { // B
                        send_cmd("hide");
                    }
                }
            }
            
            // AXES (D-Pad and Sticks)
            if (e.type & JS_EVENT_AXIS) {
                long long now = current_time_ms();
                if (now - last_nav_time > NAV_COOLDOWN_MS) {
                    bool moved = false;
                    // X-axis (even numbers)
                    if (e.number % 2 == 0) {
                        if (e.value < -10000 || e.value == -1) { send_cmd("nav_left"); moved = true; }
                        else if (e.value > 10000 || e.value == 1) { send_cmd("nav_right"); moved = true; }
                    }
                    // Y-axis (odd numbers)
                    else {
                        if (e.value < -10000 || e.value == -1) { send_cmd("nav_up"); moved = true; }
                        else if (e.value > 10000 || e.value == 1) { send_cmd("nav_down"); moved = true; }
                    }
                    if (moved) last_nav_time = now;
                }
            }
        } else if (bytes < 0 && errno != EAGAIN) {
            std::cout << "Gamepad disconnected" << std::endl;
            close(js_fd);
            js_fd = -1;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return 0;
}
