package io.github.libxposed.api;
import java.util.List;
public interface XposedInterface {
    interface HookHandle { void unhook(); }
    interface Chain {
        Object proceed() throws Throwable;
        Object proceed(Object[] args) throws Throwable;
        List<Object> getArgs();
        Object getThisObject();
        Object getArg(int index);
    }
    interface Interceptor { Object intercept(Chain chain) throws Throwable; }
}
