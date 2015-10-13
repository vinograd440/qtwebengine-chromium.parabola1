// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "config.h"
#include "platform/Timer.h"

#include "public/platform/Platform.h"
#include "public/platform/WebScheduler.h"
#include "public/platform/WebThread.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <queue>

using testing::ElementsAre;

namespace blink {
namespace {
double gCurrentTimeSecs = 0.0;

double currentTime()
{
    return gCurrentTimeSecs;
}

// This class exists because gcc doesn't know how to move an OwnPtr.
class RefCountedTaskContainer : public RefCounted<RefCountedTaskContainer> {
public:
    explicit RefCountedTaskContainer(WebTaskRunner::Task* task) : m_task(adoptPtr(task)) { }

    ~RefCountedTaskContainer() { }

    void run()
    {
        m_task->run();
    }

private:
    OwnPtr<WebTaskRunner::Task> m_task;
};

class DelayedTask {
public:
    DelayedTask(WebTaskRunner::Task* task, double delaySeconds)
        : m_task(adoptRef(new RefCountedTaskContainer(task)))
        , m_runTimeSeconds(monotonicallyIncreasingTime() + delaySeconds)
        , m_delaySeconds(delaySeconds) { }

    bool operator<(const DelayedTask& other) const
    {
        return m_runTimeSeconds > other.m_runTimeSeconds;
    }

    void run() const
    {
        m_task->run();
    }

    double runTimeSeconds() const
    {
        return m_runTimeSeconds;
    }

    double delaySeconds() const
    {
        return m_delaySeconds;
    }

private:
    RefPtr<RefCountedTaskContainer> m_task;
    double m_runTimeSeconds;
    double m_delaySeconds;
};

class MockWebTaskRunner : public WebTaskRunner {
public:
    explicit MockWebTaskRunner(std::priority_queue<DelayedTask>* timerTasks) : m_timerTasks(timerTasks) { }
    ~MockWebTaskRunner() override { }

    virtual void postTask(const WebTraceLocation&, Task* task)
    {
        m_timerTasks->push(DelayedTask(task, 0));
    }

    void postDelayedTask(const WebTraceLocation&, Task* task, double delayMs) override
    {
        m_timerTasks->push(DelayedTask(task, delayMs * 0.001));
    }

    std::priority_queue<DelayedTask>* m_timerTasks; // NOT OWNED
};

class MockWebScheduler : public WebScheduler {
public:
    MockWebScheduler() : m_timerWebTaskRunner(&m_timerTasks) { }
    ~MockWebScheduler() override { }

    bool shouldYieldForHighPriorityWork() override
    {
        return false;
    }

    bool canExceedIdleDeadlineIfRequired() override
    {
        return false;
    }

    void postIdleTask(const WebTraceLocation&, WebThread::IdleTask*) override
    {
    }

    void postNonNestableIdleTask(const WebTraceLocation&, WebThread::IdleTask*) override
    {
    }

    void postIdleTaskAfterWakeup(const WebTraceLocation&, WebThread::IdleTask*) override
    {
    }

    WebTaskRunner* timerTaskRunner() override
    {
        return &m_timerWebTaskRunner;
    }

    WebTaskRunner* loadingTaskRunner() override
    {
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    void postTimerTaskAt(const WebTraceLocation&, WebTaskRunner::Task* task, double monotonicTime) override
    {
        m_timerTasks.push(DelayedTask(task, (monotonicTime - monotonicallyIncreasingTime()) * 1000));
    }

    void runUntilIdle()
    {
        while (!m_timerTasks.empty()) {
            gCurrentTimeSecs = m_timerTasks.top().runTimeSeconds();
            m_timerTasks.top().run();
            m_timerTasks.pop();
        }
    }

    void runUntilIdleOrDeadlinePassed(double deadline)
    {
        while (!m_timerTasks.empty()) {
            if (m_timerTasks.top().runTimeSeconds() > deadline) {
                gCurrentTimeSecs = deadline;
                break;
            }
            gCurrentTimeSecs = m_timerTasks.top().runTimeSeconds();
            m_timerTasks.top().run();
            m_timerTasks.pop();
        }
    }

    void runPendingTasks()
    {
        while (!m_timerTasks.empty() && m_timerTasks.top().runTimeSeconds() <= gCurrentTimeSecs) {
            m_timerTasks.top().run();
            m_timerTasks.pop();
        }
    }

    bool hasOneTimerTask() const
    {
        return m_timerTasks.size() == 1;
    }

    double nextTimerTaskDelaySecs() const
    {
        ASSERT(hasOneTimerTask());
        return m_timerTasks.top().delaySeconds();
    }

private:
    std::priority_queue<DelayedTask> m_timerTasks;
    MockWebTaskRunner m_timerWebTaskRunner;
};

class FakeWebThread : public WebThread {
public:
    FakeWebThread() : m_webScheduler(adoptPtr(new MockWebScheduler())) { }
    ~FakeWebThread() override { }

    virtual bool isCurrentThread() const
    {
        ASSERT_NOT_REACHED();
        return true;
    }

    virtual PlatformThreadId threadId() const
    {
        ASSERT_NOT_REACHED();
        return 0;
    }

    WebTaskRunner* taskRunner() override
    {
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    WebScheduler* scheduler() const override
    {
        return m_webScheduler.get();
    }

    virtual void enterRunLoop()
    {
        ASSERT_NOT_REACHED();
    }

    virtual void exitRunLoop()
    {
        ASSERT_NOT_REACHED();
    }

private:
    OwnPtr<MockWebScheduler> m_webScheduler;
};

class TimerTestPlatform : public Platform {
public:
    TimerTestPlatform()
        : m_webThread(adoptPtr(new FakeWebThread())) { }
    ~TimerTestPlatform() override { }

    WebThread* currentThread() override
    {
        return m_webThread.get();
    }

    void cryptographicallyRandomValues(unsigned char*, size_t) override
    {
        ASSERT_NOT_REACHED();
    }

    const unsigned char* getTraceCategoryEnabledFlag(const char* categoryName) override
    {
        static const unsigned char enabled[] = {0};
        return enabled;
    }

    void runUntilIdle()
    {
        mockScheduler()->runUntilIdle();
    }

    void runPendingTasks()
    {
        mockScheduler()->runPendingTasks();
    }

    void runUntilIdleOrDeadlinePassed(double deadline)
    {
        mockScheduler()->runUntilIdleOrDeadlinePassed(deadline);
    }

    bool hasOneTimerTask() const
    {
        return mockScheduler()->hasOneTimerTask();
    }

    double nextTimerTaskDelaySecs() const
    {
        return mockScheduler()->nextTimerTaskDelaySecs();
    }

private:
    MockWebScheduler* mockScheduler() const
    {
        return static_cast<MockWebScheduler*>(m_webThread->scheduler());
    }

    OwnPtr<FakeWebThread> m_webThread;
};

class TimerTest : public testing::Test {
public:
    void SetUp() override
    {
        m_platform = adoptPtr(new TimerTestPlatform());
        m_oldPlatform = Platform::current();
        Platform::initialize(m_platform.get());
        WTF::setMonotonicallyIncreasingTimeFunction(currentTime);

        m_runTimes.clear();
        gCurrentTimeSecs = 10.0;
        m_startTime = gCurrentTimeSecs;
    }

    void TearDown() override
    {
        Platform::initialize(m_oldPlatform);
    }

    void countingTask(Timer<TimerTest>*)
    {
        m_runTimes.push_back(monotonicallyIncreasingTime());
    }

    void recordNextFireTimeTask(Timer<TimerTest>* timer)
    {
        m_nextFireTimes.push_back(monotonicallyIncreasingTime() + timer->nextFireInterval());
    }

    void advanceTimeBy(double timeSecs)
    {
        gCurrentTimeSecs += timeSecs;
    }

    void runUntilIdle()
    {
        m_platform->runUntilIdle();
    }

    void runPendingTasks()
    {
        m_platform->runPendingTasks();
    }

    void runUntilIdleOrDeadlinePassed(double deadline)
    {
        m_platform->runUntilIdleOrDeadlinePassed(deadline);
    }

    bool hasOneTimerTask() const
    {
        return m_platform->hasOneTimerTask();
    }

    double nextTimerTaskDelaySecs() const
    {
        return m_platform->nextTimerTaskDelaySecs();
    }

protected:
    double m_startTime;
    // TODO(alexclarke): Migrate to WTF::Vector and add gmock matcher support.
    std::vector<double> m_runTimes;
    std::vector<double> m_nextFireTimes;

private:
    OwnPtr<TimerTestPlatform> m_platform;
    Platform* m_oldPlatform;
};

TEST_F(TimerTest, StartOneShot_Zero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(0.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime));
}

TEST_F(TimerTest, StartOneShot_ZeroAndCancel)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(0.0, nextTimerTaskDelaySecs());

    timer.stop();

    runUntilIdle();
    EXPECT_TRUE(m_runTimes.empty());
}

TEST_F(TimerTest, StartOneShot_ZeroAndCancelThenRepost)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(0.0, nextTimerTaskDelaySecs());

    timer.stop();

    runUntilIdle();
    EXPECT_TRUE(m_runTimes.empty());

    timer.startOneShot(0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(0.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime));
}

TEST_F(TimerTest, StartOneShot_Zero_RepostingAfterRunning)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(0.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime));

    timer.startOneShot(0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(0.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime, m_startTime));
}

TEST_F(TimerTest, StartOneShot_NonZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10.0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(10.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 10.0));
}

TEST_F(TimerTest, StartOneShot_NonZeroAndCancel)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(10.0, nextTimerTaskDelaySecs());

    timer.stop();

    runUntilIdle();
    EXPECT_TRUE(m_runTimes.empty());
}

TEST_F(TimerTest, StartOneShot_NonZeroAndCancelThenRepost)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(10.0, nextTimerTaskDelaySecs());

    timer.stop();

    runUntilIdle();
    EXPECT_TRUE(m_runTimes.empty());

    double secondPostTime = monotonicallyIncreasingTime();
    timer.startOneShot(10, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(10.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(secondPostTime + 10.0));
}

TEST_F(TimerTest, StartOneShot_NonZero_RepostingAfterRunning)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(10.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 10.0));

    timer.startOneShot(20, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(20.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 10.0, m_startTime + 30.0));
}

TEST_F(TimerTest, PostingTimerTwiceWithSameRunTimeDoesNothing)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);
    timer.startOneShot(10, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(10.0, nextTimerTaskDelaySecs());

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 10.0));
}

TEST_F(TimerTest, PostingTimerTwiceWithNewerRunTimeCancelsOriginalTask)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);
    timer.startOneShot(0, FROM_HERE);

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 0.0));
}

TEST_F(TimerTest, PostingTimerTwiceWithLaterRunTimeCancelsOriginalTask)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);
    timer.startOneShot(10, FROM_HERE);

    runUntilIdle();
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 10.0));
}

TEST_F(TimerTest, StartRepeatingTask)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(1.0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(1.0, nextTimerTaskDelaySecs());

    runUntilIdleOrDeadlinePassed(m_startTime + 5.5);
    EXPECT_THAT(m_runTimes, ElementsAre(
        m_startTime + 1.0, m_startTime + 2.0, m_startTime + 3.0, m_startTime + 4.0, m_startTime + 5.0));
}

TEST_F(TimerTest, StartRepeatingTask_ThenCancel)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(1.0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(1.0, nextTimerTaskDelaySecs());

    runUntilIdleOrDeadlinePassed(m_startTime + 2.5);
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 1.0, m_startTime + 2.0));

    timer.stop();
    runUntilIdle();

    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 1.0, m_startTime + 2.0));
}

TEST_F(TimerTest, StartRepeatingTask_ThenPostOneShot)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(1.0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    EXPECT_FLOAT_EQ(1.0, nextTimerTaskDelaySecs());

    runUntilIdleOrDeadlinePassed(m_startTime + 2.5);
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 1.0, m_startTime + 2.0));

    timer.startOneShot(0, FROM_HERE);
    runUntilIdle();

    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 1.0, m_startTime + 2.0, m_startTime + 2.5));
}

TEST_F(TimerTest, IsActive_NeverPosted)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);

    EXPECT_FALSE(timer.isActive());
}

TEST_F(TimerTest, IsActive_AfterPosting_OneShotZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    EXPECT_TRUE(timer.isActive());
}

TEST_F(TimerTest, IsActive_AfterPosting_OneShotNonZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    EXPECT_TRUE(timer.isActive());
}

TEST_F(TimerTest, IsActive_AfterPosting_Repeating)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(1.0, FROM_HERE);

    EXPECT_TRUE(timer.isActive());
}

TEST_F(TimerTest, IsActive_AfterRunning_OneShotZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    runUntilIdle();
    EXPECT_FALSE(timer.isActive());
}

TEST_F(TimerTest, IsActive_AfterRunning_OneShotNonZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    runUntilIdle();
    EXPECT_FALSE(timer.isActive());
}

TEST_F(TimerTest, IsActive_AfterRunning_Repeating)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(1.0, FROM_HERE);

    runUntilIdleOrDeadlinePassed(m_startTime + 10);
    EXPECT_TRUE(timer.isActive()); // It should run until cancelled.
}

TEST_F(TimerTest, NextFireInterval_OneShotZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    EXPECT_FLOAT_EQ(0.0, timer.nextFireInterval());
}

TEST_F(TimerTest, NextFireInterval_OneShotNonZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    EXPECT_FLOAT_EQ(10.0, timer.nextFireInterval());
}

TEST_F(TimerTest, NextFireInterval_OneShotNonZero_AfterAFewSeconds)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    advanceTimeBy(2.0);
    EXPECT_FLOAT_EQ(8.0, timer.nextFireInterval());
}

TEST_F(TimerTest, NextFireInterval_Repeating)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(20, FROM_HERE);

    EXPECT_FLOAT_EQ(20.0, timer.nextFireInterval());
}

TEST_F(TimerTest, RepeatInterval_NeverStarted)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);

    EXPECT_FLOAT_EQ(0.0, timer.repeatInterval());
}

TEST_F(TimerTest, RepeatInterval_OneShotZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(0, FROM_HERE);

    EXPECT_FLOAT_EQ(0.0, timer.repeatInterval());
}

TEST_F(TimerTest, RepeatInterval_OneShotNonZero)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startOneShot(10, FROM_HERE);

    EXPECT_FLOAT_EQ(0.0, timer.repeatInterval());
}

TEST_F(TimerTest, RepeatInterval_Repeating)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(20, FROM_HERE);

    EXPECT_FLOAT_EQ(20.0, timer.repeatInterval());
}

TEST_F(TimerTest, AugmentRepeatInterval)
{
    Timer<TimerTest> timer(this, &TimerTest::countingTask);
    timer.startRepeating(10, FROM_HERE);
    EXPECT_FLOAT_EQ(10.0, timer.repeatInterval());
    EXPECT_FLOAT_EQ(10.0, timer.nextFireInterval());

    advanceTimeBy(2.0);
    timer.augmentRepeatInterval(10);

    EXPECT_FLOAT_EQ(20.0, timer.repeatInterval());
    EXPECT_FLOAT_EQ(18.0, timer.nextFireInterval());

    runUntilIdleOrDeadlinePassed(m_startTime + 50.0);
    EXPECT_THAT(m_runTimes, ElementsAre(m_startTime + 20.0, m_startTime + 40.0));
}

class MockTimerWithAlignment : public TimerBase {
public:
    MockTimerWithAlignment() : m_lastFireTime(0.0), m_alignedFireTime(0.0) { }

    void fired() override
    {
    }

    double alignedFireTime(double fireTime) const override
    {
        m_lastFireTime = fireTime;
        return m_alignedFireTime;
    }

    void setAlignedFireTime(double alignedFireTime)
    {
        m_alignedFireTime = alignedFireTime;
    }

    double lastFireTime() const
    {
        return m_lastFireTime;
    }

private:
    mutable double m_lastFireTime;
    double m_alignedFireTime;
};

TEST_F(TimerTest, TimerAlignment_OneShotZero)
{
    MockTimerWithAlignment timer;
    timer.setAlignedFireTime(m_startTime + 1.0);

    timer.start(0.0, 0.0, FROM_HERE);

    // The nextFireInterval gets overrriden.
    EXPECT_FLOAT_EQ(1.0, timer.nextFireInterval());
    EXPECT_FLOAT_EQ(0.0, timer.nextUnalignedFireInterval());
    EXPECT_FLOAT_EQ(m_startTime, timer.lastFireTime());
}

TEST_F(TimerTest, TimerAlignment_OneShotNonZero)
{
    MockTimerWithAlignment timer;
    timer.setAlignedFireTime(m_startTime + 1.0);

    timer.start(0.5, 0.0, FROM_HERE);

    // The nextFireInterval gets overrriden.
    EXPECT_FLOAT_EQ(1.0, timer.nextFireInterval());
    EXPECT_FLOAT_EQ(0.5, timer.nextUnalignedFireInterval());
    EXPECT_FLOAT_EQ(m_startTime + 0.5, timer.lastFireTime());
}

TEST_F(TimerTest, DidChangeAlignmentInterval)
{
    MockTimerWithAlignment timer;
    timer.setAlignedFireTime(m_startTime + 1.0);

    timer.start(0.0, 0.0, FROM_HERE);

    EXPECT_FLOAT_EQ(1.0, timer.nextFireInterval());
    EXPECT_FLOAT_EQ(0.0, timer.nextUnalignedFireInterval());
    EXPECT_FLOAT_EQ(m_startTime, timer.lastFireTime());

    timer.setAlignedFireTime(m_startTime);
    timer.didChangeAlignmentInterval(monotonicallyIncreasingTime());

    EXPECT_FLOAT_EQ(0.0, timer.nextFireInterval());
    EXPECT_FLOAT_EQ(0.0, timer.nextUnalignedFireInterval());
    EXPECT_FLOAT_EQ(m_startTime, timer.lastFireTime());
}

TEST_F(TimerTest, RepeatingTimerDoesNotDrift)
{
    Timer<TimerTest> timer(this, &TimerTest::recordNextFireTimeTask);
    timer.startRepeating(2.0, FROM_HERE);

    ASSERT(hasOneTimerTask());
    recordNextFireTimeTask(&timer); // Next scheduled task to run at m_startTime + 2.0

    // Simulate timer firing early. Next scheduled task to run at m_startTime + 4.0
    advanceTimeBy(1.9);
    runUntilIdleOrDeadlinePassed(gCurrentTimeSecs + 0.2);

    advanceTimeBy(2.0);
    runPendingTasks(); // Next scheduled task to run at m_startTime + 6.0

    advanceTimeBy(2.1);
    runPendingTasks(); // Next scheduled task to run at m_startTime + 8.0

    advanceTimeBy(2.9);
    runPendingTasks(); // Next scheduled task to run at m_startTime + 10.0

    advanceTimeBy(3.1);
    runPendingTasks(); // Next scheduled task to run at m_startTime + 14.0 (skips a beat)

    advanceTimeBy(4.0);
    runPendingTasks(); // Next scheduled task to run at m_startTime + 18.0 (skips a beat)

    advanceTimeBy(10.0); // Next scheduled task to run at m_startTime + 28.0 (skips 5 beats)
    runPendingTasks();

    runUntilIdleOrDeadlinePassed(m_startTime + 5.5);
    EXPECT_THAT(m_nextFireTimes, ElementsAre(
        m_startTime + 2.0,
        m_startTime + 4.0,
        m_startTime + 6.0,
        m_startTime + 8.0,
        m_startTime + 10.0,
        m_startTime + 14.0,
        m_startTime + 18.0,
        m_startTime + 28.0));
}

} // namespace
} // namespace blink
