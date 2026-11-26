package cn.xiaocaiye.scenesamplingopt;

import a.bi0;
import a.fi0;
import android.graphics.Paint;
import android.text.Editable;
import android.text.InputFilter;
import android.text.Spanned;
import android.text.TextWatcher;
import android.text.TextUtils;
import android.text.method.KeyListener;
import android.text.method.MovementMethod;
import android.text.method.TransformationMethod;
import android.widget.TextView;
import com.omarea.ui.CpuChartBarView;
import io.github.libxposed.api.XposedModule;
import java.util.concurrent.atomic.AtomicReference;

public final class MiniRenderContractTest {
    private static int cases,assertions;
    private interface Check {void run() throws Throwable;}
    private static void require(boolean ok,String message){assertions++;if(!ok)throw new AssertionError(message);}
    private static void test(String name,Check check)throws Throwable{check.run();cases++;System.out.println("PASS "+name);}
    private static final class Fixture implements AutoCloseable {
        final XposedModule module=new XposedModule();
        final MiniRenderDeduplicator guard;
        Fixture()throws Exception{
            fi0.MODULE=module;TextView.MODULE=module;CpuChartBarView.MODULE=module;
            guard=new MiniRenderDeduplicator(module,MiniRenderContractTest.class.getClassLoader());guard.install();
        }
        fi0 owner(){fi0 owner=new fi0();fi0.C=owner.c;return owner;}
        void enable(){guard.setEnabled(true);}
        void render(bi0 render)throws Throwable{
            render.run();fi0 o=render.c;
            o.c.fixtureLayout();o.d.fixtureLayout();o.e.fixtureLayout();o.f.fixtureLayout();o.g.fixtureLayout();o.h.fixtureLayout();o.i.fixtureLayout();
        }
        @Override public void close(){guard.close();fi0.MODULE=null;TextView.MODULE=null;CpuChartBarView.MODULE=null;fi0.C=null;}
    }
    private static final class SpanText implements Spanned {
        private final String value;
        SpanText(String text){value=text;}
        public int length(){return value.length();}public char charAt(int i){return value.charAt(i);}public CharSequence subSequence(int a,int b){return value.subSequence(a,b);}
        @Override public String toString(){return value;}
    }
    private static final class CustomTextView extends TextView {
        int changes;
        @Override protected void onTextChanged(CharSequence value,int start,int before,int count){changes++;}
    }
    private interface TextSetup {void apply(TextView text);}
    private static void textMustPass(String name,TextSetup setup)throws Throwable{
        test(name,()->{try(Fixture f=new Fixture()){
            fi0 owner=f.owner();f.enable();bi0 render=new bi0(owner);f.render(render);
            int before=owner.d.originalCalls;setup.apply(owner.d);f.render(render);
            require(owner.d.originalCalls==before+1,"unsafe TextView configuration must call original");
        }});
    }
    public static void main(String[] args)throws Throwable {
        test("observation preserves every original text/chart submission",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();bi0 r=new bi0(o);f.render(r);f.render(r);
            require(o.d.originalCalls==2&&o.g.originalCalls==2&&o.h.originalCalls==2&&o.i.originalCalls==2,"observation text calls");
            require(o.e.originalDataCalls==2&&o.e.invalidations==2,"observation chart calls");
        }});
        test("first update executes before later identical submissions are omitted",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();o.d.fixtureText("1000",TextView.BufferType.NORMAL);o.e.n=2;o.e.q.add(10);o.e.q.add(20);f.enable();bi0 r=new bi0(o);
            f.render(r);require(o.d.originalCalls==1&&o.e.originalDataCalls==1,"first submission always original");
            f.render(r);require(o.d.originalCalls==1&&o.e.originalDataCalls==1,"stable identical calls omitted");
            require(o.f.historyCalls==2&&o.f.invalidations==2,"GPU history always advances");
        }});
        test("mutable shared CPU array is compared by current values",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);Integer[] same=o.x;f.render(r);same[0]=90;f.render(r);
            require(o.e.originalDataCalls==2,"in-place array mutation not hidden by identity cache");
            require(o.e.q.peek()==90,"new CPU value submitted");
            o.x=new Integer[]{90,20};f.render(r);require(o.e.originalDataCalls==2,"same values in new array can omit");
        }});
        test("external queue mutation forces restoring the complete CPU data",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);o.e.q.clear();o.e.q.add(99);f.render(r);
            require(o.e.originalDataCalls==2,"actual queue checked");require(o.e.q.size()==2&&o.e.q.peek()==10,"queue restored");
        }});
        test("CPU style and geometry changes force original invalidation",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);int expected=1;
            o.e.setAccentColor(0xff00ff00);f.render(r);require(o.e.originalDataCalls==++expected,"accent color");
            o.e.setMinAlpha(10);f.render(r);require(o.e.originalDataCalls==++expected,"min alpha");
            o.e.setMaxAlpha(210);f.render(r);require(o.e.originalDataCalls==++expected,"max alpha");
            o.e.m+=1;f.render(r);require(o.e.originalDataCalls==++expected,"height geometry");
            o.e.r+=1;f.render(r);require(o.e.originalDataCalls==++expected,"bar geometry");
            o.e.l.setFlags(7);f.render(r);require(o.e.originalDataCalls==++expected,"paint flags");
            o.e.l.setStyle(Paint.Style.STROKE);f.render(r);require(o.e.originalDataCalls==++expected,"paint style");
            o.e.l.setStrokeWidth(5);f.render(r);require(o.e.originalDataCalls==++expected,"paint stroke");
            o.e.fixtureSize(500,80);f.render(r);require(o.e.originalDataCalls==++expected,"view size");
        }});
        test("chart n mismatch forces original even when queue values match",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);o.e.n=9;f.render(r);
            require(o.e.originalDataCalls==2&&o.e.n==2,"n reset by original");
        }});
        test("foreign widgets inside Mini render never omit",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();TextView other=new TextView();CpuChartBarView chart=new CpuChartBarView();bi0 r=new bi0(o);
            r.body=()->{other.setText("same");chart.setData(new Integer[]{10,20});};f.render(r);f.render(r);
            require(other.originalCalls==2&&chart.originalDataCalls==2,"foreign identity passes");
        }});
        test("same widgets outside bi0 renderer never omit",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);o.d.setText(r.cpu);o.e.setData(o.x);
            require(o.d.originalCalls==2&&o.e.originalDataCalls==2,"outside render scope passes");
        }});
        test("GPU chart setData is never treated as CPU replacement",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);r.body=()->o.f.setData(new Integer[]{1,2});f.render(r);f.render(r);
            require(o.f.originalDataCalls==2,"GPU identity excluded");
        }});
        test("new owner has independent first successful update",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);f.render(r);fi0 newer=f.owner();bi0 next=new bi0(newer);f.render(next);
            require(newer.d.originalCalls==1&&newer.e.originalDataCalls==1,"new views not pre-cached");
        }});
        test("failed original chart submission is not a successful cache entry",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);IllegalStateException expected=new IllegalStateException("chart failed");o.e.dataFailure=expected;
            try{f.render(r);throw new AssertionError("chart failure swallowed");}catch(IllegalStateException actual){require(actual==expected,"original error preserved");}
            o.e.dataFailure=null;f.render(r);require(o.e.originalDataCalls==2,"retry executes original");f.render(r);require(o.e.originalDataCalls==2,"successful retry permits later omit");
        }});
        test("failed original text submission is not a successful cache entry",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);IllegalStateException expected=new IllegalStateException("text failed");o.d.failure=expected;
            try{f.render(r);throw new AssertionError("text failure swallowed");}catch(IllegalStateException actual){require(actual==expected,"original text error preserved");}
            o.d.failure=null;f.render(r);require(o.d.originalCalls==2,"retry executes text original");f.render(r);require(o.d.originalCalls==2,"successful retry permits text omit");
        }});
        test("partial text failure invalidates the earlier successful baseline",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);IllegalStateException expected=new IllegalStateException("failed after text write");
            r.cpu="2000";o.d.failureAfterWrite=expected;
            try{f.render(r);throw new AssertionError("partial text failure swallowed");}catch(IllegalStateException actual){require(actual==expected,"partial failure propagated without retry");}
            require(o.d.originalCalls==2&&o.d.getText().equals("2000"),"fixture partially updated text before throw");
            o.d.failureAfterWrite=null;f.render(r);require(o.d.originalCalls==3,"next same text still executes original");
        }});
        test("partial chart failure invalidates the earlier successful baseline",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);int originalColor=o.e.s;
            IllegalStateException expected=new IllegalStateException("failed before chart invalidate");o.e.s=0xff123456;o.e.dataFailureAfterWrite=expected;
            try{f.render(r);throw new AssertionError("partial chart failure swallowed");}catch(IllegalStateException actual){require(actual==expected,"partial chart failure propagated without retry");}
            require(o.e.originalDataCalls==2&&o.e.invalidations==1,"fixture wrote queue but did not invalidate");
            o.e.s=originalColor;o.e.dataFailureAfterWrite=null;f.render(r);
            require(o.e.originalDataCalls==3&&o.e.invalidations==2,"next same queue/style still executes original");
        }});
        test("dynamic TextWatcher preserves repeated update callbacks",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);int[] calls={0};o.d.addTextChangedListener(new TextWatcher(){
                public void beforeTextChanged(CharSequence x,int a,int b,int c){calls[0]++;}
                public void onTextChanged(CharSequence x,int a,int b,int c){calls[0]++;}
                public void afterTextChanged(Editable x){calls[0]++;}
            });f.render(r);require(o.d.originalCalls==2&&calls[0]==3,"watcher callbacks preserved");
        }});
        textMustPass("non-normal buffer type passes",t->t.fixtureText("1000",TextView.BufferType.SPANNABLE));
        textMustPass("editor state passes",t->t.fixtureEditor(true));
        textMustPass("filter state passes",t->t.setFilters(new InputFilter[]{new InputFilter(){}}));
        textMustPass("movement state passes",t->t.setMovementMethod(new MovementMethod(){}));
        textMustPass("transformation state passes",t->t.setTransformationMethod(new TransformationMethod(){}));
        textMustPass("key listener passes",t->t.setKeyListener(new KeyListener(){}));
        textMustPass("selectable text passes",t->t.setTextIsSelectable(true));
        textMustPass("autoLink passes",t->t.setAutoLinkMask(1));
        textMustPass("error presentation passes",t->t.setError("error"));
        textMustPass("marquee reset on repeated text is preserved",t->t.setEllipsize(TextUtils.TruncateAt.MARQUEE));
        textMustPass("auto-size text update is preserved",t->t.setAutoSizeTextTypeWithDefaults(TextView.AUTO_SIZE_TEXT_TYPE_UNIFORM));
        textMustPass("accessibility live region passes",t->t.setAccessibilityLiveRegion(1));
        textMustPass("accessibility focus passes",t->t.fixtureAccessibilityFocus(true));
        textMustPass("pending text layout passes",TextView::requestLayout);
        textMustPass("text paint change passes",t->t.getPaint().setFlags(3));
        textMustPass("text color change passes",t->t.setTextColor(0xff123456));
        textMustPass("letter spacing change passes",t->t.setLetterSpacing(0.4f));
        textMustPass("text padding change passes",t->t.setPadding(1,2,3,4));
        test("spanned input is never collapsed by same visible characters",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);r.body=()->o.d.setText(new SpanText("same"));f.render(r);f.render(r);
            require(o.d.originalCalls==2,"span-bearing calls preserved");
        }});
        test("mutable input CharSequence is never treated as String",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);StringBuilder text=new StringBuilder("same");r.body=()->o.d.setText(text);f.render(r);f.render(r);
            require(o.d.originalCalls==2,"mutable input preserved");
        }});
        test("TextView subclass onTextChanged still executes",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();CustomTextView custom=new CustomTextView();o.d=custom;f.enable();bi0 r=new bi0(o);f.render(r);f.render(r);
            require(custom.originalCalls==2&&custom.changes==2,"unknown subclass behavior preserved");
        }});
        test("nested render restores outer frame",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);bi0 inner=new bi0(o);bi0 outer=new bi0(o);outer.body=()->{inner.run();o.d.setText(r.cpu);};f.render(outer);
            require(o.d.originalCalls==1,"both inner and restored outer recognize eligible text");
        }});
        test("invalid nested owner shadows rather than inherits the outer frame",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 first=new bi0(o);f.render(first);
            fi0 invalid=new fi0();invalid.d=o.d;invalid.e=o.e;
            bi0 inner=new bi0(invalid);bi0 outer=new bi0(o);outer.body=()->{inner.run();o.d.setText(first.cpu);};
            f.render(outer);
            require(o.d.originalCalls==2,"invalid nested renderer could not borrow outer text identity");
            require(o.e.originalDataCalls==2,"invalid nested renderer could not borrow outer chart identity");
        }});
        test("nested frame-construction failure cannot inherit the outer frame",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 first=new bi0(o);f.render(first);
            bi0 malformed=new bi0(null);malformed.body=()->{o.d.setText(first.cpu);o.e.setData(o.x);};
            bi0 outer=new bi0(o);outer.body=()->{malformed.run();o.d.setText(first.cpu);};f.render(outer);
            require(o.d.originalCalls==2,"construction failure shadowed outer text scope then restored it");
            require(o.e.originalDataCalls==2,"construction failure shadowed outer chart scope");
        }});
        test("renderer TLS does not reach a different thread",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 first=new bi0(o);f.render(first);AtomicReference<Throwable> error=new AtomicReference<>();
            bi0 outer=new bi0(o);outer.body=()->{Thread worker=new Thread(()->{try{o.d.setText(first.cpu);}catch(Throwable t){error.set(t);}});worker.start();worker.join();};f.render(outer);
            require(error.get()==null&&o.d.originalCalls==2,"foreign thread original update preserved");
        }});
        test("render exception releases TLS before unrelated updates",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);IllegalStateException expected=new IllegalStateException("render failed");bi0 bad=new bi0(o);bad.body=()->{throw expected;};
            try{bad.run();throw new AssertionError("render error swallowed");}catch(IllegalStateException actual){require(actual==expected,"render original error preserved");}
            o.d.setText(r.cpu);require(o.d.originalCalls==2,"no stale TLS scope outside failed render");
        }});
        test("disable and unhook preserve original repeat behavior",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);f.guard.setEnabled(false);f.render(r);require(o.d.originalCalls==2&&o.e.originalDataCalls==2,"disabled originals");
            f.guard.close();f.render(r);require(o.d.originalCalls==3&&o.e.originalDataCalls==3,"unhook originals");
        }});
        test("active render veto and resume preserve its scope",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 first=new bi0(o);f.render(first);bi0 r=new bi0(o);
            r.body=()->{require(!f.guard.tryQuiesce(),"active renderer vetoes reload");f.guard.resume();o.d.setText(first.cpu);};f.render(r);
            require(o.d.originalCalls==1,"veto/resume did not lose current render scope");
            require(f.guard.tryQuiesce(),"idle renderer may quiesce");f.guard.resume();
        }});
        test("replacement guard starts with an empty successful-publication cache",()->{try(Fixture f=new Fixture()){
            fi0 o=f.owner();f.enable();bi0 r=new bi0(o);f.render(r);f.render(r);f.guard.close();
            try(MiniRenderDeduplicator next=new MiniRenderDeduplicator(f.module,MiniRenderContractTest.class.getClassLoader())){
                next.install();next.setEnabled(true);f.render(r);
                require(o.d.originalCalls==2&&o.e.originalDataCalls==2,"replacement first update calls original");
                f.render(r);require(o.d.originalCalls==2&&o.e.originalDataCalls==2,"replacement then omits stable repeats");
            }
        }});
        System.out.println("MINI_RENDER_CONTRACTS cases="+cases+" assertions="+assertions+" deviceExecuted=false");
    }
}
