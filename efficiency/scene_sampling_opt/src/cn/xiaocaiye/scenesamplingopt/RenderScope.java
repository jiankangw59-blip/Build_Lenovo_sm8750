package cn.xiaocaiye.scenesamplingopt;

/** Same-thread, same-render ownership. No clock, persistence or shared data cache. */
public final class RenderScope<M, B> {
    public enum Query { PASS, READ_FIRST, REUSE_FIRST }

    public static final class Frame<M, B> {
        private final Frame<M, B> parent;
        private final M manager;
        private final B first;
        private int state;
        private boolean closed;

        private Frame(Frame<M, B> parent, M manager, B first) {
            this.parent = parent;
            this.manager = manager;
            this.first = first;
        }

        public B firstBuffer() { return first; }

        public Query query(M caller, B buffer) {
            if (closed || caller != manager || buffer == null) return Query.PASS;
            if (state == 0 && buffer == first) {
                state = 1;
                return Query.READ_FIRST;
            }
            if (state == 2) {
                // The pinned method's immediately following query uses a new object.
                if (buffer != first) {
                    state = 3;
                    return Query.REUSE_FIRST;
                }
                state = 4;
            }
            return Query.PASS;
        }

        public void firstSucceeded() {
            if (state == 1) state = 2;
        }

        public void firstFailed() { state = 4; }
    }

    private final ThreadLocal<Frame<M, B>> current = new ThreadLocal<>();
    private boolean accepting = true;
    private int active;

    public Frame<M, B> enter(M manager, B first) {
        synchronized (this) {
            if (!accepting || manager == null || first == null) return null;
            active++;
        }
        Frame<M, B> frame = new Frame<>(current.get(), manager, first);
        current.set(frame);
        return frame;
    }

    public Frame<M, B> current() { return current.get(); }

    public void exit(Frame<M, B> frame) {
        if (frame == null || frame.closed) return;
        frame.closed = true;
        if (current.get() == frame) {
            Frame<M, B> parent = frame.parent;
            while (parent != null && parent.closed) parent = parent.parent;
            if (parent == null) current.remove();
            else current.set(parent);
        }
        synchronized (this) { active--; }
    }

    /** Rejects new scopes first, then reports whether all previously entered scopes left. */
    public synchronized boolean tryQuiesce() {
        accepting = false;
        return active == 0;
    }

    public synchronized void resume() { accepting = true; }
    public synchronized int active() { return active; }
}
