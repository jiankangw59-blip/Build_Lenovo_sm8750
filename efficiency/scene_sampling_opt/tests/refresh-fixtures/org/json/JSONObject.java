package org.json;
import java.util.LinkedHashMap;
import java.util.Map;
public class JSONObject {
    private final Map<String,Object> values=new LinkedHashMap<>();
    public JSONObject put(String key,Object value) { values.put(key,value); return this; }
    public Object get(String key) { return values.get(key); }
    public long number(String key) { return ((Number)get(key)).longValue(); }
}
