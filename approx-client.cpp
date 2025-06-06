#include <iostream>
#include <string>
#include <cstring>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <vector>
#include <sstream>
#include <queue>
#include <fcntl.h>
#include <cerrno>
#include <algorithm>


std::string player_id, server;
int port = -1;
bool force4 = false, force6 = false, auto_mode = false;

static void print_error(const std::string& msg) {
    std::cerr << "ERROR: " << msg << "\n";
}

static bool is_valid_player_id(const std::string& player_id) {
    if (player_id.empty()) return false;
    for (char c : player_id) {
        if (!(std::isdigit(c) ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z')))
            return false;
    }
    return true;
}

// --------------------------
// 2. Parsowanie argumentów
// --------------------------

bool parse_arguments(int argc, char *argv[]) {
    bool u = false, s = false, p = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-u" && i + 1 < argc) {
            player_id = argv[++i];
            if (u || !is_valid_player_id(player_id)) {
                print_error("Invalid value for -u (player_id)");
                return false;
            }
            u = true;
        } else if (arg == "-s" && i + 1 < argc) {
            server = argv[++i];
            s = true;
        } else if (arg == "-p" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
            if (port < 1 || port > 65535 || p) {
                print_error("Invalid value for -p (port)");
                return false;
            }
            p = true;
        } else if (arg == "-4") {
            force4 = true;
        } else if (arg == "-6") {
            force6 = true;
        } else if (arg == "-a") {
            auto_mode = true;
        } else {
            print_error("Unknown or incomplete argument: " + arg);
            return false;
        }
    }
    if (!u || !s || !p) {
        if (!u) print_error("Missing required -u argument (player_id)");
        if (!s) print_error("Missing required -s argument (server)");
        if (!p) print_error("Missing required -p argument (port)");
        return false;
    }
    return true;
}

// -----------------------------
// 3. Łączenie z serwerem
// -----------------------------

int connect_to_server() {
    struct addrinfo hints{}, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = force4 ? AF_INET : (force6 ? AF_INET6 : AF_UNSPEC);

    std::string port_str = std::to_string(port);
    int err = getaddrinfo(server.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0) {
        print_error("getaddrinfo: " + std::string(gai_strerror(err)));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            char host[NI_MAXHOST], serv[NI_MAXSERV];
            getnameinfo(rp->ai_addr, rp->ai_addrlen,
                        host, sizeof(host),
                        serv, sizeof(serv),
                        NI_NUMERICSERV | NI_NUMERICHOST);
            std::cout << "Connected to server ["
            << host << ":" << serv << "].\n";
            break;
        }
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        print_error("Could not connect to server");
    }
    return sockfd;
}

// --------------------
// 4. Wysyłanie HELLO
// --------------------

bool send_hello(int sockfd) {
    std::string hello_msg = "HELLO " + player_id + "\r\n";
    ssize_t sent = send(sockfd, hello_msg.c_str(), hello_msg.size(), 0);
    if (sent < 0) {
        print_error("Failed to send HELLO message");
        return false;
    }
    std::cout << player_id << " sends HELLO.\n";
    return true;
}

// --------------------------------------------
// 5. Parsowanie i wysyłanie polecenia PUT
// --------------------------------------------

// Sprawdź, czy linia z stdin to poprawne dwie liczby
bool parse_put_line(const std::string &line, int &point, double &value) {
    std::istringstream iss(line);
    if (!(iss >> point >> value)) return false;
    std::string extra;
    if (iss >> extra) return false;
    return true;
}

// Wysyła komunikat PUT
bool send_put(int sockfd, int point, double val) {
    char buf[128];
    int len = std::snprintf(buf, sizeof(buf), "PUT %d %.10g\r\n", point, val);
    if (len <= 0 || len >= (int)sizeof(buf)) {
        print_error("Failed to format PUT command");
        return false;
    }
    std::cout << player_id << " puts " << val << " in " << point << ".\n";
    ssize_t sent = send(sockfd, buf, len, 0);
    if (sent != len) {
        print_error("Failed to send PUT command to server");
        return false;
    }
    return true;
}

// ---------------------------------------------------
// 6. Odbieranie komunikatów od serwera (COEFF, STATE, SCORING)
// ---------------------------------------------------

static std::vector<double> current_state;
static bool coeff_received = false;
static std::queue<std::pair<int,double>> queued_puts;
static std::string sock_buf;

// ----------- AUTO_MODE STRATEGIA -----------
static std::vector<double> coeffs;
static int auto_next_point = 0;
static bool auto_waiting_for_response = false;
static int auto_K = -1;

// Funkcja obliczająca wartość wielomianu z coeffs dla x
double eval_poly(const std::vector<double>& coeffs, int x) {
    double res = 0.0, xp = 1.0;
    for (size_t i = 0; i < coeffs.size(); ++i) {
        res += coeffs[i] * xp;
        xp *= x;
    }
    return res;
}

// Zmieniona obsługa COEFF: parsuje coeffs i ustala auto_K
static void handle_coeff_line(const std::string &line, int sockfd) {
    coeffs.clear();
    std::istringstream iss(line.substr(6));
    double x;
    while (iss >> x) coeffs.push_back(x);
    std::cout << player_id << " get coefficients";
    for (double c : coeffs) std::cout << " " << c;
    std::cout << ".\n";

    coeff_received = true;
    auto_next_point = 0;
    auto_waiting_for_response = false;
    // auto_K: jeżeli nie wiadomo, to po pierwszym STATE rozpoznasz, na razie -1
    // Ale jeśli wolisz, możesz ustalić z liczby STATE (w handle_state_line), lub ustalić z K (np. z argumentów serwera)
    // Tu: K nie jest przekazywany, więc ogarniemy to w handle_state_line

    // Po otrzymaniu COEFF wysyłamy wszystkie zakolejkowane PUT-y
    while (!queued_puts.empty()) {
        auto [pt, val] = queued_puts.front();
        queued_puts.pop();
        send_put(sockfd, pt, val);
    }
    if (auto_mode) {
        send_put(sockfd, 0, 0.0);
        auto_waiting_for_response = true;
        auto_next_point = 1;
    }
}

// Zmieniona obsługa STATE: zapamiętaj ile punktów (czyli auto_K)
static void handle_state_line(const std::string &line) {
    current_state.clear();
    std::istringstream iss(line.substr(6));
    double v;
    while (iss >> v) current_state.push_back(v);
    std::cout << "Received STATE:";
    for (double c : current_state) std::cout << " " << c;
    std::cout << ".\n";
    auto_waiting_for_response = false;
    if (auto_K == -1) auto_K = (int)current_state.size() - 1; // ustalamy K z pierwszej odpowiedzi STATE
}
static void handle_bad_put(const std::string &line) {
    std::cout << "Received BAD_PUT: " << line << std::endl;
    auto_waiting_for_response = false;
}
static void handle_penalty(const std::string &line) {
    std::cout << "Received PENALTY: " << line << std::endl;
    auto_waiting_for_response = false;
}

static bool handle_scoring_line(const std::string &line, int sockfd) {
    // Parsujemy „SCORING pid1 res1 pid2 res2 ...”
    std::vector<std::pair<std::string,double>> results;
    std::istringstream iss(line.substr(8));
    while (true) {
        std::string pid;
        double sc;
        if (!(iss >> pid >> sc)) break;
        results.emplace_back(pid, sc);
    }
    std::cout << "Game end, scoring:";
    for (auto &pr : results) {
        std::cout << " " << pr.first << " " << pr.second;
    }
    std::cout << ".\n";
    close(sockfd);
    return false;  // sygnał do zakończenia pętli
}

static bool process_server_data(int sockfd) {
    char buf[1024];
    ssize_t recvd = recv(sockfd, buf, sizeof(buf), 0);
    if (recvd < 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            print_error("recv() failed");
            return false;
        }
        return true;
    }
    if (recvd == 0) {
        print_error("ERROR: unexpected server disconnect");
        close(sockfd);
        return false;
    }
    sock_buf.append(buf, recvd);

    // Parsujemy linie zakończone "\r\n"
    while (true) {
        size_t pos = sock_buf.find("\r\n");
        if (pos == std::string::npos) break;
        std::string line = sock_buf.substr(0, pos);
        sock_buf.erase(0, pos + 2);

        if (!coeff_received) {
            if (line.rfind("COEFF ", 0) == 0) {
                handle_coeff_line(line, sockfd);
            } else if (line.rfind("PENALTY ", 0) == 0) {
                handle_penalty(line);
            } else if (line.rfind("BAD_PUT ", 0) == 0) {
                handle_bad_put(line);
            } else {
                print_error("Unexpected response: \"" + line + "\"");
                close(sockfd);
                return false;
            }
        } else {
            if (line.rfind("STATE ", 0) == 0) {
                handle_state_line(line);
            } else if (line.rfind("SCORING ", 0) == 0) {
                return handle_scoring_line(line, sockfd);
            } else if (line.rfind("PENALTY ", 0) == 0) {
                handle_penalty(line);
            } else if (line.rfind("BAD_PUT ", 0) == 0) {
                handle_bad_put(line);
            } else {
                print_error("Unexpected response: \"" + line + "\"");
                close(sockfd);
                return false;
            }
        }
    }
    return true;
}

// -----------------------------------------
// 7. Odczyt z stdin (gdy nie auto_mode)
// -----------------------------------------

static std::string stdin_buf;

static void process_stdin_data(int sockfd) {
    char buf[512];
    ssize_t recvd = read(0, buf, sizeof(buf));
    if (recvd < 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            print_error("read() from stdin failed");
        }
        return;
    }
    if (recvd == 0) return;
    stdin_buf.append(buf, recvd);

    // Parsujemy linie zakończone '\n'
    while (true) {
        size_t pos = stdin_buf.find('\n');
        if (pos == std::string::npos) break;
        std::string line = stdin_buf.substr(0, pos);
        stdin_buf.erase(0, pos + 1);
        if (line.empty()) continue;

        int pt; double val;
        if (!parse_put_line(line, pt, val)) {
            std::cout << "ERROR: invalid input line " << line << "\n";
            continue;
        }
        if (!coeff_received) {
            queued_puts.emplace(pt, val);
        } else {
            send_put(sockfd, pt, val);
        }
    }
}

// ----------------------------------------------
// 8. Główna pętla: poll + wywoływanie funkcji
// ----------------------------------------------

static void client_loop(int sockfd) {
    // Ustawiamy stdin i socket jako nieblokujące
    int flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    struct pollfd fds[2];
    fds[0].fd = 0;         // stdin
    fds[0].events = POLLIN;
    fds[1].fd = sockfd;    // socket
    fds[1].events = POLLIN;

    while (true) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            print_error("poll() error");
            break;
        }
        // 1) Dane od serwera
        if (fds[1].revents & POLLIN) {
            if (!process_server_data(sockfd)) {
                return;
            }
        }
        // 2) Dane ze stdin (tylko jeśli nie auto_mode)
        if (!auto_mode && (fds[0].revents & POLLIN)) {
            process_stdin_data(sockfd);
        }
        // --- AUTO_MODE STRATEGIA ---
        if (auto_mode && coeff_received && !auto_waiting_for_response && auto_K != -1 && auto_next_point <= auto_K) {
            // Wyślij PUT x f(x) saturowane do [-5,5]
            double val = eval_poly(coeffs, auto_next_point);
            val = std::max(-5.0, std::min(5.0, val));
            send_put(sockfd, auto_next_point, val);
            auto_waiting_for_response = true;
            ++auto_next_point;
        }
    }
}

// ------------------------------
// 9. Funkcja main()
// ------------------------------

int main(int argc, char* argv[]) {
    if (!parse_arguments(argc, argv)) return 1;

    int sockfd = connect_to_server();
    if (sockfd < 0) return 1;

    if (!send_hello(sockfd)) {
        close(sockfd);
        return 1;
    }

    client_loop(sockfd);
    close(sockfd);
    return 0;
}
