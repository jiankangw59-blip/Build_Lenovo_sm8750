package a;
import java.lang.reflect.Method;
public final class c30 {
    private c30() { }
    public static String M(String command,String argument,Long timeout) throws Throwable {
        Method method=c30.class.getDeclaredMethod("M",String.class,String.class,Long.class);
        if(qi0.MODULE==null) return "fixture response";
        return (String)qi0.MODULE.dispatch(method,null,new Object[]{command,argument,timeout},() -> "fixture response");
    }
    public static String s(long timeout,String command,String argument) throws Throwable { return M(command,argument,timeout); }
}
