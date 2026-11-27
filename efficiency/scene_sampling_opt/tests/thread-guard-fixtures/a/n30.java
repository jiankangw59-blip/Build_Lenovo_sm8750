package a;
public final class n30 {
    private n30() { }
    public static void k(cw0 job) { hl1 j=(hl1)job; j.cancelCalls++; j.active=false; if (j.onCancel!=null)j.onCancel.run(); }
}
