// A small line editor for the interactive shell: cursor keys, history, Ctrl-L, word deletion and
// tab completion, UTF-8 aware. POSIX terminals only; elsewhere (Windows consoles, which edit and
// keep history themselves, or anything that isn't a terminal) it reads a plain line.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lineedit {

// Given the text before the cursor, the candidates for the word being typed (each a full
// replacement for that word), and in `word_start` where that word begins (a byte offset).
using Completer = std::function<std::vector<std::string>(const std::string& before_cursor, std::size_t& word_start)>;

// Reads a line after printing `prompt` (which may hold colour codes). Returns false on end of
// input (Ctrl-D on an empty line). Ctrl-C abandons the line and returns true with it empty.
bool read_line(const std::string& prompt, std::string& out, const Completer& complete = {});

// History: ↑/↓ walk it; the shell loads it at start and saves after every line.
void history_load(const std::filesystem::path& file);
void history_add(const std::string& line);
void history_save(const std::filesystem::path& file);

}  // namespace lineedit
