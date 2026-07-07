#pragma once

// Convex file storage helpers. The flow (matching convex-js):
//   1. Your app exposes a mutation calling ctx.storage.generateUploadUrl().
//   2. store_file() POSTs the bytes to that URL; the response carries the
//      new storage id: {"storageId": "..."}.
//   3. A query calling ctx.storage.getUrl(id) yields a download URL;
//      fetch_file() GETs it.

#include <convex/error.h>
#include <convex/transport.h>
#include <convex/value.h>

#include <functional>
#include <future>
#include <string>

namespace convex {

/// Upload `data` to a generated upload URL. On success the result value is
/// the response object, e.g. {"storageId": "kg2..."}.
void store_file(http_transport& transport, const std::string& upload_url,
                const std::string& content_type, bytes data,
                std::function<void(function_result)> on_done);

std::future<function_result> store_file(http_transport& transport, const std::string& upload_url,
                                        const std::string& content_type, bytes data);

/// Download a file from a URL produced by ctx.storage.getUrl(). On success
/// the result value is the raw content as Bytes.
void fetch_file(http_transport& transport, const std::string& url,
                std::function<void(function_result)> on_done);

std::future<function_result> fetch_file(http_transport& transport, const std::string& url);

}  // namespace convex
