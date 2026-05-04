
#include "lspmb/service.h"

#include "pcr/ipc/stdio_jsonrpc_session.h"

#include <filesystem>
#include <stdexcept>
#include <utility>
#include <iostream>
#include <string_view>

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace lspmb::service {


using namespace lspx::protocol;
using namespace lspx::client;
using namespace lspx::graph;
using namespace lspx::driver::jdtls;
using namespace pcr::ipc;


namespace {


void service_trace_line(bool enabled, std::string_view line)
{
    if (!enabled) {
        return;
    }

    std::cerr << "[service] " << line << "\n";
    std::cerr.flush();
}

struct SessionEntry
{
    lspx::driver::jdtls::LaunchOptions launch;
    lspx::protocol::InitializeResult initialize;
    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
    // std::unique_ptr<StderrDrainer> stderr_drainer;
    std::unique_ptr<Session> session;
};


bool same_live_config(
    const SessionEntry &entry,
    const LaunchOptions &launch)
{
    return entry.launch.root_dir == launch.root_dir &&
           entry.launch.workspace_dir == launch.workspace_dir &&
           entry.launch.jdtls_home == launch.jdtls_home &&
           entry.launch.java_bin == launch.java_bin &&
           entry.launch.xms_mb == launch.xms_mb &&
           entry.launch.xmx_mb == launch.xmx_mb &&
           entry.launch.log_protocol == launch.log_protocol &&
           entry.launch.log_level == launch.log_level;
}

SessionOptions make_session_options(
    const LaunchOptions &launch,
    bool trace_lsp_messages,
    bool trace_request_timing)
{
    SessionOptions options;
    options.root_dir = launch.root_dir;
    options.trace_lsp_messages = trace_lsp_messages;
    options.trace_request_timing = trace_request_timing;
    return options;
}

SessionEntry create_session_entry(
    const LaunchOptions &launch,
    bool trace_lsp_messages,
    bool trace_request_timing)
{
    const bool trace = trace_lsp_messages || trace_request_timing;

    SessionOptions session_options =
        make_session_options(launch, trace_lsp_messages, trace_request_timing);


    service_trace_line(trace, "session spawn begin");
    StdioJsonRpcLaunchConfig cfg = to_ipc_launch_config(launch);
    Session session_value = Session::spawn(cfg, session_options);
    service_trace_line(trace, "session spawn done");
    auto session = std::make_unique<Session>(std::move(session_value));

    service_trace_line(trace, "initialize begin");
    InitializeResult initialize = session->initialize();
    service_trace_line(trace, "initialize done");

    session->initialized();
    service_trace_line(trace, "initialized notification sent");

    SessionEntry entry;
    entry.launch = launch;
    entry.initialize = std::move(initialize);
    entry.trace_lsp_messages = trace_lsp_messages;
    entry.trace_request_timing = trace_request_timing;
    entry.session = std::move(session);
    return entry;
}

void best_effort_shutdown(SessionEntry &entry) noexcept
{
    if (!entry.session) return;

    const bool trace = entry.trace_lsp_messages || entry.trace_request_timing;
    // shutdown_graceful(*entry.session, trace);
    entry.session->shutdown();
    entry.session.reset();
}


std::filesystem::path normalize_abs(const std::filesystem::path &path)
{
    return std::filesystem::absolute(path).lexically_normal();
}


void validate_launch(const LaunchOptions &launch)
{
    if (launch.root_dir.empty()) {
        throw std::runtime_error("LaunchOptions.root_dir must not be empty");
    }
    if (launch.workspace_dir.empty()) {
        throw std::runtime_error("LaunchOptions.workspace_dir must not be empty");
    }
    if (launch.jdtls_home.empty()) {
        throw std::runtime_error("LaunchOptions.jdtls_home must not be empty");
    }
    if (launch.java_bin.empty()) {
        throw std::runtime_error("LaunchOptions.java_bin must not be empty");
    }
}


LaunchOptions prepare_launch(const LaunchOptions &input, bool trace)
{
    LaunchOptions launch = input;
    validate_launch(launch);

    launch.root_dir = normalize_abs(launch.root_dir);
    launch.workspace_dir = normalize_abs(launch.workspace_dir);
    launch.jdtls_home = normalize_abs(launch.jdtls_home);

    service_trace_line(trace, "prepare_launch root=" + launch.root_dir.string());
    service_trace_line(trace, "prepare_launch workspaceDir=" + launch.workspace_dir.string());
    service_trace_line(trace, "prepare_launch jdtlsHome=" + launch.jdtls_home.string());
    service_trace_line(trace, "prepare_launch javaBin=" + launch.java_bin);

    std::error_code ec;
    const bool existed_before = std::filesystem::exists(launch.workspace_dir, ec);
    if (ec) {
        throw std::runtime_error(
            "failed to check workspaceDir: " + launch.workspace_dir.string() +
            ": " + ec.message());
    }

    std::filesystem::create_directories(launch.workspace_dir, ec);
    if (ec) {
        throw std::runtime_error(
            "failed to create workspaceDir: " + launch.workspace_dir.string() +
            ": " + ec.message());
    }

    service_trace_line(
        trace,
        std::string("prepare_launch workspaceDir ") +
        (existed_before ? "already exists: " : "created: ") +
        launch.workspace_dir.string());
    return launch;
}


template <typename Fn>
auto with_one_shot_session(
    const LaunchOptions &launch,
    bool trace_lsp_messages,
    bool trace_request_timing,
    Fn &&fn)
{
    const bool trace = trace_lsp_messages || trace_request_timing;
    // service_trace_line(trace, "spawn begin");
    // auto child = spawn(launch, current_platform());
    // service_trace_line(trace, "spawn done");


    // std::optional<StderrDrainer> stderr_drainer;
    // if (const char *path = std::getenv("CLSPC_CHILD_STDERR_LOG")) {
    //     // handle empty path
    //     if (*path != '\0') {
    //         service_trace_line(trace, "stderr drainer begin");
    //         stderr_drainer.emplace(child.stderr_read_fd(), path);
    //         service_trace_line(trace, "stderr drainer ready");
    //     }
    // }

    // SessionOptions session_options;
    // session_options.root_dir = launch.root_dir;
    // session_options.trace_lsp_messages = trace_lsp_messages;
    // session_options.trace_request_timing = trace_request_timing;
    //
    // Session session(std::move(child), session_options);

    SessionOptions session_options = 
        make_session_options(launch, trace_lsp_messages, trace_request_timing);

    service_trace_line(trace, "session spawn begin");

    StdioJsonRpcLaunchConfig cfg = to_ipc_launch_config(launch);
    Session session = 
        Session::spawn(cfg, session_options);
    service_trace_line(trace, "session spawn done");

    try {
        service_trace_line(trace, "initialize begin");
        const InitializeResult initialize = session.initialize();
        service_trace_line(trace, "initialize done");

        session.initialized();
        service_trace_line(trace, "initialized notification sent");

        service_trace_line(trace, "handler begin");
        auto out = std::forward<Fn>(fn)(session, initialize);
        service_trace_line(trace, "handler done");

        // service_trace_line(trace, "shutdown_and_exit begin");
        // session.shutdown_and_exit();
        // service_trace_line(trace, "shutdown_and_exit done");
        //
        // service_trace_line(trace, "wait begin");
        // session.wait();
        // service_trace_line(trace, "wait done");

        // shutdown_graceful(session, trace);
        session.shutdown();
        return out;
    } catch (...) {
        service_trace_line(trace, "exception path entered");

        // try {
        //     service_trace_line(trace, "shutdown_and_exit begin (exception path)");
        //     session.shutdown_and_exit();
        //     service_trace_line(trace, "shutdown_and_exit done (exception path)");
        // } catch (...) {
        //     service_trace_line(trace, "shutdown_and_exit failed (exception path)");
        // }
        //
        // try {
        //     service_trace_line(trace, "wait begin (exception path)");
        //     session.wait();
        //     service_trace_line(trace, "wait done (exception path)");
        // } catch (...) {
        //     service_trace_line(trace, "wait failed (exception path)");
        // }

        // shutdown_graceful(session, trace);
        session.shutdown();
        throw;
    }
}


}  // namespace


// InitializeProbeResponse run_initialize_probe(const InitializeProbeRequest &req)
// {
//     const LaunchOptions launch = prepare_launch(req.launch);
//
//     return with_one_shot_session(
//         launch,
//         req.trace_lsp_messages,
//         req.trace_request_timing,
//         [](Session &session, const InitializeResult &initialize) {
//             (void)session;
//             InitializeProbeResponse out;
//             out.initialize = initialize;
//             return out;
//         });
// }
//
//
// DocumentSymbolsResponse run_document_symbols(const DocumentSymbolsRequest &req)
// {
//     if (req.file.empty()) {
//         throw std::runtime_error("DocumentSymbolsRequest.file must not be empty");
//     }
//
//     const LaunchOptions launch = prepare_launch(req.launch);
//     const std::filesystem::path file = normalize_abs(req.file);
//
//     return with_one_shot_session(
//         launch,
//         req.trace_lsp_messages,
//         req.trace_request_timing,
//         [&](Session &session, const InitializeResult &initialize) {
//             (void)initialize;
//
//             DocumentSymbolsResponse out;
//             out.file = file;
//             out.symbols = session.document_symbols(file);
//             return out;
//         });
// }
//
//
// ResolveAnchorResponse run_resolve_anchor(const ResolveAnchorRequest &req)
// {
//     if (req.class_name.empty()) {
//         throw std::runtime_error("ResolveAnchorRequest.class_name must not be empty");
//     }
//
//     if (req.method_name.empty()) {
//         throw std::runtime_error("ResolveAnchorRequest.method_name must not be empty");
//     }
//
//     const LaunchOptions launch = prepare_launch(req.launch);
//
//     return with_one_shot_session(
//         launch,
//         req.trace_lsp_messages,
//         req.trace_request_timing,
//         [&](Session &session, const InitializeResult &initialize) {
//             (void)initialize;
//
//             ResolveAnchorOptions options;
//             options.scope_root = launch.root_dir;
//             options.ready_timeout = req.ready_timeout;
//             options.retry_interval = req.retry_interval;
//
//             ResolveAnchorResponse out;
//             out.anchor = resolve_anchor(session,
//                                                req.class_name,
//                                                req.method_name,
//                                                options);
//             return out;
//         });
// }
//

struct LiveSession::Impl
{
    std::optional<SessionEntry> entry;

    SessionEntry &ensure_started(const LaunchOptions &launch,
                                 bool trace_lsp_messages,
                                 bool trace_request_timing)
    {
        const bool trace = trace_lsp_messages || trace_request_timing;

        if (entry.has_value()) {
            if (!same_live_config(*entry, launch)) {
                throw std::runtime_error(
                    "LiveSession already bound to workspace '" +
                    entry->launch.workspace_dir.string() +
                    "'. Restart the MCP server before switching workspaces.");
            }

            service_trace_line(trace, "reusing live session workspace=" +
                                      launch.workspace_dir.string());
            return *entry;
        }

        service_trace_line(trace, "creating live session workspace=" +
                                  launch.workspace_dir.string());

        entry = create_session_entry(launch,
                                     trace_lsp_messages,
                                     trace_request_timing);
        return *entry;
    }
};

LiveSession::LiveSession()
    : impl_(std::make_unique<Impl>())
{
}

LiveSession::~LiveSession()
{
    shutdown();
}

void LiveSession::shutdown() noexcept
{
    if (!impl_) {
        return;
    }

    if (impl_->entry.has_value()) {
        best_effort_shutdown(*impl_->entry);
        impl_->entry.reset();
    }
}

// InitializeProbeResponse LiveSession::initialize_probe(const InitializeProbeRequest &req)
// {
//     const LaunchOptions launch = prepare_launch(req.launch);
//
//     SessionEntry &entry = impl_->ensure_started(
//         launch,
//         req.trace_lsp_messages,
//         req.trace_request_timing);
//
//     InitializeProbeResponse out;
//     out.initialize = entry.initialize;
//     return out;
// }
//
// DocumentSymbolsResponse LiveSession::document_symbols(const DocumentSymbolsRequest &req)
// {
//     if (req.file.empty()) {
//         throw std::runtime_error("DocumentSymbolsRequest.file must not be empty");
//     }
//
//     const LaunchOptions launch = prepare_launch(req.launch);
//     const std::filesystem::path file = normalize_abs(req.file);
//
//     SessionEntry &entry = impl_->ensure_started(
//         launch,
//         req.trace_lsp_messages,
//         req.trace_request_timing);
//
//     DocumentSymbolsResponse out;
//     out.file = file;
//     out.symbols = entry.session->document_symbols(file);
//     return out;
// }
//
// ResolveAnchorResponse LiveSession::resolve_anchor(const ResolveAnchorRequest &req)
// {
//     if (req.class_name.empty()) {
//         throw std::runtime_error("ResolveAnchorRequest.class_name must not be empty");
//     }
//
//     if (req.method_name.empty()) {
//         throw std::runtime_error("ResolveAnchorRequest.method_name must not be empty");
//     }
//
//     const LaunchOptions launch = prepare_launch(req.launch);
//
//     SessionEntry &entry = impl_->ensure_started(
//         launch,
//         req.trace_lsp_messages,
//         req.trace_request_timing);
//
//     ResolveAnchorOptions options;
//     options.scope_root = launch.root_dir;
//     options.ready_timeout = req.ready_timeout;
//     options.retry_interval = req.retry_interval;
//
//     ResolveAnchorResponse out;
//     out.anchor = lspx::graph::resolve_anchor(
//         *entry.session,
//         req.class_name,
//         req.method_name,
//         options);
//     return out;
// }
//

ExpandCallsResponse LiveSession::expand_calls(const ExpandCallsRequest &req)
{
    if (req.class_name.empty()) {
        throw std::runtime_error("ExpandCallsRequest.class_name must not be empty");
    }
    if (req.method_name.empty()) {
        throw std::runtime_error("ExpandCallsRequest.method_name must not be empty");
    }
    if (req.direction != "outgoing" &&
        req.direction != "incoming" &&
        req.direction != "both") {
        throw std::runtime_error(
            "ExpandCallsRequest.direction currently supports only "
            "'outgoing', 'incoming', or 'both'");
    }

    const bool trace = req.trace_lsp_messages || req.trace_request_timing;
    const LaunchOptions launch = prepare_launch(req.launch, trace);
    auto &entry = impl_->ensure_started(
        launch,
        req.trace_lsp_messages,
        req.trace_request_timing);

    ResolveAnchorOptions resolve_options;
    resolve_options.scope_root = launch.root_dir;
    resolve_options.ready_timeout = req.ready_timeout;
    resolve_options.retry_interval = req.retry_interval;

    const ResolvedAnchor anchor = lspx::graph::resolve_anchor(
        *entry.session,
        req.class_name,
        req.method_name,
        resolve_options);

    lspx::graph::ExpandOptions graph_options;
    graph_options.scope_root = launch.root_dir;
    graph_options.max_depth = req.max_depth;
    graph_options.ready_timeout = req.ready_timeout;
    graph_options.retry_interval = req.retry_interval;

    lspx::snippet::SourceSnippetOptions snippet_options;
    snippet_options.padding_before = req.snippet_padding_before;
    snippet_options.padding_after = req.snippet_padding_after;

    ExpandCallsResponse resp;
    resp.direction = req.direction;
    resp.resolved_anchor = anchor;


    if (req.direction == "outgoing" || req.direction == "both") {
        auto result = lspx::graph::expand_outgoing_from_function(
            *entry.session,
            anchor.file,
            req.method_name,
            graph_options);

        resp.outgoing = ExpandedCallTree{
            .root = std::move(result.root),
            .snippets = lspx::snippet::collect_call_graph_snippets_from_disk(
                result.root,
                snippet_options),
        };
    }

    if (req.direction == "incoming" || req.direction == "both") {
        auto result = lspx::graph::expand_incoming_to_function(
            *entry.session,
            anchor.file,
            req.method_name,
            graph_options);

        resp.incoming = ExpandedCallTree{
            .root = std::move(result.root),
            .snippets = lspx::snippet::collect_call_graph_snippets_from_disk(
                result.root,
                snippet_options),
        };
    }


    return resp;
}


}  // namespace lspmb::service


