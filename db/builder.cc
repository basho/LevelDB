// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/filename.h"
#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "table/table_builder2.h"
#include "util/mapbuffer.h"

namespace leveldb {

Status BuildTable(const std::string& dbname,
                  Env* env,
                  const Options& options,
                  const Comparator * user_comparator,
                  TableCache* table_cache,
                  Iterator* iter,
                  FileMetaData* meta,
                  SequenceNumber smallest_snapshot) {
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  KeyRetirement retire(user_comparator, smallest_snapshot);

  std::string fname = TableFileName(dbname, meta->number, meta->level);
  if (iter->Valid()) {
    WritableFile* file;
    s = env->NewWritableFile(fname, &file, true, options.write_buffer_size);
    if (!s.ok()) {
      return s;
    }

    // not all file systems (such as the memenv) support Allocate()
    TableBuilder * builder;
    RiakBufferPtr temp_ptr;
    if (file->SupportsBuilder2())
        builder = new TableBuilder2(options, file, meta->level);
    else
        builder = new TableBuilder(options, file);

    meta->smallest.DecodeFrom(iter->key());
    for (; iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      if (!retire(key))
      {
          meta->largest.DecodeFrom(key);
          builder->Add(key, iter->value());
          ++meta->num_entries;
      }   // if
    }

    // Finish and check for builder errors
    if (s.ok()) {
      s = builder->Finish();
      if (s.ok()) {
        meta->file_size = builder->FileSize();
        assert(meta->file_size > 0);
      }
    } else {
      builder->Abandon();
    }
    delete builder;

    // Finish and check for file errors
    if (s.ok()) {
    uint64_t timer=options.env->NowMicros();
      s = file->Sync();
    timer=options.env->NowMicros()-timer;
    Log(options.info_log, "Sync() micros: %llu", (unsigned long long)timer);
    }
    if (s.ok()) {
    uint64_t timer=options.env->NowMicros();
      s = file->Close();
    timer=options.env->NowMicros()-timer;
    Log(options.info_log, "Close() micros: %llu", (unsigned long long)timer);
    }
    delete file;
    file = NULL;

    if (s.ok()) {
      // Verify that the table is usable
      Iterator* it = table_cache->NewIterator(ReadOptions(),
                                              meta->number,
                                              meta->file_size,
                                              meta->level);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    env->DeleteFile(fname);
  }
  return s;
}

}  // namespace leveldb
