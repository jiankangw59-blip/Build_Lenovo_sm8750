package android.view;
import java.util.ArrayList;
import java.util.List;
public class View {
    public interface OnAttachStateChangeListener {
        void onViewAttachedToWindow(View view);
        void onViewDetachedFromWindow(View view);
    }
    public boolean attached;
    public int visibility;
    public RuntimeException listenerRemovalFailure;
    private final List<OnAttachStateChangeListener> listeners = new ArrayList<>();
    private ViewParent parent;
    public boolean isAttachedToWindow() { return attached; }
    public int getVisibility() { return visibility; }
    public boolean isShown() { return attached && visibility == 0; }
    public ViewParent getParent() { return parent; }
    public void addedToWindow() { if (parent==null) parent=new ViewParent(){}; }
    public void addOnAttachStateChangeListener(OnAttachStateChangeListener listener) { listeners.add(listener); }
    public void removeOnAttachStateChangeListener(OnAttachStateChangeListener listener) {
        if (listenerRemovalFailure!=null) throw listenerRemovalFailure;
        listeners.remove(listener);
    }
    public int attachListenerCount() { return listeners.size(); }
    public void attach() {
        if (attached) return;
        addedToWindow();
        attached=true;
        for (OnAttachStateChangeListener listener : new ArrayList<>(listeners)) listener.onViewAttachedToWindow(this);
    }
    public void detach() {
        parent=null;
        if (!attached) return;
        attached=false;
        for (OnAttachStateChangeListener listener : new ArrayList<>(listeners)) listener.onViewDetachedFromWindow(this);
    }
}
