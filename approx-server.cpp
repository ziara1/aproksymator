#include <chrono>
#include <iostream>
#include <string>
#include <map>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <cstring>
#include <fcntl.h>


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

std::map<int, client_info> clients; // Mapa znanych nam klientów.
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

static bool handle_hello(int key, std::string &msg) {
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
    for (int i = 6; i < msg.size(); i++) {
        char c = msg[i];
        if (!(std::isdigit(c) ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            return false;
        }
        player_id += c;
        if (c >= 'a' && c <= 'z') {
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

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_and_bind_socket(const char* port_str, int family) {
    struct addrinfo hints{}, *res, *rp;
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int s = getaddrinfo(nullptr, port_str, &hints, &res);
    if (s != 0) {
        print_error("getaddrinfo: " + std::string(gai_strerror(s)));
        return -1;
    }

    int listen_fd = -1;
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd == -1)
            continue;
        int on = 1;
        setsockopt(listen_fd, SOL_SOCKET,
            SO_REUSEADDR, &on, sizeof(on));
        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }
    freeaddrinfo(res);
    return listen_fd;
}




int main(int argc, char* argv[]) {
    if (!parse_arguments(argc, argv)) {
        return 1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int listen_fd6 = create_and_bind_socket(port_str, AF_INET6);
    int listen_fd4 = create_and_bind_socket(port_str, AF_INET);

    if (listen_fd6 == -1 && listen_fd4 == -1) {
        print_error("Cannot bind to any socket.");
        return 1;
    }

    if (port == 0) {
        int assigned_port = 0;
        if (listen_fd6 != -1) {
            sockaddr_in6 sa6{};
            socklen_t len = sizeof(sa6);
            if (getsockname(listen_fd6, (sockaddr*)&sa6, &len) == 0)
                assigned_port = ntohs(sa6.sin6_port);
        } else if (listen_fd4 != -1) {
            sockaddr_in sa4{};
            socklen_t len = sizeof(sa4);
            if (getsockname(listen_fd4, (sockaddr*)&sa4, &len) == 0)
                assigned_port = ntohs(sa4.sin_port);
        }
        std::cout << "Listening on port: " << assigned_port << std::endl;
    }

    if (listen_fd6 != -1) {
        int off = 0;
        setsockopt(listen_fd6, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        listen(listen_fd6, SOMAXCONN);
        set_nonblocking(listen_fd6);
    }
    if (listen_fd4 != -1) {
        listen(listen_fd4, SOMAXCONN);
        set_nonblocking(listen_fd4);
    }

    std::vector<pollfd> pollfds;
    if (listen_fd6 != -1)
        pollfds.push_back({listen_fd6, POLLIN, 0});
    if (listen_fd4 != -1)
        pollfds.push_back({listen_fd4, POLLIN, 0});

    while (M) {
        // Add all clients to pollfds
        pollfds.resize((listen_fd6 != -1) + (listen_fd4 != -1));
        for (const auto& [fd, info] : clients) {
            pollfds.push_back({fd, POLLIN, 0});
        }

        int ret = poll(pollfds.data(), pollfds.size(), -1);
        if (ret < 0) {
            print_error("poll() error");
            break;
        }

        // Accept new connections
        for (size_t i = 0; i < pollfds.size(); ++i) {
            pollfd& pfd = pollfds[i];
            if ((listen_fd6 != -1 && pfd.fd == listen_fd6 && (pfd.revents & POLLIN)) ||
                (listen_fd4 != -1 && pfd.fd == listen_fd4 && (pfd.revents & POLLIN))) {
                sockaddr_storage client_addr{};
                socklen_t addrlen = sizeof(client_addr);
                int client_fd = accept(pfd.fd, (sockaddr*)&client_addr, &addrlen);
                if (client_fd >= 0) {
                    set_nonblocking(client_fd);
                    std::string key = peer_key(client_addr);
                    client_info info;
                    info.socket_fd = client_fd;
                    info.addr = client_addr;
                    info.addr_text = key;
                    info.connect_time = std::chrono::steady_clock::now();
                    clients[client_fd] = info;
                    std::cout << "New client: " << key << std::endl;
                }
            }
        }


        // Handle client data
        std::vector<int> to_remove;
        for (size_t i = (listen_fd6 != -1) + (listen_fd4 != -1); i < pollfds.size(); ++i) {
            pollfd& pfd = pollfds[i];
            if (pfd.revents & POLLIN) {
                char buf[4096];
                ssize_t recvd = recv(pfd.fd, buf, sizeof(buf), 0);
                if (recvd <= 0) {
                    close(pfd.fd);
                    to_remove.push_back(pfd.fd);
                    std::cout << "Client disconnected: " << clients[pfd.fd].addr_text << std::endl;
                } else {
                    clients[pfd.fd].net_buffer.append(buf, recvd);
                    std::cout << "Received " << recvd << " bytes from " << clients[pfd.fd].addr_text << std::endl;
                }
            }
            std::string& net_buffer = clients[pfd.fd].net_buffer;
            // Jeśli zaczyna się od "HELLO ".
            if (net_buffer.size() > 7 && net_buffer.substr(0, 6) == "HELLO ") {
                size_t pos = net_buffer.find("\r\n");
                if (pos != std::string::npos) {
                    // Mamy pełną linię "HELLO <player_id>\r\n"
                    std::string msg = net_buffer.substr(0, pos); // bez \r\n
                    if (!handle_hello(pfd.fd, msg)) {
                        print_error("Invalid HELLO message\n");
                    }
                    net_buffer.erase(0, pos + 2); // usuń obsłużoną linię
                }
            }
        }
        for (int fd : to_remove) {
            clients.erase(fd);
        }

        // Clean pollfds
        pollfds.clear();
        if (listen_fd6 != -1)
            pollfds.push_back({listen_fd6, POLLIN, 0});
        if (listen_fd4 != -1)
            pollfds.push_back({listen_fd4, POLLIN, 0});
    }

    if (listen_fd6 != -1) close(listen_fd6);
    if (listen_fd4 != -1) close(listen_fd4);
    for (auto& [fd, info] : clients) {
        close(fd);
    }
    return 0;
}