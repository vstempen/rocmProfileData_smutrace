/**************************************************************************
 * Copyright (c) 2022 Advanced Micro Devices, Inc.
 **************************************************************************/
#include "SmuDumpDataSource.h"

#include <cstdint>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#include <fmt/format.h>

#include "Logger.h"

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

static SmuDumpDataSource *g_singleton = nullptr;

// Create a factory for the Logger to locate and use
extern "C" {
    DataSource *SmuDumpDataSourceFactory() { return (g_singleton = new SmuDumpDataSource()); }
}  // extern "C"


SmuDumpDataSource& SmuDumpDataSource::singleton()
{
    return *g_singleton;
}

timestamp_t SmuDumpDataSource::getTimeStamp()
{
    //return m_timestamp;
    return clocktime_ns();
}

bool SmuDumpDataSource::isLoggingEnabled()
{
    return m_loggingEnabled;
}

void SmuDumpDataSource::init()
{
    void (*dl) = dlopen("libsmutrace.so", RTLD_LAZY);
    if (dl) {
        f_smuDumpInit = (SmuDumpInitFunc) dlsym(dl, "smuDumpInit");
        f_smuDumpEnd = (SmuDumpEndFunc) dlsym(dl, "smuDumpEnd");
        f_smuDumpStop = (SmuDumpStopFunc) dlsym(dl, "smuDumpStop");
        f_smuDumpOnce = (SmuDumpOnceFunc) dlsym(dl, "smuDumpOnce");
        f_regDumpOnce = (RegDumpOnceFunc) dlsym(dl, "regDumpOnce");
        f_sviDumpOnce = (SviDumpOnceFunc) dlsym(dl, "sviDumpOnce");
        f_smuGetTraceRate = (SmuGetTraceRate) dlsym(dl, "getSmuVariablesCaptureRate");
        f_regGetTraceRate = (RegGetTraceRate) dlsym(dl, "getRegisterExpressionCaptureRate");
        f_getTraceFlags = (GetTraceFlags) dlsym(dl, "getTraceFlags");
        m_loggingEnabled = (f_smuDumpInit && f_smuDumpStop && f_smuDumpEnd &&f_smuDumpOnce &&
                            f_smuGetTraceRate && f_regGetTraceRate && f_regDumpOnce  && f_sviDumpOnce  && f_regGetTraceFlags &&
                            f_smuDumpInit(addSMUValueToSqliteDb));
    }

    // FIXME: decide how many gpus and what values to log

    m_done = false;
    m_timestamp=0;

    if (m_loggingEnabled)
    {
        m_trace_flags = f_getTraceFlags();
        m_smu_period = f_smuGetTraceRate();
        m_reg_period = f_regGetTraceRate();
        m_smu_resource = new DbResource(Logger::singleton().filename(), std::string("smudump_logger_smu_active"));
        m_reg_resource = new DbResource(Logger::singleton().filename(), std::string("regdump_logger_reg_active"));
        m_svi_resource = new DbResource(Logger::singleton().filename(), std::string("svidump_logger_svi_active"));
        m_smu_worker = new std::thread(&SmuDumpDataSource::smuwork, this);
        m_reg_worker = new std::thread(&SmuDumpDataSource::regwork, this);
        m_svi_worker = new std::thread(&SmuDumpDataSource::sviwork, this);
    }
}

void SmuDumpDataSource::end()
{
    if (m_loggingEnabled)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done = true;
        lock.unlock();
        m_smu_worker->join();
        m_reg_worker->join();
        m_svi_worker->join();
        delete m_smu_worker;
        delete m_reg_worker;
        delete m_svi_worker;
        m_smu_resource->unlock();
        m_reg_resource->unlock();   
        m_svi_resource->unlock();
        f_smuDumpEnd();
    }
}

void SmuDumpDataSource::startTracing()
{
    if (m_loggingEnabled)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_loggingActive = true;
    }
}

void SmuDumpDataSource::stopTracing()
{
    if (m_loggingEnabled)
    {
        f_smuDumpStop();
        std::unique_lock<std::mutex> lock(m_mutex);
        m_timestamp = 0;
        m_loggingActive = false;

        // Tell the monitor table that it should terminate any outstanding ranges...
        //    since we are paused/stopped.
        Logger &logger = Logger::singleton();
        logger.monitorTable().endCurrentRuns(clocktime_ns());
    }
}

void SmuDumpDataSource::flush() {
    if (m_loggingEnabled)
    {
        Logger &logger = Logger::singleton();
    logger.monitorTable().endCurrentRuns(clocktime_ns());
    }
}


void SmuDumpDataSource::delayUs(uint32_t timeUs)
{
    sqlite3_int64 startTime = clocktime_ns();
    while(clocktime_ns() < startTime+timeUs*1000);
}

bool SmuDumpDataSource::loggingGated()
{
    Logger &logger = Logger::singleton();
    uint64_t time = clocktime_ns();
    if (m_trace_flags & START_CAPTURE_AFTER_HCC_ACTIVITY)
    {
        uint64_t hccTime = logger.getHccActivityTime();
        if (hccTime > 0 && (time - hccTime) < 1000000) return true;
    }
    if (m_trace_flags & START_CAPTURE_AFTER_API_ACTIVITY)
    {
        uint64_t apiTime = logger.getApiActivityTime();
        if (apiTime > 0 && (time - apiTime) < 1000000) return true;
    }
    return false;
}

void SmuDumpDataSource::addSMUValueToSqliteDb(uint64_t did, const char* type ,const char* name, double value, uint64_t flags, uint64_t starttime, uint64_t endtime)
{
    Logger &logger = Logger::singleton();
    MonitorTable::row mrow;
    mrow.deviceId = did;
    mrow.deviceType = type;
    mrow.monitorType = name;
    uint64_t timestamp = SmuDumpDataSource::singleton().getTimeStamp();
    mrow.start = starttime;
    mrow.end = endtime;
    mrow.value = fmt::format("{}", value);
    logger.monitorTable().insert(mrow);
}

void SmuDumpDataSource::smuwork()
{   
    std::unique_lock<std::mutex> lock(m_mutex);
    sqlite3_int64 startTime = clocktime_ns()/1000;

    bool haveResource = m_smu_resource->tryLock();
    
    while (m_done == false) {

        if (haveResource && m_loggingActive && !loggingGated()) {
            lock.unlock();
            m_timestamp=clocktime_ns() + m_smu_timeshift; //single timestamp for all variables dumped at once, for easier post-processing.
            f_smuDumpOnce();
            lock.lock();
        }
        
        sqlite3_int64 endTime = clocktime_ns()/1000;
        sqlite3_int64 sleepTime = startTime + m_reg_period - endTime;
        sleepTime = (sleepTime > 0) ? sleepTime : 0;
        if (haveResource == false)
            sleepTime += m_smu_period * 10;

        lock.unlock();
        usleep(sleepTime);
        lock.lock();
        if (haveResource == false) {
            haveResource = m_smu_resource->tryLock();
        }
        startTime = clocktime_ns()/1000;
    }
}


void SmuDumpDataSource::regwork()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    sqlite3_int64 startTime = clocktime_ns()/1000;

    bool haveResource = m_reg_resource->tryLock();
    
    while (m_done == false) {

        if (haveResource && m_loggingActive && !loggingGated()) {
            lock.unlock();
            m_timestamp=clocktime_ns();
            f_regDumpOnce();
            lock.lock();
        }
        
        sqlite3_int64 endTime = clocktime_ns()/1000;
        sqlite3_int64 sleepTime = startTime + m_reg_period - endTime;
        sleepTime = (sleepTime > 0) ? sleepTime : 0;
        if (haveResource == false)
            sleepTime += m_reg_period * 10;
        lock.unlock();
        delayUs(sleepTime);
        lock.lock();
        if (haveResource == false) {
            haveResource = m_reg_resource->tryLock();
        }
        startTime = clocktime_ns()/1000;
    }
}

void SmuDumpDataSource::sviwork()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    sqlite3_int64 startTime = clocktime_ns()/1000;

    bool haveResource = m_svi_resource->tryLock();
    
    while (m_done == false) {

        if (haveResource && m_loggingActive && !loggingGated())) {
            lock.unlock();
            m_timestamp=clocktime_ns();
            f_sviDumpOnce();
            lock.lock();
        }
        
        sqlite3_int64 endTime = clocktime_ns()/1000;
        sqlite3_int64 sleepTime = startTime + m_reg_period - endTime;
        sleepTime = (sleepTime > 0) ? sleepTime : 0;
        if (haveResource == false)
            sleepTime += m_svi_period * 10;
        lock.unlock();
        delayUs(sleepTime);
        lock.lock();
        if (haveResource == false) {
            haveResource = m_svi_resource->tryLock();
        }
        startTime = clocktime_ns()/1000;
    }
}


