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
#include <algorithm>


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
    int sent_put{};
    // treść odpowiedzi, którą musimy wysłać
    std::string pending_response{};
    // kiedy wysłać pending_response
    std::chrono::steady_clock::time_point send_time{};
    bool has_pending{false}; // czy jest odpowiedź do wysłania
    client_info() = default;

};

std::map<int, client_info> clients; // Mapa znanych nam klientów.
int port = 0;
int K = 100;
int N = 4;
int M = 131;
int currM = 131;
std::string filename{};


static void print_error(const std::string& msg) {
    std::cerr << "ERROR: " << msg << std::endl;
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
    std::string msg = line + "\r\n";
    int sent = send(clients[key].socket_fd, msg.c_str(), msg.size(), 0);
    if (sent < 0) {
        print_error("Błąd wysyłania COEFF " + clients[key].addr_text);
        return false;
    }
    // Sparsowanie liczby do wektora clients[key].coeffs pomijając "COEFF"
    std::istringstream iss(line);
    std::string token;
    iss >> token; // pobierz "COEFF"
    double a;
    // oczekujemy dokładnie N+1 liczb (a0..aN)
    for (int i = 0; i <= N; i++) {
        if (!(iss >> a)) {
            print_error("Błąd parsowania współczynnika " + std::to_string(i));
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
    for (unsigned int i = 6; i < msg.size(); i++) {
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
    std::cout << clients[key].addr_text <<
        " is now known as " << clients[key].username << ".\n";
    // Po udanym HELLO od razu wysyłamy COEFF z pliku:
    if (!send_coeff_line(key)) {
        // jeśli coś nie poszło, usuwamy klienta
        print_error("Invalid COEFF message\n");
        close(clients[key].socket_fd);
        clients.erase(key);
        return false;
    }
    std::cout << clients[key].username << " get coefficients";
    for (int i = 0; i <= N; ++i) {
        std::cout << " " << clients[key].coeffs[i];
    }
    std::cout << ".\n";
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

// Pomocnicza funkcja sprawdzająca zakres point i value
static bool validate_put_range(int key, int point, double value) {
    if (point < 0 || point > K || value < -5.0 || value > 5.0) {
        clients[key].penalty += 10;
        std::ostringstream oss;
        oss << "BAD_PUT " << point << " " << value << "\r\n";
        std::string buf = oss.str();
        int sent = send(clients[key].socket_fd, buf.c_str(), buf.size(), 0);
        if (sent < 0) {
            print_error("Błąd wysyłania BAD_PUT " + clients[key].addr_text);
            return false;
        }
    }
    return true;
}

// Pomocnicza funkcja sprawdzająca stan klienta
static bool validate_put_state(int key, int point, double value) {
    if (clients[key].state != State::AwaitingPut) {
        clients[key].penalty += 20;
        std::ostringstream oss;
        oss << "PENALTY " << point << " " << value << "\r\n";
        std::string buf = oss.str();
        int sent = send(clients[key].socket_fd, buf.c_str(), buf.size(), 0);
        if (sent < 0) {
            print_error("Błąd wysyłania PENALTY " + clients[key].addr_text);
            return false;
        }
    }
    return true;
}

static void update_approximation_and_respond(int key, int point, double val) {
    // Dodaj wartość do funkcji aproksymującej
    clients[key].approx[point] += val;
    std::cout << clients[key].username
          << " puts " << val
          << " in " << point
          << ", current state";
    for (int x = 0; x <= K; ++x) {
        std::cout << " " << clients[key].approx[x];
    }
    std::cout << ".\n";
    auto t = std::chrono::steady_clock::now();
    clients[key].send_time = t + std::chrono::seconds(clients[key].lowercase);

    // Zbuduj komunikat
    std::ostringstream oss;
    oss << "STATE";
    for (int i = 0; i <= K; ++i) {
        oss << " " << clients[key].approx[i];
    }
    oss << "\r\n";
    clients[key].has_pending = true;
    clients[key].pending_response = oss.str();
}

// Główna funkcja obsługi PUT
static bool handle_put(int key, std::string &msg) {
    if (clients.find(key) == clients.end()) {
        print_error("Unknown client");
        return false;
    }

    size_t i = 4;
    int point = 0;
    double value = 0.0;

    if (!parse_point(msg, i, point)) return false;
    if (!parse_value(msg, i, value)) return false;
    if (!validate_put_range(key, point, value)) return false;
    if (!validate_put_state(key, point, value)) return false;
    if (clients[key].state != State::AwaitingPut ||
        point < 0 || point > K || value < -5.0 || value > 5.0)
        return true;

    std::cout << "Received PUT: point="
    << point << " value=" << value << std::endl;
    currM--;
    clients[key].sent_put++;
    update_approximation_and_respond(key, value, point);

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
            currM = M;
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
        print_error("Nie udało się otworzyć pliku: " + filename);
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

void prepare_pollfds(std::vector<pollfd>& pollfds,
    int listen_fd6, int listen_fd4) {
    pollfds.clear();
    if (listen_fd6 != -1)
        pollfds.push_back({listen_fd6, POLLIN, 0});
    if (listen_fd4 != -1)
        pollfds.push_back({listen_fd4, POLLIN, 0});

    for (const auto& [fd, info] : clients) {
        pollfds.push_back({fd, POLLIN, 0});
    }
}

void accept_new_clients(std::vector<pollfd>& pollfds,
    int listen_fd6, int listen_fd4) {
    for (const pollfd& pfd : pollfds) {
        if ((listen_fd6 != -1 && pfd.fd == listen_fd6 &&
            (pfd.revents & POLLIN)) || (listen_fd4 != -1 &&
                pfd.fd == listen_fd4 && (pfd.revents & POLLIN))) {
            sockaddr_storage client_addr{};
            socklen_t addrlen = sizeof(client_addr);
            int client_fd = accept(pfd.fd, (sockaddr*)&client_addr, &addrlen);
            if (client_fd >= 0) {
                set_nonblocking(client_fd);
                std::string key = peer_key(client_addr);
                std::cout << "New client [" << key << "].\n";
                client_info info;
                info.approx = std::vector<double>(K + 1, 0);
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

void handle_clients(const std::vector<pollfd>& pollfds,
    int listen_fd6, int listen_fd4) {
    std::vector<int> to_remove;

    for (size_t i = (listen_fd6 != -1) +
        (listen_fd4 != -1); i < pollfds.size(); ++i) {
        const pollfd& pfd = pollfds[i];
        if (pfd.revents & POLLIN) {
            char buf[4096];
            ssize_t recvd = recv(pfd.fd, buf, sizeof(buf), 0);
            if (recvd <= 0) {
                close(pfd.fd);
                std::cout << "Client disconnected: "
                << clients[pfd.fd].addr_text << std::endl;
                to_remove.push_back(pfd.fd);
            } else {
                clients[pfd.fd].net_buffer.append(buf, recvd);
                process_client_buffer(pfd.fd);
            }
        }
    }

    for (int fd : to_remove) {
        currM += clients[fd].sent_put;
        clients.erase(fd);
    }
}

static void send_pending_responses() {
    auto now = std::chrono::steady_clock::now();
    for (auto &kv : clients) {
        auto &info = kv.second;

        if (info.has_pending && now >= info.send_time) {
            std::cout << "Sending state";
            for (int x = 0; x <= K; ++x) {
                std::cout << " " << info.approx[x];
            }
            std::cout << " to " << info.username << ".\n";
            const std::string &msg = info.pending_response;
            ssize_t sent = send(info.socket_fd, msg.c_str(), msg.size(), 0);
            if (sent < 0) {
                print_error("Błąd wysyłania wiadomości " + info.addr_text);
            }
            info.has_pending = false;
            info.pending_response.clear();
        }
    }
}

static void end_game_and_reset() {
    // Wynik każdego klienta: ∑_{x=0..K} (approx[x] – f(x))^2  + penalty
    std::vector<std::pair<std::string, double>> results;
    results.reserve(clients.size());

    for (auto &kv : clients) {
        auto &info = kv.second;

        double sum_squares = 0.0;
        for (int x = 0; x <= K; ++x) {
            // Oblicz f(x) = a[0] + a[1]*x + a[2]*x^2 + … + a[N]*x^N
            double fx = 0.0;
            double x_pow = 1.0;
            for (int i = 0; i <= N; ++i) {
                fx += kv.second.coeffs[i] * x_pow;
                x_pow *= static_cast<double>(x);
            }
            double diff = info.approx[x] - fx;
            sum_squares += diff * diff;
        }

        double total_score = sum_squares + static_cast<double>(info.penalty);
        results.emplace_back(info.username, total_score);
    }

    // 2) Posortuj według player_id (rosnąco, ASCII)
    std::sort(results.begin(), results.end(),
              [](auto &p1, auto &p2) {
                  return p1.first < p2.first;
              });
    std::cout << "Game end, scoring:";
    for (auto &pr : results) {
        std::cout << " " << pr.first << " " << pr.second;
    }
    std::cout << ".\n";
    std::ostringstream oss;
    oss << "SCORING";
    for (auto &pr : results) {
        oss << " " << pr.first << " " << pr.second;
    }
    oss << "\r\n";
    std::string scoring_msg = oss.str();

    // 4) Wyślij do wszystkich klientów, zamknij gniazda
    for (auto &kv : clients) {
        auto &info = kv.second;
        ssize_t sent = send(info.socket_fd,
            scoring_msg.c_str(), scoring_msg.size(), 0);
        if (sent < 0) {
            print_error("Błąd wysyłania SCORING do klienta " + info.addr_text);
        }
        close(info.socket_fd);
    }
    clients.clear();
    sleep(1);
    currM = M;
}

void server_loop(int listen_fd6, int listen_fd4) {
    std::vector<pollfd> pollfds;

    while (currM) {
        prepare_pollfds(pollfds, listen_fd6, listen_fd4);

        if (poll(pollfds.data(), pollfds.size(), -1) < 0) {
            print_error("poll() error");
            break;
        }
        send_pending_responses();

        accept_new_clients(pollfds, listen_fd6, listen_fd4);
        handle_clients(pollfds, listen_fd6, listen_fd4);

        pollfds.clear();  // clean pollfds for next loop
    }
    end_game_and_reset();

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

    do {
        server_loop(listen_fd6, listen_fd4);
    } while (true);

    cleanup(listen_fd6, listen_fd4);
    return 0;
}