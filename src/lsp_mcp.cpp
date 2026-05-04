#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <fstream>
#include <fcntl.h>
#include <string>
#include <string_view>

#include <unistd.h>
#include <csignal>
#include <nlohmann/json.hpp>

#include "lspmb/service.h"
#include "lspmb/report.h"
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>

#include "pcr/proc/child_stdio.h"
#include "pcr/proc/child_proc.h"

#define ERR_PARSE -32700
#define ERR_INVAL_REQ -32600
#define ERR_INVAL_PARAM -32602
#define ERR_NO_METHOD -32601

constexpr const char *k_protocol_version = "2025-11-25";
extern char **environ;

using json = nlohmann::json;
using namespace lspx::protocol;
using namespace lspx::graph;
using namespace lspx::snippet;
using namespace lspx::driver::jdtls;
using namespace lspmb::service;
using namespace lspmb::report;

namespace {

volatile std::sig_atomic_t g_shutdown_signal = 0;

extern "C" void handle_shutdown_signal(int signum) noexcept
{
    if (g_shutdown_signal == 0) {
        g_shutdown_signal = signum;
    }
}

void install_signal_handlers()
{
    struct sigaction sa {};
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);

    // Do not set SA_RESTART. This gives blocking stdio reads a chance to
    // return when a signal arrives.
    sa.sa_flags = 0;

    if (::sigaction(SIGINT, &sa, nullptr) != 0) {
        throw std::runtime_error("sigaction(SIGINT) failed");
    }
    if (::sigaction(SIGTERM, &sa, nullptr) != 0) {
        throw std::runtime_error("sigaction(SIGTERM) failed");
    }
    if (::sigaction(SIGHUP, &sa, nullptr) != 0) {
        throw std::runtime_error("sigaction(SIGHUP) failed");
    }

    // If Codex closes the stdout pipe while we are writing, do not let the
    // default SIGPIPE action kill us before cleanup. Writes may fail instead.
    struct sigaction pipe_sa {};
    pipe_sa.sa_handler = SIG_IGN;
    sigemptyset(&pipe_sa.sa_mask);
    pipe_sa.sa_flags = 0;

    if (::sigaction(SIGPIPE, &pipe_sa, nullptr) != 0) {
        throw std::runtime_error("sigaction(SIGPIPE) failed");
    }
}

const char *signal_name(int signum)
{
    switch (signum) {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGHUP: return "SIGHUP";
        default: return "unknown";
    }
}


bool path_is_under_root(
    const std::filesystem::path &root,
    const std::filesystem::path &path)
{
    const std::filesystem::path abs_root =
        std::filesystem::absolute(root).lexically_normal();
    const std::filesystem::path abs_path =
        std::filesystem::absolute(path).lexically_normal();

    const std::filesystem::path rel = abs_path.lexically_relative(abs_root);
    if (rel.empty()) {
        return true;
    }

    if (rel.is_absolute()) {
        return false;
    }

    for (const std::filesystem::path &part : rel) {
        if (part == "..") {
            return false;
        }
    }

    return true;
}


std::string path_relative_to_root(
    const std::filesystem::path &root,
    const std::filesystem::path &path)
{
    const std::filesystem::path abs_root =
        std::filesystem::absolute(root).lexically_normal();
    const std::filesystem::path abs_path =
        std::filesystem::absolute(path).lexically_normal();

    const std::filesystem::path rel = abs_path.lexically_relative(abs_root);
    if (!rel.empty()) {
        bool escapes = false;
        for (const std::filesystem::path &part : rel) {
            if (part == "..") {
                escapes = true;
                break;
            }
        }

        if (!escapes) {
            return rel.string();
        }
    }

    return abs_path.string();
}


std::filesystem::path normalize_output_path(
    const std::filesystem::path &root,
    const json &arguments,
    std::string_view key,
    const std::filesystem::path &default_relative_path)
{
    std::filesystem::path out_path;

    const std::string key_str(key);

    if (arguments.contains(key_str) && arguments.at(key_str).is_string()) {
        out_path = arguments.at(key_str).get<std::string>();
    } else {
        out_path = default_relative_path;
    }

    if (out_path.is_relative()) {
        out_path = root / out_path;
    }

    out_path = std::filesystem::absolute(out_path).lexically_normal();

    if (!path_is_under_root(root, out_path)) {
        throw std::runtime_error(
            "path for '" + key_str + "' must be inside root. root=" +
            root.string() + " path=" + out_path.string());
    }

    return out_path;
}


void write_text_file(
    const std::filesystem::path &path,
    const std::string &text)
{
    std::filesystem::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open report file: " + path.string());
    }

    out << text;

    if (!out) {
        throw std::runtime_error("failed to write report file: " + path.string());
    }
}

std::size_t branch_snippet_count(
    const std::optional<ExpandedCallTree> &branch)
{
    return branch.has_value() ? branch->snippets.size() : 0;
}

std::size_t branch_top_level_count(
    const std::optional<ExpandedCallTree> &branch)
{
    return branch.has_value() ? branch->root.children.size() : 0;
}


LiveSession &live_session()
{
    static LiveSession s;
    return s;
}


std::string parse_required_string(const json &arguments, std::string_view key)
{
    const std::string key_str(key);
    if (!arguments.contains(key_str) || !arguments.at(key_str).is_string()) {
        throw std::runtime_error("argument '" + key_str + "' is required and must be a string");
    }
    return arguments.at(key_str).get<std::string>();
}


bool env_flag_enabled(std::string_view name)
{
    const std::string key(name);
    if (const char *v = std::getenv(key.c_str())) {
        const std::string s(v);
        return !s.empty() && s != "0" && s != "false" && s != "FALSE";
    }
    return false;
}

std::optional<std::string> getenv_nonempty(std::string_view name)
{
    const std::string key(name);
    if (const char *v = std::getenv(key.c_str())) {
        if (*v != '\0') {
            return std::string(v);
        }
    }
    return std::nullopt;
}

void maybe_redirect_stderr_to_log_file()
{
    const std::optional<std::string> path = getenv_nonempty("LSP_LOG_FILE");
    if (!path.has_value()) {
        return;
    }

    const int fd = ::open(path->c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error(
            "failed to open LSP_LOG_FILE '" + *path + "': " + std::strerror(errno));
    }

    if (::dup2(fd, STDERR_FILENO) < 0) {
        const std::string err = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error(
            "failed to redirect stderr to LSP_LOG_FILE '" + *path + "': " + err);
    }

    ::close(fd);

    std::cerr.setf(std::ios::unitbuf);
    std::cerr << "[lsp_mcp] stderr redirected to " << *path << "\n";
}


std::string getenv_or(std::string_view name, std::string fallback = {})
{
    if (const char *v = std::getenv(std::string(name).c_str())) {
        return *v ? std::string(v) : fallback;
    }
    return fallback;
}


std::string getenv_required(std::string_view name)
{
    if (const char *v = std::getenv(std::string(name).c_str())) {
        if (*v) {
            return std::string(v);
        }
    }
    throw std::runtime_error("missing required environment variable: " + std::string(name));
}


std::filesystem::path parse_required_abs_path(const json &arguments, std::string_view key)
{
    const std::string key_str(key);

    if (!arguments.contains(key_str) || !arguments.at(key_str).is_string()) {
        throw std::runtime_error("argument '" + key_str + "' is required and must be a string");
    }

    return std::filesystem::absolute(arguments.at(key_str).get<std::string>()).lexically_normal();
}


LaunchOptions parse_launch_arguments(const json &arguments)
{
    LaunchOptions launch;
    launch.root_dir = parse_required_abs_path(arguments, "root");
    launch.workspace_dir = parse_required_abs_path(arguments, "workspaceDir");

    launch.jdtls_home =
        arguments.contains("jdtlsHome") && arguments.at("jdtlsHome").is_string()
            ? std::filesystem::absolute(arguments.at("jdtlsHome").get<std::string>()).lexically_normal()
            : std::filesystem::absolute(getenv_required("LSP_JDTLS_HOME")).lexically_normal();

    launch.java_bin =
        arguments.contains("javaBin") && arguments.at("javaBin").is_string()
            ? arguments.at("javaBin").get<std::string>()
            : getenv_or("LSP_JAVA_BIN", "java");

    launch.log_protocol = false;
    launch.log_level = "INFO";
    return launch;
}

std::string extract_class_name(std::string_view input) 
{
    // strip the ".java" suffix if it exists
    constexpr std::string_view suffix = ".java";
    if (input.size() >= suffix.size() && 
        input.substr(input.size() - suffix.size()) == suffix) {
        input.remove_suffix(suffix.size());
    }

    // find the last dot in the remaining string
    const std::size_t last_dot_pos = input.find_last_of('.');

    // ff a dot was found, keep only the part after it
    if (last_dot_pos != std::string_view::npos) {
        input.remove_prefix(last_dot_pos + 1);
    }

    return std::string(input);
}

json position_json(const Position &position)
{
    return json{
        {"line", position.line},
        {"character", position.character}
    };
}

json range_json(const Range &range)
{
    return json{
        {"start", position_json(range.start)},
        {"end", position_json(range.end)}
    };
}

json document_symbol_json(const DocumentSymbol &symbol)
{
    json children = json::array();
    for (const DocumentSymbol &child : symbol.children) {
        children.push_back(document_symbol_json(child));
    }

    return json{
        {"name", symbol.name},
        {"detail", symbol.detail},
        {"kind", static_cast<int>(symbol.kind)},
        {"range", range_json(symbol.range)},
        {"selectionRange", range_json(symbol.selection_range)},
        {"children", std::move(children)}
    };
}

// json document_symbols_response_json(const DocumentSymbolsResponse &r)
// {
//     json symbols = json::array();
//     for (const DocumentSymbol &symbol : r.symbols) {
//         symbols.push_back(document_symbol_json(symbol));
//     }
//
//     return json{
//         {"ok", true},
//         {"file", r.file.string()},
//         {"topLevelCount", r.symbols.size()},
//         {"symbols", std::move(symbols)}
//     };
// }


json workspace_symbol_json(const WorkspaceSymbol &symbol)
{
    json out{
        {"name", symbol.name},
        {"detail", symbol.detail},
        {"containerName", symbol.container_name},
        {"kind", static_cast<int>(symbol.kind)},
        {"path", symbol.path.string()},
        {"uri", symbol.uri}
    };

    if (symbol.range.has_value()) {
        out["range"] = range_json(*symbol.range);
    }

    if (symbol.data_json.has_value()) {
        out["dataJson"] = *symbol.data_json;
    }

    return out;
}

json call_hierarchy_item_json(const CallHierarchyItem &item)
{
    json out{
        {"name", item.name},
        {"detail", item.detail},
        {"kind", static_cast<int>(item.kind)},
        {"path", item.path.string()},
        {"uri", item.uri},
        {"range", range_json(item.range)},
        {"selectionRange", range_json(item.selection_range)}
    };

    if (item.data_json.has_value()) {
        out["dataJson"] = *item.data_json;
    }

    return out;
}


// json resolve_anchor_response_json(const ResolveAnchorResponse &resp)
// {
//     const ResolvedAnchor &anchor = resp.anchor;
//
//     json out{
//         {"ok", true},
//         {"file", anchor.file.string()},
//         {"className", anchor.query},
//         {"methodName", anchor.function_name},
//         {"attempts", anchor.attempts},
//         {"candidateCount", anchor.candidate_count},
//         {"methodSymbol", document_symbol_json(anchor.function_symbol)},
//         {"callItem", call_hierarchy_item_json(anchor.call_item)}
//     };
//
//     if (anchor.query_symbol.has_value()) {
//         out["classSymbol"] = workspace_symbol_json(*anchor.query_symbol);
//     }
//
//     return out;
// }

json resolved_anchor_json(const ResolvedAnchor &anchor)
{
    json out{
        {"file", anchor.file.string()},
        {"className", anchor.query},
        {"methodName", anchor.function_name},
        {"attempts", anchor.attempts},
        {"candidateCount", anchor.candidate_count},
        {"methodSymbol", document_symbol_json(anchor.function_symbol)},
        {"callItem", call_hierarchy_item_json(anchor.call_item)}
    };

    if (anchor.query_symbol.has_value()) {
        out["classSymbol"] = workspace_symbol_json(*anchor.query_symbol);
    }

    return out;
}

json source_snippet_json(const SourceSnippet &snippet)
{
    return json{
        {"path", snippet.path.string()},
        {"startLine", snippet.start_line},
        {"endLine", snippet.end_line},
        {"text", snippet.numbered_text}
    };
}

json expanded_node_json(const ExpandedNode &node)
{
    json from_ranges = json::array();
    for (const Range &range : node.from_ranges) {
        from_ranges.push_back(range_json(range));
    }

    json children = json::array();
    for (const ExpandedNode &child : node.children) {
        children.push_back(expanded_node_json(child));
    }

    return json{
        {"item", call_hierarchy_item_json(node.item)},
        {"fromRanges", std::move(from_ranges)},
        {"stopReason", node.stop_reason},
        {"children", std::move(children)}
    };
}

json expanded_snippet_json(const CallGraphSnippet &snippet)
{
    return json{
        {"item", call_hierarchy_item_json(snippet.item)},
        {"stopReason", snippet.stop_reason},
        {"window", source_snippet_json(snippet.snippet)}
    };
}

json expanded_call_tree_json(const ExpandedCallTree &tree)
{
    json snippets = json::array();
    for (const CallGraphSnippet &snippet : tree.snippets) {
        snippets.push_back(expanded_snippet_json(snippet));
    }

    return json{
        {"root", expanded_node_json(tree.root)},
        {"snippetCount", tree.snippets.size()},
        {"snippets", std::move(snippets)}
    };
}


json expand_calls_response_json(const ExpandCallsResponse &resp)
{
    json out{
        {"ok", true},
        {"direction", resp.direction},
        {"resolvedAnchor", resolved_anchor_json(resp.resolved_anchor)}
    };

    if (resp.outgoing.has_value()) {
        out["outgoing"] = expanded_call_tree_json(*resp.outgoing);
    }

    if (resp.incoming.has_value()) {
        out["incoming"] = expanded_call_tree_json(*resp.incoming);
    }

    return out;
}


bool trace_enabled() 
{
    const char *env = std::getenv("LSP_MCP_TRACE");
    if (!env) return false;
    const std::string v(env);
    return !v.empty() && v != "0" && v != "false" && v != "FALSE";
}

void log_line(std::string_view msg) 
{
    if (!trace_enabled()) return;
    std::cerr << "[lsp_mcp] " << msg << "\n";
    std::cerr.flush();
}

std::string now_utc_iso8601() 
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}


void send_json(const json &msg) 
{
    // MCP stdio transport requires one JSON-RPC message per line.
    std::cout << msg.dump() << "\n";
    std::cout.flush();
}


json make_result(const json &id, json result) 
{
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
}


json make_error(
    const json &id,
    int code,
    std::string message,
    std::optional<json> data = std::nullopt) 
{
    json err{
        {"code", code},
        {"message", std::move(message)},
    };
    if (data.has_value()) {
        err["data"] = std::move(*data);
    }

    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", std::move(err)},
    };
}


std::string uppercase_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}


struct MermaidRenderResult
{
    bool attempted{false};
    bool ok{false};
    std::string renderer;
    std::filesystem::path svg_path;
    std::string error;
};

MermaidRenderResult render_mermaid_svg(
    const json &arguments,
    const std::filesystem::path &mmd_path,
    const std::filesystem::path &svg_path)
{

    MermaidRenderResult out;
    out.svg_path = svg_path;

    const bool render_svg = arguments.value("renderSvg", true);
    if (!render_svg) {
        return out;
    }

    const std::string renderer =
        getenv_or("LSP_MMDC_BIN", "mmdc");
    log_line("mmdc bin found: " + renderer);

    const int devnull_in = ::open("/dev/null", O_RDONLY);
    if (devnull_in < 0) {
        throw std::runtime_error("open(/dev/null, O_RDONLY) failed");
    }

    const int devnull_out = ::open("/dev/null", O_WRONLY);
    if (devnull_out < 0) {
        ::close(devnull_in);
        throw std::runtime_error("open(/dev/null, O_WRONLY) failed");
    }

    const std::string render_log =
        getenv_or("LSP_MMDC_LOG_FILE", "/tmp/lsp-mmdc.log");

    const int err_fd = ::open(render_log.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (err_fd < 0) {
        ::close(devnull_in);
        ::close(devnull_out);
        throw std::runtime_error("open(LSP_MMDC_LOG_FILE) failed");
    }

    try {
        pcr::proc::ProcessSpec spec;
        spec.exe = renderer;
        spec.args = {
            "-i", mmd_path.string(),
            "-o", svg_path.string(),
            "-b", "transparent"
        };

        pcr::proc::ChildStdioMap stdio;
        stdio.stdin_fd = devnull_in;
        stdio.stdout_fd = devnull_out;
        stdio.stderr_fd = err_fd;

        pcr::proc::ChildProcess child =
            pcr::proc::ChildProcess::spawn(spec, stdio);

        ::close(devnull_in);
        ::close(devnull_out);
        ::close(err_fd);

        const pcr::proc::WaitResult wr = child.wait();
        if (!wr.exited || wr.exit_code != 0) {
            throw std::runtime_error("mmdc failed");
        }

        MermaidRenderResult out;
        out.attempted = true;
        out.ok = true;
        out.renderer = renderer;
        out.svg_path = svg_path;
        return out;
    } catch (...) {
        ::close(devnull_in);
        ::close(devnull_out);
        ::close(err_fd);
        throw;
    }
}


// json jdtls_document_symbols_tool_definition()
// {
//     return json{
//         {"name", "jdtls_document_symbols"},
//         {"title", "Jdtls Document Symbols"},
//         {"description", "Launch JDTLS, initialize an LSP session, and return hierarchical document symbols for a Java source file."},
//         {"inputSchema", {
//             {"type", "object"},
//             {"properties", {
//                 {"root", {
//                     {"type", "string"},
//                     {"description", "Absolute path to the repo root."}
//                 }},
//                 {"workspaceDir", {
//                     {"type", "string"},
//                     {"description", "Absolute path to the persistent JDTLS workspace/data dir."}
//                 }},
//                 {"file", {
//                     {"type", "string"},
//                     {"description", "Absolute path to the Java source file to inspect."}
//                 }},
//                 {"jdtlsHome", {
//                     {"type", "string"},
//                     {"description", "Optional JDTLS install dir. Defaults to LSP_JDTLS_HOME."}
//                 }},
//                 {"javaBin", {
//                     {"type", "string"},
//                     {"description", "Optional java executable. Defaults to LSP_JAVA_BIN or 'java'."}
//                 }}
//             }},
//             {"required", json::array({"root", "workspaceDir", "file"})}
//         }}
//     };
// }
//
//
// json jdtls_resolve_anchor_tool_definition()
// {
//     return json{
//         {"name", "jdtls_resolve_anchor"},
//         {"title", "JDTLS Resolve Anchor"},
//         {"description", "Resolve a Java class+method into a source file, method symbol, and call-hierarchy anchor item."},
//         {"inputSchema", {
//             {"type", "object"},
//             {"properties", {
//                 {"root", {
//                     {"type", "string"},
//                     {"description", "Absolute path to the repo root."}
//                 }},
//                 {"workspaceDir", {
//                     {"type", "string"},
//                     {"description", "Absolute path to the persistent JDTLS workspace/data dir."}
//                 }},
//                 {"class", {
//                     {"type", "string"},
//                     {"description", "Simple or logical class name to resolve."}
//                 }},
//                 {"method", {
//                     {"type", "string"},
//                     {"description", "Method name to resolve."}
//                 }},
//                 {"jdtlsHome", {
//                     {"type", "string"},
//                     {"description", "Optional JDTLS install dir. Defaults to LSP_JDTLS_HOME."}
//                 }},
//                 {"javaBin", {
//                     {"type", "string"},
//                     {"description", "Optional java executable. Defaults to LSP_JAVA_BIN or 'java'."}
//                 }}
//             }},
//             {"required", json::array({"root", "workspaceDir", "class", "method"})}
//         }}
//     };
// }
//
//
// json jdtls_expand_calls_tool_definition()
// {
//     return json{
//         {"name", "jdtls_expand_calls"},
//         {"title", "JDTLS Expand Calls"},
//         {"description", "Expand a Java method call tree in the requested direction: incoming, outgoing, or both. Returns the resolved anchor, dependency tree, and relevant code snippets with file paths and line ranges."},
//         {"inputSchema", {
//             {"type", "object"},
//             {"properties", {
//                 {"root", {
//                     {"type", "string"},
//                     {"description", "Absolute path to the repo root."}
//                 }},
//                 {"workspaceDir", {
//                     {"type", "string"},
//                     {"description", "Absolute path to the persistent JDTLS workspace/data dir."}
//                 }},
//                 {"class", {
//                     {"type", "string"},
//                     {"description", "Class name to resolve."}
//                 }},
//                 {"method", {
//                     {"type", "string"},
//                     {"description", "Method name to expand."}
//                 }},
//                 {"direction", {
//                     {"type", "string"},
//                     {"description", "Call graph direction. Supports 'outgoing', 'incoming', or 'both'."},
//                     {"enum", json::array({"outgoing", "incoming", "both"})},
//                     {"default", "incoming"}
//                 }},
//                 {"maxDepth", {
//                     {"type", "integer"},
//                     {"description", "Maximum expansion depth."},
//                     {"default", 3}
//                 }},
//                 {"snippetPaddingBefore", {
//                     {"type", "integer"},
//                     {"description", "Number of context lines before the anchor range."},
//                     {"default", 1}
//                 }},
//                 {"snippetPaddingAfter", {
//                     {"type", "integer"},
//                     {"description", "Number of context lines after the anchor range."},
//                     {"default", 1}
//                 }},
//                 {"jdtlsHome", {
//                     {"type", "string"},
//                     {"description", "Optional JDTLS install dir. Defaults to LSP_JDTLS_HOME."}
//                 }},
//                 {"javaBin", {
//                     {"type", "string"},
//                     {"description", "Optional java executable. Defaults to LSP_JAVA_BIN or 'java'."}
//                 }}
//             }},
//             {"required", json::array({"root", "workspaceDir", "class", "method"})}
//         }}
//     };
// }
//

json jdtls_expand_report_tool_definition()
{
    return json{
        {"name", "jdtls_expand_report"},
        {"title", "JDTLS Expand Calls Report"},
        {"description", "Expand a Java method call tree and write a deterministic backend-rendered markdown dependency report to disk. Use this when the user asks for an impact/dependency report or asks to write semantic analysis to a file."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"root", {
                    {"type", "string"},
                    {"description", "Absolute path to the repo root."}
                }},
                {"workspaceDir", {
                    {"type", "string"},
                    {"description", "Absolute path to the persistent JDTLS workspace/data dir."}
                }},
                {"class", {
                    {"type", "string"},
                    {"description", "Class name to resolve."}
                }},
                {"method", {
                    {"type", "string"},
                    {"description", "Method name to expand."}
                }},
                {"direction", {
                    {"type", "string"},
                    {"description", "Call graph direction. Supports 'outgoing', 'incoming', or 'both'."},
                    {"enum", json::array({"outgoing", "incoming", "both"})},
                    {"default", "both"}
                }},
                {"maxDepth", {
                    {"type", "integer"},
                    {"description", "Maximum expansion depth."},
                    {"default", 5}
                }},
                {"snippetPaddingBefore", {
                    {"type", "integer"},
                    {"description", "Number of context lines before the anchor range."},
                    {"default", 2}
                }},
                {"snippetPaddingAfter", {
                    {"type", "integer"},
                    {"description", "Number of context lines after the anchor range."},
                    {"default", 3}
                }},
                {"reportPath", {
                    {"type", "string"},
                    {"description", "Optional output path for the markdown report. Defaults under .codex/analysis/."}
                }},
                {"mermaidPath", {
                    {"type", "string"},
                    {"description", "Optional output path for the Mermaid .mmd file. Defaults under .codex/analysis/."}
                }},
                {"svgPath", {
                    {"type", "string"},
                    {"description", "Optional output path for the rendered SVG file. Defaults under .codex/analysis/."}
                }},
                {"renderSvg", {
                    {"type", "boolean"},
                    {"description", "If true, invoke Mermaid CLI to render SVG."},
                    {"default", true}
                }},
                {"userRequest", {
                    {"type", "string"},
                    {"description", "Optional user request text to include in the report."}
                }},
                {"jdtlsHome", {
                    {"type", "string"},
                    {"description", "Optional JDTLS install dir. Defaults to LSP_JDTLS_HOME."}
                }},
                {"javaBin", {
                    {"type", "string"},
                    {"description", "Optional java executable. Defaults to LSP_JAVA_BIN or 'java'."}
                }}
            }},
            {"required", json::array({"root", "workspaceDir", "class", "method"})}
        }}
    };
}


// json jdtls_document_symbols_result(const json &arguments)
// {
//     if (!arguments.is_object()) {
//         throw std::runtime_error("arguments must be an object");
//     }
//
//     DocumentSymbolsRequest req;
//     req.launch = parse_launch_arguments(arguments);
//     req.file = parse_required_abs_path(arguments, "file");
//     req.trace_lsp_messages = env_flag_enabled("LSP_TRACE_LSP");
//     req.trace_request_timing = env_flag_enabled("LSP_TRACE_RPC");
//
//     log_line("jdtls_document_symbols service begin");
//
//     const DocumentSymbolsResponse resp =
//         run_document_symbols(req);
//
//     log_line("jdtls_document_symbols service returned");
//
//     const json structured = document_symbols_response_json(resp);
//
//     log_line("jdtls_document_symbols json built");
//
//     return json{
//         {"content", json::array({
//             {
//                 {"type", "text"},
//                 {"text", "jdtls_document_symbols ok: " + resp.file.filename().string() +
//                          " (" + std::to_string(resp.symbols.size()) + " top-level symbols)"}
//             }
//         })},
//         {"structuredContent", structured}
//     };
// }


json initialize_result(const json &params) 
{
    std::string negotiated = k_protocol_version;
    if (params.is_object() && 
        params.contains("protocolVersion") && 
        params.at("protocolVersion").is_string()) 
    {
        const std::string requested = params.at("protocolVersion").get<std::string>();
        if (requested == k_protocol_version) {
            negotiated = requested;
        }
    }

    return json{
        {"protocolVersion", negotiated},
        {"capabilities", {
            {"tools", json::object()}
        }},
        {"serverInfo", {
            {"name", "lsp-mcp"},
            {"version", "0.1.0"}
        }},
        {"instructions", "Use smoke_echo for simple end-to-end MCP verification."}
    };
}


// json jdtls_resolve_anchor_result(const json &arguments)
// {
//     if (!arguments.is_object()) {
//         throw std::runtime_error("arguments must be an object");
//     }
//
//     ResolveAnchorRequest req;
//     req.launch = parse_launch_arguments(arguments);
//     req.class_name = parse_required_string(arguments, "class");
//     req.method_name = parse_required_string(arguments, "method");
//     req.trace_lsp_messages = env_flag_enabled("LSP_TRACE_LSP");
//     req.trace_request_timing = env_flag_enabled("LSP_TRACE_RPC");
//
//     log_line("jdtls_resolve_anchor service begin"
//              " class=" + req.class_name +
//              " method=" + req.method_name);
//
//     // const ResolveAnchorResponse resp =
//     //     run_resolve_anchor(req);
//
//     const ResolveAnchorResponse resp =
//         live_session().resolve_anchor(req);
//
//     log_line("jdtls_resolve_anchor service returned"
//              " file=" + resp.anchor.file.string());
//
//     const json structured = resolve_anchor_response_json(resp);
//
//     return json{
//         {"content", json::array({
//             {
//                 {"type", "text"},
//                 {"text", "jdtls_resolve_anchor ok: " +
//                          resp.anchor.query + "." +
//                          resp.anchor.function_name + " -> " +
//                          resp.anchor.file.filename().string()}
//             }
//         })},
//         {"structuredContent", structured}
//     };
// }
//
//
// json jdtls_expand_calls_result(const json &arguments)
// {
//     if (!arguments.is_object()) {
//         throw std::runtime_error("arguments must be an object");
//     }
//
//     ExpandCallsRequest req;
//     req.launch = parse_launch_arguments(arguments);
//     req.class_name = parse_required_string(arguments, "class");
//     req.method_name = parse_required_string(arguments, "method");
//     req.direction = arguments.value("direction", std::string("incoming"));
//     req.max_depth = arguments.value("maxDepth", 3);
//     req.snippet_padding_before =
//         static_cast<std::size_t>(arguments.value("snippetPaddingBefore", 1));
//     req.snippet_padding_after =
//         static_cast<std::size_t>(arguments.value("snippetPaddingAfter", 1));
//     req.trace_lsp_messages = env_flag_enabled("LSP_TRACE_LSP");
//     req.trace_request_timing = env_flag_enabled("LSP_TRACE_RPC");
//
//     log_line("jdtls_expand_calls service begin"
//              " class=" + req.class_name +
//              " method=" + req.method_name +
//              " direction=" + req.direction);
//
//     const ExpandCallsResponse resp =
//         live_session().expand_calls(req);
//
//     log_line("jdtls_expand_calls service returned"
//              " direction=" + resp.direction);
//
//     const json structured = expand_calls_response_json(resp);
//
//     return json{
//         {"content", json::array({
//             {
//                 {"type", "text"},
//                 {"text", "jdtls_expand_calls ok: " +
//                          resp.direction + " tree for " +
//                          resp.resolved_anchor.query + "." +
//                          resp.resolved_anchor.function_name}
//             }
//         })},
//         {"structuredContent", structured}
//     };
// }
//

// json jdtls_expand_report_result(const json &arguments)
// {
//     if (!arguments.is_object()) {
//         throw std::runtime_error("arguments must be an object");
//     }
//
//     ExpandCallsRequest req;
//     req.launch = parse_launch_arguments(arguments);
//     req.class_name = parse_required_string(arguments, "class");
//     req.class_name = extract_class_name(req.class_name);
//     log_line("class name: " + req.class_name);
//     req.method_name = parse_required_string(arguments, "method");
//     req.direction = arguments.value("direction", std::string("both"));
//     req.max_depth = arguments.value("maxDepth", 5);
//     req.snippet_padding_before =
//         static_cast<std::size_t>(arguments.value("snippetPaddingBefore", 2));
//     req.snippet_padding_after =
//         static_cast<std::size_t>(arguments.value("snippetPaddingAfter", 3));
//     req.trace_lsp_messages = env_flag_enabled("LSP_TRACE_LSP");
//     req.trace_request_timing = env_flag_enabled("LSP_TRACE_RPC");
//
//     const std::filesystem::path report_path = normalize_output_path(
//         req.launch.root_dir,
//         arguments,
//         req.class_name,
//         req.method_name);
//
//     const std::string user_request =
//         arguments.contains("userRequest") && arguments.at("userRequest").is_string()
//             ? arguments.at("userRequest").get<std::string>()
//             : "";
//
//     log_line("jdtls_expand_report service begin"
//              " class=" + req.class_name +
//              " method=" + req.method_name +
//              " direction=" + req.direction +
//              " reportPath=" + report_path.string());
//
//     const ExpandCallsResponse resp =
//         live_session().expand_calls(req);
//
//     ExpandReportOptions report_options;
//     report_options.root_dir = req.launch.root_dir;
//     report_options.user_request = user_request;
//
//     const std::string markdown =
//         render_expand_calls_markdown(req, resp, report_options);
//
//     write_text_file(report_path, markdown);
//
//     log_line("jdtls_expand_report wrote report " + report_path.string());
//
//     json structured{
//         {"ok", true},
//         {"reportPath", report_path.string()},
//         {"reportPathRelative", path_relative_to_root(req.launch.root_dir, report_path)},
//         {"bytesWritten", markdown.size()},
//         {"direction", resp.direction},
//         {"resolvedAnchor", resolved_anchor_json(resp.resolved_anchor)},
//         {"incomingSnippetCount", branch_snippet_count(resp.incoming)},
//         {"incomingTopLevelCount", branch_top_level_count(resp.incoming)},
//         {"outgoingSnippetCount", branch_snippet_count(resp.outgoing)},
//         {"outgoingTopLevelCount", branch_top_level_count(resp.outgoing)}
//     };
//
//     return json{
//         {"content", json::array({
//             {
//                 {"type", "text"},
//                 {"text", "jdtls_expand_report ok: wrote " + report_path.string()}
//             }
//         })},
//         {"structuredContent", structured}
//     };
// }
//


json jdtls_expand_report_result(const json &arguments)
{
    if (!arguments.is_object()) {
        throw std::runtime_error("arguments must be an object");
    }

    ExpandCallsRequest req;
    req.launch = parse_launch_arguments(arguments);
    req.class_name = parse_required_string(arguments, "class");
    req.class_name = extract_class_name(req.class_name);
    req.method_name = parse_required_string(arguments, "method");
    req.direction = arguments.value("direction", std::string("both"));
    req.max_depth = arguments.value("maxDepth", 5);
    req.snippet_padding_before =
        static_cast<std::size_t>(arguments.value("snippetPaddingBefore", 2));
    req.snippet_padding_after =
        static_cast<std::size_t>(arguments.value("snippetPaddingAfter", 3));
    req.trace_lsp_messages = env_flag_enabled("LSP_TRACE_LSP");
    req.trace_request_timing = env_flag_enabled("LSP_TRACE_RPC");

    const std::filesystem::path report_path =
        normalize_output_path(
            req.launch.root_dir,
            arguments,
            "reportPath",
            std::filesystem::path(".codex") /
                "analysis" /
                default_report_file_name(
                    req.class_name,
                    req.method_name));

    const std::filesystem::path mermaid_path =
        normalize_output_path(
            req.launch.root_dir,
            arguments,
            "mermaidPath",
            std::filesystem::path(".codex") /
                "analysis" /
                default_mermaid_file_name(
                    req.class_name,
                    req.method_name));

    const std::filesystem::path svg_path =
        normalize_output_path(
            req.launch.root_dir,
            arguments,
            "svgPath",
            std::filesystem::path(".codex") /
                "analysis" /
                default_svg_file_name(
                    req.class_name,
                    req.method_name));

    const std::string user_request =
        arguments.contains("userRequest") && arguments.at("userRequest").is_string()
            ? arguments.at("userRequest").get<std::string>()
            : "";

    log_line("jdtls_expand_report service begin"
             " class=" + req.class_name +
             " method=" + req.method_name +
             " direction=" + req.direction);

    const ExpandCallsResponse resp =
        live_session().expand_calls(req);

    ExpandReportOptions report_options;
    report_options.root_dir = req.launch.root_dir;
    report_options.user_request = user_request;

    const std::string markdown =
        render_expand_calls_markdown(req, resp, report_options);

    const std::string mermaid =
        render_expand_calls_mermaid(req, resp, report_options);

    write_text_file(report_path, markdown);
    write_text_file(mermaid_path, mermaid);

    MermaidRenderResult render_result =
        render_mermaid_svg(arguments, mermaid_path, svg_path);

    json structured{
        {"ok", true},
        {"direction", resp.direction},
        {"reportPath", report_path.string()},
        {"reportPathRelative", path_relative_to_root(req.launch.root_dir, report_path)},
        {"mermaidPath", mermaid_path.string()},
        {"mermaidPathRelative", path_relative_to_root(req.launch.root_dir, mermaid_path)},
        {"bytesWritten", markdown.size()},
        {"mermaidBytesWritten", mermaid.size()},
        {"resolvedAnchor", resolved_anchor_json(resp.resolved_anchor)},
        {"incomingSnippetCount", branch_snippet_count(resp.incoming)},
        {"incomingTopLevelCount", branch_top_level_count(resp.incoming)},
        {"outgoingSnippetCount", branch_snippet_count(resp.outgoing)},
        {"outgoingTopLevelCount", branch_top_level_count(resp.outgoing)},
        {"svgRenderAttempted", render_result.attempted},
        {"svgRendered", render_result.ok}
    };

    if (render_result.attempted) {
        structured["renderer"] = render_result.renderer;
        structured["svgPath"] = render_result.svg_path.string();
        structured["svgPathRelative"] =
            path_relative_to_root(req.launch.root_dir, render_result.svg_path);
    }

    if (!render_result.error.empty()) {
        structured["svgRenderError"] = render_result.error;
    }

    std::string text =
        "jdtls_expand_report ok: wrote " + report_path.string() +
        " and " + mermaid_path.string();

    if (render_result.ok) {
        text += " and rendered " + render_result.svg_path.string();
    } else if (render_result.attempted && !render_result.error.empty()) {
        text += " (SVG render failed: " + render_result.error + ")";
    }

    return json{
        {"content", json::array({
            {
                {"type", "text"},
                {"text", text}
            }
        })},
        {"structuredContent", structured}
    };
}


} // namespace


int main() 
{

    try {
        install_signal_handlers();
        maybe_redirect_stderr_to_log_file();
    } catch (const std::exception &ex) {
        std::cerr << "[lsp_mcp] fatal logging setup error: "
                  << ex.what() << "\n";
        return 2;
    }
    log_line("server starting");

    std::string line;
    while (g_shutdown_signal == 0 && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        log_line("rx: " + line);

        json msg;
        try {
            msg = json::parse(line);
        } catch (const std::exception &ex) {
            send_json(make_error(nullptr, ERR_PARSE, std::string("parse error: ") + ex.what()));
            continue;
        }

        if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0") {
            send_json(make_error(msg.value("id", nullptr), ERR_INVAL_REQ, "invalid request"));
            continue;
        }

        const std::string method = msg.value("method", "");
        const json id = msg.contains("id") ? msg.at("id") : json(nullptr);
        const bool is_request = msg.contains("id");
        const json params = msg.value("params", json::object());

        if (method == "initialize") {
            if (!is_request) {
                continue;
            }
            send_json(make_result(id, initialize_result(params)));
            continue;
        }

        if (method == "notifications/initialized") {
            log_line("client initialized");
            continue;
        }

        if (method == "ping") {
            if (!is_request) {
                continue;
            }
            send_json(make_result(id, json::object()));
            continue;
        }

        if (method == "tools/list") {
            if (!is_request) {
                continue;
            }
            send_json(make_result(id, json{
                {"tools", json::array({
                    // jdtls_document_symbols_tool_definition(),
                    // jdtls_resolve_anchor_tool_definition(),
                    // jdtls_expand_calls_tool_definition(),
                    jdtls_expand_report_tool_definition()
                })}
            }));
            continue;
        }

        if (method == "tools/call") {
            if (!is_request) {
                continue;
            }

            if (!params.is_object()) {
                send_json(make_error(id, ERR_INVAL_PARAM, "invalid params"));
                continue;
            }

            const std::string tool_name = params.value("name", "");
            const json arguments = params.value("arguments", json::object());

            try {
                // if (tool_name == "jdtls_document_symbols") {
                //     log_line("tools/call jdtls_document_symbols begin");
                //     json result = jdtls_document_symbols_result(arguments);
                //     log_line("tools/call jdtls_document_symbols result ready");
                //     send_json(make_result(id, result));
                //     log_line("tools/call jdtls_document_symbols send done");
                //     continue;
                // }
                //
                // if (tool_name == "jdtls_resolve_anchor") {
                //     log_line("tools/call jdtls_resolve_anchor begin");
                //     json result = jdtls_resolve_anchor_result(arguments);
                //     log_line("tools/call jdtls_resolve_anchor result ready");
                //     send_json(make_result(id, result));
                //     log_line("tools/call jdtls_resolve_anchor send done");
                //     continue;
                // }

                // if (tool_name == "jdtls_expand_calls") {
                //     log_line("tools/call jdtls_expand_calls begin");
                //     json result = jdtls_expand_calls_result(arguments);
                //     log_line("tools/call jdtls_expand_calls result ready");
                //     send_json(make_result(id, result));
                //     log_line("tools/call jdtls_expand_calls send done");
                //     continue;
                // }

                if (tool_name == "jdtls_expand_report") {
                    log_line("tools/call jdtls_expand_report begin");
                    json result = jdtls_expand_report_result(arguments);
                    log_line("tools/call jdtls_expand_report result ready");
                    send_json(make_result(id, result));
                    log_line("tools/call jdtls_expand_report send done");
                    continue;
                }

                // per MCP, unknown tool should be a protocol-level error,
                // not an isError tool result.
                send_json(make_error(
                    id,
                    ERR_NO_METHOD,
                    "tool not found",
                    json{{"tool", tool_name}}
                ));
            } catch (const std::exception &ex) {
                send_json(make_result(id, json{
                    {"content", json::array({
                        {
                            {"type", "text"},
                            {"text", tool_name + " failed: " + std::string(ex.what())}
                        }
                    })},
                    {"structuredContent", {
                        {"ok", false},
                        {"error", ex.what()},
                        {"tool", tool_name}
                    }},
                    {"isError", true}
                }));
            }

            continue;
        }

        if (!is_request) {
            // Unknown notification: ignore.
            continue;
        }


        send_json(make_error(id, -32601, "method not found"));
    }

    // log_line("stdin closed, exiting");


    if (g_shutdown_signal != 0) {
        log_line(std::string("signal received: ") + signal_name(g_shutdown_signal) +
                 ", shutting down live session");
    } else {
        log_line("stdin closed, shutting down live session");
    }
    live_session().shutdown();
    log_line("exiting");
    return g_shutdown_signal == 0 ? 0 : 128 + g_shutdown_signal;
    // return 0;
}

