package cn.xiaocaiye.scenesamplingopt;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.LockSupport;
import java.util.concurrent.locks.ReentrantLock;

/** Behavioral contract tests for the production gate. No Android or target state is accessed. */
public final class GateContractTest {
    private static int assertions;
    private static final List<String> passed = new ArrayList<>();
    private static final List<String> failed = new ArrayList<>();

    @FunctionalInterface interface Checked { void run() throws Throwable; }
    @FunctionalInterface interface Condition { boolean get(); }

    static void check(boolean condition, String message) {
        assertions++;
        if (!condition) throw new AssertionError(message);
    }

    static void spec(String name, Checked test) {
        try { test.run(); passed.add(name); }
        catch (Throwable failure) { failed.add(name + ": " + failure); }
    }

    static void await(CountDownLatch latch) throws InterruptedException {
        if (!latch.await(3, TimeUnit.SECONDS)) throw new AssertionError("test coordination timed out");
    }

    static void until(Condition condition) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (!condition.get()) {
            if (System.nanoTime() >= deadline) throw new AssertionError("gate waiter did not arrive");
            LockSupport.parkNanos(100_000);
        }
    }

    static <T> T result(Future<T> future) throws Exception { return future.get(3, TimeUnit.SECONDS); }

    /** The existing APK policy, with transport/clock supplied by the test rather than Android. */
    static final class Battery {
        final SerialGate gate;
        final AtomicLong clock = new AtomicLong(10_000);
        final AtomicInteger reads = new AtomicInteger();
        long stamp;
        SerialGate.Operation<Void> transport = () -> null;
        Battery(SerialGate gate) { this.gate = gate; }
        long get(long maxAge) throws Throwable {
            return gate.call(() -> {
                if (maxAge == 0 || clock.get() - maxAge >= stamp) {
                    reads.incrementAndGet();
                    transport.run();
                    stamp = clock.get();
                }
                return stamp;
            });
        }
    }

    static final class Reading {
        final int generation;
        Reading(int generation) { this.generation = generation; }
    }

    /** Mirrors only published APK cache rules; the production gate supplies synchronization. */
    static final class Monitor {
        final SerialGate gate = new SerialGate();
        final AtomicLong clock = new AtomicLong(10_000);
        final AtomicInteger fullReads = new AtomicInteger();
        final AtomicInteger minReads = new AtomicInteger();
        long fullStamp;
        Reading cached;
        SerialGate.Operation<Void> fullTransport = () -> null;
        boolean failFull;
        boolean failMin;

        Reading full() throws Throwable {
            return gate.call(() -> {
                long requestStarted = clock.get();
                int id = fullReads.incrementAndGet();
                fullTransport.run();
                if (failFull) return null;
                Reading value = new Reading(id);
                fullStamp = requestStarted;
                cached = value;
                return value;
            });
        }

        Reading min() throws Throwable {
            return gate.call(() -> {
                if (clock.get() - fullStamp < 500 && cached != null) return cached;
                int id = minReads.incrementAndGet();
                return failMin ? null : new Reading(-id);
            });
        }
    }

    public static void main(String[] args) throws Throwable {
        spec("original return value and invocation are retained", () -> {
            SerialGate gate = new SerialGate();
            Object token = new Object();
            AtomicInteger calls = new AtomicInteger();
            check(gate.call(() -> { calls.incrementAndGet(); return token; }) == token, "result identity");
            check(calls.get() == 1, "original operation once");
        });
        spec("gate does not introduce an independent result cache", () -> {
            SerialGate gate = new SerialGate();
            AtomicInteger values = new AtomicInteger();
            check(gate.call(values::incrementAndGet) == 1, "first invocation");
            check(gate.call(values::incrementAndGet) == 2, "second invocation");
        });
        spec("original exception identity propagates and lock is released", () -> {
            SerialGate gate = new SerialGate();
            Exception sentinel = new Exception("transport failure");
            try { gate.call(() -> { throw sentinel; }); throw new AssertionError("exception swallowed"); }
            catch (Exception actual) { check(actual == sentinel, "exception identity"); }
            check(gate.call(() -> 7) == 7, "recovery after failure");
            check(gate.activeCalls() == 0 && gate.waitingCalls() == 0, "no phantom activity");
        });
        spec("same-thread nested target calls remain reentrant", () -> {
            SerialGate gate = new SerialGate();
            check(gate.call(() -> gate.call(() -> 41) + 1) == 42, "nested result");
            check(gate.activeCalls() == 0, "reentrant release");
        });
        spec("positive TTL concurrent misses recheck after the first refresh", () -> {
            Battery battery = new Battery(new SerialGate());
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            battery.transport = () -> { entered.countDown(); await(release); return null; };
            ExecutorService workers = Executors.newFixedThreadPool(2);
            try {
                Future<Long> first = workers.submit(() -> callBattery(battery, 5000));
                await(entered);
                Future<Long> second = workers.submit(() -> callBattery(battery, 5000));
                until(() -> battery.gate.waitingCalls() > 0);
                release.countDown();
                check(result(first) == 10_000 && result(second) == 10_000, "both see current stamp");
                check(battery.reads.get() == 1, "one allowed refresh");
            } finally { release.countDown(); workers.shutdownNow(); }
        });
        spec("force refresh zero is never merged", () -> {
            Battery battery = new Battery(new SerialGate());
            ExecutorService workers = Executors.newFixedThreadPool(6);
            try {
                List<Future<Long>> jobs = new ArrayList<>();
                for (int i = 0; i < 18; i++) jobs.add(workers.submit(() -> callBattery(battery, 0)));
                for (Future<Long> job : jobs) check(result(job) == 10_000, "force return");
                check(battery.reads.get() == 18, "every zero argument refreshes");
            } finally { workers.shutdownNow(); }
        });
        spec("callers retain different TTL contracts", () -> {
            Battery battery = new Battery(new SerialGate());
            battery.get(5000);
            battery.clock.set(12_000);
            check(battery.get(5000) == 10_000, "five-second caller may reuse");
            check(battery.get(1000) == 12_000, "one-second caller must refresh");
            check(battery.reads.get() == 2, "not a global fixed five-second cache");
        });
        spec("original TTL equality boundary refreshes", () -> {
            Battery battery = new Battery(new SerialGate());
            battery.get(5000);
            battery.clock.set(14_999);
            check(battery.get(5000) == 10_000, "4999 milliseconds is fresh");
            battery.clock.set(15_000);
            check(battery.get(5000) == 15_000, "5000 milliseconds is expired");
        });
        spec("original unexpected negative argument is not rewritten", () -> {
            Battery battery = new Battery(new SerialGate());
            battery.get(5000);
            battery.get(-1);
            check(battery.reads.get() == 2, "negative original condition retained");
        });
        spec("failed battery refresh does not publish success", () -> {
            Battery battery = new Battery(new SerialGate());
            battery.transport = () -> { throw new IllegalStateException("read failed"); };
            try { battery.get(5000); throw new AssertionError("failure swallowed"); }
            catch (IllegalStateException expected) { check(battery.stamp == 0, "timestamp not advanced"); }
            battery.transport = () -> null;
            battery.get(5000);
            check(battery.reads.get() == 2 && battery.stamp == 10_000, "next request retries original read");
        });
        spec("transport timeout does not poison following calls", () -> {
            SerialGate gate = new SerialGate();
            try { gate.call(() -> { throw new TimeoutException("fixture timeout"); }); throw new AssertionError("timeout swallowed"); }
            catch (TimeoutException expected) { check(gate.activeCalls() == 0, "released after timeout"); }
            check(gate.call(() -> "recovered").equals("recovered"), "following request");
        });
        spec("separate battery and monitor gates do not serialize each other", () -> {
            SerialGate battery = new SerialGate(), monitor = new SerialGate();
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            ExecutorService workers = Executors.newFixedThreadPool(2);
            try {
                Future<Integer> blocked = workers.submit(() -> callGate(battery, () -> { entered.countDown(); await(release); return 1; }));
                await(entered);
                Future<Integer> other = workers.submit(() -> callGate(monitor, () -> 2));
                check(result(other) == 2, "independent operation completes while battery is held");
                release.countDown();
                check(result(blocked) == 1, "blocked operation completes normally");
            } finally { release.countDown(); workers.shutdownNow(); }
        });
        spec("one gate excludes concurrent operations", () -> {
            SerialGate gate = new SerialGate();
            AtomicInteger active = new AtomicInteger(), peak = new AtomicInteger(), count = new AtomicInteger();
            ExecutorService workers = Executors.newFixedThreadPool(8);
            try {
                List<Future<Integer>> jobs = new ArrayList<>();
                for (int i = 0; i < 50; i++) jobs.add(workers.submit(() -> callGate(gate, () -> {
                    int live = active.incrementAndGet(); peak.accumulateAndGet(live, Math::max);
                    try { Thread.yield(); return count.incrementAndGet(); } finally { active.decrementAndGet(); }
                })));
                for (Future<Integer> job : jobs) result(job);
                check(count.get() == 50 && peak.get() == 1, "no operations lost or overlap");
            } finally { workers.shutdownNow(); }
        });
        spec("old and new wrappers use the same bootstrap lock", () -> {
            SerialGate oldGate = new SerialGate();
            ReentrantLock bootstrapLock = oldGate.sharedLock();
            SerialGate newGate = new SerialGate(bootstrapLock);
            check(newGate.sharedLock() == bootstrapLock, "bootstrap lock identity retained");
            AtomicInteger active = new AtomicInteger(), peak = new AtomicInteger(), count = new AtomicInteger();
            ExecutorService workers = Executors.newFixedThreadPool(8);
            try {
                List<Future<Integer>> jobs = new ArrayList<>();
                for (int i = 0; i < 60; i++) {
                    SerialGate generation = i % 2 == 0 ? oldGate : newGate;
                    jobs.add(workers.submit(() -> callGate(generation, () -> {
                        int live = active.incrementAndGet(); peak.accumulateAndGet(live, Math::max);
                        try { Thread.yield(); return count.incrementAndGet(); } finally { active.decrementAndGet(); }
                    })));
                }
                for (Future<Integer> job : jobs) result(job);
                check(count.get() == 60 && peak.get() == 1, "old/new hook snapshots cannot overlap the operation");
            } finally { workers.shutdownNow(); }
        });
        spec("quiesced old snapshots still serialize with the next generation", () -> {
            SerialGate oldGate = new SerialGate();
            check(oldGate.tryQuiesce() && oldGate.isQuiesced(), "idle wrapper can hand off");
            SerialGate next = new SerialGate(oldGate.sharedLock());
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            ExecutorService workers = Executors.newFixedThreadPool(2);
            try {
                Future<Integer> oldCall = workers.submit(() -> callGate(oldGate, () -> { entered.countDown(); await(release); return 1; }));
                await(entered);
                Future<Integer> newCall = workers.submit(() -> callGate(next, () -> 2));
                until(() -> next.waitingCalls() > 0);
                check(!newCall.isDone(), "quiesce did not make old snapshots bypass the lock");
                release.countDown();
                check(result(oldCall) == 1 && result(newCall) == 2, "both generations preserve original result");
            } finally { release.countDown(); workers.shutdownNow(); }
        });
        spec("exception in old generation releases shared lock for new generation", () -> {
            SerialGate oldGate = new SerialGate(), next = new SerialGate(oldGate.sharedLock());
            Error sentinel = new AssertionError("original operation failed");
            try { oldGate.call(() -> { throw sentinel; }); throw new AssertionError("error swallowed"); }
            catch (Error actual) { check(actual == sentinel, "original Error identity retained"); }
            check(next.call(() -> 3) == 3, "next wrapper can enter shared lock");
        });
        spec("active or waiting work refuses an idle handoff claim", () -> {
            SerialGate gate = new SerialGate();
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            ExecutorService worker = Executors.newSingleThreadExecutor();
            try {
                Future<Integer> busy = worker.submit(() -> callGate(gate, () -> { entered.countDown(); await(release); return 5; }));
                await(entered);
                check(!gate.tryQuiesce(), "active lock cannot be declared idle");
                release.countDown(); result(busy);
                check(gate.tryQuiesce(), "quiesce succeeds after original work exits");
                gate.resume(); check(!gate.isQuiesced(), "resume updates handoff state");
            } finally { release.countDown(); worker.shutdownNow(); }
        });
        spec("waiting on the gate does not discard the caller interrupt flag", () -> {
            SerialGate gate = new SerialGate();
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            ExecutorService worker = Executors.newSingleThreadExecutor();
            AtomicInteger observed = new AtomicInteger();
            Thread waiter = new Thread(() -> {
                try { gate.call(() -> { observed.set(Thread.currentThread().isInterrupted() ? 1 : -1); return null; }); }
                catch (Throwable failure) { observed.set(-2); }
            }, "contract-interrupted-waiter");
            try {
                Future<Integer> owner = worker.submit(() -> callGate(gate, () -> { entered.countDown(); await(release); return 1; }));
                await(entered); waiter.start();
                until(() -> gate.waitingCalls() > 0);
                waiter.interrupt(); release.countDown(); result(owner);
                waiter.join(3000);
                check(!waiter.isAlive() && observed.get() == 1, "original operation observes the original interrupt flag");
            } finally { release.countDown(); worker.shutdownNow(); waiter.interrupt(); }
        });
        spec("full monitor queries are never silently skipped", () -> {
            Monitor monitor = new Monitor();
            Reading a = monitor.full(), b = monitor.full();
            check(a != b && monitor.fullReads.get() == 2, "full remains fresh on each call");
        });
        spec("min only reuses the existing fresh full result", () -> {
            Monitor monitor = new Monitor();
            Reading full = monitor.full();
            monitor.clock.set(10_499);
            check(monitor.min() == full, "499ms full cache accepted");
            monitor.clock.set(10_500);
            check(monitor.min() != full && monitor.minReads.get() == 1, "500ms full cache rejected");
        });
        spec("slow full request does not extend its original cache lifetime", () -> {
            Monitor monitor = new Monitor();
            monitor.fullTransport = () -> { monitor.clock.set(10_600); return null; };
            Reading full = monitor.full();
            check(monitor.fullStamp == 10_000, "timestamp remains at request start");
            check(monitor.min() != full && monitor.minReads.get() == 1, "already-expired full does not satisfy min");
        });
        spec("concurrent min waits for full then uses original cache policy", () -> {
            Monitor monitor = new Monitor();
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            monitor.fullTransport = () -> { entered.countDown(); await(release); return null; };
            ExecutorService workers = Executors.newFixedThreadPool(2);
            try {
                Future<Reading> full = workers.submit(() -> invoke(monitor::full));
                await(entered);
                Future<Reading> min = workers.submit(() -> invoke(monitor::min));
                until(() -> monitor.gate.waitingCalls() > 0);
                release.countDown();
                check(result(full) == result(min), "min sees full published result");
                check(monitor.fullReads.get() == 1 && monitor.minReads.get() == 0, "no redundant min query");
            } finally { release.countDown(); workers.shutdownNow(); }
        });
        spec("min response does not create a new cache", () -> {
            Monitor monitor = new Monitor();
            Reading first = monitor.min(), second = monitor.min();
            check(first != second && monitor.minReads.get() == 2, "no added min TTL");
        });
        spec("failed full response does not become a success cache", () -> {
            Monitor monitor = new Monitor();
            monitor.failFull = true;
            check(monitor.full() == null, "full failure retained");
            check(monitor.min() != null && monitor.minReads.get() == 1, "min queries on missing success cache");
        });
        spec("failed min response is not remembered as a successful result", () -> {
            Monitor monitor = new Monitor();
            monitor.failMin = true;
            check(monitor.min() == null, "first min failure");
            monitor.failMin = false;
            check(monitor.min() != null && monitor.minReads.get() == 2, "next min still queries");
        });
        System.out.println("GATE_CONTRACT_RESULT assertions=" + assertions + " passed=" + passed.size() + " failed=" + failed.size());
        for (String name : passed) System.out.println("PASS " + name);
        for (String name : failed) System.err.println("FAIL " + name);
        if (!failed.isEmpty()) System.exit(1);
    }

    static long callBattery(Battery battery, long ttl) throws Exception {
        try { return battery.get(ttl); }
        catch (Exception failure) { throw failure; }
        catch (Throwable failure) { throw new Exception(failure); }
    }

    static <T> T callGate(SerialGate gate, SerialGate.Operation<T> operation) throws Exception {
        try { return gate.call(operation); }
        catch (Exception failure) { throw failure; }
        catch (Throwable failure) { throw new Exception(failure); }
    }

    static <T> T invoke(SerialGate.Operation<T> operation) throws Exception {
        try { return operation.run(); }
        catch (Exception failure) { throw failure; }
        catch (Throwable failure) { throw new Exception(failure); }
    }
}
