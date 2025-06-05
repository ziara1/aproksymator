#include <iostream>
#include <string>
#include <climits>

int port = 0;
int K = 100;
int N = 4;
int M = 131;
std::string filename{};

static void print_error(const std::string& msg) {
    std::cerr << "ERROR: " << msg << "\n";
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