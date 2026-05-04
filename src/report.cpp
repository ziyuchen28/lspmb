#include "lspmb/report.h"


#include <algorithm>
#include <cctype>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lspmb::report {

using namespace lspx::protocol;
using namespace lspx::graph;
using namespace lspx::snippet;
using namespace lspmb::service;

namespace {


std::string format_range(const Range &range) 
{
    std::ostringstream out;
    out << "[" << (range.start.line + 1) << ":" << (range.start.character + 1)
        << " - " << (range.end.line + 1) << ":" << (range.end.character + 1)
        << "]";
    return out.str();
}

std::string display_path(
    const std::filesystem::path &root,
    const std::filesystem::path &path)
{
    if (path.empty()) {
        return "<none>";
    }

    std::error_code ec;
    const std::filesystem::path abs_root =
        std::filesystem::absolute(root, ec).lexically_normal();
    if (ec) {
        return path.string();
    }

    const std::filesystem::path abs_path =
        std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) {
        return path.string();
    }

    const std::filesystem::path rel = abs_path.lexically_relative(abs_root);
    const std::string rel_s = rel.string();

    if (!rel_s.empty() && rel_s.rfind("..", 0) != 0) {
        return rel_s;
    }

    return abs_path.string();
}


struct MermaidState
{
    std::unordered_map<std::string, std::string> node_ids;
    std::unordered_set<std::string> emitted_nodes;
    std::size_t next_id{0};
};

std::string mermaid_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());

    for (char ch : s) {
        switch (ch) {
            case '"':
                out += '&';
                out += 'q';
                out += 'u';
                out += 'o';
                out += 't';
                out += ';';
                break;
            case '\n':
            case '\r':
                out += ' ';
                break;
            default:
                out += ch;
                break;
        }
    }

    return out;
}

std::string mermaid_item_key(const CallHierarchyItem &item)
{
    std::ostringstream out;
    out << item.path.string()
        << "|"
        << item.name
        << "|"
        << item.range.start.line
        << ":"
        << item.range.start.character
        << "-"
        << item.range.end.line
        << ":"
        << item.range.end.character;
    return out.str();
}

std::string mermaid_node_id_for_item(
    MermaidState &state,
    const CallHierarchyItem &item)
{
    const std::string key = mermaid_item_key(item);

    const auto it = state.node_ids.find(key);
    if (it != state.node_ids.end()) {
        return it->second;
    }

    std::ostringstream out;
    out << "n" << state.next_id++;
    const std::string id = out.str();
    state.node_ids.emplace(key, id);
    return id;
}

std::string mermaid_anchor_label(
    const std::filesystem::path &root,
    const ResolvedAnchor &anchor)
{
    std::ostringstream out;
    out << anchor.query
        << "."
        << anchor.function_name
        << "<br/>"
        << display_path(root, anchor.file)
        << ":"
        << (anchor.function_symbol.range.start.line + 1);
    return mermaid_escape(out.str());
}

std::string mermaid_item_label(const std::filesystem::path &root,
                               const CallHierarchyItem &item)
{
    std::ostringstream out;
    out << item.name
        << "<br/>"
        << display_path(root, item.path)
        << ":"
        << (item.range.start.line + 1);
    return mermaid_escape(out.str());
}

void emit_mermaid_node(std::ostream &os,
                       MermaidState &state,
                       const std::string &id,
                       const std::string &label)
{
    if (!state.emitted_nodes.insert(id).second) {
        return;
    }

    os << "    " << id << "[\"" << label << "\"]\n";
}

void append_mermaid_children(
    std::ostream &os,
    MermaidState &state,
    const std::filesystem::path &root,
    const ExpandedNode &node,
    const std::string &parent_id,
    bool incoming)
{
    for (const ExpandedNode &child : node.children) {
        const std::string child_id =
            mermaid_node_id_for_item(state, child.item);

        emit_mermaid_node(
            os,
            state,
            child_id,
            mermaid_item_label(root, child.item));

        if (incoming) {
            os << "    " << child_id << " --> " << parent_id << "\n";
        } else {
            os << "    " << parent_id << " --> " << child_id << "\n";
        }

        append_mermaid_children(os, state, root, child, child_id, incoming);
    }
}


std::string sanitize_file_part(std::string s)
{
    for (char &ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '-' || ch == '_') {
            continue;
        }
        ch = '-';
    }
    return s;
}

std::string line_range(const Range &range)
{
    std::ostringstream out;
    out << (range.start.line + 1);
    if (range.end.line != range.start.line) {
        out << "-" << (range.end.line + 1);
    }
    return out.str();
}

std::string source_snippet_line_range(const SourceSnippet  &snippet)
{
    std::ostringstream out;
    out << snippet.start_line;
    if (snippet.end_line != snippet.start_line) {
        out << "-" << snippet.end_line;
    }
    return out.str();
}

void append_node_tree(
    std::ostream &os,
    const ExpandedNode &node,
    const std::filesystem::path &root,
    int depth)
{
    const std::string indent(static_cast<std::size_t>(depth * 2), ' ');

    os << indent
       << "- `" << node.item.name << "`"
       << " file=`" << display_path(root, node.item.path) << "`"
       << " range=" << format_range(node.item.range);

    if (!node.stop_reason.empty()) {
        os << " stop=`" << node.stop_reason << "`";
    }

    os << "\n";

    for (const Range &range : node.from_ranges) {
        os << indent
           << "  - from=" << format_range(range)
           << "\n";
    }

    for (const ExpandedNode &child : node.children) {
        append_node_tree(os, child, root, depth + 1);
    }
}

void append_snippets(
    std::ostream &os,
    const std::vector<CallGraphSnippet> &snippets,
    const std::filesystem::path &root)
{
    if (snippets.empty()) {
        os << "None returned.\n\n";
        return;
    }

    for (const CallGraphSnippet &snippet : snippets) {
        os << "### " << snippet.item.name << "\n\n";
        os << "- File: `" << display_path(root, snippet.snippet.path) << "`\n";
        os << "- Lines: `" << source_snippet_line_range(snippet.snippet) << "`\n";
        os << "- Stop reason: `"
           << (snippet.stop_reason.empty() ? "none" : snippet.stop_reason)
           << "`\n\n";

        os << "```java\n";
        os << snippet.snippet.numbered_text;
        if (!snippet.snippet.numbered_text.empty() &&
            snippet.snippet.numbered_text.back() != '\n') {
            os << "\n";
        }
        os << "```\n\n";
    }
}

void append_branch(
    std::ostream &os,
    const char *title,
    const std::optional<lspmb::service::ExpandedCallTree> &branch,
    const std::filesystem::path &root)
{
    os << "## " << title << "\n\n";

    if (!branch.has_value()) {
        os << "Not requested.\n\n";
        return;
    }

    os << "### Dependency tree\n\n";
    append_node_tree(os, branch->root, root, 0);
    os << "\n";

    os << "### Code snippets\n\n";
    append_snippets(os, branch->snippets, root);
}

}  // namespace


std::string default_mermaid_file_name(
    const std::string &class_name,
    const std::string &method_name)
{
    return sanitize_file_part(class_name) + "-" +
           sanitize_file_part(method_name) +
           "-dependency.mmd";
}

std::string default_svg_file_name(
    const std::string &class_name,
    const std::string &method_name)
{
    return sanitize_file_part(class_name) + "-" +
           sanitize_file_part(method_name) +
           "-dependency.svg";
}

std::string render_expand_calls_mermaid(
    const ExpandCallsRequest &req,
    const ExpandCallsResponse &resp,
    const ExpandReportOptions &options)
{
    const std::filesystem::path root =
        options.root_dir.empty() ? req.launch.root_dir : options.root_dir;

    MermaidState state;
    std::ostringstream os;

    os << "flowchart LR\n";
    os << "    classDef anchor fill:#f5f7ff,stroke:#333,stroke-width:2px;\n";

    const std::string anchor_id = "anchor";
    emit_mermaid_node(
        os,
        state,
        anchor_id,
        mermaid_anchor_label(root, resp.resolved_anchor));

    os << "    class " << anchor_id << " anchor;\n";

    if (resp.incoming.has_value()) {
        os << "\n";
        os << "    subgraph Incoming[\"Incoming dependencies\"]\n";
        append_mermaid_children(
            os,
            state,
            root,
            resp.incoming->root,
            anchor_id,
            true);
        os << "    end\n";
    }

    if (resp.outgoing.has_value()) {
        os << "\n";
        os << "    subgraph Outgoing[\"Outgoing dependencies\"]\n";
        append_mermaid_children(
            os,
            state,
            root,
            resp.outgoing->root,
            anchor_id,
            false);
        os << "    end\n";
    }

    return os.str();
}


std::string default_report_file_name(
    const std::string &class_name,
    const std::string &method_name)
{
    return sanitize_file_part(class_name) + "-" +
           sanitize_file_part(method_name) +
           "-dependency-report.md";
}

std::string render_expand_calls_markdown(
    const ExpandCallsRequest &req,
    const ExpandCallsResponse &resp,
    const ExpandReportOptions &options)
{
    const std::filesystem::path root =
        options.root_dir.empty() ? req.launch.root_dir : options.root_dir;

    std::ostringstream os;

    os << "# " << resp.resolved_anchor.query
       << "." << resp.resolved_anchor.function_name
       << " dependency report\n\n";

    os << "## 1. Resolved anchor\n\n";
    os << "- Class: `" << resp.resolved_anchor.query << "`\n";
    os << "- Method: `" << resp.resolved_anchor.function_name << "`\n";
    os << "- File: `" << display_path(root, resp.resolved_anchor.file) << "`\n";
    os << "- Method symbol: `" << resp.resolved_anchor.function_symbol.name << "`\n";
    os << "- Method range: `" << format_range(resp.resolved_anchor.function_symbol.range) << "`\n";
    os << "- Candidate count: `" << resp.resolved_anchor.candidate_count << "`\n";
    os << "- Resolve attempts: `" << resp.resolved_anchor.attempts << "`\n\n";

    os << "## 2. Request\n\n";
    os << "- User request: "
       << (options.user_request.empty() ? "`not provided`" : options.user_request)
       << "\n";
    os << "- Direction: `" << resp.direction << "`\n";
    os << "- maxDepth: `" << req.max_depth << "`\n";
    os << "- snippetPaddingBefore: `" << req.snippet_padding_before << "`\n";
    os << "- snippetPaddingAfter: `" << req.snippet_padding_after << "`\n\n";

    append_branch(os, "3. Incoming dependencies", resp.incoming, root);
    append_branch(os, "4. Outgoing dependencies", resp.outgoing, root);

    return os.str();
}

}  // namespace lspmb::report


