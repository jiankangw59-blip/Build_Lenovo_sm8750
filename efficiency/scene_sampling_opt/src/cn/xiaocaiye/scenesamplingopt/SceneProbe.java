package cn.xiaocaiye.scenesamplingopt;

import org.json.JSONArray;
import org.json.JSONObject;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReferenceArray;

/** Explicit finite diagnostics against the already-bound real Scene methods. Never scheduled. */
final class SceneProbe {
    @FunctionalInterface interface Stats { JSONObject snapshot() throws Exception; }
    @FunctionalInterface private interface Operation { JSONObject run() throws Throwable; }

    private static final long LIMIT_NANOS = TimeUnit.MILLISECONDS.toNanos(7500);
    private final SceneBindings target;
    private final AtomicInteger active;
    private final Stats stats;
    private final List<Thread> threads = new ArrayList<>();
    private long deadline;

    SceneProbe(SceneBindings target, AtomicInteger active, Stats stats) {
        this.target = target;
        this.active = active;
        this.stats = stats;
    }

    synchronized void registerCoordinator(Thread thread) { threads.add(thread); }

    synchronized boolean hasLiveThreads() {
        for (Thread thread : threads) if (thread.isAlive()) return true;
        return false;
    }

    synchronized JSONArray liveThreads() {
        JSONArray result = new JSONArray();
        for (Thread thread : threads) if (thread.isAlive()) result.put(thread.getName());
        return result;
    }

    JSONObject battery() throws Exception {
        deadline = System.nanoTime() + LIMIT_NANOS;
        JSONObject result = header("battery");
        JSONObject before = stats.snapshot();
        long timestampBefore = target.batteryTimestamp.getLong(null);
        long ageBefore = System.currentTimeMillis() - timestampBefore;
        long requestedWait = ageBefore < 1100 ? 1100 - ageBefore : 0;
        // A changed wall clock must not turn this explicit diagnostic into an unbounded sleep.
        long waitMillis = Math.min(1200, Math.max(0, requestedWait));
        if (waitMillis > 0) Thread.sleep(waitMillis); // one wait, no freshness polling/retry loop
        long timestampAtStart = target.batteryTimestamp.getLong(null);
        result.put("timestampBefore", timestampBefore).put("ageBeforeMillis", ageBefore)
                .put("requestedSingleWaitMillis", requestedWait).put("singleWaitMillis", waitMillis)
                .put("timestampAtParallelStart", timestampAtStart)
                .put("ageAtParallelStartMillis", System.currentTimeMillis() - timestampAtStart);

        Operation[] parallel = new Operation[4];
        for (int i = 0; i < parallel.length; i++) parallel[i] = () -> callBattery(1000);
        JSONObject concurrent = batch("battery_ttl1000", parallel);
        result.put("parallel", concurrent);

        JSONArray forced = new JSONArray();
        for (int i = 0; i < 2; i++) {
            if (remaining() <= 0 || !concurrent.optBoolean("complete")) {
                forced.put(new JSONObject().put("skipped", true).put("reason", "prior_batch_incomplete_or_deadline"));
            } else {
                JSONObject force = batch("battery_force_" + i, new Operation[]{() -> callBattery(0)});
                forced.put(force);
                if (!force.optBoolean("complete")) concurrent = force;
            }
        }
        result.put("forcedSequential", forced).put("fieldTimestampAfter", target.batteryTimestamp.getLong(null));
        finish(result, before);
        result.put("interpretation", "Calls are real hooked g(J); forceCalls/RPC deltas are reported, not inferred from equal timestamps. Background callers can contribute to deltas.");
        return result;
    }

    JSONObject monitor() throws Exception {
        deadline = System.nanoTime() + LIMIT_NANOS;
        JSONObject result = header("monitor");
        JSONObject before = stats.snapshot();
        // Two concurrent full->min pairs: every full remains a real full call. Each min follows
        // its full without a new TTL and rechecks Scene's original request-start-based 500ms cache.
        Operation pair = () -> {
            JSONObject item = new JSONObject();
            item.put("full", callMonitor(target.full, "full"));
            if (remaining() <= 0) return item.put("minSkipped", "deadline");
            item.put("min", callMonitor(target.min, "min"));
            return item;
        };
        result.put("pairs", batch("monitor_full_then_min", new Operation[]{pair, pair}));
        finish(result, before);
        result.put("interpretation", "Two real full calls are retained; min may reuse only the existing valid full cache. Slow full responses can legitimately expire its original 500ms lifetime.");
        return result;
    }

    private JSONObject callBattery(long maxAge) throws Throwable {
        long started = System.nanoTime();
        long before = target.batteryTimestamp.getLong(null);
        JSONObject countersBefore = maxAge == 0 ? stats.snapshot() : null;
        Object returned = invoke(target.batteryRefresh, maxAge);
        JSONObject result = new JSONObject().put("maxAgeMillis", maxAge).put("returnedTimestamp", returned)
                .put("fieldBefore", before).put("fieldAfter", target.batteryTimestamp.getLong(null))
                .put("durationNanos", System.nanoTime() - started);
        if (countersBefore != null) result.put("counterDelta", delta(countersBefore, stats.snapshot()));
        return result;
    }

    private JSONObject callMonitor(Method method, String kind) throws Throwable {
        long started = System.nanoTime();
        Object returned = invoke(method);
        long cachedTimestamp = target.fullTimestamp.getLong(null);
        Object cached = target.fullResult.get(null);
        return new JSONObject().put("kind", kind).put("returnedNull", returned == null)
                .put("returnedType", returned == null ? JSONObject.NULL : returned.getClass().getName())
                .put("returnedIdentity", returned == null ? 0 : System.identityHashCode(returned))
                .put("cachedIdentity", cached == null ? 0 : System.identityHashCode(cached))
                .put("isCachedFullObject", returned != null && returned == cached)
                .put("fullTimestamp", cachedTimestamp)
                .put("fullAgeMillis", android.os.SystemClock.elapsedRealtime() - cachedTimestamp)
                .put("durationNanos", System.nanoTime() - started);
    }

    private static Object invoke(Method method, Object... arguments) throws Throwable {
        try { return method.invoke(null, arguments); }
        catch (InvocationTargetException wrapper) {
            Throwable cause = wrapper.getCause();
            throw cause == null ? wrapper : cause;
        }
    }

    private JSONObject batch(String label, Operation[] operations) throws Exception {
        CountDownLatch ready = new CountDownLatch(operations.length);
        CountDownLatch start = new CountDownLatch(1);
        CountDownLatch done = new CountDownLatch(operations.length);
        AtomicReferenceArray<JSONObject> answers = new AtomicReferenceArray<>(operations.length);
        Thread[] workers = new Thread[operations.length];
        long began = System.nanoTime();
        try {
            for (int i = 0; i < operations.length; i++) {
                final int index = i;
                boolean reserved = false;
                try {
                    workers[i] = new Thread(() -> {
                        try {
                            ready.countDown();
                            if (!start.await(Math.max(0, remaining()), TimeUnit.NANOSECONDS))
                                throw new IllegalStateException("start_deadline");
                            if (remaining() <= 0) throw new IllegalStateException("operation_deadline");
                            answers.set(index, new JSONObject().put("ok", true).put("result", operations[index].run()));
                        } catch (Throwable error) {
                            answers.set(index, failure(error));
                        } finally {
                            active.decrementAndGet();
                            done.countDown();
                        }
                    }, "SSO-Probe-" + label + "-" + i);
                    workers[i].setDaemon(true);
                    synchronized (this) { threads.add(workers[i]); }
                    active.incrementAndGet();
                    reserved = true;
                    workers[i].start();
                    // After successful start, only this worker releases its active reservation.
                } catch (Throwable startFailure) {
                    if (reserved) active.decrementAndGet();
                    ready.countDown(); done.countDown();
                    answers.set(index, failure(startFailure));
                }
            }
            ready.await(Math.max(0, remaining()), TimeUnit.NANOSECONDS);
        }
        finally { start.countDown(); }
        boolean complete = done.await(Math.max(0, remaining()), TimeUnit.NANOSECONDS);
        // At most one bounded join per finite worker; no thread-status polling.
        for (Thread worker : workers) {
            long left = remaining();
            if (left > 0 && worker != null && worker.isAlive()) {
                long millis = left / 1_000_000L;
                int nanos = (int) (left % 1_000_000L);
                worker.join(millis, nanos);
            }
        }
        JSONArray items = new JSONArray();
        for (int i = 0; i < operations.length; i++) {
            JSONObject item = answers.get(i);
            items.put(item == null ? new JSONObject().put("running", workers[i] != null && workers[i].isAlive())
                    .put("worker", workers[i] == null ? JSONObject.NULL : workers[i].getName()) : item);
        }
        return new JSONObject().put("label", label).put("complete", complete)
                .put("results", items).put("durationNanos", System.nanoTime() - began);
    }

    private JSONObject header(String kind) throws Exception {
        return new JSONObject().put("ok", true).put("probe", kind)
                .put("controlledRealCalls", true).put("naturalLoadEnergyEvidence", false)
                .put("deadlineMillis", TimeUnit.NANOSECONDS.toMillis(LIMIT_NANOS))
                .put("directTargetWrites", false).put("callsMayRefreshExistingCaches", true)
                .put("writesTargetDatabaseOrConfig", false);
    }

    private void finish(JSONObject result, JSONObject before) throws Exception {
        JSONObject after = stats.snapshot();
        result.put("before", before).put("after", after)
                .put("counterDelta", delta(before, after)).put("deadlineReached", remaining() <= 0)
                .put("liveThreadsAtResponse", liveThreads());
    }

    private static JSONObject delta(JSONObject before, JSONObject after) throws Exception {
        JSONObject result = new JSONObject();
        for (String group : new String[]{"battery", "monitor", "rpc"}) {
            JSONObject a = before.optJSONObject(group), b = after.optJSONObject(group);
            if (a == null || b == null) continue;
            JSONObject values = new JSONObject();
            java.util.Iterator<String> keys = b.keys();
            while (keys.hasNext()) {
                String key = keys.next();
                Object oldValue = a.opt(key), newValue = b.opt(key);
                if (oldValue instanceof Number && newValue instanceof Number)
                    values.put(key, ((Number) newValue).longValue() - ((Number) oldValue).longValue());
            }
            result.put(group, values);
        }
        return result;
    }

    private long remaining() { return deadline - System.nanoTime(); }

    private static JSONObject failure(Throwable error) {
        JSONObject value = new JSONObject();
        try { value.put("ok", false).put("error", error.getClass().getSimpleName() + ":" + error.getMessage()); }
        catch (Exception ignored) { }
        return value;
    }
}
