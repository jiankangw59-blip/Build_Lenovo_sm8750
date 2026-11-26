package cn.xiaocaiye.scenesamplingopt;

import android.content.Context;
import android.view.View;
import io.github.libxposed.api.XposedInterface.HookHandle;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONObject;

/** Exact-APK quick-slider persistence repair; never starts display or scheduling work. */
public final class RefreshRatePersistence implements AutoCloseable {
    private final XposedModule module;
    private final ClassLoader loader;
    private final Object lifecycle = new Object();
    private final List<HookHandle> handles = new ArrayList<>();
    private final ThreadLocal<SaveScope> saving = new ThreadLocal<>();
    private final AtomicLong promotedConstructors = new AtomicLong();
    private final AtomicLong selectionCallbacks = new AtomicLong();
    private final AtomicLong writes = new AtomicLong();
    private final AtomicLong writesSucceeded = new AtomicLong();
    private final AtomicLong writesFailed = new AtomicLong();
    private final AtomicLong missingWrites = new AtomicLong();
    private final AtomicLong errors = new AtomicLong();
    private volatile boolean installed;
    private volatile boolean closed;
    private volatile String lastError = "";
    private boolean quiescing;
    private int active;
    private Field featureEnabled;
    private Field specialTarget;
    private Field saveAllowed;

    private static final class SaveScope {
        int writeCount;
    }

    public RefreshRatePersistence(XposedModule module, ClassLoader loader) {
        this.module = module;
        this.loader = loader;
    }

    /** ModuleEntry verifies the original Scene APK hash/version before installing this helper. */
    public void install() throws ReflectiveOperationException {
        if (installed || closed) return;
        Class<?> callback = Class.forName("a.cj0", false, loader);
        Class<?> panel = Class.forName("a.lj0", false, loader);
        Class<?> store = Class.forName("a.ed1", false, loader);
        Class<?> runnable = Class.forName("a.ri0", false, loader);
        Class<?> files = Class.forName("a.mg0", false, loader);
        if (!Modifier.isFinal(callback.getModifiers())) throw new NoSuchFieldException("cj0 final ABI");
        field(callback, "d", List.class);
        featureEnabled = field(callback, "e", boolean.class);
        specialTarget = field(callback, "f", boolean.class);
        saveAllowed = field(callback, "g", boolean.class);
        field(callback, "h", panel);
        field(callback, "j", String.class);
        field(panel, "k", store);
        Constructor<?> constructor = callback.getDeclaredConstructor(List.class, boolean.class,
                boolean.class, boolean.class, panel, Context.class, String.class, runnable);
        Method select = method(callback, "i", Object.class, false, Object.class);
        Method createPanel = method(panel, "e", View.class, false, Context.class);
        Method flush = method(store, "i", void.class, false);
        method(store, "j", void.class, false, String.class, Integer.class, Integer.class);
        Method write = method(files, "i", boolean.class, true, Context.class, String.class, byte[].class);
        try {
            handles.add(module.hook(constructor).setId("scene.refresh_persistence.constructor").intercept(chain -> {
                if (!enter()) return chain.proceed();
                try {
                    Object[] args = chain.getArgs().toArray();
                    if (args.length == 8 && Boolean.TRUE.equals(args[1])
                            && Boolean.FALSE.equals(args[2]) && Boolean.FALSE.equals(args[3])) {
                        // Change the constructor input, never a published final field.
                        args[3] = Boolean.TRUE;
                        Object result = chain.proceed(args);
                        promotedConstructors.incrementAndGet();
                        return result;
                    }
                    return chain.proceed();
                } finally { leave(); }
            }));
            handles.add(module.hook(select).setId("scene.refresh_persistence.selection").intercept(chain -> {
                if (!enter()) return chain.proceed();
                SaveScope previous = saving.get();
                SaveScope scope = null;
                try {
                    Object callbackObject = chain.getThisObject();
                    if (featureEnabled.getBoolean(callbackObject)
                            && !specialTarget.getBoolean(callbackObject)
                            && saveAllowed.getBoolean(callbackObject)) {
                        scope = new SaveScope();
                        selectionCallbacks.incrementAndGet();
                    }
                    // Null isolates an ineligible nested callback from its caller's scope.
                    if (scope == null) saving.remove(); else saving.set(scope);
                    return chain.proceed();
                } catch (Throwable error) {
                    failure("selection_exception", error);
                    throw error; // Preserve Scene's original failure; never retry a user edit.
                } finally {
                    if (scope != null && scope.writeCount == 0) {
                        missingWrites.incrementAndGet();
                        failure("selection_missing_write", null);
                    }
                    if (previous == null) saving.remove(); else saving.set(previous);
                    leave();
                }
            }));
            handles.add(module.hook(write).setId("scene.refresh_persistence.observe_write").intercept(chain -> {
                SaveScope scope = saving.get();
                if (scope == null || !"refresh_rate.conf".equals(chain.getArg(1))) return chain.proceed();
                scope.writeCount++;
                writes.incrementAndGet();
                try {
                    Object result = chain.proceed();
                    if (Boolean.TRUE.equals(result)) writesSucceeded.incrementAndGet();
                    else {
                        writesFailed.incrementAndGet();
                        failure("write_returned_false", null);
                    }
                    return result;
                } catch (Throwable error) {
                    writesFailed.incrementAndGet();
                    failure("write_exception", error);
                    throw error;
                }
            }));
            // The constructor caller and writer caller can otherwise retain ART inlined code.
            for (Method caller : new Method[]{createPanel, select, flush})
                if (!module.deoptimize(caller)) throw new IllegalStateException("refresh_deoptimize_failed");
            installed = true;
        } catch (Throwable error) {
            close();
            if (error instanceof ReflectiveOperationException) throw (ReflectiveOperationException) error;
            throw new IllegalStateException("refresh_install_failed", error);
        }
    }

    private static Field field(Class<?> owner, String name, Class<?> type) throws ReflectiveOperationException {
        Field field = owner.getDeclaredField(name);
        if (field.getType() != type || Modifier.isStatic(field.getModifiers())
                || !Modifier.isFinal(field.getModifiers())) throw new NoSuchFieldException(owner.getName() + "." + name);
        field.setAccessible(true);
        return field;
    }

    private static Method method(Class<?> owner, String name, Class<?> returns, boolean isStatic,
                                 Class<?>... parameters) throws ReflectiveOperationException {
        Method method = owner.getDeclaredMethod(name, parameters);
        if (method.getReturnType() != returns || Modifier.isStatic(method.getModifiers()) != isStatic)
            throw new NoSuchMethodException(owner.getName() + "." + name);
        method.setAccessible(true);
        return method;
    }

    private boolean enter() {
        synchronized (lifecycle) {
            if (closed || quiescing) return false;
            active++;
            return true;
        }
    }

    private void leave() { synchronized (lifecycle) { active--; } }

    public boolean tryQuiesce() {
        synchronized (lifecycle) {
            if (active != 0) return false;
            quiescing = true;
            return true;
        }
    }

    public void resume() { synchronized (lifecycle) { if (!closed) quiescing = false; } }

    public JSONObject snapshot() throws Exception {
        int inFlight;
        synchronized (lifecycle) { inFlight = active; }
        return new JSONObject().put("installed", installed).put("active", inFlight)
                .put("promotedConstructors", promotedConstructors.get())
                .put("selectionCallbacks", selectionCallbacks.get()).put("writes", writes.get())
                .put("writesSucceeded", writesSucceeded.get()).put("writesFailed", writesFailed.get())
                .put("missingWrites", missingWrites.get()).put("errors", errors.get())
                .put("lastError", lastError).put("periodicTasks", 0)
                .put("scope", "quick_slider_original_store_only").put("physicalDisplayVerified", false);
    }

    private void failure(String reason, Throwable error) {
        errors.incrementAndGet();
        lastError = reason + (error == null ? "" : ":" + error.getClass().getSimpleName());
        module.log(6, "SceneSamplingOpt", "event=refresh_persistence_error reason=" + lastError);
    }

    @Override public void close() {
        synchronized (lifecycle) { closed = true; quiescing = true; }
        for (HookHandle handle : handles) {
            try { handle.unhook(); } catch (Throwable ignored) { }
        }
        handles.clear();
        installed = false;
    }
}
