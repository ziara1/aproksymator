#include <iostream>
#include <string>
#include <climits>

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
            if (force6) {
                print_error("Cannot use both -4 and -6");
                return false;
            }
            force4 = true;
        } else if (arg == "-6") {
            if (force4) {
                print_error("Cannot use both -4 and -6");
                return false;
            }
            force6 = true;
        } else if (arg == "-a") {
            auto_mode = true;
        } else {
            print_error("Unknown or incomplete argument: " + arg);
            return false;
        }
    }

    if (!u) {
        print_error("Missing required -u argument (player_id)");
        return false;
    }
    if (!s) {
        print_error("Missing required -s argument (server)");
        return false;
    }
    if (!p) {
        print_error("Missing required -p argument (port)");
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