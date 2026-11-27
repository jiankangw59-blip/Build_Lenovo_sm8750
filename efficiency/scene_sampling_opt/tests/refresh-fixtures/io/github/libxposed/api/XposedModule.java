package io.github.libxposed.api;
import java.lang.reflect.Constructor;
import java.lang.reflect.Executable;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
public class XposedModule {
    private final Map<Executable,XposedInterface.Interceptor> hooks = new ConcurrentHashMap<>();
    public boolean failDeoptimize;
    public interface Original { Object call(Object[] args) throws Throwable; }
    public final class Builder {
        private final Executable executable;
        private Builder(Executable executable) { this.executable=executable; }
        public Builder setId(String id) { return this; }
        public XposedInterface.HookHandle intercept(XposedInterface.Interceptor interceptor) {
            hooks.put(executable,interceptor);
            return () -> hooks.remove(executable,interceptor);
        }
    }
    public Builder hook(Executable executable) { return new Builder(executable); }
    public boolean deoptimize(Executable executable) { return !failDeoptimize; }
    public void log(int level,String tag,String message) { }
    public int hookCount() { return hooks.size(); }
    public Object dispatch(Executable executable,Object receiver,Object[] args,Original original) throws Throwable {
        XposedInterface.Interceptor interceptor=hooks.get(executable);
        if (interceptor==null) return original.call(args);
        return interceptor.intercept(new XposedInterface.Chain() {
            public Object proceed() throws Throwable { return original.call(args); }
            public Object proceed(Object[] changed) throws Throwable { return original.call(changed); }
            public List<Object> getArgs() { return Collections.unmodifiableList(Arrays.asList(args)); }
            public Object getThisObject() { return receiver; }
            public Object getArg(int index) { return args[index]; }
        });
    }
    public Object construct(Constructor<?> constructor,Object[] args) throws Throwable {
        return dispatch(constructor,null,args, actual -> {
            try { return constructor.newInstance(actual); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        });
    }
    public Object invoke(Method method,Object receiver,Object... args) throws Throwable {
        return dispatch(method,receiver,args, actual -> {
            try { return method.invoke(receiver,actual); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        });
    }
}
