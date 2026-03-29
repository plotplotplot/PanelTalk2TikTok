#include "memory_budget.h"

#include <QDebug>
#include <algorithm>

namespace editor {

MemoryBudget::MemoryBudget(QObject* parent) 
    : QObject(parent) {}

void MemoryBudget::setMaxCpuMemory(size_t bytes) {
    m_maxCpuMemory.store(bytes);
    checkPressure();
}

void MemoryBudget::setMaxGpuMemory(size_t bytes) {
    m_maxGpuMemory.store(bytes);
    checkPressure();
}

bool MemoryBudget::allocateCpu(size_t bytes, Priority priority) {
    if (bytes == 0) return true;
    
    size_t current = m_cpuUsage.load();
    size_t max = m_maxCpuMemory.load();
    
    // Try simple allocation first
    while (current + bytes <= max) {
        if (m_cpuUsage.compare_exchange_weak(current, current + bytes)) {
            // Update peak
            size_t peak = m_peakCpuUsage.load();
            while (current + bytes > peak && !m_peakCpuUsage.compare_exchange_weak(peak, current + bytes)) {}
            checkPressure();
            return true;
        }
    }
    
    // Out of memory - try to trigger trim
    if (m_trimCallback && static_cast<int>(priority) >= 50) {
        m_trimCallback();
        
        // Try again after trim
        current = m_cpuUsage.load();
        while (current + bytes <= max) {
            if (m_cpuUsage.compare_exchange_weak(current, current + bytes)) {
                size_t peak = m_peakCpuUsage.load();
                while (current + bytes > peak && !m_peakCpuUsage.compare_exchange_weak(peak, current + bytes)) {}
                checkPressure();
                return true;
            }
        }
    }
    
    return false;
}

bool MemoryBudget::allocateGpu(size_t bytes, Priority priority) {
    if (bytes == 0) return true;
    
    size_t current = m_gpuUsage.load();
    size_t max = m_maxGpuMemory.load();
    
    while (current + bytes <= max) {
        if (m_gpuUsage.compare_exchange_weak(current, current + bytes)) {
            size_t peak = m_peakGpuUsage.load();
            while (current + bytes > peak && !m_peakGpuUsage.compare_exchange_weak(peak, current + bytes)) {}
            checkPressure();
            return true;
        }
    }
    
    if (m_trimCallback && static_cast<int>(priority) >= 50) {
        m_trimCallback();
        
        current = m_gpuUsage.load();
        while (current + bytes <= max) {
            if (m_gpuUsage.compare_exchange_weak(current, current + bytes)) {
                size_t peak = m_peakGpuUsage.load();
                while (current + bytes > peak && !m_peakGpuUsage.compare_exchange_weak(peak, current + bytes)) {}
                checkPressure();
                return true;
            }
        }
    }
    
    return false;
}

bool MemoryBudget::allocate(size_t cpuBytes, size_t gpuBytes, Priority priority) {
    if (!allocateCpu(cpuBytes, priority)) {
        return false;
    }
    if (!allocateGpu(gpuBytes, priority)) {
        deallocateCpu(cpuBytes);
        return false;
    }
    return true;
}

void MemoryBudget::deallocateCpu(size_t bytes) {
    if (bytes == 0) return;
    
    size_t current = m_cpuUsage.load();
    size_t newValue = (current > bytes) ? current - bytes : 0;
    
    while (!m_cpuUsage.compare_exchange_weak(current, newValue)) {
        newValue = (current > bytes) ? current - bytes : 0;
    }
    
    checkPressure();
}

void MemoryBudget::deallocateGpu(size_t bytes) {
    if (bytes == 0) return;
    
    size_t current = m_gpuUsage.load();
    size_t newValue = (current > bytes) ? current - bytes : 0;
    
    while (!m_gpuUsage.compare_exchange_weak(current, newValue)) {
        newValue = (current > bytes) ? current - bytes : 0;
    }
    
    checkPressure();
}

void MemoryBudget::deallocate(size_t cpuBytes, size_t gpuBytes) {
    deallocateCpu(cpuBytes);
    deallocateGpu(gpuBytes);
}

double MemoryBudget::cpuPressure() const {
    size_t max = m_maxCpuMemory.load();
    if (max == 0) return 0.0;
    return static_cast<double>(m_cpuUsage.load()) / static_cast<double>(max);
}

double MemoryBudget::gpuPressure() const {
    size_t max = m_maxGpuMemory.load();
    if (max == 0) return 0.0;
    return static_cast<double>(m_gpuUsage.load()) / static_cast<double>(max);
}

void MemoryBudget::checkPressure() {
    double cpuP = cpuPressure();
    double gpuP = gpuPressure();

    bool emitCpuChanged = false;
    bool emitGpuChanged = false;
    bool requestTrim = false;

    {
        QMutexLocker lock(&m_pressureMutex);

        if (std::abs(cpuP - m_lastCpuPressure) > 0.05) {
            m_lastCpuPressure = cpuP;
            emitCpuChanged = true;
        }

        if (std::abs(gpuP - m_lastGpuPressure) > 0.05) {
            m_lastGpuPressure = gpuP;
            emitGpuChanged = true;
        }

        requestTrim = cpuP > 0.85 || gpuP > 0.85;
    }

    if (emitCpuChanged) {
        emit cpuPressureChanged(cpuP);
    }

    if (emitGpuChanged) {
        emit gpuPressureChanged(gpuP);
    }

    if (requestTrim) {
        if (m_trimCallback) {
            m_trimCallback();
        }
        emit trimRequested();
    }
}

void MemoryBudget::resetPeak() {
    m_peakCpuUsage.store(m_cpuUsage.load());
    m_peakGpuUsage.store(m_gpuUsage.load());
}

} // namespace editor
