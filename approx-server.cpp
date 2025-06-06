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
#include <fstream>
#include <sstream>


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
    std::vector<double> coeffs{};
    std::vector<double> approx{};
    double penalty{0.0};
    int puts_count{0};
    std::string net_buffer{};
    int lowercase{0};

    std::string pending_response{};    // treść odpowiedzi, którą musimy wysłać
    std::chrono::steady_clock::time_point send_time{}; // kiedy wysłać pending_response
    bool has_pending{false};         // czy jest odpowiedź do wysłania
    client_info() = default;

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

static std::ifstream coeff_file;

static bool send_coeff_line(int key) {
    if (!coeff_file.is_open()) {
        print_error("Plik z COEFF nie jest otwarty");
        return false;
    }
    std::string line;
    if (!std::getline(coeff_file, line)) {
        print_error("Brak kolejnej linii w pliku COEFF");
        return false;
    }
    // Plik ma linie kończące się "\r\n", std::getline usunie '\n',
    // więc sprawdzamy, czy na końcu został '\r'
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    // Teraz 'line' powinno mieć format "COEFF a0 a1 ... aN"
    // 1) wyślij klientowi tę linię + "\r\n"
    std::string to_send = line + "\r\n";
    int sent = send(clients[key].socket_fd, to_send.c_str(), to_send.size(), 0);
    if (sent < 0) {
        print_error("Błąd wysyłania COEFF do klienta " + clients[key].addr_text);
        return false;
    }
    // 2) sparsuj liczby do wektora clients[key].coeffs (pomijając pierwsze słowo "COEFF")
    std::istringstream iss(line);
    std::string token;
    iss >> token; // pobierz "COEFF"
    double a;
    // oczekujemy dokładnie N+1 liczb (a0..aN)
    for (int i = 0; i <= N; i++) {
        if (!(iss >> a)) {
            print_error("Nie udało się sparsować współczynnika nr " + std::to_string(i));
            return false;
        }
        clients[key].coeffs.push_back(a);
    }
    return true;
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
    std::string player_id{};
    for (int i = 6; i < msg.size(); i++) {
        char c = msg[i];
        if (!(std::isdigit(c) ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            print_error("Invalid player_id");
            return false;
        }
        clients[key].username += c;
        if (c >= 'a' && c <= 'z')
            clients[key].lowercase++;
    }
    clients[key].state = State::AwaitingPut;
    // Po udanym HELLO od razu wysyłamy COEFF z pliku:
    if (!send_coeff_line(key)) {
        // jeśli coś nie poszło, usuwamy klienta
        print_error("Invalid COEFF message\n");
        close(clients[key].socket_fd);
        clients.erase(key);
        return false;
    }
    return true;
}

static bool parse_point(const std::string &msg, size_t &i, int &point) {
    point = 0;
    int sign = 1;
    if (i < msg.size() && msg[i] == '-') {
        sign = -1;
        i++;
    }

    bool digit_found = false;
    while (i < msg.size() && std::isdigit(msg[i])) {
        digit_found = true;
        point = point * 10 + (msg[i] - '0');
        i++;
    }

    if (!digit_found) {
        print_error("No digits in point");
        return false;
    }

    point *= sign;

    if (i >= msg.size() || msg[i] != ' ') {
        print_error("No space after point");
        return false;
    }

    i++; // Skip space
    return true;
}

static bool parse_value(const std::string &msg, size_t &i, double &value) {
    value = 0.0;
    int sign = 1;
    if (i < msg.size() && msg[i] == '-') {
        sign = -1;
        i++;
    }

    bool value_digit_found = false;
    double frac = 0.1;

    // Integer part
    while (i < msg.size() && std::isdigit(msg[i])) {
        value_digit_found = true;
        value = value * 10.0 + (msg[i] - '0');
        i++;
    }

    // Fractional part
    if (i < msg.size() && msg[i] == '.') {
        i++;
        while (i < msg.size() && std::isdigit(msg[i])) {
            value_digit_found = true;
            value += frac * (msg[i] - '0');
            frac *= 0.1;
            i++;
        }
    }

    if (!value_digit_found) {
        print_error("No digits in value");
        return false;
    }

    value *= sign;
    return true;
}

static bool handle_put(int key, std::string &msg) {
    if (clients.find(key) == clients.end()) {
        print_error("Unknown client");
        return false;
    }
    if (clients[key].state != State::AwaitingPut) {
        print_error("Client in invalid state for PUT");
        return false;
    }

    size_t i = 4;
    int point = 0;
    double value = 0.0;

    if (!parse_point(msg, i, point)) return false;
    if (!parse_value(msg, i, value)) return false;

    if (point < 0 || point > K || value < -5.0 || value > 5.0) {
        print_error("PUT values out of range");
        return false;
    }

    std::cout << "Received PUT: point=" << point << " value=" << value << std::endl;
    // TODO: implement game logic here
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

void process_client_buffer(int fd) {
    std::string& net_buffer = clients[fd].net_buffer;
    if (net_buffer.size() > 7 && net_buffer.substr(0, 6) == "HELLO ") {
        size_t pos = net_buffer.find("\r\n");
        if (pos != std::string::npos) {
            std::string msg = net_buffer.substr(0, pos);
            if (!handle_hello(fd, msg)) {
                print_error("Invalid HELLO message\n");
            }
            net_buffer.erase(0, pos + 2);
        }
    }
    if (net_buffer.size() > 7 && net_buffer.substr(0, 4) == "PUT ") {
        size_t pos = net_buffer.find("\r\n");
        if (pos != std::string::npos) {
            std::string msg = net_buffer.substr(0, pos);
            if (!handle_put(fd, msg)) {
                print_error("Invalid PUT message\n");
            }
            net_buffer.erase(0, pos + 2);
        }
    }
}

bool initialize(int argc, char* argv[]) {
    if (!parse_arguments(argc, argv)) return false;

    coeff_file.open(filename);
    if (!coeff_file) {
        std::cerr << "ERROR: Nie udało się otworzyć pliku: " << filename << "\n";
        return false;
    }

    return true;
}

int create_server_socket(int family) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    return create_and_bind_socket(port_str, family);
}

void display_assigned_port(int listen_fd6, int listen_fd4) {
    if (port != 0) return;

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

void prepare_sockets(int listen_fd6, int listen_fd4) {
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
}

void prepare_pollfds(std::vector<pollfd>& pollfds, int listen_fd6, int listen_fd4) {
    pollfds.clear();
    if (listen_fd6 != -1)
        pollfds.push_back({listen_fd6, POLLIN, 0});
    if (listen_fd4 != -1)
        pollfds.push_back({listen_fd4, POLLIN, 0});

    for (const auto& [fd, info] : clients) {
        pollfds.push_back({fd, POLLIN, 0});
    }
}

void accept_new_clients(std::vector<pollfd>& pollfds, int listen_fd6, int listen_fd4) {
    for (const pollfd& pfd : pollfds) {
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
}

void handle_clients(const std::vector<pollfd>& pollfds, int listen_fd6, int listen_fd4) {
    std::vector<int> to_remove;

    for (size_t i = (listen_fd6 != -1) + (listen_fd4 != -1); i < pollfds.size(); ++i) {
        const pollfd& pfd = pollfds[i];
        if (pfd.revents & POLLIN) {
            char buf[4096];
            ssize_t recvd = recv(pfd.fd, buf, sizeof(buf), 0);
            if (recvd <= 0) {
                close(pfd.fd);
                std::cout << "Client disconnected: " << clients[pfd.fd].addr_text << std::endl;
                to_remove.push_back(pfd.fd);
            } else {
                clients[pfd.fd].net_buffer.append(buf, recvd);
                std::cout << "Received " << recvd << " bytes from " << clients[pfd.fd].addr_text << std::endl;
                process_client_buffer(pfd.fd);
            }
        }
    }

    for (int fd : to_remove) {
        clients.erase(fd);
    }
}

void server_loop(int listen_fd6, int listen_fd4) {
    std::vector<pollfd> pollfds;

    while (M) {
        prepare_pollfds(pollfds, listen_fd6, listen_fd4);

        if (poll(pollfds.data(), pollfds.size(), -1) < 0) {
            print_error("poll() error");
            break;
        }

        accept_new_clients(pollfds, listen_fd6, listen_fd4);
        handle_clients(pollfds, listen_fd6, listen_fd4);

        pollfds.clear();  // clean pollfds for next loop
    }
}



void cleanup(int listen_fd6, int listen_fd4) {
    if (listen_fd6 != -1) close(listen_fd6);
    if (listen_fd4 != -1) close(listen_fd4);

    for (auto& [fd, _] : clients) {
        close(fd);
    }
}

int main(int argc, char* argv[]) {
    if (!initialize(argc, argv)) return 1;

    int listen_fd6 = create_server_socket(AF_INET6);
    int listen_fd4 = create_server_socket(AF_INET);

    if (listen_fd6 == -1 && listen_fd4 == -1) return 1;

    display_assigned_port(listen_fd6, listen_fd4);
    prepare_sockets(listen_fd6, listen_fd4);

    server_loop(listen_fd6, listen_fd4);

    cleanup(listen_fd6, listen_fd4);
    return 0;
}