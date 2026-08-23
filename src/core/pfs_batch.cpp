#include "ps2hdd/pfs_batch.hpp"

#include <utility>

namespace ps2hdd::pfs {

BatchWriteResult write_batch(ImageWriter& writer,
                             std::span<const BatchFile> files,
                             const BatchWriteOptions& options)
{
    BatchWriteResult result;
    result.files.reserve(files.size());

    if (!writer.valid()) {
        result.error = writer.error().empty() ? "PFS batch writer session is invalid" : writer.error();
        return result;
    }

    bool required_failure = false;
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        BatchFileResult item;
        item.index = index;
        item.path = file.path;
        item.required = file.required;
        item.write = writer.write_file(file.path, file.bytes, file.options);

        ++result.attempted;
        result.metadata_transactions += item.write.metadata_transactions;
        result.payload_write_calls += item.write.payload_write_calls;
        result.bitmap_chunks_touched += item.write.bitmap_chunks_touched;

        if (item.write.ok) {
            ++result.succeeded;
            result.bytes_committed += item.write.bytes;
        } else {
            ++result.failed;
            if (!file.required) {
                ++result.optional_failed;
            } else {
                required_failure = true;
                if (result.error.empty()) {
                    result.error = "Required PFS batch item failed: " + file.path;
                    if (!item.write.error.empty()) {
                        result.error += ": " + item.write.error;
                    }
                }
            }
        }

        result.files.emplace_back(std::move(item));
        if (required_failure && options.stop_on_required_failure) {
            break;
        }
    }

    result.partial = result.failed != 0 || result.attempted != files.size();
    result.ok = !required_failure;
    return result;
}

} // namespace ps2hdd::pfs
