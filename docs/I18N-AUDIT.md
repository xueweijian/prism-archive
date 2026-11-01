# I18N key audit (card C38)

这个文档是 **key 审计的台账**：表格里有哪些 key、代码实际问了哪些 key、还有哪些用户可见
字符串从来没走过 `Translate`。测试 `Tests/test_i18n_audit.cpp` 是它的可执行版本——
台账会过期，测试不会。

## 0. 为什么需要这个审计

C19 建表时证明了「两张表互相一致，并且定义了 `RequiredKeys()` 列出的 surface」。
C27 接线时用 `Tr::Or(key, 代码默认值)` 处理**表里还没有的 key**：`Or()` 走
`Translator::Has()`，**不记 miss**。于是 `menu.file / action.exit / progress.compressed`
三个 key 在表里根本不存在，中文构建里菜单栏照样显示英文，而
`MissingKeys()` 一条都没记——「没有漏翻」这条门禁看不到它。

结论：**`RequiredKeys()` 只能证明人记得的 key；必须再加一条「代码问的 key」的审计。**
这就是 C38 的第 3 项，也是本卡的测试。

## 1. 测试证明了什么

`core.test_i18n_audit` 递归读 `Src/**/*.{hpp,cpp}`，剥掉注释（保留行号），抓三种调用形态

```
tr("key")            tr.Or("key", ...)      tr.Count("key", n)
Translate("key")     TranslateCount("key", n)
```

然后断言：

| # | 断言 | 红了说明什么 |
|---|------|--------------|
| 1 | 扫到 >= 20 个文件、>= 12 个不同 key，且抓到 `menu.file / action.exit / progress.compressed` | 扫描本身失效（路径错、helper 改名）——不是「没问题」 |
| 2 | 每个用到的 key 都在 `en-US.json` | 代码问了一个表里没有的 key |
| 3 | 每个用到的 key 都在 `zh-CN.json` | 同上，指向需要译者动手的那张表 |
| 4 | 两张表 key 集相等 | 一边加了 key 忘了另一边 |
| 5 | 交给 `Tr` 的字面量必须是 key 形状（小写点分 ASCII） | 有人把整句英文塞进 `tr()`：查不到 key，界面直接显示那句「key」 |
| 6 | *只报告不失败*：表里有、却没有调用点的 key | 契约 surface 还没接线（右键菜单 / 侧栏标题今天就是这样） |

第 6 条的当前清单就是 `RequiredKeys()` 里减去被调用点覆盖的部分：右键菜单 8 个
（`ContextMenuModel.hpp` 仍在用自带英文标签，shell 扩展渲染的是它）、侧栏 4 个
（`SidebarView.hpp:314/328/338/344` 直接写死英文）以及还没接线的 capsule/action/error key。

## 2. C38 补的 11 个 key（两表同时）

| key | en-US | zh-CN | 调用点 |
|-----|-------|-------|--------|
| `menu.file` | `&File` | 文件 | Main.cpp:541 |
| `action.exit` | `E&xit` | 退出 | Main.cpp:543 |
| `progress.compressed` | `Compressed` | 已压缩 | Main.cpp:649 |
| `progress.compressFailed` | `Compression failed` | 压缩失败 | Main.cpp:652 |
| `progress.compress.busy` | `Compression is already running` | 正在压缩，请稍后 | Main.cpp:645 |
| `status.refreshDeferred` | `Saved; the view refreshes when the running operation finishes` | 已写入；当前操作结束后刷新 | Main.cpp:777 |
| `error.verify.failed` | `Integrity check failed:` | 校验失败： | Main.cpp:306 |
| `error.engine.noReason` | `(the engine reported no reason)` | （引擎未报告原因） | Main.cpp:320 |
| `error.missingPath` | `This command needs an archive path.` | 这条命令缺少归档路径。 | Main.cpp:434 |
| `error.verb.unsupported` | `This build does not support this command yet:` | 本版本还不支持这个命令： | Main.cpp:270 |
| `error.verb.supported` | `Supported: ...` | 已支持：... | Main.cpp:275 |

同时 `RequiredKeys()` 从 39 扩到 50，`Tests/test_i18n.cpp` 的 census（39 -> 50）同步，
所以「表变大了但契约没跟上」和「契约变大了但表没跟上」两边都会红。

## 3. 还没走翻译的用户可见字符串（清单）

由 `grep 'L"' Src/App Src/PrismWin` 得到，人工剔除字体名/路径/XML/窗口类名。

| 位置 | 字符串 | 说明 / 建议 key |
|------|--------|-----------------|
| Main.cpp:129 | `Archives\0*.zip;*.7z;*.rar\0All files\0*.*\0` | 打开对话框过滤器，用户可见；建议 `dialog.filter.archives` / `dialog.filter.allFiles` |
| Main.cpp:195 | `7z archive\0*.7z\0Zip archive\0*.zip\0All files\0*.*\0` | 保存对话框过滤器；`dialog.filter.7z` / `dialog.filter.zip` |
| Main.cpp:217-234 | 「已依次尝试以下路径：」「修复方法（任选一）：」「程序目录：」等 8 段 | 7z.dll 缺失恢复文案，整段中文硬编码；要 5 个 key，属于一次字符串重写，留给 C34 |
| Main.cpp:460 | `L"7z.dll 加载失败：\n"` | 表里 **已有** `error.engine.load`（`{path}`）却没人用 |
| Main.cpp:473 | `L"Failed to open:\n"` | 中文构建里弹英文；`error.open` 已存在 |
| Main.cpp:491 | 「打开引擎句柄失败：」 | 建议 `error.engine.handle` |
| Main.cpp:610 | `\x89E3\x538B\x88AB\x62D2\x7EDD`（解压被拒绝） | 建议 `error.extract.refused` |
| Main.cpp:748 | `L"\\\\n"` | **双重转义 bug**：渲染成字面 `\\n`，不是换行 |
| Main.cpp:749 | `(engine reported no reason)` | `error.engine.noReason` 已由本卡补上 |
| Main.cpp:778-779 | `L"\\\\u5DF2\\\\u5199\\\\u5165..."` | **双重转义 bug**：`status.refreshDeferred` 缺失时用户看到字面 `\u5DF2...` |
| Main.cpp:784 | `L"\\\\u6253\\\\u5F00\\\\u5931\\\\u8D25"` | 同一个双重转义 bug（`error.open` 的 fallback） |
| Main.cpp:792/795 | `L"共 " + n + L" 项"` | 状态栏行；建议 `status.items.count` = `{n} items` / `共 {n} 项` |
| AppWiring.hpp:246/253/262/272 | 校验完整性 / 解压到当前文件夹 / 解压到子文件夹 / 智能解压 | HUD 标题，中文硬编码；建议 `hud.verify` / `hud.extract.here` / `hud.extract.sub` / `hud.extract.smart` |
| AppWiring.hpp:600-615 | 「已解压」「解压失败」「 个文件」「 盖章失败」「 AMSI 拦截」「 未扫描」 | 状态行合成，中文硬编码；建议 `status.extracted` / `status.extractFailed` / `status.files.suffix` / `status.stampFailed` / `status.amsiBlocked` / `status.amsiUnscanned` |
| CompressDialog.hpp:818/1030/1038/1041/1058/1060/1062 | Create archive / Solid archive / Show password / Encrypt file names / Compression level / Threads / Save to | C18 对话框，整块英文；建议 `dialog.compress.*` |
| CompanionWindow.hpp:74/83/110/114/298-303 | PrismArchive Preview / (large -- preview capped) / Progress / % ETA / kind names / No preview available | C30 预览窗，整块英文；建议 `preview.*` |
| GalleryWindow.hpp:770/776/1220 | 新建文件夹：C9 未接入 / 图标 / %llu bytes | 前两条是 C9 未接线的占位；第三条应走 `status.bytes` |
| GalleryRenderer.hpp:758 | Space close · Esc close · arrows browse | 预览浮层操作提示，英文硬编码 |

## 4. 给 C34 的接线补丁（Main.cpp）

C38 不改 `Main.cpp`（C34 在写）。按下面顺序落地；**新 key 与接线必须同一个提交**，
否则第 6 条审计会把它们列成「表里有、没人用」。

```diff
--- a/Src/App/Main.cpp
+++ b/Src/App/Main.cpp
@@ -305,7 +305,7 @@
-          tr.Or("error.verify.failed", L"\u6821\u9A8C\u5931\u8D25\uFF1A") + L"\n" +
+          tr("error.verify.failed") + L"\n" +
@@ -315,9 +315,7 @@
-    std::wstring text = tr.Or("progress.failed", L"\u89E3\u538B\u5931\u8D25") + L"\n\n";
-    text += outcome.error.empty()
-                ? tr.Or("error.engine.noReason",
-                        L"(\u5F15\u64CE\u672A\u62A5\u544A\u539F\u56E0)")
+    std::wstring text = tr("progress.failed") + L"\n\n";
+    text += outcome.error.empty()
+                ? tr("error.engine.noReason")
@@ -457,2 +455,2 @@
-    MessageBoxW(nullptr, (L"7z.dll \u52A0\u8F7D\u5931\u8D25\uFF1A\n" + dll).c_str(),
+    MessageBoxW(nullptr, (tr.Sub("error.engine.load", {{"path", prism::win::WideToUtf8(dll)}},
+                                 L"7z.dll \u52A0\u8F7D\u5931\u8D25\uFF1A") + L"\n").c_str(),
@@ -473,1 +473,1 @@
-      MessageBoxW(nullptr, (L"Failed to open:\n" + archive + L"\n\n" + trace).c_str(),
+      MessageBoxW(nullptr, (tr("error.open") + L"\n" + archive + L"\n\n" + trace).c_str(),
@@ -748,1 +748,1 @@
-              status += L"\\n";
+              status += L"\n";
@@ -777,5 +777,3 @@
-      window.SetStatus(tr.Or("status.refreshDeferred",
-                             L"\\u5DF2\\u5199\\u5165\\uFF1B\\u5F53\\u524D\\u64CD\\u4F5C\\u7ED3\\u675F"
-                             L"\\u540E\\u5237\\u65B0"));
+      window.SetStatus(tr("status.refreshDeferred"));
@@ -784,2 +782,1 @@
-      window.SetStatus(tr.Or("error.open", L"\\u6253\\u5F00\\u5931\\u8D25") + L" " +
-                           FileNameWide(app->archivePath));
+      window.SetStatus(tr("error.open") + L" " + FileNameWide(app->archivePath));
@@ -792,1 +789,1 @@
-    window.SetStatus(L"\\x5171 " + std::to_wstring(app->items.size()) + L" \\x9879");
+    window.SetStatus(tr.Sub("status.items.count",
+                            {{"n", std::to_string(app->items.size())}}, L""));
```

`Tr::Sub` 是 `AppWiring.hpp` 第 7 节要多出来的一个形状（`Translator::Translate(key, subs)`
已经存在，不需要改 `I18n.hpp`）：

```cpp
std::wstring Sub(std::string_view key, const std::vector<prism::i18n::Substitution>& subs,
                 const std::wstring& fallback) const {
  if (translator == nullptr || !translator->Has(key)) return fallback;
  return prism::win::Utf8ToWide(translator->Translate(key, subs));
}
```

## 5. 新增一个 key 的规则

1. 两表同一个提交，顺序一致（审查看 diff 就能对齐）；
2. key 形如 `group.thing[.qualifier]`，小写 ASCII + 点，占位符只写 `{name}`；
3. 调用点用 `tr("key")`（表里一定有）或 `tr.Or("key", 默认值)`（过渡期）；
4. 跑 `ctest -R i18n`：`core.test_i18n` 守 RequiredKeys，`core.test_i18n_audit` 守调用点；
5. **不要**为了「将来可能用到」加 key——第 6 条会把它列出来，reviewer 会问。
