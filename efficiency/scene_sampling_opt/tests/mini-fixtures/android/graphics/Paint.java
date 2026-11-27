package android.graphics;
public class Paint {
    public enum Style { FILL, STROKE, FILL_AND_STROKE }
    private int flags, color=0xff000000, alpha=255;
    private float textSize=16, textScaleX=1, textSkewX, strokeWidth;
    private Typeface typeface;
    private Style style=Style.FILL;
    public int getFlags(){return flags;} public void setFlags(int value){flags=value;}
    public int getColor(){return color;} public void setColor(int value){color=value;}
    public int getAlpha(){return alpha;} public void setAlpha(int value){alpha=value;}
    public float getTextSize(){return textSize;} public void setTextSize(float value){textSize=value;}
    public float getTextScaleX(){return textScaleX;} public void setTextScaleX(float value){textScaleX=value;}
    public float getTextSkewX(){return textSkewX;} public void setTextSkewX(float value){textSkewX=value;}
    public float getStrokeWidth(){return strokeWidth;} public void setStrokeWidth(float value){strokeWidth=value;}
    public Typeface getTypeface(){return typeface;} public Typeface setTypeface(Typeface value){typeface=value;return value;}
    public Style getStyle(){return style;} public void setStyle(Style value){style=value;}
}
