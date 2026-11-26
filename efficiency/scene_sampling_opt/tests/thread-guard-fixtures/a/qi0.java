package a;
import android.view.View;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Method;
public class qi0 {
    public static XposedModule MODULE;
    public static hl1 x;
    public static View y;
    public static int w;
    public final View c = new View();
    public boolean l;
    public Object m;
    public long o = 1000;
    public int starts;
    public int samplingCalls;
    public int resumeCalls;
    public Runnable onCloseReset;
    public Throwable sampleFailure;
    public Throwable closeFailure;
    public boolean attachSynchronously;
    private static Object invoke(String name,Class<?>[] types,Object receiver,Object[] args,XposedModule.Original body) throws Throwable {
        Method method=qi0.class.getDeclaredMethod(name,types);
        return MODULE==null ? body.call() : MODULE.dispatch(method,receiver,args,body);
    }
    public void b() throws Throwable {
        invoke("b",new Class<?>[0],this,new Object[0],() -> {
            if (x!=null) n30.k(x);
            x=null;
            if (onCloseReset!=null) onCloseReset.run();
            d();
            if (closeFailure!=null) throw closeFailure;
            if (y!=null) { y.detach(); y=null; }
            return null;
        });
    }
    public void c(String target,int pid) throws Throwable {
        invoke("c",new Class<?>[]{String.class,int.class},this,new Object[]{target,pid},() -> {
            b();
            d();
            // Real WindowManager.addView may return before the first traversal attaches the view.
            c.addedToWindow();
            if (attachSynchronously) c.attach();
            y=c;
            e();
            return null;
        });
    }
    public void d() throws Throwable {
        invoke("d",new Class<?>[0],this,new Object[0],() -> { l=false; m=null; o=1000; e(); return null; });
    }
    public void e() throws Throwable {
        invoke("e",new Class<?>[0],this,new Object[0],() -> { if(x!=null)n30.k(x); x=new hl1(); starts++; return null; });
    }
    public static Object a(qi0 owner,hz continuation) throws Throwable {
        return invoke("a",new Class<?>[]{qi0.class,hz.class},null,new Object[]{owner,continuation},() -> {
            if (continuation instanceof pi0 && (((pi0)continuation).i & Integer.MIN_VALUE)!=0) {
                owner.resumeCalls++;
                if (((pi0)continuation).g instanceof Throwable) throw (Throwable)((pi0)continuation).g;
            } else {
                owner.samplingCalls++;
                c30.M(owner.l ? "adb-monitor-t" : "get-threads","fixture",2000L);
            }
            if (owner.sampleFailure!=null) throw owner.sampleFailure;
            return jr1.a;
        });
    }
}
