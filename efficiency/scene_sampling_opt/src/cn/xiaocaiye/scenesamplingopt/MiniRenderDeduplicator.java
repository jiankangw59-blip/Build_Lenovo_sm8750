package cn.xiaocaiye.scenesamplingopt;

import android.graphics.Paint;
import android.os.Looper;
import android.view.View;
import android.widget.TextView;
import io.github.libxposed.api.XposedInterface.HookHandle;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONArray;
import org.json.JSONObject;

/** Same-presentation submissions only, inside the pinned Mini renderer. No sampling/cache TTL changes. */
public final class MiniRenderDeduplicator implements AutoCloseable {
    private final XposedModule module;
    private final ClassLoader loader;
    private final Object lifecycle = new Object();
    private final Object publications = new Object();
    private final ThreadLocal<Frame> current = new ThreadLocal<>();
    private final List<HookHandle> handles = new ArrayList<>();
    private final List<String> deoptimized = new ArrayList<>();
    private final Map<TextView, TextStamp> textPublished = new WeakHashMap<>();
    private final Map<Object, ChartPublication> chartPublished = new WeakHashMap<>();
    private final AtomicLong renders = new AtomicLong(), invalidRenders = new AtomicLong();
    private final AtomicLong textCalls = new AtomicLong(), textEligible = new AtomicLong();
    private final AtomicLong textSame = new AtomicLong(), textChanged = new AtomicLong(), textSkipped = new AtomicLong();
    private final AtomicLong textIneligible = new AtomicLong(), firstTextOriginal = new AtomicLong();
    private final AtomicLong chartCalls = new AtomicLong(), chartSame = new AtomicLong();
    private final AtomicLong chartPresentationSame = new AtomicLong(), chartSkipped = new AtomicLong();
    private final AtomicLong chartOriginal = new AtomicLong(), chartStyleForce = new AtomicLong(), errors = new AtomicLong();
    private final AtomicLong[] perTextCalls = counters(), perTextSame = counters(), perTextSkipped = counters();
    private final String[] lastTextClass = new String[]{"unobserved","unobserved","unobserved","unobserved"};
    private final String[] lastEligibility = new String[]{"unobserved","unobserved","unobserved","unobserved"};
    private final String[] lastBuffer = new String[]{"unobserved","unobserved","unobserved","unobserved"};
    private final String[] lastText = new String[]{"unobserved","unobserved","unobserved","unobserved"};
    private volatile boolean enabled, installed, closed;
    private boolean quiescing;
    private int active;
    private String reason = "not_installed", textAbi = "unresolved";
    private Class<?> ownerType, chartType;
    private Field renderOwner, ownerRoot, currentRoot, ownerCpu, ownerGpu;
    private Field[] textFields;
    private Field chartQueue, chartN, chartMin, chartMax, chartColor, chartM, chartR, chartPaint;
    private Field textBuffer, textListeners, textEditor;

    private static AtomicLong[] counters() {
        return new AtomicLong[]{new AtomicLong(),new AtomicLong(),new AtomicLong(),new AtomicLong()};
    }

    public MiniRenderDeduplicator(XposedModule module, ClassLoader loader) { this.module=module; this.loader=loader; }

    public void install() throws ReflectiveOperationException {
        if (installed || closed) return;
        ownerType=Class.forName("a.fi0",false,loader);
        Class<?> renderer=Class.forName("a.bi0",false,loader);
        chartType=Class.forName("com.omarea.ui.CpuChartBarView",false,loader);
        renderOwner=field(renderer,"c",ownerType,false);
        ownerRoot=field(ownerType,"c",View.class,false);
        currentRoot=field(ownerType,"C",View.class,true);
        ownerCpu=field(ownerType,"e",chartType,false); ownerGpu=field(ownerType,"f",chartType,false);
        textFields=new Field[]{field(ownerType,"d",TextView.class,false),field(ownerType,"g",TextView.class,false),
                field(ownerType,"h",TextView.class,false),field(ownerType,"i",TextView.class,false)};
        chartQueue=field(chartType,"q",LinkedBlockingQueue.class,false);
        chartN=field(chartType,"n",int.class,false); chartMin=field(chartType,"o",int.class,false);
        chartMax=field(chartType,"p",int.class,false); chartColor=field(chartType,"s",int.class,false);
        chartM=field(chartType,"m",float.class,false); chartR=field(chartType,"r",float.class,false);
        chartPaint=field(chartType,"l",Paint.class,false);
        Method run=renderer.getDeclaredMethod("run");
        Method setData=chartType.getDeclaredMethod("setData",Integer[].class);
        Method setText=TextView.class.getDeclaredMethod("setText",CharSequence.class);
        if (run.getReturnType()!=void.class || setData.getReturnType()!=void.class)
            throw new NoSuchMethodException("Mini rendering ABI");
        run.setAccessible(true); setData.setAccessible(true); setText.setAccessible(true);
        try {
            textBuffer=TextView.class.getDeclaredField("mBufferType"); textBuffer.setAccessible(true);
            textListeners=TextView.class.getDeclaredField("mListeners"); textListeners.setAccessible(true);
            textEditor=TextView.class.getDeclaredField("mEditor"); textEditor.setAccessible(true);
            if (textBuffer.getType()!=TextView.BufferType.class || !List.class.isAssignableFrom(textListeners.getType()))
                throw new NoSuchFieldException("TextView mutation ABI");
            textAbi="ready";
        } catch (ReflectiveOperationException | RuntimeException unavailable) {
            // A ROM with an unknown TextView implementation keeps every original text submission.
            textBuffer=null; textListeners=null; textEditor=null;
            textAbi="unavailable:"+unavailable.getClass().getSimpleName();
        }
        try {
            handles.add(module.hook(run).setId("scene.mini.render_scope").intercept(chain -> {
                if (!enter()) return chain.proceed();
                Frame previous=current.get();
                current.remove(); // An invalid nested render must not inherit the outer owner's scope.
                try {
                    renders.incrementAndGet();
                    Frame frame;
                    try { frame=new Frame(renderOwner.get(chain.getThisObject())); }
                    catch (ReflectiveOperationException | RuntimeException failure) {
                        errors.incrementAndGet(); invalidRenders.incrementAndGet(); return chain.proceed();
                    }
                    if (!frame.valid()) { invalidRenders.incrementAndGet(); return chain.proceed(); }
                    current.set(frame);
                    return chain.proceed();
                } finally {
                    if (previous==null) current.remove(); else current.set(previous);
                    leave();
                }
            }));
            handles.add(module.hook(setText).setId("scene.mini.same_text").intercept(chain -> {
                Frame frame=current.get();
                if (frame==null || !frame.valid()) return chain.proceed();
                int role=frame.role(chain.getThisObject());
                if (role<0) return chain.proceed();
                TextView view=(TextView)chain.getThisObject();
                Object next=chain.getArg(0);
                textCalls.incrementAndGet(); perTextCalls[role].incrementAndGet();
                lastTextClass[role]=view.getClass().getName();
                String eligibility;
                TextStamp now=null, published=null;
                boolean same=false;
                try {
                    eligibility=eligibility(view,next);
                    lastBuffer[role]=textBuffer==null?"unavailable":String.valueOf(textBuffer.get(view));
                    lastText[role]=String.valueOf(view.getText());
                    if ("eligible".equals(eligibility)) {
                        textEligible.incrementAndGet();
                        same=view.getText().equals(next);
                        if (same) { textSame.incrementAndGet(); perTextSame[role].incrementAndGet(); }
                        else textChanged.incrementAndGet();
                        now=new TextStamp(view);
                        synchronized (publications) { published=textPublished.get(view); }
                        if (published==null) firstTextOriginal.incrementAndGet();
                    } else textIneligible.incrementAndGet();
                    lastEligibility[role]=eligibility;
                } catch (ReflectiveOperationException | RuntimeException failure) {
                    errors.incrementAndGet(); eligibility="inspection_failed"; lastEligibility[role]=eligibility;
                }
                if (enabled && same && now!=null && published!=null && !view.isLayoutRequested() && published.same(now)) {
                    textSkipped.incrementAndGet(); perTextSkipped[role].incrementAndGet(); return null;
                }
                // A failed/partially-mutating original call must not leave a prior success usable for skipping.
                synchronized(publications) { textPublished.remove(view); }
                Object result=chain.proceed(); // Only a successful original submission can publish a baseline.
                try {
                    TextStamp written=null;
                    if ("eligible".equals(eligibility(view,next)) && view.getText().equals(next)) written=new TextStamp(view);
                    synchronized (publications) {
                        if (written==null) textPublished.remove(view); else textPublished.put(view,written);
                    }
                } catch (ReflectiveOperationException | RuntimeException failure) {
                    errors.incrementAndGet(); synchronized(publications) { textPublished.remove(view); }
                }
                return result;
            }));
            handles.add(module.hook(setData).setId("scene.mini.same_cpu_chart").intercept(chain -> {
                Frame frame=current.get();
                Object chart=chain.getThisObject();
                if (frame==null || !frame.valid() || chart!=frame.cpu || chart==frame.gpu) return chain.proceed();
                chartCalls.incrementAndGet();
                Object input=chain.getArg(0);
                Integer[] values=input instanceof Integer[] ? (Integer[])input : null;
                boolean safe=values!=null && noNulls(values);
                try {
                    ChartPublication prior;
                    synchronized (publications) { prior=chartPublished.get(chart); }
                    // The stored data is an independent snapshot of a successful original queue.
                    if (safe && prior!=null && Arrays.equals(prior.values,values)) {
                        Object[] actual=((LinkedBlockingQueue<?>)chartQueue.get(chart)).toArray();
                        ChartStamp now=new ChartStamp((View)chart);
                        if (Arrays.equals(actual,values) && chartN.getInt(chart)==values.length) {
                            chartSame.incrementAndGet();
                            if (prior.stamp.same(now)) {
                                chartPresentationSame.incrementAndGet();
                                if (enabled) { chartSkipped.incrementAndGet(); return null; }
                            } else chartStyleForce.incrementAndGet();
                        }
                    }
                } catch (ReflectiveOperationException | RuntimeException failure) { errors.incrementAndGet(); }
                synchronized(publications) { chartPublished.remove(chart); }
                Object result=chain.proceed(); chartOriginal.incrementAndGet();
                try {
                    ChartPublication written=null;
                    if (safe) {
                        Object[] actual=((LinkedBlockingQueue<?>)chartQueue.get(chart)).toArray();
                        if (Arrays.equals(actual,values) && chartN.getInt(chart)==values.length)
                            written=new ChartPublication(actual,new ChartStamp((View)chart));
                    }
                    synchronized (publications) {
                        if (written==null) chartPublished.remove(chart); else chartPublished.put(chart,written);
                    }
                } catch (ReflectiveOperationException | RuntimeException failure) {
                    errors.incrementAndGet(); synchronized(publications) { chartPublished.remove(chart); }
                }
                return result;
            }));
            for (Method method:new Method[]{run,setData,setText}) {
                if (!module.deoptimize(method)) throw new IllegalStateException("deopt_failed:"+method);
                deoptimized.add(method.getDeclaringClass().getName()+"#"+method.getName());
            }
            installed=true; reason="ready";
        } catch (Throwable failure) { close(); throw failure; }
    }

    private final class Frame {
        final Object owner;
        final View root;
        final TextView[] texts=new TextView[4];
        final Object cpu,gpu;
        Frame(Object owner) throws IllegalAccessException {
            this.owner=owner; root=(View)ownerRoot.get(owner);
            for(int i=0;i<4;i++) texts[i]=(TextView)textFields[i].get(owner);
            cpu=ownerCpu.get(owner); gpu=ownerGpu.get(owner);
        }
        boolean valid() {
            try { return Looper.myLooper()==Looper.getMainLooper() && root!=null
                    && currentRoot.get(null)==root && root.isAttachedToWindow(); }
            catch (IllegalAccessException | RuntimeException failure) { errors.incrementAndGet(); return false; }
        }
        int role(Object object) { for(int i=0;i<4;i++) if(object==texts[i])return i; return -1; }
    }

    private String eligibility(TextView view,Object next) throws IllegalAccessException {
        if (textBuffer==null) return "text_abi_unavailable";
        if (view.getClass()!=TextView.class) return "subclass";
        if (next==null || next.getClass()!=String.class || view.getText()==null || view.getText().getClass()!=String.class)
            return "non_plain_string";
        if (textBuffer.get(view)!=TextView.BufferType.NORMAL) return "buffer_type";
        Object listeners=textListeners.get(view);
        if (listeners!=null && (!(listeners instanceof List<?>) || !((List<?>)listeners).isEmpty())) return "text_watchers";
        if (textEditor.get(view)!=null) return "editor";
        if (view.getFilters().length!=0) return "filters";
        if (view.getTransformationMethod()!=null) return "transformation";
        if (view.getMovementMethod()!=null) return "movement";
        if (view.getKeyListener()!=null) return "key_listener";
        if (view.getEllipsize()!=null) return "ellipsize"; // Same text can reset a marquee's visible progress.
        if (view.getAutoSizeTextType()!=TextView.AUTO_SIZE_TEXT_TYPE_NONE) return "auto_size";
        if (view.isTextSelectable()) return "selectable";
        if (view.getSelectionStart()!=-1 || view.getSelectionEnd()!=-1) return "selection";
        if (view.getAutoLinkMask()!=0) return "autolink";
        if (view.getError()!=null) return "error_text";
        if (view.getAccessibilityLiveRegion()!=View.ACCESSIBILITY_LIVE_REGION_NONE) return "live_region";
        if (view.isAccessibilityFocused()) return "accessibility_focused";
        return "eligible";
    }

    private static boolean noNulls(Integer[] values) { for(Integer value:values)if(value==null)return false; return true; }

    private static final class TextStamp {
        final Object paint,typeface,colors;
        final int width,height,left,top,right,bottom,flags,color,currentColor,alpha;
        final String fontFeatures;
        final float size,scale,skew,spacing;
        TextStamp(TextView view) {
            Paint p=view.getPaint(); paint=p; typeface=p.getTypeface(); colors=view.getTextColors();
            width=view.getWidth();height=view.getHeight();left=view.getPaddingLeft();top=view.getPaddingTop();
            right=view.getPaddingRight();bottom=view.getPaddingBottom();flags=p.getFlags();color=p.getColor();alpha=p.getAlpha();
            currentColor=view.getCurrentTextColor();fontFeatures=view.getFontFeatureSettings();
            size=p.getTextSize();scale=p.getTextScaleX();skew=p.getTextSkewX();spacing=view.getLetterSpacing();
        }
        boolean same(TextStamp s) { return paint==s.paint && typeface==s.typeface && colors==s.colors && width==s.width
                && height==s.height && left==s.left && top==s.top && right==s.right && bottom==s.bottom && flags==s.flags
                && color==s.color && currentColor==s.currentColor && java.util.Objects.equals(fontFeatures,s.fontFeatures)
                && alpha==s.alpha && Float.floatToIntBits(size)==Float.floatToIntBits(s.size)
                && Float.floatToIntBits(scale)==Float.floatToIntBits(s.scale) && Float.floatToIntBits(skew)==Float.floatToIntBits(s.skew)
                && Float.floatToIntBits(spacing)==Float.floatToIntBits(s.spacing); }
    }

    private final class ChartStamp {
        final Object paint,paintStyle;
        final int n,min,max,color,width,height,paintFlags;
        final float m,r,stroke;
        ChartStamp(View view) throws IllegalAccessException {
            n=chartN.getInt(view);min=chartMin.getInt(view);max=chartMax.getInt(view);color=chartColor.getInt(view);
            m=chartM.getFloat(view);r=chartR.getFloat(view);width=view.getWidth();height=view.getHeight();
            Paint p=(Paint)chartPaint.get(view);paint=p;paintFlags=p==null?0:p.getFlags();
            paintStyle=p==null?null:p.getStyle();stroke=p==null?0:p.getStrokeWidth();
        }
        boolean same(ChartStamp s) { return n==s.n && min==s.min && max==s.max && color==s.color && width==s.width
                && height==s.height && paint==s.paint && paintFlags==s.paintFlags && paintStyle==s.paintStyle
                && Float.floatToIntBits(m)==Float.floatToIntBits(s.m) && Float.floatToIntBits(r)==Float.floatToIntBits(s.r)
                && Float.floatToIntBits(stroke)==Float.floatToIntBits(s.stroke); }
    }
    private static final class ChartPublication {
        final Object[] values;
        final MiniRenderDeduplicator.ChartStamp stamp;
        ChartPublication(Object[] values,MiniRenderDeduplicator.ChartStamp stamp) {
            this.values=values.clone();this.stamp=stamp;
        }
    }

    public void setEnabled(boolean value) {
        synchronized(lifecycle) {
            if(closed)return;
            if(enabled!=value) { synchronized(publications){textPublished.clear();chartPublished.clear();} enabled=value; }
        }
    }
    private boolean enter() { synchronized(lifecycle){if(!installed||closed||quiescing)return false;active++;return true;} }
    private void leave() { synchronized(lifecycle){active--;} }
    public boolean tryQuiesce() { synchronized(lifecycle){if(active!=0)return false;quiescing=true;return true;} }
    public void resume() { synchronized(lifecycle){if(!closed)quiescing=false;} }
    @Override public void close() {
        synchronized(lifecycle){closed=true;quiescing=true;installed=false;}
        for(int i=handles.size()-1;i>=0;i--){try{handles.get(i).unhook();}catch(Throwable ignored){}}
        handles.clear(); current.remove(); synchronized(publications){textPublished.clear();chartPublished.clear();}
    }
    public JSONObject snapshot() {
        JSONObject out=new JSONObject();
        try {
            out.put("installed",installed).put("enabled",enabled).put("reason",reason).put("textAbi",textAbi)
                    .put("hookHandles",handles.size()).put("renders",renders.get()).put("invalidRenders",invalidRenders.get())
                    .put("textCalls",textCalls.get()).put("textEligible",textEligible.get()).put("textSame",textSame.get())
                    .put("textChanged",textChanged.get()).put("textIneligible",textIneligible.get()).put("textSkipped",textSkipped.get())
                    .put("firstTextOriginal",firstTextOriginal.get()).put("chartCalls",chartCalls.get())
                    .put("chartSameData",chartSame.get()).put("chartSamePresentation",chartPresentationSame.get())
                    .put("chartSkipped",chartSkipped.get()).put("chartStyleForce",chartStyleForce.get())
                    .put("chartOriginal",chartOriginal.get()).put("errors",errors.get())
                    .put("gpuHistoryHooked",false).put("deoptimized",new JSONArray(deoptimized));
            JSONArray per=new JSONArray();String[] names={"cpuFrequency","gpuFrequency","battery","fpsOrRam"};
            for(int i=0;i<4;i++)per.put(new JSONObject().put("field",names[i]).put("concreteClass",lastTextClass[i])
                    .put("eligibility",lastEligibility[i]).put("calls",perTextCalls[i].get()).put("same",perTextSame[i].get())
                    .put("bufferType",lastBuffer[i]).put("currentText",lastText[i])
                    .put("skipped",perTextSkipped[i].get()));
            out.put("texts",per);
            synchronized(lifecycle){out.put("active",active).put("quiescing",quiescing);}
        } catch(org.json.JSONException impossible){throw new IllegalStateException(impossible);}
        return out;
    }

    private static Field field(Class<?> owner,String name,Class<?> type,boolean isStatic) throws ReflectiveOperationException {
        Field f=owner.getDeclaredField(name);
        if(f.getType()!=type || Modifier.isStatic(f.getModifiers())!=isStatic)throw new NoSuchFieldException(owner.getName()+"."+name);
        f.setAccessible(true);return f;
    }
}
