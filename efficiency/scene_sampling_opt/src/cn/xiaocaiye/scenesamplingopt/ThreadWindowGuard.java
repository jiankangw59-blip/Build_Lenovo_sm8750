package cn.xiaocaiye.scenesamplingopt;

import android.os.Handler;
import android.os.Looper;
import android.view.View;
import io.github.libxposed.api.XposedInterface.HookHandle;
import io.github.libxposed.api.XposedModule;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONArray;
import org.json.JSONObject;

/** Pinned qi0 lifecycle fix. Observe-only until explicitly enabled; no periodic work. */
public final class ThreadWindowGuard implements AutoCloseable {
    private final XposedModule module;
    private final ClassLoader loader;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Object lifecycle = new Object();
    private final List<HookHandle> handles = new ArrayList<>();
    private final List<String> deoptimized = new ArrayList<>();
    private final ThreadLocal<Integer> closing = new ThreadLocal<>();
    private final AtomicLong closes = new AtomicLong();
    private final AtomicLong starts = new AtomicLong();
    private final AtomicLong blockedClosing = new AtomicLong();
    private final AtomicLong blockedStart = new AtomicLong();
    private final AtomicLong samples = new AtomicLong();
    private final AtomicLong resumed = new AtomicLong();
    private final AtomicLong blockedSample = new AtomicLong();
    private final AtomicLong threadRpc = new AtomicLong();
    private final AtomicLong detailRpc = new AtomicLong();
    private final AtomicLong cleanupAttempts = new AtomicLong();
    private final AtomicLong cancelledOrphans = new AtomicLong();
    private final AtomicLong visibleKept = new AtomicLong();
    private final AtomicLong attachDeferred = new AtomicLong();
    private final AtomicLong attachStarts = new AtomicLong();
    private final AtomicLong attachDiscarded = new AtomicLong();
    private final AtomicLong errors = new AtomicLong();
    private volatile boolean enabled;
    private volatile boolean installed;
    private volatile boolean closed;
    private boolean quiescing;
    private int active;
    private boolean cleanupPending;
    private PendingAttach pendingAttach;
    private volatile String reason = "not_installed";
    private volatile String lastCleanup = "not_requested";
    private volatile String lastAttach = "not_requested";
    private volatile WeakReference<Object> lastOwner = new WeakReference<>(null);
    private Class<?> windowType;
    private Class<?> continuationType;
    private Field shownView;
    private Field currentJob;
    private Field ownerView;
    private Field ownerPeriod;
    private Field ownerDetail;
    private Field continuationLabel;
    private Method cancelJob;
    private Method jobActive;
    private Method startTask;
    private Object unit;
    private final Runnable cleanup = this::cleanupOnMain;

    public ThreadWindowGuard(XposedModule module, ClassLoader loader) {
        this.module = module;
        this.loader = loader;
    }

    /** Resolve every pinned ABI first; unknown versions fail without modifying target state. */
    public void install() throws ReflectiveOperationException {
        if (installed || closed) return;
        windowType = Class.forName("a.qi0", false, loader);
        continuationType = Class.forName("a.pi0", false, loader);
        Class<?> hz = Class.forName("a.hz", false, loader);
        Class<?> job = Class.forName("a.hl1", false, loader);
        Class<?> jobInterface = Class.forName("a.cw0", false, loader);
        Class<?> n30 = Class.forName("a.n30", false, loader);
        Class<?> unitType = Class.forName("a.jr1", false, loader);
        if (!jobInterface.isAssignableFrom(job)) throw new NoSuchFieldException("qi0 job ABI");
        shownView = field(windowType, "y", View.class, true);
        currentJob = field(windowType, "x", job, true);
        ownerView = field(windowType, "c", View.class, false);
        ownerPeriod = field(windowType, "o", long.class, false);
        ownerDetail = field(windowType, "l", boolean.class, false);
        continuationLabel = field(continuationType, "i", int.class, false);
        Field unitField = field(unitType, "a", unitType, true);
        unit = unitField.get(null);
        if (unit == null) throw new NoSuchFieldException("null Kotlin Unit");
        cancelJob = method(n30, "k", void.class, true, jobInterface);
        jobActive = method(jobInterface, "a", boolean.class, false);
        Method close = method(windowType, "b", void.class, false);
        Method start = method(windowType, "e", void.class, false);
        startTask = start;
        Method sample = method(windowType, "a", Object.class, true, windowType, hz);
        Method rpc = method(Class.forName("a.c30", false, loader), "M", String.class,
                true, String.class, String.class, Long.class);
        List<Method> callers = new ArrayList<>();
        callers.add(close);
        callers.add(start);
        callers.add(sample);
        callers.add(method(windowType, "c", void.class, false, String.class, int.class));
        callers.add(method(windowType, "d", void.class, false));
        callers.add(method(Class.forName("a.oi0", false, loader), "e", Object.class, false, Object.class));
        callers.add(method(continuationType, "e", Object.class, false, Object.class));
        callers.add(method(Class.forName("a.ni0", false, loader), "onClick", void.class, false, View.class));
        callers.add(method(Class.forName("a.tp", false, loader), "run", void.class, false));
        callers.add(method(Class.forName("a.xo1", false, loader), "a", java.io.Serializable.class,
                false, int.class, hz));
        callers.add(method(Class.forName("a.c30", false, loader), "s", String.class,
                true, long.class, String.class, String.class));
        callers.add(rpc);
        try {
            handles.add(module.hook(close).setId("scene.thread_window.close").intercept(chain -> {
                if (!enter()) return chain.proceed();
                closes.incrementAndGet();
                boolean scoped = enabled;
                Integer previous = null;
                if (scoped) {
                    previous = closing.get();
                    closing.set(previous == null ? 1 : previous + 1);
                }
                try {
                    if (scoped) clearPendingAttach();
                    return chain.proceed();
                }
                finally {
                    if (scoped) {
                        if (previous == null) closing.remove(); else closing.set(previous);
                    }
                    leave();
                }
            }));
            handles.add(module.hook(start).setId("scene.thread_window.start").intercept(chain -> {
                if (!enter()) return chain.proceed();
                try {
                    starts.incrementAndGet();
                    Object owner = chain.getThisObject();
                    lastOwner = new WeakReference<>(owner);
                    if (enabled) {
                        Integer depth = closing.get();
                        if (depth != null && depth > 0) {
                            blockedClosing.incrementAndGet();
                            return null;
                        }
                        Boolean valid = currentAttached(owner);
                        if (Boolean.FALSE.equals(valid)) {
                            // addView returns before the first attach on this ROM. Subscribe once.
                            if (deferUntilAttach(owner)) return null;
                            blockedStart.incrementAndGet();
                            return null;
                        }
                        if (Boolean.TRUE.equals(valid)) clearPendingAttach();
                    }
                    return chain.proceed();
                } finally { leave(); }
            }));
            handles.add(module.hook(sample).setId("scene.thread_window.sample").intercept(chain -> {
                if (!enter()) return chain.proceed();
                try {
                    samples.incrementAndGet();
                    Object owner = chain.getArg(0);
                    lastOwner = new WeakReference<>(owner);
                    // pi0.e resumes qi0.a with i|MIN_VALUE. Always consume original success/failure.
                    // Its resume branches do not issue a new RPC. Skipping them could swallow cancellation.
                    boolean isResume = isResume(chain.getArg(1));
                    if (isResume) resumed.incrementAndGet();
                    if (enabled && !isResume && Boolean.FALSE.equals(currentAttached(owner))) {
                        blockedSample.incrementAndGet();
                        return unit;
                    }
                    return chain.proceed();
                } finally { leave(); }
            }));
            handles.add(module.hook(rpc).setId("scene.thread_window.observe_rpc").intercept(chain -> {
                if (!enter()) return chain.proceed();
                try {
                    if ("get-threads".equals(chain.getArg(0))) threadRpc.incrementAndGet();
                    else if ("adb-monitor-t".equals(chain.getArg(0))) detailRpc.incrementAndGet();
                    return chain.proceed();
                } finally { leave(); }
            }));
            for (Method caller : callers) {
                if (!module.deoptimize(caller)) throw new IllegalStateException("deopt_failed:"
                        + caller.getDeclaringClass().getName() + "#" + caller.getName());
                deoptimized.add(caller.getDeclaringClass().getName() + "#" + caller.getName());
            }
            synchronized (lifecycle) { installed = true; quiescing = false; }
            reason = "ready";
        } catch (Throwable failure) {
            reason = "install_failed:" + failure.getClass().getSimpleName();
            close();
            throw failure;
        }
    }

    private boolean isResume(Object continuation) {
        try {
            return continuationType.isInstance(continuation)
                    && (continuationLabel.getInt(continuation) & Integer.MIN_VALUE) != 0;
        } catch (ReflectiveOperationException | RuntimeException failure) {
            errors.incrementAndGet();
            // Fail open as a resumed call: never discard an unknown continuation failure.
            return true;
        }
    }

    /** null means inspection failed; callers must preserve the original behavior. */
    private Boolean currentAttached(Object owner) {
        try {
            // Read-only guard: launch/close and all View mutation remain on the target main thread.
            // A coroutine may race detach; the original job cancellation bounds an already-started RPC.
            View view = (View) ownerView.get(owner);
            return view != null && shownView.get(null) == view && view.isAttachedToWindow();
        } catch (ReflectiveOperationException | RuntimeException failure) {
            errors.incrementAndGet();
            return null;
        }
    }

    public void setEnabled(boolean value) {
        boolean schedule = false;
        synchronized (lifecycle) {
            if (closed) return;
            // Disabling must not drop the legitimate start already deferred for a current window.
            if (!value && enabled && pendingAttach != null)
                throw new IllegalStateException("thread_window_attaching");
            boolean changed = enabled != value;
            enabled = value;
            if (value && changed && installed && !cleanupPending) {
                cleanupPending = true;
                schedule = true;
            }
        }
        if (!schedule) return;
        if (Looper.myLooper() == Looper.getMainLooper()) cleanupOnMain();
        else if (!main.post(cleanup)) {
            synchronized (lifecycle) { cleanupPending = false; }
            errors.incrementAndGet();
            lastCleanup = "main_post_failed";
        }
    }

    /** Exactly one event-triggered cleanup. Visibility check and cancellation share the main turn. */
    private void cleanupOnMain() {
        synchronized (lifecycle) {
            cleanupPending = false;
            if (closed || !installed || !enabled || quiescing) return;
            active++;
        }
        cleanupAttempts.incrementAndGet();
        try {
            if (Looper.myLooper() != Looper.getMainLooper()) throw new IllegalStateException("cleanup_not_main");
            View shown = (View) shownView.get(null);
            if (shown != null && shown.isAttachedToWindow()) {
                visibleKept.incrementAndGet();
                lastCleanup = "kept_attached_window";
                return;
            }
            boolean waitingAttach;
            synchronized (lifecycle) {
                waitingAttach = shown != null && pendingAttach != null && pendingAttach.view == shown;
            }
            if (shown != null && (waitingAttach || shown.getParent() != null)) {
                visibleKept.incrementAndGet();
                lastCleanup = "kept_pending_window";
                return;
            }
            Object stale = currentJob.get(null);
            if (stale == null) { lastCleanup = "no_job"; return; }
            module.getInvoker(cancelJob).invoke(null, stale);
            // No message-loop turn was yielded. Still guard the slot against synchronous replacement.
            if (currentJob.get(null) == stale) currentJob.set(null, null);
            cancelledOrphans.incrementAndGet();
            lastCleanup = "cancelled_detached_window_job";
        } catch (Throwable failure) {
            errors.incrementAndGet();
            lastCleanup = "cleanup_failed:" + failure.getClass().getSimpleName();
        } finally { leave(); }
    }

    public JSONObject snapshot() {
        JSONObject result = new JSONObject();
        try {
            result.put("installed", installed).put("enabled", enabled).put("reason", reason)
                    .put("hookHandles", handles.size())
                    .put("closeCalls", closes.get()).put("startCalls", starts.get())
                    .put("blockedCloseRestarts", blockedClosing.get()).put("blockedDetachedStarts", blockedStart.get())
                    .put("sampleCalls", samples.get()).put("resumeCalls", resumed.get())
                    .put("blockedDetachedSamples", blockedSample.get()).put("getThreadsRpc", threadRpc.get())
                    .put("detailRpc", detailRpc.get()).put("cleanupAttempts", cleanupAttempts.get())
                    .put("cancelledOrphans", cancelledOrphans.get()).put("visibleJobsKept", visibleKept.get())
                    .put("attachDeferred", attachDeferred.get()).put("attachStarts", attachStarts.get())
                    .put("attachDiscarded", attachDiscarded.get()).put("lastAttach", lastAttach)
                    .put("lastCleanup", lastCleanup).put("errors", errors.get())
                    .put("deoptimized", new JSONArray(deoptimized));
            synchronized (lifecycle) {
                result.put("active", active).put("cleanupPending", cleanupPending).put("quiescing", quiescing);
                result.put("pendingAttach", pendingAttach != null);
            }
            if (installed && !closed) {
                View shown = (View) shownView.get(null);
                Object job = currentJob.get(null);
                result.put("stateReadOnMain", Looper.myLooper() == Looper.getMainLooper())
                        .put("windowPresent", shown != null)
                        .put("windowAttached", shown != null && shown.isAttachedToWindow())
                        .put("jobPresent", job != null)
                        .put("jobActive", job != null && Boolean.TRUE.equals(jobActive.invoke(job)));
                Object owner = lastOwner.get();
                result.put("ownerObserved", owner != null);
                if (owner != null) result.put("lastOwnerCurrentAttached", currentAttached(owner))
                        .put("lastOwnerPeriodMs", ownerPeriod.getLong(owner))
                        .put("lastOwnerDetail", ownerDetail.getBoolean(owner));
            }
        } catch (Exception failure) {
            errors.incrementAndGet();
            try { result.put("stateError", failure.getClass().getSimpleName()); }
            catch (org.json.JSONException ignored) { }
        }
        return result;
    }

    private boolean enter() {
        synchronized (lifecycle) {
            if (!installed || closed || quiescing) return false;
            active++;
            return true;
        }
    }

    private void leave() { synchronized (lifecycle) { active--; } }

    /** Caller must call resume() if any module component vetoes the hot reload. */
    public boolean tryQuiesce() {
        synchronized (lifecycle) {
            // A veto must not strand a pending one-shot cleanup between prepare/resume.
            if (active != 0 || cleanupPending || pendingAttach != null) return false;
            quiescing = true;
            return true;
        }
    }

    public void resume() { synchronized (lifecycle) { if (!closed) quiescing = false; } }

    @Override public void close() {
        synchronized (lifecycle) { closed = true; quiescing = true; installed = false; }
        clearPendingAttach();
        main.removeCallbacks(cleanup);
        synchronized (lifecycle) { cleanupPending = false; }
        for (int i = handles.size() - 1; i >= 0; i--) {
            try { handles.get(i).unhook(); } catch (Throwable ignored) { }
        }
        handles.clear();
        closing.remove();
        lastOwner = new WeakReference<>(null);
    }

    private final class PendingAttach implements View.OnAttachStateChangeListener {
        final Object owner;
        final View view;
        final Runnable arm = () -> handlePendingAttach(this);
        volatile boolean live = true;
        boolean registered;

        PendingAttach(Object owner, View view) { this.owner = owner; this.view = view; }

        @Override public void onViewAttachedToWindow(View attached) {
            if (attached == view) dispatchPendingAttach(this);
        }

        @Override public void onViewDetachedFromWindow(View detached) {
            if (detached == view) retireAttach(this, true);
        }
    }

    /** Only current y==owner.c can wait for first attachment; pre-addView d/e is still rejected. */
    private boolean deferUntilAttach(Object owner) {
        try {
            View view = (View) ownerView.get(owner);
            if (view == null || shownView.get(null) != view) return false;
            PendingAttach replaced;
            PendingAttach next;
            synchronized (lifecycle) {
                if (closed || !installed || !enabled || quiescing) return false;
                if (pendingAttach != null && pendingAttach.owner == owner && pendingAttach.view == view) return true;
                replaced = pendingAttach;
                if (replaced != null) replaced.live = false;
                next = new PendingAttach(owner, view);
                pendingAttach = next;
            }
            if (replaced != null) {
                attachDiscarded.incrementAndGet();
                disposeAttach(replaced);
            }
            attachDeferred.incrementAndGet();
            lastAttach = "waiting_for_current_window";
            dispatchPendingAttach(next);
            return true;
        } catch (ReflectiveOperationException | RuntimeException failure) {
            errors.incrementAndGet();
            return false;
        }
    }

    private void dispatchPendingAttach(PendingAttach pending) {
        if (Looper.myLooper() == Looper.getMainLooper()) handlePendingAttach(pending);
        else if (!main.post(pending.arm)) {
            errors.incrementAndGet();
            lastAttach = "main_post_failed";
            retireAttach(pending, true);
        }
    }

    /** Main-thread attach event, never a delayed check/retry. Runs the original start at most once. */
    private void handlePendingAttach(PendingAttach pending) {
        synchronized (lifecycle) {
            if (!pending.live || pendingAttach != pending || closed || !installed || !enabled || quiescing) return;
            active++;
        }
        try {
            if (Looper.myLooper() != Looper.getMainLooper()) throw new IllegalStateException("attach_not_main");
            Integer depth = closing.get();
            if ((depth != null && depth > 0) || shownView.get(null) != pending.view
                    || ownerView.get(pending.owner) != pending.view) {
                retireAttach(pending, true);
                return;
            }
            if (!pending.view.isAttachedToWindow()) {
                if (!pending.registered) {
                    pending.view.addOnAttachStateChangeListener(pending);
                    pending.registered = true;
                }
                return;
            }
            if (!retireAttach(pending, false)) return;
            // Same main turn: a close/owner change cannot run between validation and original start.
            lastOwner = new WeakReference<>(pending.owner);
            module.getInvoker(startTask).invoke(pending.owner);
            attachStarts.incrementAndGet();
            lastAttach = "started_after_attach";
        } catch (Throwable failure) {
            errors.incrementAndGet();
            lastAttach = "attach_failed:" + failure.getClass().getSimpleName();
            retireAttach(pending, true);
        } finally { leave(); }
    }

    private boolean retireAttach(PendingAttach pending, boolean discarded) {
        synchronized (lifecycle) {
            if (!pending.live || pendingAttach != pending) return false;
            pending.live = false;
            pendingAttach = null;
        }
        if (discarded) attachDiscarded.incrementAndGet();
        disposeAttach(pending);
        return true;
    }

    private void clearPendingAttach() {
        PendingAttach pending;
        synchronized (lifecycle) {
            pending = pendingAttach;
            if (pending == null) return;
            pending.live = false;
            pendingAttach = null;
        }
        attachDiscarded.incrementAndGet();
        disposeAttach(pending);
    }

    private void disposeAttach(PendingAttach pending) {
        main.removeCallbacks(pending.arm);
        Runnable remove = () -> {
            if (pending.registered) {
                try { pending.view.removeOnAttachStateChangeListener(pending); }
                catch (RuntimeException failure) {
                    errors.incrementAndGet();
                    lastAttach = "listener_remove_failed:" + failure.getClass().getSimpleName();
                } finally { pending.registered = false; }
            }
        };
        if (Looper.myLooper() == Looper.getMainLooper()) remove.run();
        else if (!main.post(remove)) {
            errors.incrementAndGet();
            // The listener was atomically disarmed first, so it can never launch a stale owner.
            lastAttach = "listener_remove_post_failed";
        }
    }

    private static Field field(Class<?> owner, String name, Class<?> type, boolean isStatic)
            throws ReflectiveOperationException {
        Field field = owner.getDeclaredField(name);
        if (field.getType() != type || Modifier.isStatic(field.getModifiers()) != isStatic)
            throw new NoSuchFieldException(owner.getName() + "." + name);
        field.setAccessible(true);
        return field;
    }

    private static Method method(Class<?> owner, String name, Class<?> returns,
                                 boolean isStatic, Class<?>... args) throws ReflectiveOperationException {
        Method method = owner.getDeclaredMethod(name, args);
        if (method.getReturnType() != returns || Modifier.isStatic(method.getModifiers()) != isStatic)
            throw new NoSuchMethodException(owner.getName() + "#" + name);
        method.setAccessible(true);
        return method;
    }
}
