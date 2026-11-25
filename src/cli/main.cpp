#include "talas/cli/Cli.h"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::string_view> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index)
        args.emplace_back(argv[index]);

    return talas::cli::runCli(args, std::cout, std::cerr);
}
