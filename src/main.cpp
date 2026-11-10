#include "cli.hpp"
#include "gui.hpp"
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    // Check if CLI mode requested
    if (argc > 1) {
        std::string first_arg = argv[1];
        if (first_arg == "--cli" || first_arg == "-c") {
            // Shift arguments for cli_main
            std::vector<char*> cli_args;
            cli_args.push_back(argv[0]);
            for (int i = 2; i < argc; ++i) {
                cli_args.push_back(argv[i]);
            }
            return cli_main(static_cast<int>(cli_args.size()), cli_args.data());
        } else if (first_arg == "--help" || first_arg == "-h") {
            std::vector<char*> cli_args = { argv[0], (char*)"--help" };
            return cli_main(2, cli_args.data());
        }
    }

    // Default: Launch GUI
    return gui_main(argc, argv);
}
