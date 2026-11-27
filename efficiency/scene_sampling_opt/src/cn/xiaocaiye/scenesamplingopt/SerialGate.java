package cn.xiaocaiye.scenesamplingopt;

import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;

/** Serializes the original operation; freshness and force-refresh remain its responsibility. */
public final class SerialGate {
    @FunctionalInterface public interface Operation<T> { T run() throws Throwable; }

    private final ReentrantLock lock;
    private final AtomicInteger active = new AtomicInteger();
    private final AtomicInteger waiting = new AtomicInteger();
    private final AtomicLong calls = new AtomicLong();
    private final AtomicLong contended = new AtomicLong();
    private final AtomicLong waitNanos = new AtomicLong();
    private volatile boolean quiesced;

    public SerialGate() { this(new ReentrantLock()); }
    public SerialGate(ReentrantLock lock) {
        if (lock == null) throw new NullPointerException("lock");
        this.lock = lock;
    }

    public <T> T call(Operation<T> operation) throws Throwable {
        if (operation == null) throw new NullPointerException("operation");
        calls.incrementAndGet();
        if (!lock.tryLock()) {
            contended.incrementAndGet();
            waiting.incrementAndGet();
            long start = System.nanoTime();
            try { lock.lock(); }
            finally {
                waitNanos.addAndGet(System.nanoTime() - start);
                waiting.decrementAndGet();
            }
        }
        active.incrementAndGet();
        try { return operation.run(); }
        finally {
            active.decrementAndGet();
            lock.unlock();
        }
    }

    /**
     * Only reports an idle handoff point; in-flight old hook snapshots still use this lock.
     * The bootstrap ReentrantLock is shared with the next module generation, so this never
     * creates an unlocked fallback while old/new chains overlap.
     */
    public boolean tryQuiesce() {
        if (!lock.tryLock()) return false;
        try {
            if (active.get() != 0 || waiting.get() != 0 || lock.hasQueuedThreads()) return false;
            quiesced = true;
            return true;
        } finally { lock.unlock(); }
    }

    public void resume() { quiesced = false; }
    public boolean isQuiesced() { return quiesced; }
    public int activeCalls() { return active.get(); }
    public int waitingCalls() { return waiting.get(); }
    public long callCount() { return calls.get(); }
    public long contendedCount() { return contended.get(); }
    public long waitNanos() { return waitNanos.get(); }
    public ReentrantLock sharedLock() { return lock; }
    public void resetCounters() {
        calls.set(0); contended.set(0); waitNanos.set(0);
    }
}
