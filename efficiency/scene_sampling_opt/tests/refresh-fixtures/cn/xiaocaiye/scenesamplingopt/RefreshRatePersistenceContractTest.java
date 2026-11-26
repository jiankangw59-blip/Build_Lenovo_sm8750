package cn.xiaocaiye.scenesamplingopt;

import a.cj0;
import a.ed1;
import a.lj0;
import a.mg0;
import a.ri0;
import android.content.Context;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;

/** Runs the production interceptors; fixtures implement only the pinned original DEX behavior. */
public final class RefreshRatePersistenceContractTest {
    private static int cases, assertions;
    private static final Constructor<?> CONSTRUCTOR;
    private static final Method SELECT;
    static {
        try {
            CONSTRUCTOR=cj0.class.getDeclaredConstructor(java.util.List.class,boolean.class,
                    boolean.class,boolean.class,lj0.class,Context.class,String.class,ri0.class);
            SELECT=cj0.class.getDeclaredMethod("i",Object.class);
        } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }
    private static final class Fixture implements AutoCloseable {
        final XposedModule module=new XposedModule();
        final RefreshRatePersistence repair=new RefreshRatePersistence(module,
                RefreshRatePersistenceContractTest.class.getClassLoader());
        final ed1 store=new ed1();
        Fixture() throws Exception { mg0.reset(module); repair.install(); }
        cj0 callback(boolean feature,boolean special,boolean dynamic) throws Throwable {
            return (cj0)module.construct(CONSTRUCTOR,new Object[]{Arrays.asList(0,60,120,144,165),
                    feature,special,dynamic,new lj0(store),new Context(),"test.game",new ri0()});
        }
        void select(cj0 callback,int index) throws Throwable {
            check(module.invoke(SELECT,callback,index)==callback,"original callback result retained");
        }
        JSONObject stats() throws Exception { return repair.snapshot(); }
        @Override public void close() { repair.close(); }
    }
    private static void check(boolean condition,String message) {
        assertions++;
        if (!condition) throw new AssertionError(message);
    }
    private static void put28(ed1 store) {
        for (int i=0;i<27;i++) store.rules.put("fixture.app"+i,new int[]{60,90});
        store.rules.put("test.game",new int[]{120,165});
    }
    private static Map<String,int[]> copy(Map<String,int[]> source) {
        Map<String,int[]> result=new LinkedHashMap<>();
        source.forEach((key,value)->result.put(key,value.clone()));
        return result;
    }
    private static void gateMatrix() throws Throwable {
        for (int mask=0;mask<8;mask++) {
            try (Fixture f=new Fixture()) {
                boolean enabled=(mask&1)!=0,special=(mask&2)!=0,dynamic=(mask&4)!=0;
                cj0 callback=f.callback(enabled,special,dynamic);
                boolean promoted=enabled && !special && !dynamic;
                check(callback.g==(dynamic || promoted),"only eligible false gate promoted");
                check(callback.e==enabled && callback.f==special,"feature/special flags unchanged");
                f.select(callback,3);
                check(callback.manualCalls==1,"all original manual-control calls retained");
                check(f.store.updates==(enabled && !special?1:0),"original write eligibility retained");
                check(f.stats().number("promotedConstructors")== (promoted?1:0),"promotion accounting");
            }
        }
    }
    private static void preservesOther27AndGlobalConfiguration() throws Throwable {
        try (Fixture f=new Fixture()) {
            put28(f.store);
            Map<String,int[]> original=copy(f.store.rules);
            byte[] global="enable=1\napps_default=high\nscreen_off=high\n".getBytes(StandardCharsets.UTF_8);
            mg0.files.put("features/refresh_rate.conf",global.clone());
            cj0 callback=f.callback(true,false,false);
            f.select(callback,3);
            check(f.store.rules.size()==28,"no entries dropped");
            for (Map.Entry<String,int[]> entry:original.entrySet())
                if (!entry.getKey().equals("test.game"))
                    check(Arrays.equals(entry.getValue(),f.store.rules.get(entry.getKey())),"unrelated range retained");
            check(Arrays.equals(f.store.rules.get("test.game"),new int[]{120,144}),"only selected max changed");
            check(Arrays.equals(global,mg0.files.get("features/refresh_rate.conf")),"global file untouched");
            check(f.store.updates==1 && f.store.flushes==1 && mg0.calls.get()==1,"no duplicate supplemental save");
            check(f.stats().number("writesSucceeded")==1,"actual writer success observed");
        }
    }
    private static void dynamicAlreadyOnKeepsOriginalSingleSave() throws Throwable {
        try (Fixture f=new Fixture()) {
            cj0 callback=f.callback(true,false,true);
            f.select(callback,4);
            check(f.stats().number("promotedConstructors")==0,"existing scheduling path unchanged");
            check(f.store.updates==1 && f.store.flushes==1,"existing path saves once");
            check(!Boolean.TRUE.equals(f.stats().get("physicalDisplayVerified")),"file success not physical ACK");
        }
    }
    private static void newMinZeroAndDeletionStayOriginal() throws Throwable {
        try (Fixture f=new Fixture()) {
            cj0 callback=f.callback(true,false,false);
            f.select(callback,4);
            check(Arrays.equals(f.store.rules.get("test.game"),new int[]{0,165}),"new row retains zero min");
            f.select(callback,0);
            check(!f.store.rules.containsKey("test.game"),"zero max retains original deletion semantics");
            check(callback.manualCalls==2 && f.store.updates==2,"original control sequence not replayed");
        }
    }
    private static void synchronousLatestEditWinsWithoutDeferredReplays() throws Throwable {
        try (Fixture f=new Fixture()) {
            put28(f.store);
            cj0 callback=f.callback(true,false,false);
            f.select(callback,3);
            f.select(callback,4);
            check(Arrays.equals(f.store.rules.get("test.game"),new int[]{120,165}),"last user edit retained");
            String content=new String(mg0.files.get("refresh_rate.conf"),StandardCharsets.UTF_8);
            check(content.contains("test.game 120 165\n"),"file reflects last synchronous edit");
            check(f.store.flushes==2 && mg0.calls.get()==2,"no queued old saves");
        }
    }
    private static void writerFalseIsReportedWithoutPretendingSuccess() throws Throwable {
        try (Fixture f=new Fixture()) {
            mg0.writeResult=false;
            cj0 callback=f.callback(true,false,false);
            f.select(callback,4);
            check(f.stats().number("writesFailed")==1 && f.stats().number("writesSucceeded")==0,"false not ACK success");
            check("write_returned_false".equals(f.stats().get("lastError")),"failure diagnostic visible");
            check(!f.store.dirty && callback.manualCalls==1,"original callback return/dirty behavior retained");
            check(!mg0.files.containsKey("refresh_rate.conf"),"no synthetic persistence success");
        }
    }
    private static void writerExceptionPropagatesOriginalIdentity() throws Throwable {
        try (Fixture f=new Fixture()) {
            RuntimeException expected=new IllegalStateException("fixture write failure");
            mg0.writeException=expected;
            cj0 callback=f.callback(true,false,false);
            try { f.select(callback,4); throw new AssertionError("expected original failure"); }
            catch (RuntimeException actual) { check(actual==expected,"original exception preserved"); }
            check(callback.manualCalls==0,"original exception still interrupts original callback");
            check(f.stats().number("writesFailed")==1 && f.stats().number("active")==0,"error unwinds lifecycle");
        }
    }
    private static void unrelatedWritesOutsideSelectionAreNotObserved() throws Throwable {
        try (Fixture f=new Fixture()) {
            mg0.i(new Context(),"refresh_rate.conf",new byte[]{1});
            mg0.i(new Context(),"features/refresh_rate.conf",new byte[]{2});
            check(f.stats().number("writes")==0,"no general file-write observation");
            f.select(f.callback(true,true,false),4);
            check(f.stats().number("selectionCallbacks")==0,"special target excluded");
        }
    }
    private static void nestedSelectionsRestoreTheirThreadLocalScopes() throws Throwable {
        try (Fixture f=new Fixture()) {
            cj0 outer=f.callback(true,false,false);
            cj0 nested=f.callback(true,false,false);
            AtomicBoolean entered=new AtomicBoolean();
            mg0.duringWrite=()->{
                if (entered.compareAndSet(false,true)) {
                    try { f.select(nested,2); }
                    catch (Throwable error) { throw new AssertionError(error); }
                }
            };
            f.select(outer,4);
            check(f.stats().number("writes")==2 && f.stats().number("missingWrites")==0,"nested scopes isolated");
            mg0.duringWrite=null;
            mg0.i(new Context(),"refresh_rate.conf",new byte[]{3});
            check(f.stats().number("writes")==2,"scope removed after return");
        }
    }
    private static void hotReloadVetoesInFlightAndResumesWithoutNewWorkers() throws Throwable {
        try (Fixture f=new Fixture()) {
            cj0 callback=f.callback(true,false,false);
            CountDownLatch inWrite=new CountDownLatch(1),release=new CountDownLatch(1);
            AtomicReference<Throwable> error=new AtomicReference<>();
            mg0.duringWrite=()->{
                inWrite.countDown();
                try { if (!release.await(5,TimeUnit.SECONDS)) throw new AssertionError("test release timeout"); }
                catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new AssertionError(interrupted); }
            };
            Thread worker=new Thread(()->{
                try { f.module.invoke(SELECT,callback,4); } catch (Throwable failure) { error.set(failure); }
            });
            worker.start();
            try {
                check(inWrite.await(5,TimeUnit.SECONDS),"real callback reached writer");
                check(!f.repair.tryQuiesce(),"reload refused while original save active");
            } finally { release.countDown(); worker.join(5000); }
            check(!worker.isAlive() && error.get()==null,"original save completed");
            check(f.repair.tryQuiesce(),"quiesce succeeds after completion");
            check(!f.callback(true,false,false).g,"quiescent generation does not change new callbacks");
            f.repair.resume();
            check(f.callback(true,false,false).g,"resume restores repair");
            check(f.stats().number("periodicTasks")==0,"production helper adds no jobs");
        }
    }
    private static void closeUnhooksFutureCallbacksWithoutRewritingOldFinalFields() throws Throwable {
        try (Fixture f=new Fixture()) {
            cj0 old=f.callback(true,false,false);
            f.repair.close();
            check(f.module.hookCount()==0,"all three hooks removed");
            cj0 fresh=f.callback(true,false,false);
            check(!fresh.g && old.g,"only constructor inputs changed; old final object unchanged on teardown");
            f.select(fresh,4);
            check(f.store.updates==0 && fresh.manualCalls==1,"unhooked new callback is original");
        }
    }
    private static void deoptimizationFailureRemovesPartialHooks() throws Throwable {
        XposedModule module=new XposedModule();
        module.failDeoptimize=true;
        RefreshRatePersistence repair=new RefreshRatePersistence(module,
                RefreshRatePersistenceContractTest.class.getClassLoader());
        try { repair.install(); throw new AssertionError("expected guarded install failure"); }
        catch (IllegalStateException expected) { check(module.hookCount()==0,"partial hooks removed"); }
        check(!Boolean.TRUE.equals(repair.snapshot().get("installed")),"failed helper not reported ready");
    }
    private static void invalidSelectionRetainsOriginalFailureAndReportsMissingWrite() throws Throwable {
        try (Fixture f=new Fixture()) {
            cj0 callback=f.callback(true,false,false);
            try { f.select(callback,99); throw new AssertionError("expected original index failure"); }
            catch (IndexOutOfBoundsException expected) { check(callback.manualCalls==0,"original bounds failure retained"); }
            check(f.stats().number("missingWrites")==1 && f.stats().number("active")==0,"missing write is visible");
        }
    }
    private interface Contract { void run() throws Throwable; }
    private static void run(Contract test) throws Throwable { test.run(); cases++; }
    public static void main(String[] args) throws Throwable {
        run(RefreshRatePersistenceContractTest::gateMatrix);
        run(RefreshRatePersistenceContractTest::preservesOther27AndGlobalConfiguration);
        run(RefreshRatePersistenceContractTest::dynamicAlreadyOnKeepsOriginalSingleSave);
        run(RefreshRatePersistenceContractTest::newMinZeroAndDeletionStayOriginal);
        run(RefreshRatePersistenceContractTest::synchronousLatestEditWinsWithoutDeferredReplays);
        run(RefreshRatePersistenceContractTest::writerFalseIsReportedWithoutPretendingSuccess);
        run(RefreshRatePersistenceContractTest::writerExceptionPropagatesOriginalIdentity);
        run(RefreshRatePersistenceContractTest::unrelatedWritesOutsideSelectionAreNotObserved);
        run(RefreshRatePersistenceContractTest::nestedSelectionsRestoreTheirThreadLocalScopes);
        run(RefreshRatePersistenceContractTest::hotReloadVetoesInFlightAndResumesWithoutNewWorkers);
        run(RefreshRatePersistenceContractTest::closeUnhooksFutureCallbacksWithoutRewritingOldFinalFields);
        run(RefreshRatePersistenceContractTest::deoptimizationFailureRemovesPartialHooks);
        run(RefreshRatePersistenceContractTest::invalidSelectionRetainsOriginalFailureAndReportsMissingWrite);
        System.out.println("REFRESH_PERSISTENCE_CONTRACTS cases="+cases+" assertions="+assertions);
    }
}
