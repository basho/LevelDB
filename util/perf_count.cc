// -------------------------------------------------------------------
//
// perf_count.cc:  performance counters LevelDB (http://code.google.com/p/leveldb/)
//
// Copyright (c) 2012 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// -------------------------------------------------------------------

#include <limits.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef STORAGE_LEVELDB_INCLUDE_PERF_COUNT_H_
#include "leveldb/perf_count.h"
#endif

#include "util/coding.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#ifdef OS_SOLARIS
#  include <atomic.h>
#endif


namespace leveldb
{

// always have something active in gPerfCounters, eliminates
//  need to test for "is shared object attached yet"
static PerformanceCounters LocalStartupCounters;
PerformanceCounters * gPerfCounters(&LocalStartupCounters);


    SstCounters::SstCounters()
        : m_IsReadOnly(false),
          m_Version(eSstCountVersion),
          m_CounterSize(eSstCountEnumSize)
    {
        memset(m_Counter, 0, sizeof(m_Counter));

        m_Counter[eSstCountKeySmallest]=CounterInfo::MAX_VALUE;
        m_Counter[eSstCountValueSmallest]=CounterInfo::MAX_VALUE;

        return;

    };  // SstCounters::SstCounters


    void
    SstCounters::EncodeTo(
        std::string & Dst) const
    {
        unsigned loop;

        PutVarint32(&Dst, m_Version);
        PutVarint32(&Dst, m_CounterSize);

        for(loop=0; loop<eSstCountEnumSize; ++loop)
            PutVarint64(&Dst, (uint64_t)m_Counter[loop]);
    }   // SstCounters::EncodeTo


    Status
    SstCounters::DecodeFrom(
        const Slice& src)
    {
        Status ret_status;
        Slice cursor;
        bool good;
        int loop;

        cursor=src;
        m_IsReadOnly=true;
        good=GetVarint32(&cursor, &m_Version);
        good=good && (m_Version<=eSstCountVersion);

        // all lesser number of stats to be read
        good=good && GetVarint32(&cursor, &m_CounterSize);
        if (good && eSstCountEnumSize < m_CounterSize)
            m_CounterSize=eSstCountEnumSize;

        for (loop=0; good && loop<eSstCountEnumSize; ++loop)
        {
            uint64_t val;
            good=GetVarint64(&cursor, &val);
            m_Counter[loop] = (CounterInt) val;
        }   // for

        // if (!good) change ret_status to bad

        return(ret_status);

    }   // SstCounters::DecodeFrom


    SstCounters::CounterInt
    SstCounters::Inc(
        unsigned Index)
    {
        CounterInt ret_val;

        ret_val=0;
        if (!m_IsReadOnly && Index<m_CounterSize)
        {
            ++m_Counter[Index];
            ret_val=m_Counter[Index];
        }   // if

        return(ret_val);
    }   // SstCounters::Inc


    SstCounters::CounterInt
    SstCounters::Add(
        unsigned Index,
        SstCounters::CounterInt Amount)
    {
        CounterInt ret_val;

        ret_val=0;
        if (!m_IsReadOnly && Index<m_CounterSize)
        {
            m_Counter[Index]+=Amount;
            ret_val=m_Counter[Index];
        }   // if

        return(ret_val);
    }   // SstCounters::Add


    SstCounters::CounterInt
    SstCounters::Value(
        unsigned Index) const
    {
        CounterInt ret_val;

        ret_val=0;
        if (Index<m_CounterSize)
        {
            ret_val=m_Counter[Index];
        }   // if

        return(ret_val);
    }   // SstCounters::Value


    void
    SstCounters::Set(
        unsigned Index,
        SstCounters::CounterInt Value)
    {
        if (Index<m_CounterSize)
        {
            m_Counter[Index]=Value;
        }   // if

        return;
    }   // SstCounters::Set


    void
    SstCounters::Dump() const
    {
        unsigned loop;

        printf("SstCounters:\n");
        printf("   m_IsReadOnly: %u\n", m_IsReadOnly);
        printf("      m_Version: %u\n", m_Version);
        printf("  m_CounterSize: %u\n", m_CounterSize);
        for (loop=0; loop<m_CounterSize; ++loop)
            printf("    Counter[%2u]: %" CNTR_FMT "\n", loop, m_Counter[loop]);

        return;

    }   // SstCounters::Dump


    // only used for local static objects, not shared memory objects
    PerformanceCounters::PerformanceCounters()
    {
        m_Version=ePerfVersion;
        m_CounterSize=ePerfCountEnumSize;
        // cast away "volatile"
        memset((void*)m_Counter, 0, sizeof(m_Counter));

        return;

    }   // PerformanceCounters::PerformanceCounters

    PerformanceCounters *
    PerformanceCounters::Init(
        bool IsReadOnly)
    {
        PerformanceCounters * ret_ptr;

        ret_ptr=NULL;

        // attempt to attach/create to shared memory instance
        m_PerfSharedId=shmget(ePerfKey, sizeof(PerformanceCounters), IPC_CREAT | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (-1!=m_PerfSharedId)
        {
            ret_ptr=(PerformanceCounters *)shmat(m_PerfSharedId, NULL, (IsReadOnly ? SHM_RDONLY : 0));
            if ((void*)-1 != ret_ptr)
            {
                // initialize?
                if (0==ret_ptr->m_Version || ePerfCountEnumSize!=ret_ptr->m_CounterSize)
                {
                    if (!IsReadOnly)
                    {
                        memset(ret_ptr, 0, sizeof(PerformanceCounters));
                        ret_ptr->m_Version=ePerfVersion;
                        ret_ptr->m_CounterSize=ePerfCountEnumSize;
                    }   // if

                    // bad version match to existing segment
                    else
                    {
                        ret_ptr=(PerformanceCounters *)-1;
                        errno=EINVAL;
                    }   // else
                }   // if
            }   // if

            if ((void*)-1 != ret_ptr)
            {
                // make this available process wide
                gPerfCounters=ret_ptr;
            }   // if
            else
            {
                ret_ptr=NULL;
                m_LastError=errno;
            }   // else
        }   // if
        else
        {
            m_LastError=errno;
            ret_ptr=NULL;
        }   // else

        return(ret_ptr);

    };  // PerformanceCounters::Init


    PerformanceCounters::CounterInt
    PerformanceCounters::Inc(
        unsigned Index)
    {
        CounterInt ret_val;

        ret_val=0;
        if (Index<m_CounterSize)
        {
            volatile CounterInt * val_ptr;

            val_ptr=&m_Counter[Index];

# if ULONG_MAX != 4294967295UL
#ifdef OS_SOLARIS
            atomic_inc_64(val_ptr);
#else
            __sync_add_and_fetch(val_ptr, 1);
#endif
#else
            // hack fest for 64 bit semi-atomic on 32bit machine
            uint32_t ret_32, * ptr_32;

            ptr_32=(uint32_t *)&val_ptr;
            ret_32=__sync_add_and_fetch(ptr_32, 1);
            if (0==ret_32)
            {
                ++ptr_32;
                __sync_add_and_fetch(ptr_32, 1);
            }   // if
#endif
            ret_val=*val_ptr;
        }   // if

        return(ret_val);
    }   // PerformanceCounters::Inc


    PerformanceCounters::CounterInt
    PerformanceCounters::Dec(
        unsigned Index)
    {
        CounterInt ret_val;

        ret_val=0;
        if (Index<m_CounterSize)
        {
            volatile CounterInt * val_ptr;

            val_ptr=&m_Counter[Index];

# if ULONG_MAX != 4294967295UL
#ifdef OS_SOLARIS
            atomic_dec_64(val_ptr);
#else
            __sync_sub_and_fetch(val_ptr, 1);
#endif
#else
            // hack fest for 64 bit semi-atomic on 32bit machine
            uint32_t ret_32, * ptr_32;

            ptr_32=(uint32_t *)&val_ptr;
            ret_32=__sync_sub_and_fetch(ptr_32, 1);
            if (0xFFFFFFFF==ret_32)
            {
                ++ptr_32;
                __sync_sub_and_fetch(ptr_32, 1);
            }   // if
#endif
            ret_val=*val_ptr;
        }   // if

        return(ret_val);
    }   // PerformanceCounters::Dec


    PerformanceCounters::CounterInt
    PerformanceCounters::Add(
        unsigned Index,
        PerformanceCounters::CounterInt Amount)
    {
        CounterInt ret_val;

        ret_val=0;
        if (Index<m_CounterSize)
        {
            volatile CounterInt * val_ptr;

            val_ptr=&m_Counter[Index];

# if ULONG_MAX != 4294967295UL
#ifdef OS_SOLARIS
            ret_val=atomic_add_64_nv(val_ptr, Amount);
#else
            ret_val=__sync_add_and_fetch(val_ptr, Amount);
#endif
#else
            // hack fest for 64 bit semi-atomic on 32bit machine
            uint32_t old_32, ret_32, * ptr_32;

            ptr_32=(uint32_t *)&val_ptr;
            old_32=*ptr_32;
            ret_32=__sync_add_and_fetch(ptr_32, Amount);
            if (ret_32<old_32)
            {
                ++ptr_32;
                __sync_add_and_fetch(ptr_32, 1);
            }   // if

            ret_val=*val_ptr;
#endif
        }   // if

        return(ret_val);
    }   // PerformanceCounters::Add


    PerformanceCounters::CounterInt
    PerformanceCounters::Value(
        unsigned Index) const
    {
        CounterInt ret_val;

        ret_val=0;
        if (Index<m_CounterSize)
        {
            ret_val=m_Counter[Index];
        }   // if

        return(ret_val);
    }   // SstCounters::Value


    void
    PerformanceCounters::Set(
        unsigned Index,
        PerformanceCounters::CounterInt Amount)
    {
        if (Index<m_CounterSize)
        {
            volatile CounterInt * val_ptr;

            val_ptr=&m_Counter[Index];

            *val_ptr=Amount;
        }   // if

        return;
    }   // PerformanceCounters::Set


    volatile const PerformanceCounters::CounterInt *
    PerformanceCounters::GetPtr(
        unsigned Index) const
    {
        const volatile CounterInt * ret_ptr;

        if (Index<m_CounterSize)
            ret_ptr=&m_Counter[Index];
        else
            ret_ptr=&m_BogusCounter;

        return(ret_ptr);

    }   // PerformanceCounters::GetPtr


    const char *
    PerformanceCounters::GetNamePtr(
        unsigned Index)
    {
        const char * ret_ptr;

        if (Index<ePerfCountEnumSize)
            ret_ptr=m_PerfCounterNames[Index];
        else
            ret_ptr="???";

        return(ret_ptr);

    }   // PerformanceCounters::GetPtr


    int PerformanceCounters::m_PerfSharedId=-1;
    int PerformanceCounters::m_LastError=0;
    volatile PerformanceCounters::CounterInt PerformanceCounters::m_BogusCounter=0;
    const char * PerformanceCounters::m_PerfCounterNames[]=
    {
        "ROFileOpen",
        "ROFileClose",
        "ROFileUnmap",
        "RWFileOpen",
        "RWFileClose",
        "RWFileUnmap",
        "ApiOpen",
        "ApiGet",
        "ApiWrite",
        "WriteSleep",
        "WriteWaitImm",
        "WriteWaitLevel0",
        "WriteNewMem",
        "WriteError",
        "WriteNoWait",
        "GetMem",
        "GetImm",
        "GetVersion",
        "SearchLevel[0]",
        "SearchLevel[1]",
        "SearchLevel[2]",
        "SearchLevel[3]",
        "SearchLevel[4]",
        "SearchLevel[5]",
        "SearchLevel[6]",
        "TableCached",
        "TableOpened",
        "TableGet",
        "BGCloseUnmap",
        "BGCompactImm",
        "BGNormal",
        "BGCompactLevel0",
        "BlockFiltered",
        "BlockFilterFalse",
        "BlockCached",
        "BlockRead",
        "BlockFilterRead",
        "BlockValidGet",
        "Debug[0]",
        "Debug[1]",
        "Debug[2]",
        "Debug[3]",
        "Debug[4]",
        "ReadBlockError",
        "DBIterNew",
        "DBIterNext",
        "DBIterPrev",
        "DBIterSeek",
        "DBIterSeekFirst",
        "DBIterSeekLast",
        "DBIterDelete",
        "eleveldbDirect",
        "eleveldbQueued",
        "eleveldbDequeued",
        "elevelRefCreate",
        "elevelRefDelete",
        "ThrottleGauge",
        "ThrottleCounter",
        "ThrottleMicros0",
        "ThrottleKeys0",
        "ThrottleBacklog0",
        "ThrottleCompacts0",
        "ThrottleMicros1",
        "ThrottleKeys1",
        "ThrottleBacklog1",
        "ThrottleCompacts1"
    };


    int
    PerformanceCounters::LookupCounter(
        const char * Name)
    {
        int index,loop;

        index=-1;

        if (NULL!=Name && '\0'!=*Name)
        {
            for (loop=0; loop<ePerfCountEnumSize && -1==index; ++loop)
            {
                if (0==strcmp(m_PerfCounterNames[loop], Name))
                    index=loop;
            }   // loop
        }   // if

        return(index);
    };

    void
    PerformanceCounters::Dump()
    {
        int loop;

        printf(" m_Version: %u\n", m_Version);
        printf(" m_CounterSize: %u\n", m_CounterSize);

        for (loop=0; loop<ePerfCountEnumSize; ++loop)
        {
            printf("  %s: %" CNTR_FMT "\n", m_PerfCounterNames[loop], m_Counter[loop]);
        }   // loop
    };  // Dump

}  // namespace leveldb
