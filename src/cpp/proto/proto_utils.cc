/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/cpp/proto/proto_utils.h"
#include <grpc++/config.h>

#include <grpc/grpc.h>
#include <grpc/byte_buffer.h>
#include <grpc/support/slice.h>
#include <grpc/support/slice_buffer.h>
#include <grpc/support/port_platform.h>

const int kMaxBufferLength = 8192;

class GrpcBufferWriter GRPC_FINAL
    : public ::grpc::protobuf::io::ZeroCopyOutputStream {
 public:
  explicit GrpcBufferWriter(grpc_byte_buffer **bp,
                            int block_size = kMaxBufferLength)
      : block_size_(block_size), byte_count_(0), have_backup_(false) {
    *bp = grpc_byte_buffer_create(NULL, 0);
    slice_buffer_ = &(*bp)->data.slice_buffer;
  }

  ~GrpcBufferWriter() GRPC_OVERRIDE {
    if (have_backup_) {
      gpr_slice_unref(backup_slice_);
    }
  }

  bool Next(void **data, int *size) GRPC_OVERRIDE {
    if (have_backup_) {
      slice_ = backup_slice_;
      have_backup_ = false;
    } else {
      slice_ = gpr_slice_malloc(block_size_);
    }
    *data = GPR_SLICE_START_PTR(slice_);
    byte_count_ += *size = GPR_SLICE_LENGTH(slice_);
    gpr_slice_buffer_add(slice_buffer_, slice_);
    return true;
  }

  void BackUp(int count) GRPC_OVERRIDE {
    gpr_slice_buffer_pop(slice_buffer_);
    if (count == block_size_) {
      backup_slice_ = slice_;
    } else {
      backup_slice_ =
          gpr_slice_split_tail(&slice_, GPR_SLICE_LENGTH(slice_) - count);
      gpr_slice_buffer_add(slice_buffer_, slice_);
    }
    have_backup_ = true;
    byte_count_ -= count;
  }

  grpc::protobuf::int64 ByteCount() const GRPC_OVERRIDE { return byte_count_; }

 private:
  const int block_size_;
  gpr_int64 byte_count_;
  gpr_slice_buffer *slice_buffer_;
  bool have_backup_;
  gpr_slice backup_slice_;
  gpr_slice slice_;
};

class GrpcBufferReader GRPC_FINAL
    : public ::grpc::protobuf::io::ZeroCopyInputStream {
 public:
  explicit GrpcBufferReader(grpc_byte_buffer *buffer)
      : byte_count_(0), backup_count_(0) {
    reader_ = grpc_byte_buffer_reader_create(buffer);
  }
  ~GrpcBufferReader() GRPC_OVERRIDE {
    grpc_byte_buffer_reader_destroy(reader_);
  }

  bool Next(const void **data, int *size) GRPC_OVERRIDE {
    if (backup_count_ > 0) {
      *data = GPR_SLICE_START_PTR(slice_) + GPR_SLICE_LENGTH(slice_) -
              backup_count_;
      *size = backup_count_;
      backup_count_ = 0;
      return true;
    }
    if (!grpc_byte_buffer_reader_next(reader_, &slice_)) {
      return false;
    }
    gpr_slice_unref(slice_);
    *data = GPR_SLICE_START_PTR(slice_);
    byte_count_ += *size = GPR_SLICE_LENGTH(slice_);
    return true;
  }

  void BackUp(int count) GRPC_OVERRIDE {
    backup_count_ = count;
  }

  bool Skip(int count) GRPC_OVERRIDE {
    const void *data;
    int size;
    while (Next(&data, &size)) {
      if (size >= count) {
        BackUp(size - count);
        return true;
      }
      // size < count;
      count -= size;
    }
    // error or we have too large count;
    return false;
  }

  grpc::protobuf::int64 ByteCount() const GRPC_OVERRIDE {
    return byte_count_ - backup_count_;
  }

 private:
  gpr_int64 byte_count_;
  gpr_int64 backup_count_;
  grpc_byte_buffer_reader *reader_;
  gpr_slice slice_;
};

namespace grpc {

bool SerializeProto(const grpc::protobuf::Message &msg, grpc_byte_buffer **bp) {
  GrpcBufferWriter writer(bp);
  return msg.SerializeToZeroCopyStream(&writer);
}

bool DeserializeProto(grpc_byte_buffer *buffer, grpc::protobuf::Message *msg) {
  GrpcBufferReader reader(buffer);
  return msg->ParseFromZeroCopyStream(&reader);
}

}  // namespace grpc
