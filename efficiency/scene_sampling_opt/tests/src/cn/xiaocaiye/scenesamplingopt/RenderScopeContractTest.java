package cn.xiaocaiye.scenesamplingopt;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

/** Exercises the actual render ownership policy without Android or Binder fakes. */
public final class RenderScopeContractTest {
    private static int assertions;
    private static final List<String> passed = new ArrayList<>();
    private static final List<String> failed = new ArrayList<>();
    @FunctionalInterface interface Checked { void run() throws Throwable; }

    static void check(boolean condition, String message) {
        assertions++;
        if (!condition) throw new AssertionError(message);
    }
    static void spec(String name, Checked test) {
        try { test.run(); passed.add(name); }
        catch (Throwable failure) { failed.add(name + ": " + failure); }
    }
    static void await(CountDownLatch latch) throws InterruptedException {
        if (!latch.await(3, TimeUnit.SECONDS)) throw new AssertionError("thread coordination timed out");
    }

    public static void main(String[] args) {
        spec("outside a render has no current ownership", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            check(scopes.current() == null && scopes.active() == 0, "no global memory cache");
        });
        spec("only the owned manager and first buffer identify the first read", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), first = new Object();
            var frame = scopes.enter(manager, first);
            try {
                check(frame.query(new Object(), first) == RenderScope.Query.PASS, "other manager passes");
                check(frame.query(manager, new Object()) == RenderScope.Query.PASS, "other initial buffer passes");
                check(frame.query(manager, null) == RenderScope.Query.PASS, "null buffer passes");
                check(frame.query(manager, first) == RenderScope.Query.READ_FIRST, "owned first query reads");
                check(frame.firstBuffer() == first, "identity retained");
            } finally { scopes.exit(frame); }
        });
        spec("no data is reused before the first call completes", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), first = new Object();
            var frame = scopes.enter(manager, first);
            try {
                check(frame.query(manager, first) == RenderScope.Query.READ_FIRST, "first call reads");
                check(frame.query(manager, new Object()) == RenderScope.Query.PASS, "nested call during read passes");
                frame.firstSucceeded();
                check(frame.query(manager, new Object()) == RenderScope.Query.REUSE_FIRST, "completion allows the second query");
            } finally { scopes.exit(frame); }
        });
        spec("successful first read permits exactly one later buffer reuse", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), first = new Object();
            var frame = scopes.enter(manager, first);
            try {
                check(frame.query(manager, first) == RenderScope.Query.READ_FIRST, "first call reads");
                frame.firstSucceeded();
                check(frame.query(manager, new Object()) == RenderScope.Query.REUSE_FIRST, "second query reuses");
                check(frame.query(manager, new Object()) == RenderScope.Query.PASS, "third query not cached");
            } finally { scopes.exit(frame); }
        });
        spec("first read failure disables reuse for the current render", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), first = new Object();
            var frame = scopes.enter(manager, first);
            try {
                check(frame.query(manager, first) == RenderScope.Query.READ_FIRST, "first call reads");
                frame.firstFailed();
                check(frame.query(manager, new Object()) == RenderScope.Query.PASS, "failure is not a successful cache");
                frame.firstSucceeded();
                check(frame.query(manager, new Object()) == RenderScope.Query.PASS, "late success cannot revive failure");
            } finally { scopes.exit(frame); }
        });
        spec("different manager is never served another manager's data", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), first = new Object();
            var frame = scopes.enter(manager, first);
            try {
                frame.query(manager, first); frame.firstSucceeded();
                check(frame.query(new Object(), new Object()) == RenderScope.Query.PASS, "foreign manager passes");
                check(frame.query(manager, new Object()) == RenderScope.Query.REUSE_FIRST, "original manager still owns its second query");
            } finally { scopes.exit(frame); }
        });
        spec("unexpected second query to the first buffer prevents later reuse", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), first = new Object();
            var frame = scopes.enter(manager, first);
            try {
                frame.query(manager, first); frame.firstSucceeded();
                check(frame.query(manager, first) == RenderScope.Query.PASS, "re-reading owned object is an original query");
                check(frame.query(manager, new Object()) == RenderScope.Query.PASS, "unexpected sequence is not generalized into caching");
            } finally { scopes.exit(frame); }
        });
        spec("invalid entry cannot create a scope", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            check(scopes.enter(null, new Object()) == null, "null manager rejected");
            check(scopes.enter(new Object(), null) == null, "null buffer rejected");
            check(scopes.active() == 0 && scopes.current() == null, "no leaked scope");
        });
        spec("next render always performs a new first read", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), first = new Object();
            var frame1 = scopes.enter(manager, first);
            frame1.query(manager, first); frame1.firstSucceeded();
            check(frame1.query(manager, new Object()) == RenderScope.Query.REUSE_FIRST, "first render reuses once");
            scopes.exit(frame1);
            var frame2 = scopes.enter(manager, first);
            try {
                check(frame2.query(manager, first) == RenderScope.Query.READ_FIRST, "new render must read again");
                check(scopes.active() == 1, "only current render counted");
            } finally { scopes.exit(frame2); }
            check(scopes.current() == null && scopes.active() == 0, "all current ownership removed");
        });
        spec("nested render restores outer ownership", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager1 = new Object(), buffer1 = new Object(), manager2 = new Object(), buffer2 = new Object();
            var outer = scopes.enter(manager1, buffer1);
            try {
                outer.query(manager1, buffer1); outer.firstSucceeded();
                var inner = scopes.enter(manager2, buffer2);
                try {
                    check(scopes.current() == inner && scopes.active() == 2, "inner is current");
                    check(inner.query(manager1, new Object()) == RenderScope.Query.PASS, "inner cannot reuse outer data");
                } finally { scopes.exit(inner); }
                check(scopes.current() == outer && scopes.active() == 1, "outer restored");
                check(outer.query(manager1, new Object()) == RenderScope.Query.REUSE_FIRST, "outer completion retained");
            } finally { scopes.exit(outer); }
            check(scopes.current() == null, "no outer leak");
        });
        spec("finally after render exception removes TLS ownership", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            var frame = scopes.enter(new Object(), new Object());
            try { throw new IllegalStateException("fixture rendering failed"); }
            catch (IllegalStateException expected) { check(scopes.current() == frame, "active until finally"); }
            finally { scopes.exit(frame); }
            check(scopes.current() == null && scopes.active() == 0, "TLS and active counter cleared");
            check(frame.query(new Object(), new Object()) == RenderScope.Query.PASS, "closed scope cannot serve data");
        });
        spec("out-of-order defensive exit does not restore a closed ancestor", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            var outer = scopes.enter(new Object(), new Object());
            var inner = scopes.enter(new Object(), new Object());
            scopes.exit(outer);
            check(scopes.current() == inner && scopes.active() == 1, "inner survives outer defensive exit");
            scopes.exit(inner);
            check(scopes.current() == null && scopes.active() == 0, "closed ancestor is not put back into TLS");
        });
        spec("multiple closed ancestors are skipped on nested completion", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            var outer = scopes.enter(new Object(), new Object());
            var middle = scopes.enter(new Object(), new Object());
            var inner = scopes.enter(new Object(), new Object());
            scopes.exit(middle); scopes.exit(outer);
            check(scopes.current() == inner && scopes.active() == 1, "only final live frame remains");
            scopes.exit(inner);
            check(scopes.current() == null && scopes.active() == 0, "all closed ancestors skipped");
        });
        spec("exit is idempotent", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            var frame = scopes.enter(new Object(), new Object());
            scopes.exit(frame); scopes.exit(frame); scopes.exit(null);
            check(scopes.active() == 0 && scopes.current() == null, "no negative active count");
        });
        spec("thread-local ownership cannot be observed on another thread", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), buffer = new Object();
            var owner = scopes.enter(manager, buffer);
            ExecutorService worker = Executors.newSingleThreadExecutor();
            try {
                Future<Boolean> other = worker.submit(() -> {
                    if (scopes.current() != null) return false;
                    var own = scopes.enter(manager, buffer);
                    try { return own.query(manager, buffer) == RenderScope.Query.READ_FIRST; }
                    finally { scopes.exit(own); }
                });
                check(other.get(3, TimeUnit.SECONDS), "other thread starts with fresh independent ownership");
                check(scopes.current() == owner && scopes.active() == 1, "original owner not altered");
            } finally { worker.shutdownNow(); scopes.exit(owner); }
        });
        spec("quiesce rejects new scopes but permits entered render to finish", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            Object manager = new Object(), buffer = new Object();
            var owner = scopes.enter(manager, buffer);
            check(!scopes.tryQuiesce(), "active frame refuses immediate shutdown");
            check(scopes.enter(manager, buffer) == null, "new scope rejected while draining");
            check(owner.query(manager, buffer) == RenderScope.Query.READ_FIRST, "existing frame retains ownership");
            owner.firstSucceeded();
            check(owner.query(manager, new Object()) == RenderScope.Query.REUSE_FIRST, "already entered frame can finish");
            scopes.exit(owner);
            check(scopes.tryQuiesce(), "drained frame permits shutdown");
            check(scopes.active() == 0 && scopes.current() == null, "no retained old render");
            scopes.resume();
            var next = scopes.enter(manager, buffer);
            try { check(next != null, "resume allows a new render"); }
            finally { scopes.exit(next); }
        });
        spec("two real concurrent scopes drain before quiesce succeeds", () -> {
            RenderScope<Object,Object> scopes = new RenderScope<>();
            CountDownLatch entered = new CountDownLatch(2), release = new CountDownLatch(1);
            ExecutorService workers = Executors.newFixedThreadPool(2);
            try {
                List<Future<Boolean>> jobs = new ArrayList<>();
                for (int i = 0; i < 2; i++) jobs.add(workers.submit(() -> {
                    var frame = scopes.enter(new Object(), new Object());
                    entered.countDown();
                    try { await(release); return scopes.current() == frame; }
                    finally { scopes.exit(frame); }
                }));
                await(entered);
                check(scopes.active() == 2 && !scopes.tryQuiesce(), "both active render frames are tracked");
                release.countDown();
                for (Future<Boolean> job : jobs) check(job.get(3, TimeUnit.SECONDS), "each thread retains its own frame");
                check(scopes.active() == 0 && scopes.tryQuiesce(), "both must exit before quiesce succeeds");
            } finally { release.countDown(); workers.shutdownNow(); }
        });
        System.out.println("RENDER_CONTRACT_RESULT assertions=" + assertions + " passed=" + passed.size() + " failed=" + failed.size());
        for (String name : passed) System.out.println("PASS " + name);
        for (String name : failed) System.err.println("FAIL " + name);
        if (!failed.isEmpty()) System.exit(1);
    }
}
