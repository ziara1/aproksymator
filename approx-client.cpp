#include <iostream>
#include <string>
#include <cstring>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <vector>
#include <sstream>


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
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return false;
    }
    return true;
}

bool parse_arguments(int argc, char *argv[]) {
    bool u = false; bool s = false; bool p = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-u" && i + 1 < argc) {
            player_id = argv[++i];
            if (u || !is_valid_player_id(player_id)) {
                print_error("Invalid value for -u (player_id)");
            }
            u = true;
        } else if (arg == "-s" && i + 1 < argc) {
            if (s) {
                print_error("Invalid value for -s (server)");
            }
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

bool handle_coeffs(int sockfd, std::vector<double>& coeffs) {
    std::string response;
    char buffer[1024];
    // 1) Czytaj, aż znajdziemy "\r\n"
    while (true) {
        ssize_t recvd = recv(sockfd, buffer, sizeof(buffer), 0);
        if (recvd < 0) {
            print_error("recv() failed");
            return false;
        }
        if (recvd == 0) {
            print_error("Server closed connection before sending COEFF");
            return false;
        }
        response.append(buffer, recvd);
        size_t pos = response.find("\r\n");
        if (pos != std::string::npos) {
            // Wyciągamy fragment od początku do pozycji "\r\n" (bez samych CRLF)
            response = response.substr(0, pos);
            break;
        } // w przeciwnym razie kontynuujemy czytanie
    }
    const std::string prefix = "COEFF "; // 2) Sprawdź format
    if (response.size() < prefix.size() ||
        response.compare(0, prefix.size(), prefix) != 0) {
        print_error("Unexpected response from server: \"" + response + "\"");
        return false;
        }
    // 3) Parsuj liczby po słowie "COEFF "
    std::string numbers_str = response.substr(prefix.size());
    std::istringstream iss(numbers_str);
    double x;
    while (iss >> x)
        coeffs.push_back(x);

    if (coeffs.empty()) {
        print_error("No coefficients parsed from: \"" + numbers_str + "\"");
        return false;
    }
    return true;
}

int connect_to_server() {
    struct addrinfo hints{}, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = force4 ? AF_INET : (force6 ? AF_INET6 : AF_UNSPEC);

    std::string port_str = std::to_string(port);
    int err = getaddrinfo(server.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0) {
        print_error("getaddrinfo: " + std::string(gai_strerror(err)));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd == -1)
        print_error("Could not connect to server");

    return sockfd;
}

bool send_hello(int sockfd) {
    std::string hello_msg = "HELLO " + player_id + "\r\n";
    ssize_t sent = send(sockfd, hello_msg.c_str(), hello_msg.size(), 0);
    if (sent < 0) {
        print_error("Failed to send HELLO message");
        return false;
    }

    std::cout << "Sent to server: " << hello_msg;
    return true;
}

bool receive_coeffs(int sockfd, std::vector<double>& coeffs) {
    std::string response;
    char buffer[1024];

    while (true) {
        ssize_t recvd = recv(sockfd, buffer, sizeof(buffer), 0);
        if (recvd < 0) {
            print_error("recv() failed");
            return false;
        }
        if (recvd == 0) {
            print_error("Server closed connection before sending COEFF");
            return false;
        }
        response.append(buffer, recvd);
        size_t pos = response.find("\r\n");
        if (pos != std::string::npos) {
            response = response.substr(0, pos);
            break;
        }
    }

    const std::string prefix = "COEFF ";
    if (response.compare(0, prefix.size(), prefix) != 0) {
        print_error("Unexpected response from server: \"" + response + "\"");
        return false;
    }

    std::istringstream iss(response.substr(prefix.size()));
    double x;
    while (iss >> x) coeffs.push_back(x);

    if (coeffs.empty()) {
        print_error("No coefficients parsed from: \"" + response + "\"");
        return false;
    }

    return true;
}

void print_coeffs(const std::vector<double>& coeffs) {
    std::cout << "Received COEFF:";
    for (double c : coeffs) std::cout << " " << c;
    std::cout << "\n";
}

// Zakładamy, że socket_fd to deskryptor już połączonego gniazda TCP
// point - punkt (0..K), value - wartość (-5..5)
bool send_put_command(int socket_fd, int point, double value) {
    // Budujemy komunikat zgodnie z protokołem
    char buf[128];
    int len = std::snprintf(buf, sizeof(buf), "PUT %d %.10g\r\n", point, value);

    // Sprawdzamy, czy komunikat się zmieścił
    if (len <= 0 || len >= (int)sizeof(buf)) {
        print_error("Failed to format PUT command");
        return false;
    }

    // Wysyłamy komunikat do serwera
    ssize_t sent = send(socket_fd, buf, len, 0);
    if (sent != len) {
        print_error("Failed to send PUT command to server");
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    if (!parse_arguments(argc, argv)) return 1;
    int sockfd = connect_to_server();
    if (sockfd < 0) return 1;

    if (!send_hello(sockfd)) {
        close(sockfd);
        return 1;
    }
    send_put_command(sockfd, 1, 4.0);
    std::vector<double> coeffs;
    if (!receive_coeffs(sockfd, coeffs)) {
        close(sockfd);
        return 1;
    }
    print_coeffs(coeffs);

    close(sockfd);
    return 0;
}
