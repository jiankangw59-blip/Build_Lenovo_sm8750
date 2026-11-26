package com.omarea.ui;

import android.view.View;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.LinkedBlockingQueue;
import android.graphics.Paint;

public final class CpuChartBarView extends View {
    public static XposedModule MODULE;
    public Paint l=new Paint();
    public float m=24;
    public int n=5;
    public int o=35;
    public int p=255;
    public final LinkedBlockingQueue<Integer> q=new LinkedBlockingQueue<>();
    public float r=10;
    public int s=579373192;
    public int originalDataCalls;
    public int historyCalls;
    public int invalidations;
    public RuntimeException dataFailure;
    public RuntimeException dataFailureAfterWrite;
    public void setData(Integer[] values) throws Throwable {
        Method method=CpuChartBarView.class.getDeclaredMethod("setData",Integer[].class);
        XposedModule.Original original=() -> {
            originalDataCalls++;
            if(dataFailure!=null)throw dataFailure;
            if(values==null)throw new NullPointerException("data");
            n=values.length;q.clear();q.addAll(Arrays.asList(values));if(dataFailureAfterWrite!=null)throw dataFailureAfterWrite;invalidate();return null;
        };
        if(MODULE==null)original.call();else MODULE.dispatch(method,this,new Object[]{values},original);
    }
    public void a(float max,float value){historyCalls++;q.add(100-(int)value);if(q.size()>n)q.poll();invalidate();}
    public void invalidate(){invalidations++;}
    public void setAccentColor(int color){s=color;}
    public void setMinAlpha(int alpha){o=alpha;}
    public void setMaxAlpha(int alpha){p=alpha;}
    public void setMaxHistory(int count){n=count;while(q.size()<count)q.add(0);while(q.size()>count)q.poll();r=100f/count;}
}
