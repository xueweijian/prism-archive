# Tests/suites/ — 一个卡片一个注册文件

**为什么有这个目录**：24 个并行 agent 分支如果都往 `Tests/CMakeLists.txt` 追加注册块，
每一条分支都会在文件尾冲突一次。这里让每个卡片只新增**一个**文件，主清单用 glob 收集。

## 规则

1. 文件名 = 卡片号，例如 `c9-compress.cmake`。
2. 只写你自己的 `add_executable` / `add_test` / `target_link_libraries` / `set_tests_properties`。
3. **不要**修改 `Tests/CMakeLists.txt`，也不要修改别人的 suite 文件。
4. 文件在 `Tests/CMakeLists.txt` 的作用域里被 `include()`，
   所以 `prism_core` / `prism_win_bridge` / `PRISM_BUILD_WIN_LAYER` 等变量和目标都可见。
5. 会 SKIP 的集成测试必须带 `PASS_REGULAR_EXPRESSION`（CONTRACTS D1）。
6. Windows 目标记得 `target_compile_options(... PRIVATE /WX-)`（SDK 头不是 /W4 干净的）。

## 模板

```cmake
# c9-compress: archive creation (blueprint ch.2)
add_executable(core_test_compression_plan test_compression_plan.cpp)
target_link_libraries(core_test_compression_plan PRIVATE prism_core)
add_test(NAME "core.test_compression_plan" COMMAND core_test_compression_plan)

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_test_write_archive win/test_write_archive.cpp)
  target_link_libraries(win_test_write_archive PRIVATE prism_core prism_win_bridge
                        ole32.lib shlwapi.lib)
  target_compile_options(win_test_write_archive PRIVATE /WX-)
  add_test(NAME "win.write_archive" COMMAND win_test_write_archive)
  set_tests_properties(win.write_archive PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL WRITE-ARCHIVE CHECKS PASSED")
endif()
```
