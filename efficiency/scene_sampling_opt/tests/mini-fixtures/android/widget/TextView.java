package android.widget;

import android.text.Editable;
import android.text.InputFilter;
import android.text.TextWatcher;
import android.text.TextUtils;
import android.text.method.KeyListener;
import android.text.method.MovementMethod;
import android.text.method.TransformationMethod;
import android.view.View;
import io.github.libxposed.api.XposedModule;
import java.lang.reflect.Method;
import java.util.ArrayList;
import android.graphics.Typeface;
import android.text.TextPaint;
import android.content.res.ColorStateList;

public class TextView extends View {
    public static final int AUTO_SIZE_TEXT_TYPE_NONE=0;
    public static final int AUTO_SIZE_TEXT_TYPE_UNIFORM=1;
    public enum BufferType { NORMAL, SPANNABLE, EDITABLE }
    public static XposedModule MODULE;
    private CharSequence mText="";
    private BufferType mBufferType=BufferType.NORMAL;
    private ArrayList<TextWatcher> mListeners;
    private Object mEditor;
    private InputFilter[] filters=new InputFilter[0];
    private MovementMethod movement;
    private TransformationMethod transformation;
    private KeyListener keyListener;
    private boolean selectable;
    private int autoLinkMask;
    private CharSequence error;
    private final TextPaint paint=new TextPaint();
    private float letterSpacing;
    private String fontFeatures;
    private TextUtils.TruncateAt ellipsize;
    private int autoSizeTextType;
    private ColorStateList textColors=new ColorStateList(0xff000000);
    public int originalCalls;
    public RuntimeException failure;
    public RuntimeException failureAfterWrite;

    public void setText(CharSequence value) throws Throwable {
        Method method=TextView.class.getDeclaredMethod("setText",CharSequence.class);
        XposedModule.Original original=() -> {
            originalCalls++;
            if(failure!=null)throw failure;
            CharSequence normalized=value==null?"":value;
            if(mListeners!=null)for(TextWatcher w:mListeners)w.beforeTextChanged(mText,0,mText.length(),normalized.length());
            mText=normalized;
            if(failureAfterWrite!=null)throw failureAfterWrite;
            onTextChanged(normalized,0,0,normalized.length());
            if(mListeners!=null)for(TextWatcher w:mListeners)w.onTextChanged(normalized,0,0,normalized.length());
            if(mListeners!=null)for(TextWatcher w:mListeners)w.afterTextChanged(getEditableText());
            return null;
        };
        if(MODULE==null)original.call(); else MODULE.dispatch(method,this,new Object[]{value},original);
    }
    protected void onTextChanged(CharSequence value,int start,int before,int count) { }
    public CharSequence getText(){return mText;}
    public void fixtureText(CharSequence text,BufferType type){mText=text;mBufferType=type;}
    public BufferType getBufferType(){return mBufferType;}
    public Editable getEditableText(){return mText instanceof Editable?(Editable)mText:null;}
    public InputFilter[] getFilters(){return filters;}
    public void setFilters(InputFilter[] value){filters=value;}
    public MovementMethod getMovementMethod(){return movement;}
    public void setMovementMethod(MovementMethod value){movement=value;}
    public TransformationMethod getTransformationMethod(){return transformation;}
    public void setTransformationMethod(TransformationMethod value){transformation=value;}
    public KeyListener getKeyListener(){return keyListener;}
    public void setKeyListener(KeyListener value){keyListener=value;}
    public boolean isTextSelectable(){return selectable;}
    public void setTextIsSelectable(boolean value){selectable=value;}
    public int getAutoLinkMask(){return autoLinkMask;}
    public void setAutoLinkMask(int value){autoLinkMask=value;}
    public int getSelectionStart(){return selectable?0:-1;}
    public int getSelectionEnd(){return selectable?mText.length():-1;}
    public void addTextChangedListener(TextWatcher watcher){if(mListeners==null)mListeners=new ArrayList<>();mListeners.add(watcher);}
    public void removeTextChangedListener(TextWatcher watcher){if(mListeners!=null)mListeners.remove(watcher);}
    public void fixtureEditor(boolean value){mEditor=value?new Object():null;}
    public CharSequence getError(){return error;} public void setError(CharSequence value){error=value;}
    public TextPaint getPaint(){return paint;}
    public float getLetterSpacing(){return letterSpacing;} public void setLetterSpacing(float value){letterSpacing=value;}
    public String getFontFeatureSettings(){return fontFeatures;} public void setFontFeatureSettings(String value){fontFeatures=value;}
    public TextUtils.TruncateAt getEllipsize(){return ellipsize;} public void setEllipsize(TextUtils.TruncateAt value){ellipsize=value;}
    public int getAutoSizeTextType(){return autoSizeTextType;} public void setAutoSizeTextTypeWithDefaults(int value){autoSizeTextType=value;}
    public ColorStateList getTextColors(){return textColors;}
    public int getCurrentTextColor(){return textColors.getDefaultColor();}
    public void setTextColor(int value){textColors=new ColorStateList(value);paint.setColor(value);}
    public float getTextSize(){return paint.getTextSize();} public void setTextSize(float value){paint.setTextSize(value);}
    public float getTextScaleX(){return paint.getTextScaleX();} public void setTextScaleX(float value){paint.setTextScaleX(value);}
    public Typeface getTypeface(){return paint.getTypeface();} public void setTypeface(Typeface value){paint.setTypeface(value);}
}
