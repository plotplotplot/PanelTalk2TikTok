#include <QtTest/QtTest>
#include "../memory_budget.h"

using namespace editor;

class TestMemoryBudget : public QObject {
    Q_OBJECT

private slots:
    void testInitialState();
    void testAllocation();
    void testDeallocation();
    void testPressureCalculation();
    void testPriorityAllocation();
    void testTrimCallback();
};

void TestMemoryBudget::testInitialState() {
    MemoryBudget budget;
    
    QCOMPARE(budget.currentCpuUsage(), 0);
    QCOMPARE(budget.currentGpuUsage(), 0);
    QCOMPARE(budget.cpuPressure(), 0.0);
    QCOMPARE(budget.gpuPressure(), 0.0);
    QVERIFY(!budget.isUnderPressure());
}

void TestMemoryBudget::testAllocation() {
    MemoryBudget budget;
    budget.setMaxCpuMemory(1024 * 1024);  // 1MB
    budget.setMaxGpuMemory(1024 * 1024);  // 1MB
    
    // Should be able to allocate within budget
    QVERIFY(budget.allocateCpu(100000, MemoryBudget::Priority::Normal));
    QCOMPARE(budget.currentCpuUsage(), 100000);
    
    QVERIFY(budget.allocateGpu(100000, MemoryBudget::Priority::Normal));
    QCOMPARE(budget.currentGpuUsage(), 100000);
}

void TestMemoryBudget::testDeallocation() {
    MemoryBudget budget;
    budget.setMaxCpuMemory(1024 * 1024);
    
    QVERIFY(budget.allocateCpu(100000, MemoryBudget::Priority::Normal));
    QCOMPARE(budget.currentCpuUsage(), 100000);
    
    budget.deallocateCpu(50000);
    QCOMPARE(budget.currentCpuUsage(), 50000);
    
    budget.deallocateCpu(50000);
    QCOMPARE(budget.currentCpuUsage(), 0);
}

void TestMemoryBudget::testPressureCalculation() {
    MemoryBudget budget;
    budget.setMaxCpuMemory(1000);
    budget.setMaxGpuMemory(1000);
    
    // Allocate 50% of CPU budget
    QVERIFY(budget.allocateCpu(500, MemoryBudget::Priority::Normal));
    QCOMPARE(budget.cpuPressure(), 0.5);
    QVERIFY(!budget.isCpuUnderPressure());
    
    // Allocate 90% of CPU budget
    QVERIFY(budget.allocateCpu(400, MemoryBudget::Priority::Normal));
    QVERIFY(budget.cpuPressure() > 0.8);
    QVERIFY(budget.isCpuUnderPressure());
}

void TestMemoryBudget::testPriorityAllocation() {
    MemoryBudget budget;
    budget.setMaxCpuMemory(1000);
    
    // Fill up budget
    QVERIFY(budget.allocateCpu(1000, MemoryBudget::Priority::Normal));
    
    // Should fail to allocate more
    QVERIFY(!budget.allocateCpu(1, MemoryBudget::Priority::Normal));
    
    // But critical priority might succeed if trim callback frees memory
    // (depends on implementation)
}

void TestMemoryBudget::testTrimCallback() {
    MemoryBudget budget;
    bool trimCalled = false;
    
    budget.setTrimCallback([&trimCalled]() {
        trimCalled = true;
    });
    
    budget.setMaxCpuMemory(100);
    QVERIFY(budget.allocateCpu(90, MemoryBudget::Priority::Normal));
    
    // Should trigger trim callback when pressure is high
    QVERIFY(trimCalled);
}

QTEST_MAIN(TestMemoryBudget)
#include "test_memory_budget.moc"
