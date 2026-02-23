#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class Logger
{
public:
    enum class Level : uint8_t
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Fatal = 5
    };

    struct Config
    {
        Level minLevel = Level::Info;
        std::string targetFile{};
    };

    static Logger& Instance()
    {
        static Logger instance;
        return instance;
    }

    Logger(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger& operator=(Logger&&) = delete;

    void Initialize(const Config& config)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_running)
        {
            return;
        }

        _config = config;
        _minLevel.store(_config.minLevel, std::memory_order_release);
        _nextSeq.store(1, std::memory_order_release);
        _flushedSeq = 0;
        _flushRequested = false;
        _flushTargetSeq = 0;
        _pendingSwap = false;
        _activeBuffer.clear();
        _flushBuffer.clear();
        _activeBuffer.reserve(kInitialRecordReserve);
        _flushBuffer.reserve(kInitialRecordReserve);
        _activeBytes = 0;
        OpenTargetFileLocked();

        _running = true;
        _worker = std::thread(&Logger::WorkerLoop, this);
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running)
            {
                return;
            }

            _running = false;
            TrySwapActiveToFlushLocked();
        }

        _wakeUp.notify_one();
        _spaceAvailable.notify_all();
        if (_worker.joinable())
        {
            _worker.join();
        }

        std::lock_guard<std::mutex> lock(_mutex);
        CloseTargetFileLocked();
    }

    void SetMinLevel(Level level)
    {
        _minLevel.store(level, std::memory_order_release);
    }

    [[nodiscard]] bool ShouldLog(Level level) const
    {
        return static_cast<uint8_t>(level) >= static_cast<uint8_t>(_minLevel.load(std::memory_order_acquire));
    }

    void Log(Level level, const char* file, int line, std::string_view message, bool forceFlush = false)
    {
        EnsureInitialized();
        if (!ShouldLog(level))
        {
            return;
        }

        Record record;
        record.seq = _nextSeq.fetch_add(1, std::memory_order_acq_rel);
        record.level = level;
        record.timestampUtc = QueryUtcFileTime();
        record.threadId = ::GetCurrentThreadId();
        record.file = (file == nullptr) ? "<unknown>" : file;
        record.line = line;
        record.message.assign(message.data(), message.size());
        const size_t recordBytes = EstimateRecordBytes(record);
        bool needWake = false;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            needWake = EnqueueRecordLocked(lock, std::move(record), recordBytes);
            if (!_running)
            {
                return;
            }
        }

        if (needWake)
        {
            _wakeUp.notify_one();
        }
        if (forceFlush)
        {
            Flush();
        }
    }

    void Flush()
    {
        EnsureInitialized();
        const uint64_t targetSeq = _nextSeq.load(std::memory_order_acquire) - 1;
        if (targetSeq == 0)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running || _flushedSeq >= targetSeq)
            {
                return;
            }

            _flushRequested = true;
            if (targetSeq > _flushTargetSeq)
            {
                _flushTargetSeq = targetSeq;
            }

            TrySwapActiveToFlushLocked();
        }

        _wakeUp.notify_one();
        std::unique_lock<std::mutex> lock(_mutex);
        _flushDone.wait(lock, [this, targetSeq]() { return !_running || _flushedSeq >= targetSeq; });
    }

private:
    struct Record
    {
        uint64_t seq = 0;
        Level level = Level::Info;
        FILETIME timestampUtc{};
        DWORD threadId = 0;
        const char* file = "<unknown>";
        int line = 0;
        std::string message;
    };

    static constexpr uint32_t kBufferTimeoutMs = 100;
    static constexpr size_t kBufferSizeBytes = 4096ull * 1024ull;
    static constexpr size_t kInitialRecordReserve = 1024;

    Logger() = default;

    ~Logger()
    {
        Flush();
        Shutdown();
    }

    static FILETIME QueryUtcFileTime()
    {
        FILETIME fileTime{};
        using PreciseClockFn = VOID(WINAPI*)(LPFILETIME);
        static PreciseClockFn preciseFn = []() -> PreciseClockFn {
            HMODULE module = ::GetModuleHandleW(L"kernel32.dll");
            if (module == nullptr)
            {
                return nullptr;
            }
            return reinterpret_cast<PreciseClockFn>(::GetProcAddress(module, "GetSystemTimePreciseAsFileTime"));
        }();

        if (preciseFn != nullptr)
        {
            preciseFn(&fileTime);
        }
        else
        {
            ::GetSystemTimeAsFileTime(&fileTime);
        }
        return fileTime;
    }

    static const char* LevelToText(Level level)
    {
        switch (level)
        {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
        default:           return "UNKNOWN";
        }
    }

    std::string FormatRecord(const Record& record) const
    {
        SYSTEMTIME utc{};
        ::FileTimeToSystemTime(&record.timestampUtc, &utc);
        return std::format(
            "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] [{}] [T{}] [{}:{}] {}\n",
            static_cast<unsigned>(utc.wYear),
            static_cast<unsigned>(utc.wMonth),
            static_cast<unsigned>(utc.wDay),
            static_cast<unsigned>(utc.wHour),
            static_cast<unsigned>(utc.wMinute),
            static_cast<unsigned>(utc.wSecond),
            static_cast<unsigned>(utc.wMilliseconds),
            LevelToText(record.level),
            static_cast<unsigned long>(record.threadId),
            record.file,
            record.line,
            record.message);
    }

    static size_t EstimateRecordBytes(const Record& record)
    {
        return sizeof(Record) + record.message.size();
    }

    bool TrySwapActiveToFlushLocked()
    {
        if (_pendingSwap || _activeBuffer.empty())
        {
            return false;
        }
        _activeBuffer.swap(_flushBuffer);
        _activeBytes = 0;
        _pendingSwap = true;
        return true;
    }

    bool EnqueueRecordLocked(std::unique_lock<std::mutex>& lock, Record&& record, size_t recordBytes)
    {
        if (!_running)
        {
            return false;
        }

        bool needWake = false;
        while (_running && _pendingSwap && (_activeBytes + recordBytes > kBufferSizeBytes))
        {
            _wakeUp.notify_one();
            _spaceAvailable.wait(lock, [this, recordBytes]() {
                return !_running || !_pendingSwap || (_activeBytes + recordBytes <= kBufferSizeBytes);
            });
        }

        if (!_running)
        {
            return false;
        }

        if (!_pendingSwap && !_activeBuffer.empty() && (_activeBytes + recordBytes > kBufferSizeBytes))
        {
            needWake |= TrySwapActiveToFlushLocked();
        }

        _activeBuffer.emplace_back(std::move(record));
        _activeBytes += recordBytes;
        if (_activeBytes >= kBufferSizeBytes && !_pendingSwap)
        {
            needWake |= TrySwapActiveToFlushLocked();
        }
        return needWake;
    }

    void UpdateFlushStateLocked(uint64_t flushedUpto)
    {
        if (flushedUpto > _flushedSeq)
        {
            _flushedSeq = flushedUpto;
        }
        if (_flushRequested && _flushedSeq >= _flushTargetSeq)
        {
            _flushRequested = false;
        }
        _flushDone.notify_all();
    }

    void WorkerLoop()
    {
        std::vector<Record> localBatch;
        localBatch.reserve(kInitialRecordReserve);

        for (;;)
        {
            bool shouldExit = false;
            uint64_t flushedUpto = 0;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _wakeUp.wait_for(
                    lock,
                    std::chrono::milliseconds(kBufferTimeoutMs),
                    [this]() { return !_running || _pendingSwap || _flushRequested; });

                TrySwapActiveToFlushLocked();
                flushedUpto = _flushedSeq;

                if (_pendingSwap)
                {
                    localBatch.swap(_flushBuffer);
                    _pendingSwap = false;
                    _spaceAvailable.notify_all();
                    flushedUpto = localBatch.back().seq;
                }
                else
                {
                    localBatch.clear();
                }

                shouldExit = (!_running && localBatch.empty() && _activeBuffer.empty());
            }

            if (!localBatch.empty())
            {
                WriteBatch(localBatch);
                localBatch.clear();
            }

            {
                std::lock_guard<std::mutex> lock(_mutex);
                UpdateFlushStateLocked(flushedUpto);
            }

            if (shouldExit)
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _flushRequested = false;
                _flushDone.notify_all();
                _spaceAvailable.notify_all();
                break;
            }
        }
    }

    void WriteBatch(const std::vector<Record>& batch)
    {
        if (_targetFile == nullptr)
        {
            for (const Record& record : batch)
            {
                std::cout << FormatRecord(record);
            }
            std::cout.flush();
            return;
        }

        for (const Record& record : batch)
        {
            const std::string line = FormatRecord(record);
            std::fwrite(line.data(), 1, line.size(), _targetFile);
        }
        std::fflush(_targetFile);
    }

    void OpenTargetFileLocked()
    {
        CloseTargetFileLocked();
        if (_config.targetFile.empty())
        {
            return;
        }
        fopen_s(&_targetFile, _config.targetFile.c_str(), "ab");
    }

    void CloseTargetFileLocked()
    {
        if (_targetFile != nullptr)
        {
            std::fflush(_targetFile);
            std::fclose(_targetFile);
            _targetFile = nullptr;
        }
    }

    void EnsureInitialized()
    {
        bool needInit = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            needInit = !_running;
        }
        if (needInit)
        {
            Initialize(Config{});
        }
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _wakeUp;
    std::condition_variable _flushDone;
    std::condition_variable _spaceAvailable;
    std::thread _worker;

    bool _running = false;
    bool _pendingSwap = false;
    bool _flushRequested = false;
    uint64_t _flushTargetSeq = 0;
    uint64_t _flushedSeq = 0;

    Config _config{};
    std::atomic<Level> _minLevel{ Level::Info };
    std::atomic<uint64_t> _nextSeq{ 1 };
    std::vector<Record> _activeBuffer;
    std::vector<Record> _flushBuffer;
    size_t _activeBytes = 0;
    FILE* _targetFile = nullptr;
};

#define LOG_TRACE(msg) ::Logger::Instance().Log(::Logger::Level::Trace, __FILE__, __LINE__, (msg))
#define LOG_DEBUG(msg) ::Logger::Instance().Log(::Logger::Level::Debug, __FILE__, __LINE__, (msg))
#define LOG_INFO(msg)  ::Logger::Instance().Log(::Logger::Level::Info,  __FILE__, __LINE__, (msg))
#define LOG_WARN(msg)  ::Logger::Instance().Log(::Logger::Level::Warn,  __FILE__, __LINE__, (msg))
#define LOG_ERROR(msg) ::Logger::Instance().Log(::Logger::Level::Error, __FILE__, __LINE__, (msg))
#define LOG_FATAL(msg) ::Logger::Instance().Log(::Logger::Level::Fatal, __FILE__, __LINE__, (msg))
#define LOG_ERROR_FLUSH(msg) ::Logger::Instance().Log(::Logger::Level::Error, __FILE__, __LINE__, (msg), true)
#define LOG_FATAL_FLUSH(msg) ::Logger::Instance().Log(::Logger::Level::Fatal, __FILE__, __LINE__, (msg), true)
#define LOG_FLUSH() ::Logger::Instance().Flush()
