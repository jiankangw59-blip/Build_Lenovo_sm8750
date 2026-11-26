package android.os;
import java.util.concurrent.ConcurrentLinkedQueue;
public final class Looper {
    private static final Thread OWNER = Thread.currentThread();
    private static final Looper MAIN = new Looper();
    private static final ConcurrentLinkedQueue<Runnable> TASKS = new ConcurrentLinkedQueue<>();
    private Looper() { }
    public static Looper getMainLooper() { return MAIN; }
    public static Looper myLooper() { return Thread.currentThread() == OWNER ? MAIN : null; }
    static boolean enqueue(Runnable task) { TASKS.add(task); return true; }
    static void remove(Runnable task) { TASKS.removeIf(item -> item == task); }
    public static void drain() {
        if (myLooper() != MAIN) throw new AssertionError("must drain on main");
        Runnable task;
        while ((task = TASKS.poll()) != null) task.run();
    }
}
