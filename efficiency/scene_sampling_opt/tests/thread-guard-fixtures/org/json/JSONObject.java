package org.json;
import java.util.LinkedHashMap;
import java.util.Map;
public class JSONObject {
    public static final Object NULL = new Object();
    private final Map<String,Object> values = new LinkedHashMap<>();
    public JSONObject put(String name, Object value) { values.put(name, value); return this; }
    public Object get(String name) { if (!values.containsKey(name)) throw new IllegalArgumentException(name); return values.get(name); }
    public Object opt(String name) { return values.get(name); }
    public boolean has(String name) { return values.containsKey(name); }
    public boolean optBoolean(String name) { return optBoolean(name, false); }
    public boolean optBoolean(String name, boolean fallback) { Object v=values.get(name); return v instanceof Boolean ? (Boolean)v : fallback; }
    public int optInt(String name) { Object v=values.get(name); return v instanceof Number ? ((Number)v).intValue() : 0; }
    public long optLong(String name) { Object v=values.get(name); return v instanceof Number ? ((Number)v).longValue() : 0; }
    public String optString(String name) { Object v=values.get(name); return v == null ? "" : v.toString(); }
    public JSONObject getJSONObject(String name) { return (JSONObject)get(name); }
    @Override public String toString() { return values.toString(); }
}
