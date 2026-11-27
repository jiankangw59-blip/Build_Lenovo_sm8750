package cn.xiaocaiye.scenesamplingopt;

import android.app.Application;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import org.json.JSONObject;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/** Explicit UI verification only. No timer, automatic opening, or sampling task. */
final class ThreadWindowUiProbe {
    private final Application app;
    private final Class<?> type;
    private final Field window, target, view, period, detail, title, detailInfo;
    private final Method open;
    private Object owner;
    private int previousTarget;

    ThreadWindowUiProbe(Application app, ClassLoader loader) throws ReflectiveOperationException {
        this.app = app;
        type = Class.forName("a.qi0", false, loader);
        window = field("y"); target = field("w"); view = field("c");
        period = field("o"); detail = field("l"); title = field("d"); detailInfo = field("h");
        open = type.getDeclaredMethod("c", String.class, int.class); open.setAccessible(true);
    }

    private Field field(String name) throws ReflectiveOperationException {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }

    synchronized boolean busy() { return owner != null; }

    synchronized JSONObject execute(String command) throws Throwable {
        if (Looper.myLooper() != Looper.getMainLooper()) throw new IllegalStateException("main_thread_required");
        if ("thread_ui_open".equals(command)) {
            if (owner != null || window.get(null) != null) throw new IllegalStateException("thread_window_already_present");
            previousTarget = target.getInt(null);
            owner = type.getDeclaredConstructor(android.content.Context.class).newInstance(app);
            try { invoke(open, owner, "com.android.launcher", 0); }
            catch (Throwable error) {
                // Retain an owner with a mounted view for explicit cleanup; otherwise restore only our target.
                if (window.get(null) != view.get(owner)) { owner = null; target.setInt(null, previousTarget); }
                throw error;
            }
        } else if ("thread_ui_state".equals(command)) {
            // Read-only observation of the explicitly opened test window.
        } else {
            requireOwner();
            View root = (View) view.get(owner);
            if ("thread_ui_detail".equals(command)) {
                View list = root.findViewById(id("fw_threads"));
                if (!(list instanceof ViewGroup) || ((ViewGroup) list).getChildCount() == 0)
                    throw new IllegalStateException("thread_list_not_rendered");
                View item = ((ViewGroup) list).getChildAt(0);
                if (!item.performClick()) throw new IllegalStateException("thread_item_not_clickable");
            } else if ("thread_ui_back".equals(command)) {
                View button = root.findViewById(id("fw_back"));
                if (button == null || !button.performClick()) throw new IllegalStateException("back_not_clickable");
            } else if ("thread_ui_close".equals(command)) {
                Object closingOwner = owner;
                View mounted = (View) window.get(null);
                if (mounted != null && mounted != root) throw new IllegalStateException("another_thread_window_replaced_probe");
                if (mounted == root) {
                    View button = root.findViewById(id("close"));
                    if (button == null || !button.performClick()) throw new IllegalStateException("close_not_clickable");
                    if (window.get(null) == root) throw new IllegalStateException("close_did_not_remove_window");
                }
                if (owner == closingOwner) owner = null;
                // No preferences are written. Restore the in-memory target selected before this explicit test.
                if (window.get(null) == null) target.setInt(null, previousTarget);
            } else throw new IllegalArgumentException("unknown_thread_ui_command");
        }
        return state().put("command", command).put("explicitUiProbe", true);
    }

    private void requireOwner() throws IllegalAccessException {
        if (owner == null) throw new IllegalStateException("no_probe_thread_window");
        Object mounted = window.get(null);
        if (mounted != null && mounted != view.get(owner)) throw new IllegalStateException("probe_owner_is_not_current_window");
    }

    private int id(String name) {
        int value = app.getResources().getIdentifier(name, "id", ModuleEntry.TARGET);
        if (value == 0) throw new IllegalStateException("missing_id_" + name);
        return value;
    }

    private JSONObject state() throws Exception {
        JSONObject result = new JSONObject().put("ok", true).put("probeOwnerPresent", owner != null)
                .put("windowPresent", window.get(null) != null).put("targetPid", target.getInt(null));
        if (owner == null) return result;
        View root = (View) view.get(owner);
        View list = root.findViewById(id("fw_threads"));
        CharSequence value = ((TextView) detailInfo.get(owner)).getText();
        String text = value == null ? "" : value.toString();
        return result.put("ownerIsCurrentWindow", window.get(null) == root)
                .put("attached", root.isAttachedToWindow()).put("shown", root.isShown())
                .put("width", root.getWidth()).put("height", root.getHeight())
                .put("periodMs", period.getLong(owner)).put("detailMode", detail.getBoolean(owner))
                .put("title", ((TextView) title.get(owner)).getText().toString())
                .put("renderedRows", list instanceof ViewGroup ? ((ViewGroup) list).getChildCount() : -1)
                .put("detailTextLength", text.length()).put("detailTextPrefix", text.substring(0, Math.min(120, text.length())));
    }

    private static Object invoke(Method method, Object receiver, Object... args) throws Throwable {
        try { return method.invoke(receiver, args); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
}
