package a;
import java.lang.reflect.Method;
import io.github.libxposed.api.XposedModule;
public final class bi0 {
    public interface Body { void run() throws Throwable; }
    public final fi0 c;
    public Body body;
    public String cpu="1000",gpu="0",battery="30C",fps="60.0";
    public bi0(fi0 owner){c=owner;}
    public void run() throws Throwable {
        Method method=bi0.class.getDeclaredMethod("run");
        XposedModule.Original original=() -> {
            if(body!=null){body.run();return null;}
            c.d.setText(cpu);c.e.setData(c.x);c.f.a(100,95);c.g.setText(gpu);c.h.setText(battery);c.i.setText(fps);return null;
        };
        if(fi0.MODULE==null)original.call();else fi0.MODULE.dispatch(method,this,new Object[0],original);
    }
}
