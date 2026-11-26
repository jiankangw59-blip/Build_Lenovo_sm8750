package android.os;
public final class Handler {
    public Handler(Looper looper) { if (looper == null) throw new NullPointerException("looper"); }
    public boolean post(Runnable task) { return Looper.enqueue(task); }
    public void removeCallbacks(Runnable task) { Looper.remove(task); }
    public void removeCallbacksAndMessages(Object token) { }
}
