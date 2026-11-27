package a;
import android.content.Context;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
public final class mg0 {
    public static XposedModule hooks;
    public static boolean writeResult=true;
    public static RuntimeException writeException;
    public static Runnable duringWrite;
    public static final AtomicInteger calls=new AtomicInteger();
    public static final Map<String,byte[]> files=new ConcurrentHashMap<>();
    private static final Method WRITE;
    static {
        try { WRITE=mg0.class.getDeclaredMethod("i",Context.class,String.class,byte[].class); }
        catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }
    public static boolean i(Context context,String name,byte[] bytes) {
        try {
            if (hooks==null) return raw(name,bytes);
            return (Boolean)hooks.dispatch(WRITE,null,new Object[]{context,name,bytes},
                    actual -> raw((String)actual[1],(byte[])actual[2]));
        } catch (RuntimeException | Error error) { throw error; }
        catch (Throwable error) { throw new AssertionError(error); }
    }
    private static boolean raw(String name,byte[] bytes) {
        calls.incrementAndGet();
        Runnable callback=duringWrite;
        if (callback!=null) callback.run();
        if (writeException!=null) throw writeException;
        if (writeResult) files.put(name,bytes.clone());
        return writeResult;
    }
    public static void reset(XposedModule module) {
        hooks=module; writeResult=true; writeException=null; duringWrite=null;
        calls.set(0); files.clear();
    }
}
