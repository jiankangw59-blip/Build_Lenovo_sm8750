package cn.xiaocaiye.scenesamplingopt;

import a.hl1;
import a.hz;
import a.jr1;
import a.pi0;
import a.qi0;
import android.os.Looper;
import io.github.libxposed.api.XposedModule;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;

/** Exercises production hook interceptors; fake Android/Scene model only the pinned ABI contracts. */
public final class ThreadWindowGuardContractTest {
    private static int assertions;
    private static int cases;
    private interface Check { void run() throws Throwable; }
    private static void require(boolean ok,String message) {
        assertions++;
        if (!ok) throw new AssertionError(message);
    }
    private static void test(String name,Check action) throws Throwable {
        action.run(); cases++; System.out.println("PASS " + name);
    }
    private static final class Fixture implements AutoCloseable {
        final XposedModule module = new XposedModule();
        final ThreadWindowGuard guard;
        Fixture() throws Exception {
            Looper.getMainLooper();
            Looper.drain();
            qi0.x=null; qi0.y=null; qi0.w=0; qi0.MODULE=module;
            guard=new ThreadWindowGuard(module,ThreadWindowGuardContractTest.class.getClassLoader());
            guard.install();
        }
        void enable() { guard.setEnabled(true); Looper.drain(); }
        @Override public void close() { guard.close(); Looper.drain(); qi0.MODULE=null; }
    }
    private static qi0 visibleOwner() {
        qi0 owner=new qi0(); owner.c.attach(); qi0.y=owner.c; return owner;
    }
    private static hl1 liveJob() { hl1 job=new hl1(); qi0.x=job; return job; }

    public static void main(String[] args) throws Throwable {
        test("observation mode leaves original close behavior",() -> {
            try(Fixture f=new Fixture()) {
                require(!f.guard.snapshot().optBoolean("enabled"),"default observe mode");
                qi0 owner=visibleOwner(); hl1 old=liveJob(); owner.b();
                require(!old.active,"original close cancels old job");
                require(qi0.x!=null && qi0.x.active,"observation preserves original restart bug");
                require(owner.starts==1,"one original restart");
                require(qi0.y==null,"original view removal preserved");
            }
        });
        test("close cancels without restarting and still removes view",() -> {
            try(Fixture f=new Fixture()) {
                qi0 owner=visibleOwner(); hl1 old=liveJob(); f.enable(); owner.b();
                require(!old.active,"old job cancelled");
                require(qi0.x==null,"no resurrected job");
                require(owner.starts==0,"close e suppressed");
                require(qi0.y==null && !owner.c.attached,"view removed by original close");
                require(owner.o==1000 && !owner.l,"original list reset still ran");
            }
        });
        test("screen-off style close of fresh unattached owner creates no job",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 fresh=new qi0(); fresh.b();
                require(qi0.x==null,"no ghost job from new owner's close");
                require(fresh.starts==0,"no starts");
            }
        });
        test("opening starts once after attachment",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.c("test.pkg",0);
                require(owner.starts==0,"addView return is not treated as attachment");
                require(qi0.y==owner.c && !owner.c.attached,"current view awaits asynchronous attachment");
                require(owner.c.attachListenerCount()==1,"one attachment event registered");
                owner.c.attach(); Looper.drain();
                require(owner.starts==1,"only final attached start allowed");
                require(qi0.x!=null && qi0.x.active,"visible owner has job");
                require(qi0.y==owner.c && owner.c.attached,"current attached view");
                require(owner.o==1000,"list period preserved");
                require(owner.c.attachListenerCount()==0,"one-shot listener removed after use");
            }
        });
        test("visible detail and return-to-list keep original periods",() -> {
            try(Fixture f=new Fixture()) {
                qi0 owner=visibleOwner(); f.enable(); owner.l=true; owner.o=250; owner.e();
                hl1 detail=qi0.x;
                require(detail!=null && detail.active && owner.o==250,"detail period unchanged");
                owner.d();
                require(!detail.active,"return list original cancellation");
                require(qi0.x!=null && qi0.x.active,"list restarted while visible");
                require(owner.o==1000 && !owner.l,"list reset preserved");
            }
        });
        test("stale owner cannot restart or cancel current owner's job",() -> {
            try(Fixture f=new Fixture()) {
                qi0 current=visibleOwner(); hl1 currentJob=liveJob(); qi0 old=new qi0(); f.enable();
                old.e();
                require(qi0.x==currentJob && currentJob.active,"stale start did not cancel new job");
                require(old.starts==0,"stale start skipped");
                Object result=qi0.a(old,new hz(){});
                require(result==jr1.a,"fresh stale sample returns real Unit");
                require(old.samplingCalls==0,"stale sample sends no request");
                require(qi0.x==currentJob && currentJob.active,"sample rejection preserved newer job");
                require(qi0.y==current.c,"new window preserved");
            }
        });
        test("fresh attached sample and original failure are preserved",() -> {
            try(Fixture f=new Fixture()) {
                qi0 owner=visibleOwner(); f.enable();
                require(qi0.a(owner,new hz(){})==jr1.a,"normal sample result");
                require(owner.samplingCalls==1,"one original sampling call");
                IllegalStateException expected=new IllegalStateException("original sampling failure");
                owner.sampleFailure=expected;
                try { qi0.a(owner,new hz(){}); throw new AssertionError("failure swallowed"); }
                catch(IllegalStateException actual) { require(actual==expected,"same original failure"); }
            }
        });
        test("stale coroutine resume still runs original completion and exception path",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 stale=new qi0(); pi0 continuation=new pi0(); continuation.i=Integer.MIN_VALUE|1;
                require(qi0.a(stale,continuation)==jr1.a,"successful resume returns original result");
                require(stale.resumeCalls==1 && stale.samplingCalls==0,"resume not treated as new sampling");
                IllegalStateException expected=new IllegalStateException("resumed cancellation or failure");
                continuation.g=expected;
                try { qi0.a(stale,continuation); throw new AssertionError("resume failure swallowed"); }
                catch(IllegalStateException actual) { require(actual==expected,"resume original error preserved"); }
            }
        });
        test("close finally releases suppression after original exception",() -> {
            try(Fixture f=new Fixture()) {
                qi0 owner=visibleOwner(); f.enable(); IllegalStateException expected=new IllegalStateException("remove failed");
                owner.closeFailure=expected;
                try { owner.b(); throw new AssertionError("close failure swallowed"); }
                catch(IllegalStateException actual) { require(actual==expected,"original close failure"); }
                owner.closeFailure=null; owner.e();
                require(owner.starts==1,"later independent visible start is allowed");
                require(qi0.x!=null && qi0.x.active,"suppression did not leak");
            }
        });
        test("enabling removes existing unattached ghost only",() -> {
            try(Fixture f=new Fixture()) {
                hl1 ghost=liveJob(); f.enable();
                require(!ghost.active && ghost.cancelCalls==1,"only known ghost cancelled once");
                require(qi0.x==null,"cancelled ghost reference cleared");
            }
        });
        test("queued cleanup rechecks attachment and protects newly opened window",() -> {
            try(Fixture f=new Fixture()) {
                hl1 ghost=liveJob(); AtomicReference<Throwable> error=new AtomicReference<>();
                Thread worker=new Thread(() -> { try { f.guard.setEnabled(true); } catch(Throwable t) { error.set(t); } });
                worker.start(); worker.join(); require(error.get()==null,"background enable succeeded");
                require(ghost.active,"background thread did not cancel before main turn");
                qi0 owner=visibleOwner(); hl1 replacement=liveJob(); Looper.drain();
                require(qi0.x==replacement && replacement.active,"new visible job protected");
                require(qi0.y==owner.c,"new view protected");
            }
        });
        test("disabled guard leaves later callbacks alone",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); f.guard.setEnabled(false); qi0 owner=new qi0(); owner.e();
                require(owner.starts==1 && qi0.x!=null,"disabled starts use original behavior");
                require(qi0.a(owner,new hz(){})==jr1.a && owner.samplingCalls==1,"disabled samples use original behavior");
            }
        });
        test("unhook restores original methods",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); f.guard.close(); qi0 owner=new qi0(); owner.e();
                require(owner.starts==1,"hook removed");
                require(qi0.a(owner,new hz(){})==jr1.a && owner.samplingCalls==1,"sample hook removed");
            }
        });
        test("nested closes restore the full suppression scope",() -> {
            try(Fixture f=new Fixture()) {
                qi0 outer=visibleOwner(); qi0 inner=new qi0(); f.enable();
                outer.onCloseReset=() -> {
                    try { inner.b(); }
                    catch(Throwable error) { throw new AssertionError(error); }
                };
                outer.b();
                require(outer.starts==0 && inner.starts==0,"both nested restarts blocked");
                require(qi0.x==null,"nested closes leave no ghost");
                qi0 current=visibleOwner(); current.e();
                require(current.starts==1 && qi0.x.active,"scope restored after outermost close");
            }
        });
        test("pending cleanup veto does not strand its main-thread event",() -> {
            try(Fixture f=new Fixture()) {
                hl1 ghost=liveJob();
                Thread worker=new Thread(() -> f.guard.setEnabled(true)); worker.start(); worker.join();
                require(f.guard.snapshot().optBoolean("cleanupPending"),"cleanup queued");
                require(!f.guard.tryQuiesce(),"reload vetoes while cleanup pending");
                require(!f.guard.snapshot().optBoolean("quiescing"),"veto did not quiesce callback");
                Looper.drain();
                require(!ghost.active && qi0.x==null,"pending cleanup ran before any resume");
                require(!f.guard.snapshot().optBoolean("cleanupPending"),"pending state cleared");
            }
        });
        test("active close veto keeps nested interception active",() -> {
            try(Fixture f=new Fixture()) {
                qi0 owner=visibleOwner(); f.enable(); AtomicBoolean veto=new AtomicBoolean();
                owner.onCloseReset=() -> veto.set(!f.guard.tryQuiesce()); owner.b();
                require(veto.get(),"active close vetoed reload");
                require(!f.guard.snapshot().optBoolean("quiescing"),"veto preserves interception");
                require(owner.starts==0 && qi0.x==null,"nested e still suppressed");
            }
        });
        test("successful quiesce can be resumed without losing enabled policy",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); require(f.guard.tryQuiesce(),"idle can quiesce");
                require(f.guard.snapshot().optBoolean("quiescing"),"state reports quiesced");
                f.guard.resume(); qi0 stale=new qi0(); qi0.a(stale,new hz(){});
                require(stale.samplingCalls==0,"policy restored after resume");
                require(f.guard.snapshot().optBoolean("enabled"),"enabled flag retained");
            }
        });
        test("cleanup preserves a job synchronously replaced during cancellation",() -> {
            try(Fixture f=new Fixture()) {
                hl1 ghost=liveJob(); AtomicReference<hl1> replacement=new AtomicReference<>();
                ghost.onCancel=() -> { visibleOwner(); replacement.set(liveJob()); };
                f.enable();
                require(!ghost.active,"old orphan cancelled");
                require(replacement.get()!=null && replacement.get().active,"replacement remains active");
                require(qi0.x==replacement.get(),"slot compare protected new reference");
            }
        });
        test("disable before queued cleanup prevents mutation and allows later reenable",() -> {
            try(Fixture f=new Fixture()) {
                hl1 ghost=liveJob(); Thread worker=new Thread(() -> f.guard.setEnabled(true));
                worker.start(); worker.join(); f.guard.setEnabled(false); Looper.drain();
                require(ghost.active && qi0.x==ghost,"disabled callback did not mutate job");
                require(!f.guard.snapshot().optBoolean("cleanupPending"),"aborted event cleared pending");
                f.enable(); require(!ghost.active && qi0.x==null,"new enable event cleans orphan");
            }
        });
        test("synchronous attachment still starts exactly once",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.attachSynchronously=true; owner.c("test.pkg",0);
                require(owner.starts==1 && qi0.x!=null && qi0.x.active,"synchronous path starts normally");
                require(owner.c.attachListenerCount()==0,"no unnecessary listener after synchronous attach");
            }
        });
        test("close before attachment removes deferred start",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.c("test.pkg",0);
                require(owner.c.attachListenerCount()==1,"awaiting attachment");
                owner.b();
                require(owner.c.attachListenerCount()==0,"close removed listener");
                owner.c.attach(); Looper.drain();
                require(owner.starts==0 && qi0.x==null,"late attach cannot resurrect closed job");
            }
        });
        test("replacement owner invalidates the older deferred listener",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 old=new qi0(); old.c("old.pkg",0); qi0 current=new qi0(); current.c("new.pkg",0);
                require(old.c.attachListenerCount()==0,"old listener removed when window replaced");
                require(current.c.attachListenerCount()==1,"new owner awaits attachment");
                old.c.attach(); Looper.drain();
                require(old.starts==0 && qi0.x==null,"old late attach cannot start");
                current.c.attach(); Looper.drain();
                require(current.starts==1 && qi0.x!=null && qi0.x.active,"new owner starts once");
                require(qi0.y==current.c,"new owner still current");
            }
        });
        test("attachment callback is one-shot across detach and reattach",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.c("test.pkg",0); owner.c.attach(); Looper.drain();
                hl1 job=qi0.x; require(owner.starts==1,"first attach started");
                owner.c.detach(); owner.c.attach(); Looper.drain();
                require(owner.starts==1 && qi0.x==job && job.active,"listener did not remain as restart loop");
                require(owner.c.attachListenerCount()==0,"no listener after first use");
            }
        });
        test("disabling while awaiting attach explicitly vetoes without losing startup",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.c("test.pkg",0);
                try { f.guard.setEnabled(false); throw new AssertionError("disable should veto while attaching"); }
                catch(IllegalStateException expected) { require("thread_window_attaching".equals(expected.getMessage()),"explicit attaching veto"); }
                require(f.guard.snapshot().optBoolean("enabled"),"veto did not change feature state");
                require(owner.c.attachListenerCount()==1,"veto retained listener");
                owner.c.attach(); Looper.drain();
                require(owner.starts==1 && qi0.x.active,"legitimate pending open still starts");
                f.guard.setEnabled(false); require(!f.guard.snapshot().optBoolean("enabled"),"can disable after attachment");
                owner.e(); require(owner.starts==2,"later explicit start uses original disabled behavior");
            }
        });
        test("unhook while awaiting attach removes its listener",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.c("test.pkg",0); f.guard.close(); Looper.drain();
                require(owner.c.attachListenerCount()==0,"unhook removed old-classloader listener");
                owner.c.attach(); Looper.drain();
                require(owner.starts==0 && qi0.x==null,"late attach cannot execute closed guard callback");
            }
        });
        test("pending attach veto preserves its future start event",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.c("test.pkg",0);
                require(!f.guard.tryQuiesce(),"reload veto while attachment is pending");
                require(!f.guard.snapshot().optBoolean("quiescing"),"pending-attach veto did not quiesce listener");
                owner.c.attach(); Looper.drain();
                require(owner.starts==1 && qi0.x.active,"attach event survived veto");
                require(f.guard.tryQuiesce(),"reload allowed after listener completed");
                f.guard.close(); require(owner.c.attachListenerCount()==0,"no listener survives unload");
            }
        });
        test("background arm observes attachment that occurred before its main turn",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 owner=new qi0(); owner.c.addedToWindow(); qi0.y=owner.c;
                AtomicReference<Throwable> error=new AtomicReference<>();
                Thread worker=new Thread(() -> { try { owner.e(); } catch(Throwable t) { error.set(t); } });
                worker.start(); worker.join(); require(error.get()==null,"background deferred request accepted");
                require(owner.starts==0,"no background start before attachment");
                owner.c.attach(); Looper.drain();
                require(owner.starts==1 && qi0.x!=null && qi0.x.active,"main arm saw already-attached owner");
                require(owner.c.attachListenerCount()==0,"no missed-event listener left behind");
            }
        });
        test("enabling keeps a legitimate parented window before first traversal",() -> {
            try(Fixture f=new Fixture()) {
                qi0 owner=new qi0(); owner.c("test.pkg",0); hl1 original=qi0.x;
                require(qi0.y==owner.c && owner.c.getParent()!=null && !owner.c.attached,"original open awaits first traversal");
                require(original!=null && original.active,"original job already started");
                f.enable();
                require(original.active && qi0.x==original,"cleanup kept legitimate addView-before-attach job");
                owner.c.attach(); Looper.drain();
                require(original.active && qi0.x==original,"attachment did not replace original valid job");
            }
        });
        test("listener-removal failure preserves original close and releases suppression",() -> {
            try(Fixture f=new Fixture()) {
                f.enable(); qi0 old=new qi0(); old.c("test.pkg",0);
                require(old.c.attachListenerCount()==1,"pending listener exists");
                old.c.listenerRemovalFailure=new IllegalStateException("framework listener removal failed");
                old.b();
                require(qi0.y==null && qi0.x==null,"original close still completed");
                require(f.guard.snapshot().optLong("errors")==1,"removal failure observable");
                require(f.guard.snapshot().optInt("active")==0,"active scope released");
                qi0 current=visibleOwner(); current.e(); hl1 currentJob=qi0.x;
                require(current.starts==1 && currentJob.active,"suppression did not leak to later visible start");
                old.c.attach(); Looper.drain();
                require(old.starts==0 && qi0.x==currentJob && currentJob.active,"disarmed unremovable listener cannot revive old owner");
            }
        });
        System.out.println("THREAD_GUARD_CONTRACTS cases="+cases+" assertions="+assertions+" deviceExecuted=false");
    }
}
