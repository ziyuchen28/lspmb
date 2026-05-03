#pragma once

#include "lspmb/service.h"

#include <filesystem>
#include <string>

namespace lspmb::report {

struct ExpandReportOptions
{
    std::filesystem::path root_dir;
    std::string user_request;
};

std::string default_report_file_name(
    const std::string &class_name,
    const std::string &method_name);

std::string default_mermaid_file_name(
    const std::string &class_name,
    const std::string &method_name);

std::string render_expand_calls_markdown(
    const lspmb::service::ExpandCallsRequest &req,
    const lspmb::service::ExpandCallsResponse &resp,
    const ExpandReportOptions &options);

std::string default_svg_file_name(
    const std::string &class_name,
    const std::string &method_name);

std::string render_expand_calls_markdown(
    const lspmb::service::ExpandCallsRequest &req,
    const lspmb::service::ExpandCallsResponse &resp,
    const ExpandReportOptions &options);

std::string render_expand_calls_mermaid(
    const lspmb::service::ExpandCallsRequest &req,
    const lspmb::service::ExpandCallsResponse &resp,
    const ExpandReportOptions &options);

}  // namespace lspmb::report
