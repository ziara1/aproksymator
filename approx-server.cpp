#include <chrono>
#include <iostream>
#include <string>
#include <map>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <cstring>
#include <unistd.h>

#include <poll.h>

#define TIMEOUT 3

enum class State { AwaitingHello, AwaitingPut, WaitingForState };


struct client_info {
    std::string username{};
    // Czas połączenia z klientem.
    std::chrono::steady_clock::time_point connect_time{};
    int socket_fd{-1};
    sockaddr_storage addr{};
    std::string addr_text{}; // Wersja tekstowa do logów.
    State state{State::AwaitingHello};
    std::vector<double> coeffs;
    std::vector<double> approx;
    double penalty{0.0};
    int puts_count{0};
    std::string net_buffer;
    int lowercase{0};
};

std::map<std::string, client_info> clients; // Mapa znanych nam klientów.
int port = 0;
int K = 100;
int N = 4;
int M = 131;
std::string filename{};


static void print_error(const std::string& msg) {
    std::cerr << "ERROR: " << msg << "\n";
}

// Funkcja do tworzenia kluczy do mapy klientów.
static std::string peer_key(const sockaddr_storage &addr) {
    char buf[INET6_ADDRSTRLEN];
    uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const sockaddr_in *a = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
        port = ntohs(a->sin_port);
    } else {
        const sockaddr_in6 *a6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
        port = ntohs(a6->sin6_port);
    }
    return std::string(buf) + ":" + std::to_string(port);
}

static bool handle_hello(std::string key) {
    if (clients.find(key) == clients.end()) {
        print_error("Unknown client");
        return false;
    }
    if (clients[key].state != State::AwaitingHello) {
        print_error("Client already sent HELLO");
        return false;
    }
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    if (t - clients[key].connect_time > std::chrono::seconds(TIMEOUT)) {
        clients.erase(key);
        return true;
    }
    int lowercase = 0;
    std::string player_id{};
    if (player_id.empty()) return false;
    for (char c : player_id) {
        if (!(std::isdigit(c) ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            return false;
        } else if (c >= 'a' && c <= 'z') {
            lowercase++;
        }
    }

    clients[key].lowercase = lowercase;
    clients[key].state = State::AwaitingPut;
    clients[key].username = player_id;
    return true;
}

static bool parse_arguments(int argc, char *argv[]) {
    bool p = false; bool k = false; bool n = false;
    bool m = false; bool f = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
            if (port < 0 || port > 65535 || p) {
                print_error("Invalid value for -p (port)");
                return false;
            }
            p = true;
        } else if (arg == "-k" && i + 1 < argc) {
            K = std::atoi(argv[++i]);
            if (K < 1 || K > 10000 || k) {
                print_error("Invalid value for -k (K)");
                return false;
            }
            k = true;
        } else if (arg == "-n" && i + 1 < argc) {
            N = std::atoi(argv[++i]);
            if (N < 1 || N > 8 || n) {
                print_error("Invalid value for -n (N)");
                return false;
            }
            n = true;
        } else if (arg == "-m" && i + 1 < argc) {
            M = std::atoi(argv[++i]);
            if (M < 1 || M > 12341234 || m) {
                print_error("Invalid value for -m (M)");
                return false;
            }
            m = true;
        } else if (arg == "-f" && i + 1 < argc) {
            if (f) {
                print_error("Invalid value for -f (f)");
                return false;
            }
            filename = argv[++i];
            f = true;
        } else {
            print_error("Unknown or incomplete argument: " + arg);
            return false;
        }
    }
    if (!f) {
        print_error("Missing required -f argument (filename)");
        return false;
    }
    return true;
}





int main(int argc, char* argv[]) {
    if (!parse_arguments(argc, argv)) {
        return 1;
    }


    return 0;
}