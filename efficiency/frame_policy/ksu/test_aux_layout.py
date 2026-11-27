"""Cross-check shared UAPI declarations and real JS bytes without a C runtime.

This is a structural decoder, not the Linux production decoder/ioctl tests.
"""
import ctypes as c
import json
from pathlib import Path
import re
import subprocess

HERE=Path(__file__).resolve().parent
U,S,Q=c.c_uint32,c.c_int32,c.c_uint64


class Additive(c.LittleEndianStructure):
    _fields_=[("threshold_khz",U),("offset_khz",S)]


class Cpu(c.LittleEndianStructure):
    _fields_=[("valid_mask",U),("up_us",U),("down_us",U),("hispeed_khz",U),("walt_boost",S),
              ("core_enable",U),("min_cpus",U),("max_cpus",U),("excludes_mask",Q),("prefer",U),
              ("limiter_cluster_count",U),("limiter_clusters_packed",U),("feedback_mask",U),
              ("perfect_khz",U),("smoothness",U),("mt_milli_percent",U),("additive_count",U),("additive",Additive*8)]


class Options(c.LittleEndianStructure):
    _fields_=[(name,U) for name in ("valid_mask","source","stat_source","gesture_boost","gesture_duration_ms",
              "launcher_boost","perf_notify","perf_notify_in_apps","perf_use_presets","kswapd_boost","panel_peak_hz","gpu_lock")]


class Profile(c.LittleEndianStructure):
    _fields_=[("cpus",Cpu*4)]


class Rule(c.LittleEndianStructure):
    _fields_=[("package",c.c_char*128),("profiles",U*3)]


class Table(c.LittleEndianStructure):
    _fields_=[("abi",U),("count",U),("cpu_count",U),("profile_count",U),("cpu_ids",U*4),("defaults",U*3),
              ("reserved",U),("expected_scene_epoch",Q),("fds",(S*7)*4),("options",Options),
              ("profiles",Profile*32),("rules",Rule*64)]


class Request(c.LittleEndianStructure):
    _fields_=[("abi",U),("flags",U),("length",U),("reserved",U),("user_ptr",Q)]


def fields_in(header, name):
    body=re.search(r"struct "+name+r"\s*\{(.*?)\};",header,re.S).group(1)
    return re.findall(r"(?:__u32|__s32|__u64|struct\s+\w+)\s+(\w+)(?:\[[^]]+\])*\s*;",body)


def main():
    raw=(HERE.parent/"scene_aux_uapi.h").read_text(encoding="utf-8")
    header=re.sub(r"/\*.*?\*/","",raw,flags=re.S)
    for name,kind in (("fp_scene_aux_cpu",Cpu),("fp_scene_aux_options",Options),
                      ("fp_scene_aux_table",Table),("fp_scene_aux_request",Request)):
        assert fields_in(header,name)==[name for name,_ in kind._fields_],name
    assert c.sizeof(Cpu)==136 and c.sizeof(Options)==48 and c.sizeof(Profile)==544
    assert c.sizeof(Table)==26584 and c.sizeof(Request)==24
    assert (Table.fds.offset,Table.options.offset,Table.profiles.offset,Table.rules.offset)==(56,168,216,17624)
    assert Options.gpu_lock.offset==44 and Table.options.offset+Options.gpu_lock.offset==212
    assert re.search(r"#define FP_SCENE_AUX_SIZE 26584U",header)
    assert re.search(r"#define FP_AUX_OPT_GPU_LOCK \(1U << 10\)",header)
    script=r'''
const fs=require('fs');const p=require('./webroot/protocol.js').frameSettingsProtocol;
const load=n=>JSON.parse(fs.readFileSync('./defaults/'+n+'.json','utf8'));
const s={...load('settings'),sceneMode:'powersave',sceneAux:true};
process.stdout.write(Buffer.from(p(s,load('seed'),load('scene-seed'),load('scene-aux-seed')).aux));
'''
    result=subprocess.run(["node","-e",script],cwd=HERE,capture_output=True,check=True,timeout=15)
    assert len(result.stdout)==c.sizeof(Table)
    table=Table.from_buffer_copy(result.stdout)
    assert table.abi==1 and table.expected_scene_epoch==0
    assert all(value==-1 for cpu in table.fds for value in cpu)
    assert table.options.valid_mask==2047 and table.options.gpu_lock==1 and table.options.panel_peak_hz==185
    active=table.profiles[table.defaults[0]].cpus[0]
    assert active.walt_boost==-2 and active.excludes_mask==1 and active.limiter_clusters_packed==0x100
    assert active.additive_count==3
    assert [(s.threshold_khz,s.offset_khz) for s in active.additive[:3]]==[(0,150000),(1152000,100000),(1785600,70000)]
    print("Aux ABI structure + production JS: CPU136/options48/table26584/request24, GPU@212 and signed/additive fields passed")


if __name__=="__main__":main()
