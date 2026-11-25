# 26项state的native配置假设核对

本轮找到且只找到一项错误的无条件native=y预期：`crystal_sddc_wb_sparse_state_test`。在当前NATIVE=n配置下，稀疏wb_state不应暴露成snapshot身份，但旧用例无条件期待DELTA/epoch17/尺寸与hash；`snapshot_locked`在n时直接返回原resident mutation_seq和NONE。安装wb_state的helper不创建resident slot state，不能令旧预期成立。

独立`../sddc-kunit-config.patch`只改该KUnit函数体，保持安装、snapshot匹配、erase、XA为空和释放检查；设置resident mutation_seq=5，与WB epoch17区分。y保留原全部五项字段断言；n断言mutation_seq=5、kind=NONE、cookie/size/hash均0，不跳过测试，不改变生产函数、原始source、已有integrity/stats patch或profile。

## 全部26项核对

| 用例（省略共同crystal_sddc_前缀） | 数量 | native假设核对 |
| --- | ---: | --- |
| delta_proof_identity_test、resident_hash_error_test | 2 | proof谓词与真实resident REF读取；不需要native WB |
| ref_publish_test、ref_generation_test、ref_reservation_pin_test、slot_state_ref_obj_pin_test | 4 | 通用引用/cookie/pin；无native条件 |
| snapshot_identity_test | 1 | 装入resident state，snapshot在native条件之前返回；n/y同样有效 |
| slot_owner_test | 1 | 释放resident所有权；该私有ctx无已发布wb_state |
| stale_job_key_test | 1 | resident job key与mutation/handle等一致性，不要求WB身份 |
| sparse_reservation_test | 1 | resident reservation，未安装wb_state，n/y均应NONE |
| wb_sparse_state_test | 1 | 唯一错误y假设，已分n/y断言修正 |
| wb_abort_reservation_test | 1 | 直接检查未发布WB reservation清理helper，不依赖native入口开启 |
| reservation_job_test | 1 | resident reservation/job拒绝逻辑 |
| mutation_wrap_test | 1 | mutation跳过0，与native无关 |
| flatten_snapshot_test | 1 | resident快照及无效存储路径，不要求native capture/install |
| delta_header_test | 1 | delta/alias v1/v2解析helper本身不按native配置门控，可在n检查格式；不等于实际落盘 |
| manager_admission_test | 1 | stop/admission与active_ops，不要求native |
| observe_bitmap_test、observe_discard_test | 2 | resident观察队列状态 |
| index_quality_test、similarity_test、sample_eligibility_test、delta_admission_test | 4 | 通用采样、匹配、准入及引用质量 |
| release_null_test、release_zero_capture_test、release_aborts_unpublished_test | 3 | config无关native release/abort helper，模拟token/reservation；不是成功native事务 |
| **合计** | **26** | 另有6项codec测试，声明共32 |

核对方法：枚举integrity后26项实际KUNIT_CASE及其调用链，再查整份SDDC源两处NATIVE配置条件（snapshot读取wb_state、native_wb_kind）。除上面唯一case外，其余路径要么先返回resident state、没有WB state，要么只测试不按该配置门控的内部helper。没有通过打开native配置改变结论。

## 固定SHA与叠加阶段

- 目标：`drivers/crystalfrostwork/crystal_hybridswap/sddc/crystal_sddc.c`
- 原始 → 已有integrity：`7f0b3dc9c0cc9a0ce54d3aa5dbc05aac2bb464f7778969281112981055a9af2d` → `e51140d12d447b21dc61101f819ad764a60d84ca949c8956a9a55f8095f6aa05`
- 新测试patch SHA256：`570bd70e0866ab4ac97eb2a04dc0e8d0ff64b55a05941b1281d692f16843c766`
- integrity → 测试patch：`e51140d12d447b21dc61101f819ad764a60d84ca949c8956a9a55f8095f6aa05` → `fe72aa7337b979eea9c6a296531bb2f79e0663fa56cfc966bca3032391e2f02f`
- 若另有stats：先integrity→stats得到`ed2e2a521865eced8bffac5629b5d1d72bb0e44f69ce19a90f3fddc2fd4f58a8`，再用同一测试patch得到`572cf6a8e82efbd24358e85ac6cddbff0806efdcd54fac43f1994000973efaab`。测试与stats修改不同函数，但集成器必须按所选链校验阶段SHA，不能向已有stats元数据偷换e511输入。

两个基底均已用GNU patch -p1 --fuzz=0应用确认。精确元数据为`sddc-kunit-config.json`，生产代码逐字节不变的检查也通过。

## host证据边界

`verify_kunit_config_host.py`提取真实snapshot_locked、snapshot_matches_locked及该测试函数，LLVM22分别以NATIVE=0/1编译运行：旧测试n有5项断言失败、y为0；新测试n/y均为0。XArray、锁、分配及KUnit断言框架使用私有stub，所以这是实际函数分支/测试预期检查，不是Linux KUnit运行，不证明并发、ZMS或BIO正确。

证据位于audit的`crystal-reverse-review/sddc-kunit-config-host-v1/kunit-config-checks.json`。本轮未编译完整内核、启动测试、ADB或改profile。32仍只是26 state+6 codec的库存，必须启动取得对应boot/notes的全部KTAP结果后才能称32项通过，更不能称32项native落盘测试。
