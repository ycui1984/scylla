/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdexcept>
#include <cstdlib>

#include "core/align.hh"
#include "core/unaligned.hh"

#include "compress.hh"

#include <lz4.h>
#include <zlib.h>
#include <snappy-c.h>

#include "unimplemented.hh"

namespace sstables {

void compression::update(uint64_t compressed_file_length) {
    // FIXME: also process _compression.options (just for crc-check frequency)
     if (name.value == "LZ4Compressor") {
         _uncompress = uncompress_lz4;
     } else if (name.value == "SnappyCompressor") {
         _uncompress = uncompress_snappy;
     } else if (name.value == "DeflateCompressor") {
         _uncompress = uncompress_deflate;
     } else {
         throw std::runtime_error("unsupported compression type");
     }

     _compressed_file_length = compressed_file_length;
}

void compression::set_compressor(compressor c) {
     if (c == compressor::lz4) {
         _compress = compress_lz4;
         _compress_max_size = compress_max_size_lz4;
         name.value = "LZ4Compressor";
     } else if (c == compressor::snappy) {
         _compress = compress_snappy;
         _compress_max_size = compress_max_size_snappy;
         name.value = "SnappyCompressor";
     } else if (c == compressor::deflate) {
         _compress = compress_deflate;
         _compress_max_size = compress_max_size_deflate;
         name.value = "DeflateCompressor";
     } else {
         throw std::runtime_error("unsupported compressor type");
     }
}

compression::chunk_and_offset
compression::locate(uint64_t position) const {
    auto ucl = uncompressed_chunk_length();
    auto chunk_index = position / ucl;
    decltype(ucl) chunk_offset = position % ucl;
    auto chunk_start = offsets.elements.at(chunk_index);
    auto chunk_end = (chunk_index + 1 == offsets.elements.size())
            ? _compressed_file_length
            : offsets.elements.at(chunk_index + 1);
    return { chunk_start, chunk_end - chunk_start, chunk_offset };
}

}

size_t uncompress_lz4(const char* input, size_t input_len,
        char* output, size_t output_len) {
    // We use LZ4_decompress_safe(). According to the documentation, the
    // function LZ4_decompress_fast() is slightly faster, but maliciously
    // crafted compressed data can cause it to overflow the output buffer.
    // Theoretically, our compressed data is created by us so is not malicious
    // (and accidental corruption is avoided by the compressed-data checksum),
    // but let's not take that chance for now, until we've actually measured
    // the performance benefit that LZ4_decompress_fast() would bring.

    // Cassandra's LZ4Compressor prepends to the chunk its uncompressed length
    // in 4 bytes little-endian (!) order. We don't need this information -
    // we already know the uncompressed data is at most the given chunk size
    // (and usually is exactly that, except in the last chunk). The advance
    // knowledge of the uncompressed size could be useful if we used
    // LZ4_decompress_fast(), but we prefer LZ4_decompress_safe() anyway...
    input += 4;
    input_len -= 4;

    auto ret = LZ4_decompress_safe(input, output, input_len, output_len);
    if (ret < 0) {
        throw std::runtime_error("LZ4 uncompression failure");
    }
    return ret;
}

size_t compress_lz4(const char* input, size_t input_len,
        char* output, size_t output_len) {
    if (output_len < LZ4_COMPRESSBOUND(input_len) + 4) {
        throw std::runtime_error("LZ4 compression failure: length of output is too small");
    }
    // Write input_len (32-bit data) to beginning of output in little-endian representation.
    output[0] = input_len & 0xFF;
    output[1] = (input_len >> 8) & 0xFF;
    output[2] = (input_len >> 16) & 0xFF;
    output[3] = (input_len >> 24) & 0xFF;
    auto ret = LZ4_compress(input, output + 4, input_len);
    if (ret == 0) {
        throw std::runtime_error("LZ4 compression failure: LZ4_compress() failed");
    }
    return ret + 4;
}

size_t uncompress_deflate(const char* input, size_t input_len,
        char* output, size_t output_len) {
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.avail_in = 0;
    zs.next_in = Z_NULL;
    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("deflate uncompression init failure");
    }
    // yuck, zlib is not const-correct, and also uses unsigned char while we use char :-(
    zs.next_in = reinterpret_cast<unsigned char*>(const_cast<char*>(input));
    zs.avail_in = input_len;
    zs.next_out = reinterpret_cast<unsigned char*>(output);
    zs.avail_out = output_len;
    auto res = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (res == Z_STREAM_END) {
        return output_len - zs.avail_out;
    } else {
        throw std::runtime_error("deflate uncompression failure");
    }
}

size_t compress_deflate(const char* input, size_t input_len,
        char* output, size_t output_len) {
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.avail_in = 0;
    zs.next_in = Z_NULL;
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("deflate compression init failure");
    }
    zs.next_in = reinterpret_cast<unsigned char*>(const_cast<char*>(input));
    zs.avail_in = input_len;
    zs.next_out = reinterpret_cast<unsigned char*>(output);
    zs.avail_out = output_len;
    auto res = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (res == Z_STREAM_END) {
        return output_len - zs.avail_out;
    } else {
        throw std::runtime_error("deflate compression failure");
    }
}

size_t uncompress_snappy(const char* input, size_t input_len,
        char* output, size_t output_len) {
    if (snappy_uncompress(input, input_len, output, &output_len)
            == SNAPPY_OK) {
        return output_len;
    } else {
        throw std::runtime_error("deflate uncompression failure");
    }
}

size_t compress_snappy(const char* input, size_t input_len,
        char* output, size_t output_len) {
    auto ret = snappy_compress(input, input_len, output, &output_len);
    if (ret != SNAPPY_OK) {
        throw std::runtime_error("snappy compression failure: snappy_compress() failed");
    }
    return output_len;
}

size_t compress_max_size_lz4(size_t input_len) {
    return LZ4_COMPRESSBOUND(input_len) + 4;
}

size_t compress_max_size_deflate(size_t input_len) {
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.avail_in = 0;
    zs.next_in = Z_NULL;
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        throw std::runtime_error("deflate compression init failure");
    }
    auto res = deflateBound(&zs, input_len);
    deflateEnd(&zs);
    return res;
}

size_t compress_max_size_snappy(size_t input_len) {
    return snappy_max_compressed_length(input_len);
}

class compressed_file_data_source_impl : public data_source_impl {
    file _file;
    sstables::compression* _compression_metadata;
    uint64_t _pos = 0;
    const io_priority_class* _pc;
public:
    compressed_file_data_source_impl(file f, const io_priority_class& pc,
            sstables::compression* cm, uint64_t pos)
            : _file(std::move(f)), _compression_metadata(cm)
            , _pos(pos)
            , _pc(&pc)
            {}
    virtual future<temporary_buffer<char>> get() override {
        if (_pos >= _compression_metadata->data_len) {
            return make_ready_future<temporary_buffer<char>>();
        }
        auto addr = _compression_metadata->locate(_pos);
        return _file.dma_read_exactly<char>(addr.chunk_start, addr.chunk_len, *_pc).
            then([this, addr](temporary_buffer<char> buf) {
                // The last 4 bytes of the chunk are the adler32 checksum
                // of the rest of the (compressed) chunk.
                auto compressed_len = addr.chunk_len - 4;
                // FIXME: Do not always calculate checksum - Cassandra has a
                // probability (defaulting to 1.0, but still...)
                uint32_t checksum = ntohl(*unaligned_cast<const uint32_t *>(
                        buf.get() + compressed_len));
                if (checksum != checksum_adler32(buf.get(), compressed_len)) {
                    throw std::runtime_error("compressed chunk failed checksum");
                }

                // We know that the uncompressed data will take exactly
                // chunk_length bytes (or less, if reading the last chunk).
                temporary_buffer<char> out(
                        _compression_metadata->uncompressed_chunk_length());
                // The compressed data is the whole chunk, minus the last 4
                // bytes (which contain the checksum verified above).
                auto len = _compression_metadata->uncompress(
                        buf.get(), compressed_len,
                        out.get_write(), out.size());
                out.trim(len);
                out.trim_front(addr.offset);
                _pos += out.size();
                return out;
        });
    }
};

class compressed_file_data_source : public data_source {
public:
    compressed_file_data_source(file f, const io_priority_class& pc,
            sstables::compression* cm, uint64_t offset)
        : data_source(std::make_unique<compressed_file_data_source_impl>(
                std::move(f), pc, cm, offset))
        {}
};

input_stream<char> make_compressed_file_input_stream(
        file f, sstables::compression* cm, const io_priority_class& pc, uint64_t offset)
{
    return input_stream<char>(compressed_file_data_source(
            std::move(f), pc, cm, offset));
}
