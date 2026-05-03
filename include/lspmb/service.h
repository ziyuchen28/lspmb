#pragma once

// #include "lspx/driver/jdtls/driver.h"
// #include "lspx/protocol/types.h"
// #include "lspx/graph/callgraph.h"
// #include "lspx/snippet/callgraph_snippet.h"

#include "lspx/lspx_jdtls.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clspc::service {

struct InitializeProbeRequest
{
    lspx::driver::jdtls::LaunchOptions launch;
    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

struct InitializeProbeResponse
{
    lspx::protocol::InitializeResult initialize;
};

struct DocumentSymbolsRequest
{
    lspx::driver::jdtls::LaunchOptions launch;
    std::filesystem::path file;
    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

struct DocumentSymbolsResponse
{
    std::filesystem::path file;
    std::vector<lspx::protocol::DocumentSymbol> symbols;
};

struct ResolveAnchorRequest
{
    lspx::driver::jdtls::LaunchOptions launch;

    std::string class_name;
    std::string method_name;

    std::chrono::milliseconds ready_timeout{std::chrono::milliseconds{20000}};
    std::chrono::milliseconds retry_interval{std::chrono::milliseconds{250}};

    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

struct ResolveAnchorResponse
{
    lspx::graph::ResolvedAnchor anchor;
};

struct ExpandCallsRequest
{
    lspx::driver::jdtls::LaunchOptions launch;

    std::string class_name;
    std::string method_name;

    std::string direction{"incoming"};

    int max_depth{3};
    std::size_t snippet_padding_before{1};
    std::size_t snippet_padding_after{1};

    std::chrono::milliseconds ready_timeout{std::chrono::milliseconds{20000}};
    std::chrono::milliseconds retry_interval{std::chrono::milliseconds{250}};

    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

struct ExpandedCallTree
{
    lspx::graph::ExpandedNode root;
    std::vector<lspx::snippet::CallGraphSnippet> snippets;
};

struct ExpandCallsResponse
{
    std::string direction;
    lspx::graph::ResolvedAnchor resolved_anchor;

    std::optional<ExpandedCallTree> outgoing;
    std::optional<ExpandedCallTree> incoming;
};

InitializeProbeResponse run_initialize_probe(const InitializeProbeRequest &req);
DocumentSymbolsResponse run_document_symbols(const DocumentSymbolsRequest &req);
ResolveAnchorResponse run_resolve_anchor(const ResolveAnchorRequest &req);

class LiveSession
{
public:
    LiveSession();
    ~LiveSession();

    LiveSession(const LiveSession &) = delete;
    LiveSession &operator=(const LiveSession &) = delete;

    InitializeProbeResponse initialize_probe(const InitializeProbeRequest &req);
    DocumentSymbolsResponse document_symbols(const DocumentSymbolsRequest &req);
    ResolveAnchorResponse resolve_anchor(const ResolveAnchorRequest &req);
    ExpandCallsResponse expand_calls(const ExpandCallsRequest &req);

    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clspc::service
