#pragma once

#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include <QMutex>
#include <QObject>
#include <atomic>
#include <functional>
#include <cstddef>

namespace editor {

// ============================================================================
// MemoryBudget - Manages total memory usage for frame cache
// 
// Tracks CPU and GPU memory separately, enforces limits with pressure callbacks
// ============================================================================
class MemoryBudget : public QObject {
    Q_OBJECT

public:
    enum class Priority : int {
        Low = 0,      // Thumbnails, background prefetch
        Normal = 50,  // Standard preview
        High = 100,   // Current frame
        Critical = 200 // Export, blocking operations
    };

    explicit MemoryBudget(QObject* parent = nullptr);

    // Configuration
    void setMaxCpuMemory(size_t bytes);
    void setMaxGpuMemory(size_t bytes);
    
    size_t maxCpuMemory() const { return m_maxCpuMemory; }
    size_t maxGpuMemory() const { return m_maxGpuMemory; }
    
    // Allocation tracking
    bool allocateCpu(size_t bytes, Priority priority);
    bool allocateGpu(size_t bytes, Priority priority);
    void deallocateCpu(size_t bytes);
    void deallocateGpu(size_t bytes);
    
    // Combined allocation
    bool allocate(size_t cpuBytes, size_t gpuBytes, Priority priority);
    void deallocate(size_t cpuBytes, size_t gpuBytes);
    
    // Current usage
    size_t currentCpuUsage() const { return m_cpuUsage.load(); }
    size_t currentGpuUsage() const { return m_gpuUsage.load(); }
    size_t peakCpuUsage() const { return m_peakCpuUsage.load(); }
    size_t peakGpuUsage() const { return m_peakGpuUsage.load(); }
    
    // Pressure levels (0.0 - 1.0)
    double cpuPressure() const;
    double gpuPressure() const;
    
    // Check if under pressure
    bool isCpuUnderPressure() const { return cpuPressure() > 0.8; }
    bool isGpuUnderPressure() const { return gpuPressure() > 0.8; }
    bool isUnderPressure() const { return isCpuUnderPressure() || isGpuUnderPressure(); }

    // Set callback for when trim is needed
    void setTrimCallback(std::function<void()> callback) { m_trimCallback = callback; }

    // Reset statistics
    void resetPeak();

signals:
    void cpuPressureChanged(double pressure);
    void gpuPressureChanged(double pressure);
    void trimRequested();  // Emitted when memory pressure is high

private:
    void checkPressure();
    bool tryEvictForAllocation(size_t cpuNeeded, size_t gpuNeeded, Priority priority);

    std::atomic<size_t> m_cpuUsage{0};
    std::atomic<size_t> m_gpuUsage{0};
    std::atomic<size_t> m_peakCpuUsage{0};
    std::atomic<size_t> m_peakGpuUsage{0};
    
    std::atomic<size_t> m_maxCpuMemory{256 * 1024 * 1024};  // 256MB default
    std::atomic<size_t> m_maxGpuMemory{512 * 1024 * 1024};  // 512MB default
    
    std::function<void()> m_trimCallback;
    
    mutable QMutex m_pressureMutex;
    double m_lastCpuPressure = 0.0;
    double m_lastGpuPressure = 0.0;
};

} // namespace editor
