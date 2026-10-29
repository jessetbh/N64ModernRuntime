// [wcw] Sends the fatal breadcrumb (aki-roadmap#125): the part of dd_breadcrumb that needs the network and the running
// game, kept out of the unit tests. See dd_breadcrumb.hpp.

#include "librecomp/dd_breadcrumb.hpp"

#include <cstdint>
#include <cstdlib>

#if defined(_WIN32)
#   include <winsock2.h>
#   include <ws2tcpip.h>
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <sys/socket.h>
#   include <unistd.h>
#endif


namespace recomp {
    namespace dd {
        static void send_fatal(const std::string& title, const std::string& text);

        static std::string exe_name() {
#if defined(_WIN32)
            char path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
            return std::string(path, (n < MAX_PATH) ? n : 0);
#else
            char path[4096] = {};
            const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
            return (n > 0) ? std::string(path, size_t(n)) : std::string();
#endif
        }

        void fatal(const std::string& title, const std::string& text) noexcept {
            try {
                send_fatal(title, text);
            }
            catch (...) {
                // Nothing to do on the way out.
            }
        }

        void send_fatal(const std::string& title, const std::string& text) {
            if (!switch_on(std::getenv("WCW_DD_METRICS"))) {
                return;
            }

            const std::string game = fatal_game(std::getenv("WCW_DD_GAME"), exe_name());
            const std::string line = event_line(fatal_title(game, title), text, fatal_tags(game, std::getenv("WCW_DD_TAGS")),
                "error", "fatal");
            unsigned port = 8125;
            if (const char* p = std::getenv("WCW_DD_PORT")) {
                const long v = std::strtol(p, nullptr, 10);
                if ((v > 0) && (v < 65536)) {
                    port = unsigned(v);
                }
            }

#if defined(_WIN32)
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                return;
            }

            const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s != INVALID_SOCKET) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(uint16_t(port));
                inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
                sendto(s, line.data(), int(line.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
                closesocket(s);
            }

            WSACleanup();
#else
            const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s >= 0) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(uint16_t(port));
                inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
                sendto(s, line.data(), line.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
                close(s);
            }
#endif
        }
    }
}
