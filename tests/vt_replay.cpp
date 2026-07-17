#include <sakura/terminal/core.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string Trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char ch) {
                                            return std::isspace(ch) != 0;
                                        });
    value.erase(value.begin(), first);
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                       [](unsigned char ch) {
                                           return std::isspace(ch) != 0;
                                       }).base();
    value.erase(last, value.end());
    return value;
}

bool DecodeHex(const std::string& token, std::string& result)
{
    if (token == "-") {
        result.clear();
        return true;
    }
    if (token.empty() || token.size() % 2 != 0)
        return false;

    result.clear();
    result.reserve(token.size() / 2);
    for (std::size_t index = 0; index < token.size(); index += 2) {
        const auto high = static_cast<unsigned char>(token[index]);
        const auto low = static_cast<unsigned char>(token[index + 1]);
        const auto hex = [](unsigned char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        const int high_value = hex(high);
        const int low_value = hex(low);
        if (high_value < 0 || low_value < 0)
            return false;
        result.push_back(static_cast<char>((high_value << 4) | low_value));
    }
    return true;
}

unsigned int ParseDecimal(const std::string& token, const char* what)
{
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(token, &consumed, 10);
    if (consumed != token.size())
        throw std::runtime_error(std::string("invalid ") + what);
    return static_cast<unsigned int>(value);
}

uint32_t ParseHexNumber(const std::string& token, const char* what)
{
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(token, &consumed, 16);
    if (consumed != token.size())
        throw std::runtime_error(std::string("invalid ") + what);
    return static_cast<uint32_t>(value);
}

TerminalCursorStyle ParseCursorStyle(const std::string& token)
{
    if (token == "block") return TerminalCursorStyle::Block;
    if (token == "underline") return TerminalCursorStyle::Underline;
    if (token == "bar") return TerminalCursorStyle::Bar;
    throw std::runtime_error("invalid cursor style");
}

const TerminalCell& CellAt(const TerminalSnapshot& snapshot,
                           unsigned int column, unsigned int row)
{
    if (column >= snapshot.columns || row >= snapshot.rows)
        throw std::runtime_error("cell coordinate is outside the snapshot");
    return snapshot.cells[row * snapshot.columns + column];
}

class ReplayRunner final {
public:
    ReplayRunner()
        : core_([this](const char* data, std::size_t length) {
              writes_.append(data, length);
          })
    {
    }

    void Run(const std::string& path)
    {
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("could not open replay fixture: " + path);

        std::string line;
        unsigned int line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            try {
                RunLine(Trim(line));
            } catch (const std::exception& error) {
                throw std::runtime_error(path + ":" +
                                         std::to_string(line_number) + ": " +
                                         error.what());
            }
        }
    }

private:
    void RunLine(const std::string& line)
    {
        if (line.empty() || line[0] == '#')
            return;

        std::istringstream command(line);
        std::string operation;
        command >> operation;
        if (operation == "resize") {
            std::string columns;
            std::string rows;
            command >> columns >> rows;
            if (columns.empty() || rows.empty() ||
                !core_.Resize(ParseDecimal(columns, "column count"),
                              ParseDecimal(rows, "row count")))
                throw std::runtime_error("resize failed");
            return;
        }

        if (operation == "output" || operation == "paste") {
            std::string encoded;
            command >> encoded;
            std::string data;
            if (encoded.empty() || !DecodeHex(encoded, data))
                throw std::runtime_error("invalid hexadecimal payload");
            if (operation == "output")
                core_.FeedOutput(data.data(), data.size());
            else
                core_.Paste(data);
            return;
        }

        if (operation == "select") {
            std::string phase;
            std::string column;
            std::string row;
            command >> phase >> column >> row;
            if (phase == "start")
                core_.StartSelection(ParseDecimal(column, "selection column"),
                                     ParseDecimal(row, "selection row"));
            else if (phase == "end")
                core_.UpdateSelection(ParseDecimal(column, "selection column"),
                                      ParseDecimal(row, "selection row"));
            else
                throw std::runtime_error("selection phase must be start or end");
            return;
        }

        if (operation != "expect")
            throw std::runtime_error("unknown replay operation: " + operation);

        std::string expectation;
        command >> expectation;
        if (expectation == "cell") {
            std::string column;
            std::string row;
            std::string codepoint;
            std::string width;
            std::string encoded_text;
            command >> column >> row >> codepoint >> width >> encoded_text;
            std::string expected_text;
            if (encoded_text.empty() || !DecodeHex(encoded_text, expected_text))
                throw std::runtime_error("invalid cell text hexadecimal payload");
            const TerminalSnapshot snapshot = core_.TakeSnapshot();
            const TerminalCell& cell = CellAt(
                snapshot, ParseDecimal(column, "cell column"),
                ParseDecimal(row, "cell row"));
            const uint32_t expected_codepoint = ParseHexNumber(
                codepoint, "cell codepoint");
            const unsigned int expected_width = ParseDecimal(width, "cell width");
            if (cell.codepoint != expected_codepoint ||
                cell.width != expected_width || cell.text != expected_text) {
                std::ostringstream message;
                message << "cell mismatch: got codepoint 0x" << std::hex
                        << cell.codepoint << ", width " << std::dec << cell.width
                        << ", text size " << cell.text.size();
                throw std::runtime_error(message.str());
            }
            return;
        }

        if (expectation == "cursor") {
            std::string column;
            std::string row;
            std::string style;
            command >> column >> row >> style;
            const TerminalSnapshot snapshot = core_.TakeSnapshot();
            if (snapshot.cursor_x != ParseDecimal(column, "cursor column") ||
                snapshot.cursor_y != ParseDecimal(row, "cursor row") ||
                snapshot.cursor_style != ParseCursorStyle(style))
                throw std::runtime_error("cursor mismatch");
            return;
        }

        if (expectation == "title" || expectation == "selection") {
            std::string encoded;
            command >> encoded;
            std::string expected;
            if (encoded.empty() || !DecodeHex(encoded, expected))
                throw std::runtime_error("invalid expectation hexadecimal payload");
            const std::string actual = expectation == "title"
                ? core_.Title() : core_.CopySelection();
            if (actual != expected)
                throw std::runtime_error(expectation + " mismatch");
            return;
        }

        if (expectation == "writes") {
            std::string encoded;
            command >> encoded;
            std::string expected;
            if (encoded.empty() || !DecodeHex(encoded, expected))
                throw std::runtime_error("invalid writes hexadecimal payload");
            if (writes_ != expected)
                throw std::runtime_error("transport writes mismatch");
            return;
        }

        if (expectation == "metric") {
            std::string name;
            std::string value;
            command >> name >> value;
            const uint64_t expected = ParseDecimal(value, "metric value");
            const TerminalMetrics metrics = core_.GetMetrics();
            uint64_t actual = 0;
            if (name == "output_bytes") actual = metrics.output_bytes;
            else if (name == "output_chunks") actual = metrics.output_chunks;
            else if (name == "paste_bytes") actual = metrics.paste_bytes;
            else if (name == "selection_copies") actual = metrics.selection_copies;
            else if (name == "title_changes") actual = metrics.title_changes;
            else if (name == "cursor_style_changes")
                actual = metrics.cursor_style_changes;
            else
                throw std::runtime_error("unknown metric: " + name);
            if (actual != expected)
                throw std::runtime_error("metric mismatch: " + name);
            return;
        }

        throw std::runtime_error("unknown expectation: " + expectation);
    }

    std::string writes_;
    TerminalCore core_;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: sakura-vt-replay FIXTURE.vtlog\n";
        return 2;
    }

    try {
        ReplayRunner runner;
        runner.Run(argv[1]);
        std::cout << "replay passed: " << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "replay failed: " << error.what() << '\n';
        return 1;
    }
}
