package io.github.libxposed.api;
public interface XposedInterface {
    interface HookHandle { void unhook(); }
    interface Chain {
        Object proceed() throws Throwable;
        Object getThisObject();
        Object getArg(int index);
    }
    interface Interceptor { Object intercept(Chain chain) throws Throwable; }
}
