package cn.xiaocaiye.scenesamplingopt;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Parcel;
import io.github.libxposed.api.XposedInterface.HookHandle;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONObject;

/** Reuses only the pinned bi0.run's two consecutive memory reads, never a timed cache. */
public final class MemoryDeduplicator implements AutoCloseable {
    private final XposedModule module;
    private final ClassLoader loader;
    private final RenderScope<ActivityManager, ActivityManager.MemoryInfo> scopes = new RenderScope<>();
    private final List<HookHandle> handles = new ArrayList<>();
    private final ThreadLocal<Boolean> fixture = new ThreadLocal<>();
    private final AtomicLong renders = new AtomicLong();
    private final AtomicLong firstReads = new AtomicLong();
    private final AtomicLong reused = new AtomicLong();
    private final AtomicLong fixtureReused = new AtomicLong();
    private final AtomicLong copyFailures = new AtomicLong();
    private final AtomicLong contextFailures = new AtomicLong();
    private volatile boolean enabled = true;
    private boolean installed;
    private boolean renderDeoptimized;
    private boolean platformSupported;
    private String platform = "unknown";
    private String reason = "not_loaded";
    private Field owner;
    private Field manager;
    private Field firstBuffer;

    public MemoryDeduplicator(XposedModule module, ClassLoader loader) {
        this.module = module;
        this.loader = loader;
    }

    public synchronized void install() throws ReflectiveOperationException {
        if (installed) return;
        try {
            Method get = Class.forName("android.os.SystemProperties")
                    .getDeclaredMethod("get", String.class, String.class);
            platform = String.valueOf(module.getInvoker(get).invoke(null, "ro.board.platform", ""))
                    .trim().toLowerCase(java.util.Locale.ROOT);
            platformSupported = platform.matches("mt[0-9]{2,}");
        } catch (Throwable unavailable) {
            platform = "unavailable";
            platformSupported = false;
        }
        if (!platformSupported) {
            // fi0.t -> gh0 variant 2 -> jz.I() selects this branch only for MediaTek.
            // Qualcomm uses FPS here and never performs the duplicate memory reads.
            reason = "inactive_scene_memory_branch";
            return;
        }
        Class<?> window = Class.forName("a.fi0", false, loader);
        Class<?> render = Class.forName("a.bi0", false, loader);
        owner = render.getDeclaredField("c");
        manager = window.getDeclaredField("j");
        firstBuffer = window.getDeclaredField("k");
        if (owner.getType() != window || manager.getType() != ActivityManager.class
                || firstBuffer.getType() != ActivityManager.MemoryInfo.class) {
            throw new NoSuchFieldException("Pinned Scene render/memory ownership differs");
        }
        owner.setAccessible(true);
        manager.setAccessible(true);
        firstBuffer.setAccessible(true);
        Method run = render.getDeclaredMethod("run");
        if (run.getReturnType() != void.class) throw new NoSuchMethodException("bi0.run return type");
        Method read = ActivityManager.class.getDeclaredMethod("getMemoryInfo", ActivityManager.MemoryInfo.class);
        try {
            handles.add(module.hook(read).setId("scene.memory.query").intercept(chain -> {
                RenderScope.Frame<ActivityManager, ActivityManager.MemoryInfo> frame = scopes.current();
                if (frame == null || !(chain.getThisObject() instanceof ActivityManager)
                        || !(chain.getArg(0) instanceof ActivityManager.MemoryInfo)) return chain.proceed();
                ActivityManager.MemoryInfo buffer = (ActivityManager.MemoryInfo) chain.getArg(0);
                RenderScope.Query decision = frame.query((ActivityManager) chain.getThisObject(), buffer);
                if (decision == RenderScope.Query.READ_FIRST) {
                    try {
                        Object result = chain.proceed();
                        frame.firstSucceeded();
                        firstReads.incrementAndGet();
                        return result;
                    } catch (Throwable originalFailure) {
                        frame.firstFailed();
                        throw originalFailure;
                    }
                }
                if (decision == RenderScope.Query.REUSE_FIRST) {
                    try {
                        copy(frame.firstBuffer(), buffer);
                        if (Boolean.TRUE.equals(fixture.get())) fixtureReused.incrementAndGet();
                        else reused.incrementAndGet();
                        return null;
                    } catch (RuntimeException | LinkageError copyFailure) {
                        copyFailures.incrementAndGet();
                        // The second original read remains the exact failure fallback.
                    }
                }
                return chain.proceed();
            }));
            handles.add(module.hook(run).setId("scene.memory.render").intercept(chain -> {
                if (!enabled) return chain.proceed();
                RenderScope.Frame<ActivityManager, ActivityManager.MemoryInfo> frame;
                try {
                    Object windowObject = owner.get(chain.getThisObject());
                    ActivityManager currentManager = (ActivityManager) manager.get(windowObject);
                    ActivityManager.MemoryInfo currentBuffer = (ActivityManager.MemoryInfo) firstBuffer.get(windowObject);
                    frame = scopes.enter(currentManager, currentBuffer);
                } catch (ReflectiveOperationException | ClassCastException | NullPointerException failure) {
                    contextFailures.incrementAndGet();
                    return chain.proceed();
                }
                if (frame == null) return chain.proceed();
                renders.incrementAndGet();
                try { return chain.proceed(); }
                finally { scopes.exit(frame); }
            }));
            renderDeoptimized = module.deoptimize(run);
            installed = true;
            reason = "ready";
        } catch (Throwable failure) {
            close();
            throw failure;
        }
    }

    private static void copy(ActivityManager.MemoryInfo from, ActivityManager.MemoryInfo to) {
        // Framework parcel methods retain all fields, including ROM additions absent from SDK stubs.
        Parcel parcel = Parcel.obtain();
        try {
            from.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            to.readFromParcel(parcel);
        } finally { parcel.recycle(); }
    }

    private static byte[] bytes(ActivityManager.MemoryInfo value) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            return parcel.marshall();
        } finally { parcel.recycle(); }
    }

    public void setEnabled(boolean value) { enabled = value; }
    public boolean tryQuiesce() { return scopes.tryQuiesce(); }
    public void resume() { scopes.resume(); }

    public JSONObject snapshot() {
        JSONObject result = new JSONObject();
        try {
            result.put("installed", installed);
            result.put("render_deoptimized", renderDeoptimized);
            result.put("platform", platform);
            result.put("platform_supported", platformSupported);
            result.put("reason", reason);
            result.put("enabled", enabled && installed);
            result.put("requested_enabled", enabled);
            result.put("renders", renders.get());
            result.put("first_reads", firstReads.get());
            result.put("binder_reads_avoided", reused.get());
            result.put("fixture_binder_reads_avoided", fixtureReused.get());
            result.put("copy_failures", copyFailures.get());
            result.put("context_failures", contextFailures.get());
            result.put("active", scopes.active());
        } catch (org.json.JSONException impossible) { throw new IllegalStateException(impossible); }
        return result;
    }

    /** Explicit diagnostic only. Fixture counters remain separate from real bi0.run observations. */
    public synchronized JSONObject selfTest(Context context) throws Exception {
        if (!platformSupported) return new JSONObject().put("skipped", true)
                .put("reason", reason).put("platform", platform).put("target_queries", 0);
        if (!installed) throw new IllegalStateException("Memory hooks not installed");
        ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        long before = fixtureReused.get();
        int passed = 0;
        fixture.set(Boolean.TRUE);
        try {
            for (int round = 0; round < 3; round++) {
                ActivityManager.MemoryInfo a = new ActivityManager.MemoryInfo();
                ActivityManager.MemoryInfo b = new ActivityManager.MemoryInfo();
                RenderScope.Frame<ActivityManager, ActivityManager.MemoryInfo> frame = scopes.enter(am, a);
                if (frame == null) throw new IllegalStateException("Scope quiesced");
                try {
                    am.getMemoryInfo(a);
                    am.getMemoryInfo(b);
                    if (a.totalMem <= 0 || !Arrays.equals(bytes(a), bytes(b))) {
                        throw new AssertionError("Same-render parcel values differ");
                    }
                    passed++;
                    long reusedBeforeThird = fixtureReused.get();
                    am.getMemoryInfo(new ActivityManager.MemoryInfo());
                    if (fixtureReused.get() != reusedBeforeThird) throw new AssertionError("Third read cached");
                    passed++;
                } finally { scopes.exit(frame); }
                if (scopes.current() != null) throw new AssertionError("Scope leaked");
                long beforeOutside = fixtureReused.get();
                am.getMemoryInfo(new ActivityManager.MemoryInfo());
                if (fixtureReused.get() != beforeOutside) throw new AssertionError("Outside read cached");
                passed++;
            }
        } finally { fixture.remove(); }
        if (fixtureReused.get() - before != 3) throw new AssertionError("Expected one actual reuse per round");
        JSONObject result = new JSONObject();
        result.put("passed", passed + 1);
        result.put("actual_framework_hook_reuses", fixtureReused.get() - before);
        result.put("scope", "explicit fixture, not real UI verification");
        return result;
    }

    @Override public synchronized void close() {
        scopes.tryQuiesce();
        for (int i = handles.size() - 1; i >= 0; i--) handles.get(i).unhook();
        handles.clear();
        installed = false;
    }
}
