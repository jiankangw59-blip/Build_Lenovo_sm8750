package cn.xiaocaiye.scenesamplingopt;

import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import io.github.libxposed.api.XposedInterface;
import io.github.libxposed.api.XposedModule;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;

/** Scene-only, exact-build sampling de-duplication. No periodic jobs are created. */
public final class ModuleEntry extends XposedModule {
    public static final String TARGET = "com.omarea.vtools";
    public static final String ACTION = "cn.xiaocaiye.scenesamplingopt.DIAGNOSTIC";
    public static final String TOKEN_FILE = "scene_sampling_opt_token";
    private static final String TAG = "SceneSamplingOpt";
    private static final long SCENE_VERSION = 920261102L;
    private static final String SCENE_SHA = "11b9ce48ebddb1ffab21601b44bccc0ae68d5bda325923fd924380b060010ff3";
    private static final String PREFS = "scene_sampling_opt_settings";

    private final List<XposedInterface.HookHandle> handles = new ArrayList<>();
    private final List<String> deoptFailures = new ArrayList<>();
    private final AtomicLong batteryCalls = new AtomicLong();
    private final AtomicLong batteryForceCalls = new AtomicLong();
    private final AtomicLong batteryDueAtEntry = new AtomicLong();
    private final AtomicLong batteryDueUnderLock = new AtomicLong();
    private final AtomicLong batteryJoinedFresh = new AtomicLong();
    private final AtomicLong fullCalls = new AtomicLong();
    private final AtomicLong minCalls = new AtomicLong();
    private final AtomicLong minFullCacheHits = new AtomicLong();
    private final AtomicLong minJoinedFull = new AtomicLong();
    private final AtomicLong temperatureRpc = new AtomicLong();
    private final AtomicLong batteryRpc = new AtomicLong();
    private final AtomicLong minRpc = new AtomicLong();
    private final AtomicLong fullRpc = new AtomicLong();
    private final AtomicLong callErrors = new AtomicLong();
    private final AtomicInteger activeProbes = new AtomicInteger();
    private volatile SceneProbe lastProbe;
    private SerialGate batteryGate = new SerialGate();
    private SerialGate monitorGate = new SerialGate();
    private volatile boolean batteryEnabled = true;
    private volatile boolean monitorEnabled = true;
    private volatile boolean memoryEnabled = true;
    // First deployment observes the pre-existing job before the explicit, persisted enable.
    private volatile boolean threadWindowEnabled;
    private volatile boolean miniRenderEnabled;
    private volatile boolean installed;
    private volatile boolean verified;
    private volatile String reason = "not_loaded";
    private volatile String process = "";
    private String token = "";
    private String apkSha = "";
    private int deoptimized;
    private SceneBindings bindings;
    private MemoryDeduplicator memory;
    private ThreadWindowGuard threadWindow;
    private ThreadWindowUiProbe threadUi;
    private MiniRenderDeduplicator miniRender;
    private String miniRenderInstallFailure = "";
    private RefreshRatePersistence refreshPersistence;
    private String refreshPersistenceInstallFailure = "";
    private Application application;
    private BroadcastReceiver receiver;

    @Override public void onModuleLoaded(ModuleLoadedParam param) {
        process = param.getProcessName();
    }

    @Override public synchronized void onPackageReady(PackageReadyParam param) {
        if (!TARGET.equals(param.getPackageName()) || !TARGET.equals(process) || installed) return;
        install(param.getClassLoader(), param.getApplicationInfo(), null);
    }

    private void install(ClassLoader loader, ApplicationInfo info, Object saved) {
        try {
            restoreState(saved);
            if (info == null || !TARGET.equals(info.packageName)) throw new IllegalStateException("wrong_application");
            apkSha = sha256(new File(info.sourceDir));
            if (!SCENE_SHA.equals(apkSha)) throw new IllegalStateException("unsupported_scene_apk");
            bindings = new SceneBindings(loader);
            Application app = currentApplication();
            if (app != null) verifyVersion(app);
            installBattery();
            installMonitor();
            installRpcCounters();
            memory = new MemoryDeduplicator(this, loader);
            memory.install();
            memory.setEnabled(memoryEnabled);
            threadWindow = new ThreadWindowGuard(this, loader);
            threadWindow.install();
            miniRender = new MiniRenderDeduplicator(this, loader);
            try { miniRender.install(); }
            catch (Throwable miniFailure) {
                // Preserve the already-verified recording/thread-window fixes if a ROM differs here.
                miniRenderInstallFailure = miniFailure.getClass().getSimpleName() + ":" + miniFailure.getMessage();
                miniRender.close();
                logFailure("mini_render_skipped", miniFailure);
            }
            refreshPersistence = new RefreshRatePersistence(this, loader);
            try { refreshPersistence.install(); }
            catch (Throwable refreshFailure) {
                refreshPersistenceInstallFailure = refreshFailure.getClass().getSimpleName()
                        + ":" + refreshFailure.getMessage();
                refreshPersistence.close();
                logFailure("refresh_persistence_skipped", refreshFailure);
            }
            deoptimizeReviewedCallers(loader);
            installed = verified = true;
            reason = "ready";
            if (app != null) registerDiagnostics(app);
            else {
                Class<?> appClass = Class.forName("com.omarea.Scene", false, loader);
                Method onCreate = appClass.getDeclaredMethod("onCreate");
                handles.add(hook(onCreate).setId("scene_sampling.application_created").intercept(chain -> {
                    Object result = chain.proceed();
                    try { registerDiagnostics((Application) chain.getThisObject()); }
                    catch (Throwable error) { logFailure("diagnostic_registration", error); }
                    return result;
                }));
            }
            log(Log.INFO, TAG, "event=ready version=" + BuildInfo.VERSION_NAME + " pid=" + Process.myPid()
                    + " periodic_tasks=0 target=" + TARGET);
        } catch (Throwable error) {
            reason = error.getClass().getSimpleName() + ":" + String.valueOf(error.getMessage());
            installed = false;
            verified = false;
            closeHooks();
            logFailure("install_skipped", error);
        }
    }

    private void installBattery() {
        handles.add(hook(bindings.batteryRefresh).setId("scene_sampling.battery_gate").intercept(chain -> {
            batteryCalls.incrementAndGet();
            long maxAge = ((Number) chain.getArg(0)).longValue();
            if (maxAge == 0) batteryForceCalls.incrementAndGet();
            boolean dueBefore = batteryDue(maxAge);
            if (dueBefore) batteryDueAtEntry.incrementAndGet();
            if (!batteryEnabled) return chain.proceed();
            return batteryGate.call(() -> {
                boolean due = batteryDue(maxAge);
                if (due) batteryDueUnderLock.incrementAndGet();
                else if (dueBefore && maxAge != 0) batteryJoinedFresh.incrementAndGet();
                try { return chain.proceed(); }
                catch (Throwable error) { callErrors.incrementAndGet(); throw error; }
            });
        }));
    }

    private boolean batteryDue(long maxAge) throws IllegalAccessException {
        return maxAge == 0 || System.currentTimeMillis() - maxAge >= bindings.batteryTimestamp.getLong(null);
    }

    private void installMonitor() {
        handles.add(hook(bindings.full).setId("scene_sampling.monitor_full").intercept(chain -> {
            fullCalls.incrementAndGet();
            if (!monitorEnabled) return chain.proceed();
            return monitorGate.call(() -> {
                try { return chain.proceed(); }
                catch (Throwable error) { callErrors.incrementAndGet(); throw error; }
            });
        }));
        handles.add(hook(bindings.min).setId("scene_sampling.monitor_min").intercept(chain -> {
            minCalls.incrementAndGet();
            boolean freshBefore = fullFresh();
            if (!monitorEnabled) return chain.proceed();
            return monitorGate.call(() -> {
                if (fullFresh()) {
                    minFullCacheHits.incrementAndGet();
                    if (!freshBefore) minJoinedFull.incrementAndGet();
                }
                try { return chain.proceed(); }
                catch (Throwable error) { callErrors.incrementAndGet(); throw error; }
            });
        }));
    }

    private boolean fullFresh() throws IllegalAccessException {
        return SystemClock.elapsedRealtime() - bindings.fullTimestamp.getLong(null) < 500
                && bindings.fullResult.get(null) != null;
    }

    private void installRpcCounters() {
        handles.add(hook(bindings.rpcSync).setId("scene_sampling.observe_monitor_rpc").intercept(chain -> {
            Object command = chain.getArg(0);
            if ("adb-monitor-min".equals(command)) minRpc.incrementAndGet();
            else if ("adb-monitor-full".equals(command)) fullRpc.incrementAndGet();
            return chain.proceed();
        }));
        handles.add(hook(bindings.rpcTemperature).setId("scene_sampling.observe_temperature_rpc").intercept(chain -> {
            if ("temperature".equals(chain.getArg(1)) && "battery".equals(chain.getArg(2)))
                temperatureRpc.incrementAndGet();
            return chain.proceed();
        }));
        handles.add(hook(bindings.rpcBattery).setId("scene_sampling.observe_battery_rpc").intercept(chain -> {
            if ("battery".equals(chain.getArg(0))) batteryRpc.incrementAndGet();
            return chain.proceed();
        }));
    }

    private void deoptimizeReviewedCallers(ClassLoader loader) throws ReflectiveOperationException {
        deopt(bindings.battery.getDeclaredMethod("h"));
        deopt(Class.forName("a.zu", false, loader).getDeclaredMethod("j"));
        deopt(Class.forName("a.di0", false, loader).getDeclaredMethod("e", Object.class));
        deopt(Class.forName("a.ih0", false, loader).getDeclaredMethod("e", Object.class));
        deopt(Class.forName("a.r81", false, loader).getDeclaredMethod("c"));
        Class<?> renderer = Class.forName("com.omarea.ui.fw.FloatMonitorRender", false, loader);
        Class<?> continuation = Class.forName("a.hz", false, loader);
        deopt(renderer.getDeclaredMethod("c", renderer, continuation));
        Class<?> batteryReceiver = Class.forName("com.omarea.data.customer.BatteryReceiver", false, loader);
        deopt(batteryReceiver.getDeclaredMethod("setChargerLimitToValue", int.class,
                Class.forName("a.ie0", false, loader), boolean.class));
        // These bridge methods are exactly the observed RPC wrappers, not transport-wide hooks.
        deopt(bindings.rpcSync);
        deopt(bindings.rpcTemperature);
        deopt(bindings.rpcBattery);
    }

    private void deopt(Method method) {
        method.setAccessible(true);
        if (deoptimize(method)) deoptimized++;
        else deoptFailures.add(method.getDeclaringClass().getName() + "#" + method.getName());
    }

    private Application currentApplication() throws Throwable {
        Class<?> activityThread = Class.forName("android.app.ActivityThread");
        Method current = activityThread.getDeclaredMethod("currentApplication");
        return (Application) getInvoker(current).invoke(null);
    }

    private void verifyVersion(Application app) throws Exception {
        PackageInfo info = app.getPackageManager().getPackageInfo(TARGET, 0);
        if (info.getLongVersionCode() != SCENE_VERSION) throw new IllegalStateException("unsupported_scene_version");
    }

    private synchronized void registerDiagnostics(Application app) throws Exception {
        if (!TARGET.equals(app.getPackageName()) || receiver != null) return;
        verifyVersion(app);
        application = app;
        threadUi = new ThreadWindowUiProbe(app, app.getClassLoader());
        SharedPreferences prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        batteryEnabled = prefs.getBoolean("battery", batteryEnabled);
        monitorEnabled = prefs.getBoolean("monitor", monitorEnabled);
        memoryEnabled = prefs.getBoolean("memory", memoryEnabled);
        threadWindowEnabled = prefs.getBoolean("threadWindow", threadWindowEnabled);
        miniRenderEnabled = prefs.getBoolean("miniRender", miniRenderEnabled);
        if (memory != null) memory.setEnabled(memoryEnabled);
        if (threadWindow != null) threadWindow.setEnabled(threadWindowEnabled);
        if (miniRender != null) miniRender.setEnabled(miniRenderEnabled);
        File tokenFile = new File(app.getFilesDir(), TOKEN_FILE);
        if (token.isEmpty() && tokenFile.isFile()) {
            byte[] bytes = java.nio.file.Files.readAllBytes(tokenFile.toPath());
            String stored = new String(bytes, StandardCharsets.US_ASCII).trim();
            if (stored.matches("[0-9a-f]{64}")) token = stored;
        }
        if (token.isEmpty()) {
            byte[] secret = new byte[32];
            new SecureRandom().nextBytes(secret);
            token = hex(secret);
        }
        try (FileOutputStream output = new FileOutputStream(tokenFile)) {
            output.write(token.getBytes(StandardCharsets.US_ASCII));
        }
        tokenFile.setReadable(false, false); tokenFile.setWritable(false, false);
        tokenFile.setReadable(true, true); tokenFile.setWritable(true, true);
        receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                try {
                    String supplied = intent.getStringExtra("token");
                    if (supplied == null || !MessageDigest.isEqual(token.getBytes(StandardCharsets.US_ASCII),
                            supplied.getBytes(StandardCharsets.US_ASCII))) {
                        setResultCode(-1); setResultData("{\"ok\":false,\"error\":\"unauthorized\"}"); return;
                    }
                    String command = intent.getStringExtra("cmd");
                    if ("probe_battery".equals(command) || "probe_monitor".equals(command)) {
                        if (!reserveProbe()) {
                            setResultCode(-1); setResultData("{\"ok\":false,\"error\":\"probe_busy_or_module_unavailable\"}");
                            return;
                        }
                        PendingResult pending;
                        try { pending = goAsync(); }
                        catch (Throwable failure) { activeProbes.decrementAndGet(); throw failure; }
                        launchProbe(command, pending);
                        return;
                    }
                    JSONObject response;
                    if (command != null && command.startsWith("thread_ui_")) {
                        if (probeBusy()) throw new IllegalStateException("probe_in_progress");
                        response = threadUi.execute(command);
                    } else if ("configure".equals(command)) {
                        configure(intent);
                        response = snapshot();
                    } else if ("reset".equals(command)) {
                        resetCounters(); response = snapshot();
                    } else if ("selftest".equals(command)) {
                        response = new JSONObject().put("ok", true).put("gate", gateSelfTest());
                        if (memory != null && memory.snapshot().optBoolean("installed"))
                            response.put("memory", memory.selfTest(context));
                        else response.put("memory", new JSONObject().put("skipped", true)
                                .put("reason", "memory_path_not_supported_or_not_installed"));
                    } else if ("stats".equals(command)) response = snapshot();
                    else response = new JSONObject().put("ok", false).put("error", "unknown_command");
                    setResultCode(0); setResultData(response.toString());
                } catch (Throwable error) {
                    setResultCode(-1);
                    try { setResultData(new JSONObject().put("ok", false).put("error",
                            error.getClass().getSimpleName() + ":" + error.getMessage()).toString()); }
                    catch (Throwable ignored) { setResultData("{\"ok\":false}"); }
                }
            }
        };
        app.registerReceiver(receiver, new IntentFilter(ACTION), "android.permission.DUMP", null,
                Context.RECEIVER_EXPORTED);
        log(Log.INFO, TAG, "event=diagnostics_ready permission=DUMP private_token=true");
    }

    private synchronized void configure(Intent intent) {
        if (probeBusy()) throw new IllegalStateException("probe_in_progress");
        boolean nextThreadWindow = intent.hasExtra("threadWindow")
                ? intent.getBooleanExtra("threadWindow", threadWindowEnabled) : threadWindowEnabled;
        // A pending attach may veto disabling. Keep the module flags/prefs unchanged on veto.
        if (threadWindow != null) threadWindow.setEnabled(nextThreadWindow);
        boolean nextMiniRender = intent.hasExtra("miniRender")
                ? intent.getBooleanExtra("miniRender", miniRenderEnabled) : miniRenderEnabled;
        if (miniRender != null) miniRender.setEnabled(nextMiniRender);
        if (intent.hasExtra("battery")) batteryEnabled = intent.getBooleanExtra("battery", batteryEnabled);
        if (intent.hasExtra("monitor")) monitorEnabled = intent.getBooleanExtra("monitor", monitorEnabled);
        if (intent.hasExtra("memory")) memoryEnabled = intent.getBooleanExtra("memory", memoryEnabled);
        threadWindowEnabled = nextThreadWindow;
        miniRenderEnabled = nextMiniRender;
        if (memory != null) memory.setEnabled(memoryEnabled);
        if (application != null) application.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putBoolean("battery", batteryEnabled).putBoolean("monitor", monitorEnabled)
                .putBoolean("memory", memoryEnabled).putBoolean("threadWindow", threadWindowEnabled)
                .putBoolean("miniRender", miniRenderEnabled).commit();
    }

    public synchronized JSONObject snapshot() throws Exception {
        JSONObject result = new JSONObject().put("ok", true).put("pid", Process.myPid())
                .put("moduleVersionCode", BuildInfo.VERSION_CODE).put("moduleVersion", BuildInfo.VERSION_NAME)
                .put("installed", installed).put("verified", verified).put("reason", reason)
                .put("sceneApkSha256", apkSha).put("sceneVersion", SCENE_VERSION)
                .put("periodicTasks", 0).put("hookHandles", handles.size())
                .put("deoptimized", deoptimized).put("deoptFailed", new JSONArray(deoptFailures))
                .put("batteryEnabled", batteryEnabled).put("monitorEnabled", monitorEnabled)
                .put("memoryEnabled", memoryEnabled).put("threadWindowEnabled", threadWindowEnabled)
                .put("miniRenderEnabled", miniRenderEnabled);
        result.put("activeProbes", activeProbeCount());
        result.put("battery", gateSnapshot(batteryGate).put("calls", batteryCalls.get())
                .put("forceCalls", batteryForceCalls.get()).put("dueAtEntry", batteryDueAtEntry.get())
                .put("dueUnderLock", batteryDueUnderLock.get()).put("joinedFresh", batteryJoinedFresh.get()));
        result.put("monitor", gateSnapshot(monitorGate).put("fullCalls", fullCalls.get())
                .put("minCalls", minCalls.get()).put("minFullCacheHits", minFullCacheHits.get())
                .put("minJoinedFull", minJoinedFull.get()));
        result.put("rpc", new JSONObject().put("temperatureBattery", temperatureRpc.get())
                .put("battery", batteryRpc.get()).put("monitorMin", minRpc.get()).put("monitorFull", fullRpc.get()));
        result.put("callErrors", callErrors.get());
        if (memory != null) result.put("memory", memory.snapshot());
        if (threadWindow != null) result.put("threadWindow", threadWindow.snapshot());
        if (miniRender != null) result.put("miniRender", miniRender.snapshot().put("installFailure", miniRenderInstallFailure));
        if (refreshPersistence != null) result.put("refreshPersistence", refreshPersistence.snapshot()
                .put("installFailure", refreshPersistenceInstallFailure));
        return result;
    }

    private static JSONObject gateSnapshot(SerialGate gate) throws Exception {
        return new JSONObject().put("serializedCalls", gate.callCount()).put("contended", gate.contendedCount())
                .put("active", gate.activeCalls()).put("waiting", gate.waitingCalls()).put("waitNanos", gate.waitNanos());
    }

    private void resetCounters() {
        if (probeBusy()) throw new IllegalStateException("probe_in_progress");
        batteryGate.resetCounters(); monitorGate.resetCounters();
        for (AtomicLong value : new AtomicLong[]{batteryCalls, batteryForceCalls, batteryDueAtEntry,
                batteryDueUnderLock, batteryJoinedFresh, fullCalls, minCalls, minFullCacheHits,
                minJoinedFull, temperatureRpc, batteryRpc, minRpc, fullRpc, callErrors}) value.set(0);
    }

    private static JSONObject gateSelfTest() throws Throwable {
        SerialGate first = new SerialGate();
        SerialGate second = new SerialGate(first.sharedLock());
        int value = first.call(() -> second.call(() -> 7));
        if (value != 7 || first.activeCalls() != 0 || second.activeCalls() != 0)
            throw new AssertionError("reentrant_shared_gate");
        try { first.call(() -> { throw new IllegalArgumentException("fixture"); }); }
        catch (IllegalArgumentException expected) { }
        if (first.call(() -> 9) != 9 || first.activeCalls() != 0) throw new AssertionError("exception_release");
        return new JSONObject().put("passed", 2).put("targetMethodsInvoked", 0);
    }

    @Override public synchronized boolean onHotReloading(HotReloadingParam param) {
        if (probeBusy() || (threadUi != null && threadUi.busy())) return false;
        if (!batteryGate.tryQuiesce() || !monitorGate.tryQuiesce()
                || (memory != null && !memory.tryQuiesce())
                || (threadWindow != null && !threadWindow.tryQuiesce())
                || (miniRender != null && !miniRender.tryQuiesce())
                || (refreshPersistence != null && !refreshPersistence.tryQuiesce())) {
            batteryGate.resume(); monitorGate.resume();
            if (memory != null) memory.resume();
            if (threadWindow != null) threadWindow.resume();
            if (miniRender != null) miniRender.resume();
            if (refreshPersistence != null) refreshPersistence.resume();
            return false;
        }
        Bundle state = new Bundle();
        state.putString("token", token);
        state.putBoolean("battery", batteryEnabled); state.putBoolean("monitor", monitorEnabled);
        state.putBoolean("memory", memoryEnabled);
        state.putBoolean("threadWindow", threadWindowEnabled);
        state.putBoolean("miniRender", miniRenderEnabled);
        // Only bootstrap/Android types cross generations; no module object or target ClassLoader.
        param.setSavedInstanceState(new Object[]{batteryGate.sharedLock(), monitorGate.sharedLock(), state});
        closeHooks();
        installed = false; reason = "reloading";
        return true;
    }

    @Override public synchronized void onHotReloaded(HotReloadedParam param) {
        process = param.getProcessName();
        for (XposedInterface.HookHandle handle : param.getOldHookHandles()) {
            try { handle.unhook(); } catch (Throwable ignored) { }
        }
        if (!TARGET.equals(process)) return;
        try {
            Application app = currentApplication();
            if (app == null) throw new IllegalStateException("no_scene_application");
            install(app.getClassLoader(), app.getApplicationInfo(), param.getSavedInstanceState());
        } catch (Throwable error) { reason = "reload_failed"; logFailure("reload_failed", error); }
    }

    private void restoreState(Object saved) {
        if (!(saved instanceof Object[])) return;
        Object[] values = (Object[]) saved;
        if (values.length != 3 || !(values[0] instanceof ReentrantLock)
                || !(values[1] instanceof ReentrantLock) || !(values[2] instanceof Bundle)) return;
        batteryGate = new SerialGate((ReentrantLock) values[0]);
        monitorGate = new SerialGate((ReentrantLock) values[1]);
        Bundle state = (Bundle) values[2];
        token = state.getString("token", "");
        batteryEnabled = state.getBoolean("battery", true);
        monitorEnabled = state.getBoolean("monitor", true);
        memoryEnabled = state.getBoolean("memory", true);
        threadWindowEnabled = state.getBoolean("threadWindow", false);
        miniRenderEnabled = state.getBoolean("miniRender", false);
    }

    private void closeHooks() {
        if (receiver != null && application != null) {
            try { application.unregisterReceiver(receiver); } catch (Throwable ignored) { }
        }
        receiver = null;
        if (memory != null) { try { memory.close(); } catch (Throwable ignored) { } }
        if (threadWindow != null) { try { threadWindow.close(); } catch (Throwable ignored) { } }
        if (miniRender != null) { try { miniRender.close(); } catch (Throwable ignored) { } }
        if (refreshPersistence != null) { try { refreshPersistence.close(); } catch (Throwable ignored) { } }
        for (XposedInterface.HookHandle handle : handles) {
            try { handle.unhook(); } catch (Throwable ignored) { }
        }
        handles.clear();
    }

    private synchronized boolean reserveProbe() {
        if (!installed || !verified || probeBusy()) return false;
        return activeProbes.compareAndSet(0, 1); // reserve the coordinator before it starts
    }

    private boolean probeBusy() { return activeProbeCount() != 0; }

    private int activeProbeCount() {
        int count = activeProbes.get();
        SceneProbe run = lastProbe;
        // Thread finally counters can precede JVM Thread termination by a few instructions.
        // Externally report nonzero, and refuse reload, until every tracked Thread has ended.
        return count != 0 ? count : run != null && run.hasLiveThreads() ? 1 : 0;
    }

    private void launchProbe(String command, BroadcastReceiver.PendingResult pending) {
        try {
            SceneProbe run = new SceneProbe(bindings, activeProbes, this::snapshot);
            lastProbe = run;
            Thread coordinator = new Thread(() -> {
                try {
                    JSONObject result = "probe_battery".equals(command) ? run.battery() : run.monitor();
                    pending.setResultCode(0); pending.setResultData(result.toString());
                } catch (Throwable failure) {
                    pending.setResultCode(-1);
                    try { pending.setResultData(new JSONObject().put("ok", false).put("error",
                            failure.getClass().getSimpleName() + ":" + failure.getMessage())
                            .put("liveThreads", run.liveThreads()).toString()); }
                    catch (Throwable ignored) { pending.setResultData("{\"ok\":false}"); }
                } finally {
                    try { pending.finish(); }
                    finally { activeProbes.decrementAndGet(); }
                }
            }, "SSO-Probe-Control");
            coordinator.setDaemon(true);
            run.registerCoordinator(coordinator);
            // After successful start, only the coordinator owns pending.finish/count release.
            coordinator.start();
        } catch (Throwable startFailure) {
            try {
                pending.setResultCode(-1);
                pending.setResultData("{\"ok\":false,\"error\":\"probe_thread_start_failed\"}");
            } finally {
                try { pending.finish(); }
                finally { activeProbes.decrementAndGet(); }
            }
        }
    }

    private void logFailure(String event, Throwable error) {
        log(Log.WARN, TAG, "event=" + event + " type=" + error.getClass().getSimpleName()
                + " detail=" + error.getMessage());
    }

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] block = new byte[32768];
        try (FileInputStream input = new FileInputStream(file)) {
            int size; while ((size = input.read(block)) != -1) digest.update(block, 0, size);
        }
        return hex(digest.digest());
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) text.append(Character.forDigit((value >>> 4) & 15, 16))
                .append(Character.forDigit(value & 15, 16));
        return text.toString();
    }
}
