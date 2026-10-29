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

#include "librecomp/game.hpp"

namespace recomp {
    namespace dd {
        void fatal(const std::string& title, const std::string& text) {
            if (!switch_on(std::getenv("WCW_DD_METRICS"))) {
                return;
            }

            std::string game;
            if (const char* g = std::getenv("WCW_DD_GAME")) {
                game = g;
            }
            else {
                for (char8_t c : recomp::current_game_id()) {
                    game.push_back(char(c));
                }
            }

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
