#include "zepto.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>


namespace fs = std::filesystem;

static const char* VERSION = "zdb 1.0.0";

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool looks_incomplete(const std::string& sql) {
    std::string t = trim(sql);
    if (t.empty()) return false;

    int parens = 0;
    int brackets = 0;
    bool in_single = false, in_double = false;
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (c == '\\' && i + 1 < t.size()) { i++; continue; }
        if (c == '\'' && !in_double) in_single = !in_single;
        if (c == '"' && !in_single) in_double = !in_double;
        if (!in_single && !in_double) {
            if (c == '(') parens++;
            if (c == ')') parens--;
            if (c == '[') brackets++;
            if (c == ']') brackets--;
        }
    }
    if (in_single || in_double || parens > 0 || brackets > 0) return true;

    std::string upper = t;
    for (auto& c : upper) c = (char)std::toupper((unsigned char)c);

    if (upper.size() >= 2 && upper.substr(upper.size() - 2) == "--") return false;

    static const char* incomplete_kw[] = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "ALTER",
        "DROP", "BEGIN", "COMMIT", "ROLLBACK", "WITH"
    };
    for (auto kw : incomplete_kw) {
        if (upper.size() >= strlen(kw) && upper.substr(0, strlen(kw)) == kw) {
            if (upper.back() != ';') return true;
        }
    }

    return false;
}

static std::string get_db_display_name(const std::string& dir) {
    if (dir.empty()) return ":memory:";
    return fs::path(dir).filename().string();
}

struct CliState {
    std::string db_dir;
    bool timer_enabled = false;
    bool headers_enabled = true;
    enum OutputMode { TABLE, CSV, JSON } output_mode = TABLE;
    std::string history_file;
    std::vector<std::string> history;
    size_t history_idx = 0;
};

static void load_history(CliState& st) {
    char* home = nullptr;
#ifdef _WIN32
    home = getenv("USERPROFILE");
#else
    home = getenv("HOME");
#endif
    if (home) {
        fs::path p = fs::path(home) / ".zdb_history";
        st.history_file = p.string();
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) st.history.push_back(line);
        }
    }
}

static void save_history_line(CliState& st, const std::string& line) {
    if (line.empty()) return;
    if (!st.history.empty() && st.history.back() == line) return;
    st.history.push_back(line);
    if (!st.history_file.empty()) {
        std::ofstream f(st.history_file, std::ios::app);
        f << line << "\n";
    }
}

static void print_version() {
    std::cout << VERSION << "\n";
    std::cout << "  Enter SQL statements or meta-commands.\n";
    std::cout << "  Type .help for available commands.\n";
}

static void print_help() {
    std::cout << R"(  Meta-commands:
    .open <path>          Open or create a database directory
    .tables               List all tables
    .schema               Show table schema
    .read <file>          Read and execute SQL from a file
    .output <mode>        Set output mode: table, csv, json
    .timer on|off         Toggle query timing
    .headers on|off       Toggle column headers
    .checkpoints          Force a checkpoint
    .snapshot <name>      Create a CoW snapshot
    .snapshots            List snapshots
    .restore <name>       Restore from a snapshot
    .version              Show version
    .quit / .exit         Exit zcli
    .help                 Show this help

  SQL:
    CREATE TABLE <name> (<col> <type> [NOT NULL] [DICT|BIT_PACKED|DELTA|RLE], ...)
    INSERT INTO <name> VALUES (<val>, ...)
    SELECT [*|<cols>|<agg>(<col>)] FROM <name>
      [WHERE <cond>] [GROUP BY <col>] [ORDER BY <col> [ASC|DESC]]
      [LIMIT n] [OFFSET n]
    UPDATE <name> SET <col>=<val>, ... WHERE <cond>
    DELETE FROM <name> WHERE <cond>
    DROP TABLE <name>
    ALTER TABLE <name> ADD|DROP COLUMN <col> [<type>]
    BEGIN | COMMIT | ROLLBACK

  Types: INT/I32, BIGINT/I64, FLOAT/F32, DOUBLE/F64, STRING/TEXT/VARCHAR
  Operators: = != < > <= >= LIKE IS NULL IS NOT NULL IN BETWEEN
  Scalar: UPPER(col) LOWER(col) LENGTH(col)
  Aggregates: COUNT(*) SUM AVG MIN MAX
)" << std::endl;
}

static void print_table_header(const std::vector<std::string>& cols, size_t* widths) {
    for (size_t i = 0; i < cols.size(); i++) {
        if (i > 0) std::cout << " | ";
        std::string c = cols[i];
        if (c.size() > widths[i]) c = c.substr(0, widths[i]);
        std::cout << c;
        for (size_t j = c.size(); j < widths[i]; j++) std::cout << ' ';
    }
    std::cout << "\n";
    for (size_t i = 0; i < cols.size(); i++) {
        if (i > 0) std::cout << "-+-";
        for (size_t j = 0; j < widths[i]; j++) std::cout << '-';
    }
    std::cout << "\n";
}

static void print_csv(const std::vector<std::string>& cols, const std::vector<std::vector<std::string>>& data) {
    auto escape_csv = [](const std::string& s) -> std::string {
        if (s.find(',') != std::string::npos || s.find('"') != std::string::npos ||
            s.find('\n') != std::string::npos) {
            std::string r = "\"";
            for (char c : s) {
                if (c == '"') r += "\"\"";
                else r += c;
            }
            r += "\"";
            return r;
        }
        return s;
    };
    for (size_t i = 0; i < cols.size(); i++) {
        if (i > 0) std::cout << ",";
        std::cout << escape_csv(cols[i]);
    }
    std::cout << "\n";
    for (auto& row : data) {
        for (size_t i = 0; i < row.size(); i++) {
            if (i > 0) std::cout << ",";
            std::cout << escape_csv(row[i]);
        }
        std::cout << "\n";
    }
}

static void print_json(const std::vector<std::string>& cols, const std::vector<std::vector<std::string>>& data) {
    std::cout << "[\n";
    for (size_t ri = 0; ri < data.size(); ri++) {
        std::cout << "  {";
        for (size_t ci = 0; ci < cols.size() && ci < data[ri].size(); ci++) {
            if (ci > 0) std::cout << ", ";
            std::cout << "\"" << cols[ci] << "\": ";
            const std::string& v = data[ri][ci];
            if (v == "NULL") std::cout << "null";
            else if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
                std::cout << "\"" << v.substr(1, v.size() - 2) << "\"";
            } else {
                bool is_num = !v.empty() && (std::isdigit(v[0]) || v[0] == '-' || v[0] == '+');
                if (is_num) std::cout << v;
                else std::cout << "\"" << v << "\"";
            }
        }
        std::cout << "}" << (ri + 1 < data.size() ? ",\n" : "\n");
    }
    std::cout << "]\n";
}

static void exec_sql(CliState& st, zepto::Database& db, const std::string& sql) {
    bool is_meta = !sql.empty() && sql[0] == '.';

    auto t0 = std::chrono::high_resolution_clock::now();

    db.exec(sql);

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    if (is_meta) {
        if (st.timer_enabled) printf("  (%.4fs)\n", elapsed);
        return;
    }

    auto& names = db.query_col_names();
    auto& rows = db.query_rows();
    size_t nc = names.size();

    if (nc == 0 && rows.empty()) {
        if (st.timer_enabled) {
            printf("  (%.4fs)\n", elapsed);
        }
        return;
    }

    // Compute column widths
    std::vector<size_t> widths(nc, 0);
    for (size_t i = 0; i < nc; i++) {
        widths[i] = names[i].size();
    }
    for (auto& row : rows) {
        for (size_t i = 0; i < nc && i < row.size(); i++) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    // Cap widths at 60
    for (auto& w : widths) w = std::min(w, (size_t)60);

    // Collect as strings for multi-mode output
    std::vector<std::vector<std::string>> data;
    for (auto& row : rows) {
        std::vector<std::string> srow;
        for (size_t i = 0; i < nc; i++) {
            srow.push_back(i < row.size() ? row[i] : "");
        }
        data.push_back(std::move(srow));
    }

    switch (st.output_mode) {
        case CliState::CSV:
            if (st.headers_enabled) print_csv(names, data);
            else {
                std::vector<std::string> empty;
                print_csv(empty, data);
            }
            break;
        case CliState::JSON:
            print_json(names, data);
            break;
        case CliState::TABLE:
        default:
            if (st.headers_enabled) print_table_header(names, widths.data());
            for (auto& row : data) {
                for (size_t i = 0; i < nc; i++) {
                    if (i > 0) std::cout << " | ";
                    const std::string& v = row[i];
                    std::string disp = v.size() > widths[i] ? v.substr(0, widths[i]) : v;
                    std::cout << disp;
                    for (size_t j = disp.size(); j < widths[i]; j++) std::cout << ' ';
                }
                std::cout << "\n";
            }
            break;
    }

    printf("  (%zu row(s))\n", rows.size());
    if (st.timer_enabled) {
        printf("  (%.4fs)\n", elapsed);
    }
}

static void read_file(CliState& st, zepto::Database& db, const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "  error: cannot open '" << path << "'\n";
        return;
    }
    std::string line, sql;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '-') continue;
        sql += line + "\n";
        if (looks_incomplete(sql)) continue;
        std::string stmt = trim(sql);
        if (!stmt.empty() && stmt.back() == ';') stmt.pop_back();
        stmt = trim(stmt);
        if (!stmt.empty()) {
            exec_sql(st, db, stmt);
        }
        sql.clear();
    }
    if (!trim(sql).empty()) {
        std::string stmt = trim(sql);
        if (stmt.back() == ';') stmt.pop_back();
        stmt = trim(stmt);
        if (!stmt.empty()) exec_sql(st, db, stmt);
    }
}

static bool is_directory_db(const std::string& path) {
    return fs::exists(path) && fs::is_directory(path);
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string ensure_db_dir(const std::string& path) {
    if (ends_with(path, ".zdb")) return path; // a .zdb checkpoint file, not a directory
    if (is_directory_db(path)) return path;
    if (!fs::exists(path)) {
        fs::create_directories(path);
        return path;
    }
    return path;
}

int main(int argc, char* argv[]) {
    CliState st;
    std::vector<std::string> sql_args;
    std::vector<std::string> file_args;
    bool exec_mode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        }
        if (arg == "-c" || arg == "-cmd") {
            if (i + 1 < argc) {
                sql_args.push_back(argv[++i]);
                exec_mode = true;
            }
        } else if (arg == "-f" || arg == "-file") {
            if (i + 1 < argc) {
                file_args.push_back(argv[++i]);
                exec_mode = true;
            }
        } else if (arg == "-timer") {
            st.timer_enabled = true;
        } else if (arg == "-csv") {
            st.output_mode = CliState::CSV;
        } else if (arg == "-json") {
            st.output_mode = CliState::JSON;
        } else if (arg == "-noheader") {
            st.headers_enabled = false;
        } else if (arg[0] != '-') {
            st.db_dir = ensure_db_dir(arg);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    load_history(st);

    std::unique_ptr<zepto::Database> db;
    auto open_db = [&](const std::string& dir) {
        db = std::make_unique<zepto::Database>();
        if (!db->open(dir)) {
            std::cerr << "  error: cannot open database '" << dir << "'\n";
            db.reset();
            return false;
        }
        st.db_dir = dir;
        return true;
    };

    if (!st.db_dir.empty()) {
        open_db(st.db_dir);
    }

    if (exec_mode) {
        if (!db) {
            // Create in-memory
            std::string tmp = (fs::temp_directory_path() / "zcli_tmp").string();
            fs::create_directories(tmp);
            open_db(tmp);
        }
        if (db) {
            for (auto& path : file_args) {
                read_file(st, *db, path);
            }
            for (auto& arg : sql_args) {
                exec_sql(st, *db, arg);
            }
        }
        if (db) {
            db->close();
            if (!st.db_dir.empty() && st.db_dir.find("zcli_tmp") != std::string::npos) {
                fs::remove_all(st.db_dir);
            }
        }
        return 0;
    }

    // Interactive REPL
    print_version();
    std::cout << "\n";

    std::string prompt;
    auto update_prompt = [&]() {
        std::string name = get_db_display_name(st.db_dir);
        prompt = name + "> ";
    };
    update_prompt();

    std::string sql;
    bool multiline = false;

    while (true) {
        if (multiline) {
            std::cout << "... ";
        } else {
            std::cout << prompt;
        }
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) break;

        if (!multiline) {
            std::string trimmed = trim(line);
            if (trimmed.empty()) continue;

            save_history_line(st, trimmed);

            // Handle .quit / .exit before anything else
            std::string upper = trimmed;
            for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
            if (upper == ".QUIT" || upper == ".EXIT" || upper == ".Q") break;

            // Meta-commands
            if (trimmed[0] == '.') {
                std::istringstream iss(trimmed);
                std::string cmd;
                iss >> cmd;
                for (auto& c : cmd) c = (char)std::toupper((unsigned char)c);

                if (cmd == ".HELP" || cmd == ".H" || cmd == "?") {
                    print_help();
                    continue;
                }
                if (cmd == ".VERSION" || cmd == ".V") {
                    print_version();
                    continue;
                }
                if (cmd == ".TABLES" || cmd == ".T") {
                    if (!db) { std::cout << "  no database open\n"; continue; }
                    std::string q = ".tables";
                    exec_sql(st, *db, q);
                    continue;
                }
                if (cmd == ".SCHEMA" || cmd == ".S") {
                    if (!db) { std::cout << "  no database open\n"; continue; }
                    std::string q = ".schema";
                    exec_sql(st, *db, q);
                    continue;
                }
                if (cmd == ".OPEN") {
                    std::string path;
                    iss >> path;
                    if (path.empty()) {
                        std::cout << "  usage: .open <directory>\n";
                        continue;
                    }
                    path = ensure_db_dir(path);
                    if (db) db->close();
                    if (open_db(path)) {
                        update_prompt();
                        std::cout << "  opened '" << path << "'\n";
                    }
                    continue;
                }
                if (cmd == ".READ" || cmd == ".R") {
                    std::string path;
                    iss >> path;
                    if (path.empty()) {
                        std::cout << "  usage: .read <file>\n";
                        continue;
                    }
                    if (!db) {
                        std::string tmp = (fs::temp_directory_path() / "zcli_tmp").string();
                        fs::create_directories(tmp);
                        open_db(tmp);
                    }
                    if (db) read_file(st, *db, path);
                    continue;
                }
                if (cmd == ".OUTPUT" || cmd == ".O") {
                    std::string mode;
                    iss >> mode;
                    for (auto& c : mode) c = (char)std::tolower((unsigned char)c);
                    if (mode == "table") st.output_mode = CliState::TABLE;
                    else if (mode == "csv") st.output_mode = CliState::CSV;
                    else if (mode == "json") st.output_mode = CliState::JSON;
                    else std::cout << "  modes: table, csv, json\n";
                    continue;
                }
                if (cmd == ".TIMER" || cmd == ".TI") {
                    std::string arg;
                    iss >> arg;
                    for (auto& c : arg) c = (char)std::tolower((unsigned char)c);
                    if (arg == "on" || arg == "1" || arg == "true") st.timer_enabled = true;
                    else if (arg == "off" || arg == "0" || arg == "false") st.timer_enabled = false;
                    else st.timer_enabled = !st.timer_enabled;
                    std::cout << "  timer: " << (st.timer_enabled ? "on" : "off") << "\n";
                    continue;
                }
                if (cmd == ".HEADERS" || cmd == ".HE") {
                    std::string arg;
                    iss >> arg;
                    for (auto& c : arg) c = (char)std::tolower((unsigned char)c);
                    if (arg == "on" || arg == "1") st.headers_enabled = true;
                    else if (arg == "off" || arg == "0") st.headers_enabled = false;
                    else st.headers_enabled = !st.headers_enabled;
                    std::cout << "  headers: " << (st.headers_enabled ? "on" : "off") << "\n";
                    continue;
                }
                if (cmd == ".CHECKPOINT" || cmd == ".CP") {
                    if (!db) { std::cout << "  no database open\n"; continue; }
                    exec_sql(st, *db, ".checkpoint");
                    continue;
                }
                if (cmd == ".SNAPSHOT") {
                    if (!db) { std::cout << "  no database open\n"; continue; }
                    std::string name;
                    iss >> name;
                    if (name.empty()) { std::cout << "  usage: .snapshot <name>\n"; continue; }
                    exec_sql(st, *db, ".snapshot " + name);
                    continue;
                }
                if (cmd == ".SNAPSHOTS" || cmd == ".SS") {
                    if (!db) { std::cout << "  no database open\n"; continue; }
                    exec_sql(st, *db, ".snapshots");
                    continue;
                }
                if (cmd == ".RESTORE") {
                    if (!db) { std::cout << "  no database open\n"; continue; }
                    std::string name;
                    iss >> name;
                    if (name.empty()) { std::cout << "  usage: .restore <name>\n"; continue; }
                    exec_sql(st, *db, ".restore " + name);
                    continue;
                }
                std::cout << "  unknown command: " << trimmed << "\n";
                std::cout << "  Type .help for available commands.\n";
                continue;
            }
        }

        sql += line + "\n";

        if (looks_incomplete(sql)) {
            multiline = true;
            continue;
        }

        multiline = false;
        std::string stmt = trim(sql);
        sql.clear();

        if (stmt.empty()) continue;
        if (stmt.back() == ';') stmt.pop_back();
        stmt = trim(stmt);
        if (stmt.empty()) continue;

        if (!db) {
            std::string tmp = (fs::temp_directory_path() / "zcli_tmp").string();
            fs::create_directories(tmp);
            open_db(tmp);
            if (!db) {
                std::cerr << "  error: cannot create in-memory database\n";
                continue;
            }
            std::cout << "  (in-memory database created)\n";
        }

        exec_sql(st, *db, stmt);
    }

    if (db) {
        db->close();
        // Clean up temp db
        if (st.db_dir.find("zcli_tmp") != std::string::npos) {
            fs::remove_all(st.db_dir);
        }
    }

    std::cout << "Bye.\n";
    return 0;
}
