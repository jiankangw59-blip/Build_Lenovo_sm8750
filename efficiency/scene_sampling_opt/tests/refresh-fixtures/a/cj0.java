package a;
import android.content.Context;
import java.util.List;
/** Relevant original DEX branch: saving and manual control remain independent. */
public final class cj0 {
    public final List<?> d;
    public final boolean e,f,g;
    public final lj0 h;
    public final Context i;
    public final String j;
    public final Runnable k;
    public int manualCalls;
    public cj0(List<?> rates,boolean feature,boolean special,boolean dynamic,
               lj0 panel,Context context,String app,ri0 runnable) {
        d=rates; e=feature; f=special; g=dynamic; h=panel; i=context; j=app; k=runnable;
    }
    public Object i(Object selected) {
        int hz=((Number)d.get(((Number)selected).intValue())).intValue();
        if (e && !f && g) { h.k.j(j,null,hz); h.k.i(); }
        manualCalls++;
        return this;
    }
}
