/**************************************************************************
 * Copyright (c) 2022 Advanced Micro Devices, Inc.
 **************************************************************************/
#pragma once

#include "DataSource.h"
#include "DbResource.h"
#include "Utility.h"

#include <cstdint>
#include <sqlite3.h>
#include <thread>
#include <mutex>
#include <condition_variable>

enum TraceFlags{
    START_CAPTURE_AFTER_HCC_ACTIVITY = 1,
    START_CAPTURE_AFTER_API_ACTIVITY = 2,
};

typedef void (*SmuDumpCallback)(uint64_t, const char*, const char*, double, uint64_t, uint64_t, uint64_t);
typedef bool (*SmuDumpInitFunc) (SmuDumpCallback callback);
typedef void (*SmuDumpEndFunc) (void);
typedef void (*SmuDumpStopFunc) (void);
typedef void (*SmuDumpOnceFunc) (void);
typedef void (*RegDumpOnceFunc) (void);
typedef void (*SviDumpOnceFunc) (void);
typedef uint32_t (*RegGetTraceRate) (void);
typedef uint32_t (*GetTraceFlags) (void);
typedef uint32_t (*SmuGetTraceRate) (void);

class SmuDumpDataSource : public DataSource
{
public:
    //RoctracerDataSource();
    void init() override;
    void end() override;
    void startTracing() override;
    void stopTracing() override;
    virtual void flush() override;
    static SmuDumpDataSource& singleton();
    timestamp_t getTimeStamp();
    bool isLoggingEnabled();
    void delayUs(uint32_t timeUs);
    bool loggingGated();

private:
    std::mutex m_mutex;
    SmuDumpInitFunc f_smuDumpInit;
    SmuDumpEndFunc f_smuDumpEnd;
    SmuDumpStopFunc f_smuDumpStop;
    SmuDumpOnceFunc f_smuDumpOnce;
    RegDumpOnceFunc f_regDumpOnce;
    SviDumpOnceFunc f_sviDumpOnce;
    RegGetTraceRate f_regGetTraceRate;
    SmuGetTraceRate f_smuGetTraceRate;
    GetTraceFlags f_getTraceFlags;
    DbResource *m_smu_resource {nullptr};
    DbResource *m_reg_resource {nullptr};
    DbResource *m_svi_resource {nullptr};
    timestamp_t m_timestamp;

    bool m_loggingActive {false};
    bool m_loggingEnabled {false};
    static void addSMUValueToSqliteDb(uint64_t, const char* type ,const char* name, double value, uint64_t flags, uint64_t starttime, uint64_t endtime);


    void smuwork(); 
    void regwork();     
    void sviwork();           
    std::thread *m_smu_worker {nullptr};
    std::thread *m_reg_worker {nullptr};
    std::thread *m_svi_worker {nullptr};
    volatile bool m_done {false};
    sqlite3_int64 m_smu_period { 1000 };
    sqlite3_int64 m_smu_timeshift { -1000000 };
    sqlite3_int64 m_reg_period { 10 };
    sqlite3_int64 m_svi_period { 1 };
    uint32_t m_trace_flags {0};
};

