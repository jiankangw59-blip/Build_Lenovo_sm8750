package cn.xiaocaiye.scenesamplingopt;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/** Exact, reviewed Scene ABI. No heuristic method selection on unknown versions. */
final class SceneBindings {
    final ClassLoader loader;
    final Class<?> battery;
    final Class<?> monitor;
    final Method batteryRefresh;
    final Method full;
    final Method min;
    final Field batteryTimestamp;
    final Field fullTimestamp;
    final Field fullResult;
    final Method rpcSync;
    final Method rpcTemperature;
    final Method rpcBattery;

    SceneBindings(ClassLoader loader) throws ReflectiveOperationException {
        this.loader = loader;
        battery = Class.forName("a.at0", false, loader);
        monitor = Class.forName("a.c41", false, loader);
        Class<?> fullType = Class.forName("a.x31", false, loader);
        Class<?> minType = Class.forName("a.a41", false, loader);
        batteryRefresh = method(battery, "g", long.class, long.class);
        full = method(monitor, "b", fullType);
        min = method(monitor, "c", minType);
        batteryTimestamp = staticField(battery, "b", long.class);
        fullTimestamp = staticField(monitor, "c", long.class);
        fullResult = staticField(monitor, "d", fullType);
        if (!minType.isAssignableFrom(fullType)) throw new NoSuchFieldException("full is not min-compatible");
        Class<?> rpc = Class.forName("a.c30", false, loader);
        Class<?> continuation = Class.forName("a.hz", false, loader);
        rpcSync = method(rpc, "M", String.class, String.class, String.class, Long.class);
        rpcTemperature = method(rpc, "L", Object.class, rpc, String.class, String.class,
                Long.class, continuation, int.class);
        rpcBattery = rpc.getDeclaredMethod("i", String.class, String[].class, continuation);
        if (rpcBattery.getReturnType() != Object.class || Modifier.isStatic(rpcBattery.getModifiers()))
            throw new NoSuchMethodException("rpc battery");
        rpcBattery.setAccessible(true);
    }

    static Method method(Class<?> owner, String name, Class<?> returns, Class<?>... args)
            throws ReflectiveOperationException {
        Method m = owner.getDeclaredMethod(name, args);
        if (!Modifier.isStatic(m.getModifiers()) || m.getReturnType() != returns)
            throw new NoSuchMethodException(owner.getName() + "#" + name);
        m.setAccessible(true);
        return m;
    }

    static Field staticField(Class<?> owner, String name, Class<?> type)
            throws ReflectiveOperationException {
        Field f = owner.getDeclaredField(name);
        if (!Modifier.isStatic(f.getModifiers()) || f.getType() != type)
            throw new NoSuchFieldException(owner.getName() + "." + name);
        f.setAccessible(true);
        return f;
    }
}
