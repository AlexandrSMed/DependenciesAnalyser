#include "Analyser.hpp"
#include "Utils.hpp"
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <regex>
#include <set>

#pragma region Static
std::vector<tdw::Analyser::Include> tdw::Analyser::getIncludes(const path_type& _path) {
    std::ifstream ifs;
    // Makes the `ifstream` throw an exception in case it fails to open the file
    ifs.exceptions(ifs.exceptions() | std::ios::failbit);
    ifs.open(_path);
    using if_stream_buf_it = typename std::istreambuf_iterator<std::ifstream::char_type>;
    const std::string fileData{ {if_stream_buf_it{ifs}}, if_stream_buf_it{} };
    ifs.close();

    // https://regex101.com/r/8IegEz/2
    const std::regex noiseRegex{ R"reg(((?:(?:R"(.*)\()(?:[\s\S\n]*?)(?:\)\2")|(?:R"(.*)\()(?:[\s\S\n]*))|(?:(?:\/\*)(?:[\s\S\n]*?)(?:\*\/)|(?:\/\*)(?:[\s\S\n]*))))reg" };
    const auto filteredFileData = std::regex_replace(fileData, noiseRegex, "");

    // https://regex101.com/r/sd5Gpb/1
    const std::regex includeRegex{ R"reg((?:^|\n)[^\S\r\n]*#[^\S\r\n]*include[^\S\r\n]*("([\w.\/\\]+(\.hpp|\.cpp))"|\<([\w.\/\\]+(\.hpp|\.cpp))\>))reg" };
    constexpr auto quoteMatchIndex = static_cast<std::smatch::size_type>(2);
    constexpr auto bracketMatchIndex = static_cast<std::smatch::size_type>(4);

    std::vector<Include> includes;
    for(std::sregex_iterator it{ filteredFileData.cbegin(), filteredFileData.end(), includeRegex }; it != std::sregex_iterator{}; ++it) {
        const auto& match = *it;

        if(match.size() <= std::max(quoteMatchIndex, bracketMatchIndex)) {
            continue;
        }

        if(match[quoteMatchIndex].matched) {
            includes.emplace_back(match.str(quoteMatchIndex), Include::Type::q_char);
        } else if(match[bracketMatchIndex].matched) {
            includes.emplace_back(match.str(bracketMatchIndex), Include::Type::h_char);
        }
    }

    return includes;
}

tdw::Analyser::path_type tdw::Analyser::printDependencyTree(const Include& _sourceFile,
                                        const path_type _currentPath,
                                        const std::vector<path_type>& _includePaths,
                                        include_counter_map_type& _includeCounter,
                                        include_chain_set_type& _includeChain,
                                        unsigned _depth) {
    constexpr auto depthStep = static_cast<decltype(_depth)>(2);
    using namespace std::filesystem;

    const auto parentPath = findIncludeParentPath(_sourceFile, _currentPath, _includePaths);
    const auto cycleInclude = !_includeChain.insert(std::make_pair(_sourceFile.path, parentPath)).second;
    if(_sourceFile.path.is_relative()) {
        printIncludeBranchRecord(_sourceFile.path, _depth, !parentPath.empty(), cycleInclude);
    } else {
        printIncludeBranchRecord(_sourceFile.path, _depth, !parentPath.empty(), cycleInclude, parentPath);
    }

    if(!parentPath.empty() && !cycleInclude) {
        const auto filePath = parentPath / _sourceFile.path;

        // For each file the search should happen relative to the directory it is in
        const auto directoryPath = filePath.parent_path();
        const auto includes = getIncludes(filePath);
        for(const auto& include : includes) {
            auto subIncludeChain{ _includeChain }; // each include branch needs to track it's chain independently
            const auto includeParentPath = printDependencyTree(
                include, directoryPath, _includePaths, _includeCounter, subIncludeChain, _depth + depthStep);
            // Cycle includes still count, but nothing after it (because it gets printed and needs to be consistent)
            const auto counterKey = std::make_pair(include.path, includeParentPath);
            _includeCounter[counterKey]++;
        }
    }

    return parentPath;
}

tdw::Analyser::path_type tdw::Analyser::findIncludeParentPath(const Include& _sourceFile, const path_type _currentPath, const std::vector<path_type>& _includePaths) {
    using namespace std::filesystem;

    // Follows C standard "6.10.2 Source file inclusion" - http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf#page=182
    if(_sourceFile.type == Include::Type::q_char) {
        const auto searchPath = _currentPath / _sourceFile.path;

        if(is_regular_file(searchPath)) {
            return _currentPath;
        }
    }

    for(const auto& includePath : _includePaths) {
        const auto searchPath = includePath / _sourceFile.path;
        if(is_regular_file(searchPath)) {
            return includePath;
        }
    }

    return path_type();
}

#pragma endregion

#pragma region Lifecycle
tdw::Analyser::Analyser(const path_type& _path) : path{ std::filesystem::canonical(_path) } {
    using namespace std::filesystem;
    using utils::operator==;

    utils::directoryArgumentAssert(_path);

    source_files_type tmp;
    for(const auto& entry : recursive_directory_iterator(path)) {
        // Copies the reference value (`.extension()` returns a temporary, while `native()` returns a reference to the content of it)
        const auto extension = path_type::string_type(entry.path().extension().native());
        const auto comparisonResult = (".hpp" == extension) || (".cpp" == extension);
        if(!entry.is_regular_file() || !comparisonResult) {
            // Ignore folders, irregular and irrelevant files
            continue;
        }

        tmp.emplace(entry.path(), Include::Type::q_char);
    }
    sourceFiles = std::move(tmp);
}
#pragma endregion

#pragma region Actions
void tdw::Analyser::printDependencyTree(const std::vector<path_type>& _includePaths) const {
    using namespace std::filesystem;

    include_counter_map_type includesCounter;

    for(const auto& sourceFile : sourceFiles) {
        const auto counterKey = std::make_pair(relative(sourceFile.path, path), path);
        includesCounter[counterKey] += 0; // initializes counter for the source in case it doesn't exist
        include_chain_set_type includeChain;
        printDependencyTree(sourceFile, path, _includePaths, includesCounter, includeChain);
    }

    std::cout << std::endl;
    
    const auto counterLess = [&includesCounter](const include_counter_map_type::value_type& left, const include_counter_map_type::value_type& right) {
        if(left.second == right.second) {
            return includesCounter.key_comp()(left.first, right.first);
        } else {
            return left.second > right.second;
        }
    };
    std::set<include_counter_map_type::value_type, decltype(counterLess)> sortedIncludeCounters{counterLess};
    for(const auto& counter : includesCounter) {
        sortedIncludeCounters.insert(counter);
    }

    for(const auto& counter : sortedIncludeCounters) {
        std::cout << counter.first.first << " " << counter.second << std::endl;
    }
}
#pragma endregion
