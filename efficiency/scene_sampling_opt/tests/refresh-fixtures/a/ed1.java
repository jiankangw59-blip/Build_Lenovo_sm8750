package a;
import android.content.Context;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
/** Original null/zero update and complete-map flush semantics. */
public final class ed1 {
    public final Map<String,int[]> rules=new LinkedHashMap<>();
    public boolean dirty;
    public int updates,flushes;
    public void j(String app,Integer minimum,Integer maximum) {
        updates++; dirty=true;
        if ((minimum!=null && minimum==0) || (maximum!=null && maximum==0)
                || (minimum==null && maximum==null)) { rules.remove(app); return; }
        int[] prior=rules.get(app);
        rules.put(app,new int[]{minimum!=null?minimum:prior!=null?prior[0]:0,
                maximum!=null?maximum:prior!=null?prior[1]:0});
    }
    public void i() {
        if (!dirty) return;
        flushes++;
        StringBuilder text=new StringBuilder();
        for (Map.Entry<String,int[]> entry:rules.entrySet())
            text.append(entry.getKey()).append(' ').append(entry.getValue()[0])
                    .append(' ').append(entry.getValue()[1]).append('\n');
        mg0.i(new Context(),"refresh_rate.conf",text.toString().getBytes(StandardCharsets.UTF_8));
        dirty=false; // Pinned original ignores mg0.i's false return.
    }
}
