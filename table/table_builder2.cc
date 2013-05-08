// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// A riak addon to original TableBuilder creating parallel compression for multiple blocks

#include "table/table_builder2.h"
#include "table/table_builder_rep.h"

#include <unistd.h>
#include <assert.h>
#include <sstream>
#include <syslog.h>
#include <errno.h>
#include <pthread.h>
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/mapbuffer.h"

namespace leveldb {


TableBuilder2::TableBuilder2(
    const Options& options,
    WritableFile* file,
    int PriorityLevel)
    : TableBuilder(options, file), m_Abort(false), m_Finish(false),
    m_CondVar(&m_CvMutex), m_NextAdd(0), m_NextWrite(0),
    m_PriorityLevel(PriorityLevel)
{
    int loop, ret_val;

    for (loop=0; loop<eTB2Buffers; ++loop)
        m_Blocks[loop].m_Block.SetOptions(&rep_->options);

    for (loop=0; loop<eTB2Threads; ++loop)
    {
        ret_val=pthread_create(&m_Writers[loop], NULL, &WorkerThreadEntry, this);
        if (0!=ret_val)
            Log(options.info_log, "thread creation failure in TableBuilder2 (ret_val=%d)", ret_val);
    }   // for

    m_TimerReadWait=0;

    return;

}   // TableBuilder2::TableBuilder2


TableBuilder2::~TableBuilder2()
{
    // ?? double check threads are joined?
    Log(rep_->options.info_log, "m_TimerReadWait: %llu", (unsigned long long)m_TimerReadWait);

    return;
}   // TableBuilder2::~TableBuilder2


// reminder:  only one thread calling Add(), but it interacts with background
//            threads performing write operations
void
TableBuilder2::Add(
    const Slice& key,
    const Slice& value)
{
    BlockNState * block_ptr;
    Rep* r = rep_;

    assert(!r->closed);
    if (!ok()) return;

    block_ptr=NULL;

    // quick test without lock, most common case is state is already useful
    if (BlockNState::eBNStateLoading!=m_Blocks[m_NextAdd].m_State
        && BlockNState::eBNStateEmpty!=m_Blocks[m_NextAdd].m_State)
    {
        uint64_t timer=r->options.env->NowMicros();

        port::MutexLock lock(m_CvMutex);

        // loop until state is helpful
        while(BlockNState::eBNStateLoading!=m_Blocks[m_NextAdd].m_State
              && BlockNState::eBNStateEmpty!=m_Blocks[m_NextAdd].m_State)
        {
            m_CondVar.Wait();
        }   // while

        m_TimerReadWait+=r->options.env->NowMicros() - timer;
    }   // if

    block_ptr=&m_Blocks[m_NextAdd];

    // sanity tests
    assert(BlockNState::eBNStateLoading==m_Blocks[m_NextAdd].m_State
           || BlockNState::eBNStateEmpty==m_Blocks[m_NextAdd].m_State);

//    if (r->num_entries > 0 )
    if (BlockNState::eBNStateEmpty!=block_ptr->m_State)
    {
        assert(r->options.comparator->Compare(key, Slice(block_ptr->m_LastKey)) > 0);
    }

    // this is first key of this new block
    // shorten key of prior block, if it exists
    if (BlockNState::eBNStateEmpty==block_ptr->m_State)
    {
        {
            port::MutexLock lock(m_CvMutex);
            int idx;

            assert(block_ptr->m_Block.empty());
            block_ptr->m_State=BlockNState::eBNStateLoading;
            idx= (m_NextAdd +eTB2Buffers-1) % eTB2Buffers;

            if (BlockNState::eBNStateEmpty!=m_Blocks[idx].m_State)
            {
                r->options.comparator->FindShortestSeparator(&m_Blocks[idx].m_LastKey, key);
                assert(false==m_Blocks[idx].m_KeyShortened);
                m_Blocks[idx].m_KeyShortened=true;

                // if block's progress is waiting for this key ... mark it ready
                if (BlockNState::eBNStateKeyWait==m_Blocks[idx].m_State)
                {
                    m_Blocks[idx].m_State=BlockNState::eBNStateReady;
                    m_CondVar.SignalAll();
                }   // if
            }   // if
        }   // mutex
    }   // if

    if (r->filter_block != NULL)
    {
        // r->filter_block->AddKey(key);
        block_ptr->m_FiltLengths.push_back(key.size());
        block_ptr->m_FiltKeys.append(key.data(), key.size());
    }   // if

    block_ptr->m_LastKey.assign(key.data(), key.size());
    r->num_entries++;
    block_ptr->m_Block.Add(key, value);
    r->sst_counters.Inc(eSstCountKeys);
    r->sst_counters.Add(eSstCountKeySize, key.size());
    r->sst_counters.Add(eSstCountValueSize, value.size());

    // has this block reach size limit
    const size_t estimated_block_size = block_ptr->m_Block.CurrentSizeEstimate();
    if (estimated_block_size >= r->options.block_size) {
        Flush();
    }   // if
}


// Flush() is only called by singleton read thread.
void
TableBuilder2::Flush()
{
    BlockNState * block_ptr(NULL);

    assert(!rep_->closed);
    if (ok())
    {
        port::MutexLock lock(m_CvMutex);

        block_ptr=&m_Blocks[m_NextAdd];
        if (BlockNState::eBNStateLoading==block_ptr->m_State)
        {
            block_ptr->m_State=BlockNState::eBNStateFull;
            m_NextAdd=(m_NextAdd+1) % eTB2Buffers;
            m_CondVar.SignalAll();
        }   // if

    }   // mutex

    return;

}   // TableBuilder2::Flush


void
TableBuilder2::WorkerThread()
{
    bool running, all_empty;
    int idx, loop;

    running=true;

    do
    {

        // look for work
        {
            port::MutexLock lock(m_CvMutex);
            bool again;

            // check overall status
            for (loop=0, all_empty=true; loop<eTB2Buffers && all_empty; ++loop)
                all_empty=m_Blocks[loop].empty();

            // set state variables for this loop iteration
            running=(!m_Abort && !(m_Finish && all_empty));
            again=running;
            idx=-1;

            // find work if loop still running
            if (running && !all_empty)
            {
                for (loop=m_NextWrite; again && loop<(m_NextWrite+eTB2Buffers); ++loop)
                {
                    idx=loop % eTB2Buffers;

                    // ready to write?
                    if (idx==m_NextWrite && BlockNState::eBNStateReady==m_Blocks[idx].m_State)
                    {
                        again=false;
                        m_Blocks[idx].m_State=BlockNState::eBNStateWriting;
                    }   // if

                    // ready for generic work
                    else if (BlockNState::eBNStateFull==m_Blocks[idx].m_State)
                    {
                        again=false;
                        m_Blocks[idx].m_State=BlockNState::eBNStateCompress;
                    }   // else if

                    // last block is done, finish it off
                    else if (m_Finish && idx==m_NextWrite && BlockNState::eBNStateKeyWait==m_Blocks[idx].m_State
                             && BlockNState::eBNStateEmpty==m_Blocks[(idx+1) % eTB2Buffers].m_State)
                    {
                        rep_->options.comparator->FindShortSuccessor(&m_Blocks[idx].m_LastKey);
                        assert(false==m_Blocks[idx].m_KeyShortened);
                        m_Blocks[idx].m_KeyShortened=true;

                        again=false;
                        m_Blocks[idx].m_State=BlockNState::eBNStateWriting;
                    }   // else if
                }   // for
            }   // if

            // loop again?
            if (again)
            {
                // set idx as marker of "no work found"
                idx=-1;
                m_CondVar.Wait();
            }   // if
        }   // mutex lock scope

        // outside mutex
        if (running)
        {
            // perform work
            if (-1!=idx)
            {
                if (BlockNState::eBNStateCompress==m_Blocks[idx].m_State)
                    CompressBlock(m_Blocks[idx]);
//                else // eBNStateReady
                else if (BlockNState::eBNStateWriting==m_Blocks[idx].m_State)
                    WriteBlock2(m_Blocks[idx]);
                else
                    assert(false);
            }   // if
        }   // if

    } while(running);

}   // TableBuilder2::WorkerThread


void
TableBuilder2::CompressBlock(
     BlockNState & state)
{
    assert(BlockNState::eBNStateCompress==state.m_State);
    bool our_write(false);

    assert(ok());
    Rep* r = rep_;
    Slice raw = state.m_Block.Finish();

    r->sst_counters.Inc(eSstCountBlocks);
    r->sst_counters.Add(eSstCountBlockSize, raw.size());

    state.m_Type = r->options.compression;
    // TODO(postrelease): Support more compression options: zlib?
    switch (state.m_Type)
    {
        case kNoCompression:
            // do nothing
            break;

        case kSnappyCompression: {
            std::string compressed;
            compressed.resize(raw.size());
            if (port::Snappy_Compress(raw.data(), raw.size(), &compressed) &&
                compressed.size() < raw.size() - (raw.size() / 8u)) {
                state.m_Block.OverwriteBuffer(compressed);
            } else {
                // Snappy not supported, or compressed less than 12.5%, so just
                // store uncompressed form
                state.m_Type = kNoCompression;
                r->sst_counters.Inc(eSstCountCompressAborted);
            }
            break;
        }
    }   // switch

    r->sst_counters.Add(eSstCountBlockWriteSize, state.m_Block.CurrentBuffer().size());

    // calculate the crc32c for the data
    char trailer[kBlockTrailerSize];
    trailer[0] = state.m_Type;
    uint32_t crc = crc32c::Value(state.m_Block.CurrentBuffer().data(),
                                 state.m_Block.CurrentBuffer().size());
    state.m_Crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type

    // update object state
    {
        port::MutexLock lock(m_CvMutex);

        // ready for write?
        if (state.m_KeyShortened)
        {
            if (&state==&m_Blocks[m_NextWrite])
            {
                state.m_State=BlockNState::eBNStateWriting;
                our_write=true;
            }   // if
            else
                state.m_State=BlockNState::eBNStateReady;
        }   // if
        else
            state.m_State=BlockNState::eBNStateKeyWait;

    }   // mutex scope

    // go straight to next action if block ready
    if (our_write && BlockNState::eBNStateWriting==state.m_State)
        WriteBlock2(state);
    else
    {
        port::MutexLock lock(m_CvMutex);
        m_CondVar.SignalAll();
    }   // else

    return;

}   // TableBuilder2::CompressBlock


void
TableBuilder2::WriteBlock2(
    BlockNState & state)
{
    // File format contains a sequence of blocks where each block has:
    //    block_data: uint8[n]
    //    type: uint8
    //    crc: uint32
    assert(BlockNState::eBNStateWriting==state.m_State);

    BlockHandle handle;
    Rep* r = rep_;

    size_t total_size;
    RiakBufferPtr dest_ptr;

#if 0
    if (0!=m_PriorityLevel)
    {
        bool again;
        int ret_val;
        struct timespec timeout;

        do
        {
            again=false;

            // pause to potentially hand off disk to
            //  memtable threads
#if defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS - 200112L) >= 0L
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec+=1;
            ret_val=pthread_rwlock_timedwrlock(&gThreadLock0, &timeout);
#else
            // ugly spin lock
            ret_val=pthread_rwlock_trywrlock(&gThreadLock0);
            if (EBUSY==ret_val)
            {
                ret_val=ETIMEDOUT;
                env_->SleepForMicroseconds(10000);  // 10milliseconds
            }   // if
#endif
            if (0==ret_val)
                pthread_rwlock_unlock(&gThreadLock0);
            again=(ETIMEDOUT==ret_val);

            // Give priorities to level 0 compactions, unless
            //  this compaction is blocking a level 0 in this database
            if (1<m_PriorityLevel && !again)
            {
#if defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS - 200112L) >= 0L
                clock_gettime(CLOCK_REALTIME, &timeout);
                timeout.tv_sec+=1;
                ret_val=pthread_rwlock_timedwrlock(&gThreadLock1, &timeout);
#else
                // ugly spin lock
                ret_val=pthread_rwlock_trywrlock(&gThreadLock1);
                if (EBUSY==ret_val)
                {
                    ret_val=ETIMEDOUT;
                    env_->SleepForMicroseconds(10000);  // 10milliseconds
                }   // if
#endif

                if (0==ret_val)
                    pthread_rwlock_unlock(&gThreadLock1);
                again=again || (ETIMEDOUT==ret_val);
            }   // if
        } while(again);
    }   // if

#endif
    //
    // activities that must be performed in file sequence
    //
    total_size=state.m_Block.CurrentBuffer().size()
        + kBlockTrailerSize;
    r->status=r->file->Allocate(total_size, dest_ptr);
    assert(ok());

    handle.set_offset(r->offset);
    handle.set_size(state.m_Block.CurrentBuffer().size());
    r->offset += total_size;

    if (r->filter_block != NULL)
    {
        // push all the keys into filter
        r->filter_block->AddKeys(state.m_FiltLengths, state.m_FiltKeys);
        r->filter_block->StartBlock(r->offset);
    }   // if

    std::string handle_encoding;
    handle.EncodeTo(&handle_encoding);
    assert(true==state.m_KeyShortened);
    r->index_block.Add(state.m_LastKey, Slice(handle_encoding));
    r->sst_counters.Inc(eSstCountIndexKeys);

// this allows multiple memcpy
    // let next block into this portion of code
    {
        port::MutexLock lock(m_CvMutex);

        state.m_State=BlockNState::eBNStateCopying;
        m_NextWrite=(m_NextWrite +1 ) % eTB2Buffers;
        m_CondVar.SignalAll();
    }   // mutex scope

    if (r->status.ok())
    {
        r->status=dest_ptr.assign(state.m_Block.CurrentBuffer().data(), state.m_Block.CurrentBuffer().size());
        assert(ok());
        if (r->status.ok())
        {
            char trailer[kBlockTrailerSize];
            trailer[0] = state.m_Type;
            EncodeFixed32(trailer+1, crc32c::Mask(state.m_Crc));
            r->status = dest_ptr.append(trailer, kBlockTrailerSize);
            assert(ok());
            if (r->status.ok())
            {
//                r->offset += state.m_Block.CurrentBuffer().size() + kBlockTrailerSize;
            }   // if
        }   // if
    }   // if

    // buffer done, put back in pile
    {
        port::MutexLock lock(m_CvMutex);

        state.reset();
        m_CondVar.SignalAll();
    }   // mutex scope

    return;

}   // TableBuilder2::WriteBlock2


Status
TableBuilder2::Finish()
{
    int loop, idx;

    Flush();
    assert(!rep_->closed);
    // set by TableBuilder::Finish()  r->closed = true;

    // let all workers complete
    {
        port::MutexLock lock(m_CvMutex);

        m_Finish=true;
        m_CondVar.SignalAll();
    }

    for (loop=0; loop<eTB2Threads; ++loop)
    {
        pthread_join(m_Writers[loop], NULL);
        m_Writers[loop]=0;
    }   // for

    Status status=TableBuilder::Finish();

    return(status);

}


void
TableBuilder2::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;

    // let all workers complete
    int loop;
    m_Finish=true;
    m_Abort=true;
    {
        port::MutexLock lock(m_CvMutex);
        m_CondVar.SignalAll();
    }
    for (loop=0; loop<eTB2Threads; ++loop)
    {
        if (0!=m_Writers[loop])
            pthread_join(m_Writers[loop], NULL);
    }   // for
}


void
TableBuilder2::Dump() const
{
    int loop;
    std::ostringstream str;

    str << "Buffer states[";

    for (loop=0; loop<eTB2Buffers; ++loop)
        str << " " << m_Blocks[loop].m_State;

    str << "]" << std::ends;

    Log(rep_->options.info_log, "%s", str.str().c_str());

    return;

}   // TableBuilder2::Dump

}  // namespace leveldb
