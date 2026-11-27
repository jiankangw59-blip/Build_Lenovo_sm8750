package io.github.libxposed.api;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;
public class XposedModule {
    private final Map<Method,XposedInterface.Interceptor> hooks = new HashMap<>();
    public interface Original { Object call() throws Throwable; }
    public final class Builder {
        private final Method method;
        private Builder(Method method) { this.method=method; }
        public Builder setId(String id) { return this; }
        public XposedInterface.HookHandle intercept(XposedInterface.Interceptor interceptor) {
            hooks.put(method,interceptor);
            return () -> hooks.remove(method,interceptor);
        }
    }
    public static final class Invoker {
        private final Method method;
        private Invoker(Method method) { this.method=method; method.setAccessible(true); }
        public Object invoke(Object receiver,Object... args) throws Throwable {
            try { return method.invoke(receiver,args); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
    }
    public Builder hook(Method method) { return new Builder(method); }
    public Invoker getInvoker(Method method) { return new Invoker(method); }
    public boolean deoptimize(Method method) { return true; }
    public void log(int level,String tag,String message) { }
    public Object dispatch(Method method,Object receiver,Object[] args,Original original) throws Throwable {
        XposedInterface.Interceptor interceptor=hooks.get(method);
        if (interceptor==null) return original.call();
        return interceptor.intercept(new XposedInterface.Chain() {
            public Object proceed() throws Throwable { return original.call(); }
            public Object getThisObject() { return receiver; }
            public Object getArg(int index) { return args[index]; }
        });
    }
}
