package android.view;
public class View {
    public static final int ACCESSIBILITY_LIVE_REGION_NONE=0;
    public boolean attached=true;
    private boolean layoutRequested, accessibilityFocused;
    private int liveRegion,width=120,height=24,paddingLeft,paddingTop,paddingRight,paddingBottom;
    public boolean isAttachedToWindow(){return attached;}
    public boolean isShown(){return attached;}
    public boolean isLayoutRequested(){return layoutRequested;}
    public void requestLayout(){layoutRequested=true;}
    public void fixtureLayout(){layoutRequested=false;}
    public int getWidth(){return width;} public int getHeight(){return height;}
    public void fixtureSize(int w,int h){width=w;height=h;}
    public int getPaddingLeft(){return paddingLeft;} public int getPaddingTop(){return paddingTop;}
    public int getPaddingRight(){return paddingRight;} public int getPaddingBottom(){return paddingBottom;}
    public void setPadding(int l,int t,int r,int b){paddingLeft=l;paddingTop=t;paddingRight=r;paddingBottom=b;}
    public int getAccessibilityLiveRegion(){return liveRegion;}
    public void setAccessibilityLiveRegion(int value){liveRegion=value;}
    public boolean isAccessibilityFocused(){return accessibilityFocused;}
    public void fixtureAccessibilityFocus(boolean value){accessibilityFocused=value;}
}
