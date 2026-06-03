# semu 的建置流程與 CI/CD pipeline

## Big picture

這份文件用來解釋 semu 內的建置流程與 CI/CD pipeline。 整個 CI/CD 與建置流程可以分成本機、CI、測試與發布四個情境來看

它們的核心差別在於「由誰決定要使用 release artifact，還是要重新建置」。 本機流程維持接近原本的 Make file-target 模型，由開發者明確決定什麼時候要從 source 重建。 CI 則會自動比對目前 checkout、rolling release 和 workflow-local cache，盡量避免每次 push 都完整重建 guest artifact

本機建置時，Make 會將 `Image`、`rootfs.cpio` 和 `test-tools.img` 視為普通的 file target。 當某個 Make target 需要這些檔案，或使用者直接指定其中一個檔名時，如果檔案不存在，Make 會從 rolling `prebuilt` release 下載對應的 `.bz2`

預設 `make check` 會要求 `Image`、`rootfs.cpio`，並用 `rootfs.cpio` 生成預設的 `ext4.img`。 `test-tools.img` 則由 `make test-tools.img` 或 source build 的 `test-tools` class 取得

若開發者想用目前 working tree 從 source 重建，需要明確執行 `make build-artifacts`，讓 build system 判斷哪些部分需要重編。 Source build 會使用不同的 Buildroot output directory 存放 default rootfs 和 test-tools rootfs，避免 test-tools 的檔案影響到預設的 `rootfs.cpio` 或 `ext4.img`

```text
local developer
  │
  └─ make / make check
     │
     └─ mk/external.mk
        │
        ├─ artifact exists        -> use existing local file
        ├─ artifact missing       -> download and verify release .bz2
        └─ explicit source build  -> make build-artifacts
```

CI 情境中，`guest-artifact-build` 會先判斷目前 checkout 需要哪一組 guest artifact，然後先和 rolling release 比對。 如果 release 已經符合目前需求，這個 job 會跳過 materialize、package 和 upload，後續 test jobs 會直接下載 release artifact

如果 release 不符合目前需求，CI 會 restore workflow-local raw cache，再檢查 cache 裡是否已經有可用的 artifact。 當 Release 和 workflow-local raw cache 都不符合目前需求時，CI 會從 source build。 CI 的加速來源是 workflow-local raw cache

```text
Actions
  │
  └─ guest-artifact-build
     │
     ├─ resolve current checkout against release manifest
     │
     ├─ release current?
     │  │
     │  ├─ yes -> skip materialize/package/upload in this job
     │  └─ no  -> restore workflow-local raw cache
     │
     ├─ resolve again after cache restore
     │
     ├─ materialize supplied plan
     │  │
     │  ├─ use-action-cache restored raw artifacts
     │  ├─ download matching release artifacts
     │  └─ build changed artifacts from source
     │
     └─ package and upload artifacts for later jobs
```

測試與發布階段會沿用前面已經決定好的 artifact 內容。 Test jobs 會透過 `.github/actions/provide-guest-artifacts` 取得 artifact 後執行 `make`、`make check` 或 `.ci/autorun.sh`。 當 release 在本次 workflow 中被判定需要更新時，test jobs 會使用本次 workflow 產生的 raw artifact

Master push、artifact 需要更新，而且所有必要測試都成功時，`publish-prebuilt` 會驗證 packaged artifact 並更新 rolling `prebuilt` release

```text
Test jobs
  │
  ├─ provide-guest-artifacts
  │  │
  │  ├─ release stale   -> use this workflow run raw bundle
  │  └─ release current -> download release artifacts
  │
  └─ make / make check / .ci/autorun.sh

publish-prebuilt
  │
  ├─ master push + release_needs_update + all required tests passed
  ├─ verify packaged bundle
  └─ update rolling prebuilt release
```

## 檔案分工

下面依照主要呼叫順序排列。 同一層中，被 `source` 的 helper 放在呼叫者後面。 本機 source build 和 CI build 會共用 `scripts/prebuilt/` 底下的 recipe

### 本機 Source Build Path

本機 source build 從 Make target 進入，再落到使用者 CLI 和共用 recipe

- `Makefile`：提供 `build-artifacts`、`check`、`bench-login` 等本機入口，並 include `mk/external.mk`
- `mk/external.mk`：定義 `Image`、`rootfs.cpio` 和 `test-tools.img` 這些本機 raw artifact 下載 target
- `scripts/build-artifacts.sh`：`make build-artifacts` 呼叫的使用者 CLI，負責解析 build class、本機 flags、Buildroot cleanup policy，以及本機 `ext4.img` 衍生輸出
- `scripts/prebuilt/artifact-classes.sh`：source build 的共用 artifact contract，定義 class 名稱和 raw output 檔名
- `scripts/prebuilt/artifact-recipes.sh`：source build 的共用 recipe library，實際建 Linux、Buildroot rootfs 和 test-tools raw artifact
- `scripts/prebuilt/artifact-recipe.env`：`artifact-recipes.sh` 載入的 external revisions 和 size constants
- `scripts/prebuilt/test-tools-recipe.sh`：`artifact-recipes.sh` 和 CI recipe key 共用的 test-tools recipe 選擇邏輯
- `scripts/prebuilt/download-release-artifact.sh`：local Make download 和 CI materializer 共用的 release transport helper

### CI Prebuilt Layer

CI prebuilt layer 從 workflow 進入，先決定 artifact 來源，再 materialize、stamp 與 package

- `.github/workflows/main.yml`：啟動 `guest-artifact-build`，串接 cache、workflow artifact、publish gate 和 job output
- `.ci/prebuilt/artifact-inputs.sh`：被 resolver、materializer 和 package helpers source，把共用 artifact contract 接到 CI recipe-key helpers 和 plan-key helpers
- `.ci/prebuilt/resolve-artifacts.sh`：依目前 checkout、release manifest 和 restored raw cache 產生 materialization plan
- `.ci/prebuilt/materialize-artifacts.sh`：執行 resolver 給的 plan，動作可能是 use-action-cache、download-release 或 build
- `.ci/prebuilt/build-plan-artifacts.sh`：materializer 需要 source build 時呼叫的 internal CI build wrapper，會 source `scripts/prebuilt/artifact-recipes.sh`
- `.ci/prebuilt/stamp-artifacts.sh`：materialization 成功後寫入 `.stamps/prebuilt-local/*.recipe-key` stamps
- `.ci/prebuilt/package.sh`：壓縮 raw artifacts，並產生 release manifest
- `.ci/prebuilt/verify-package.sh`：publish 前檢查 packaged artifact 和 manifest shape

### Test And GitHub Helpers

測試 job 會在 artifact 決策完成後透過 composite actions 取得環境和 guest artifacts。 GitHub helper 則提供 workflow output 和 PR suggestion 的共用操作

- `.github/actions/setup-semu/action.yml`：GitHub test dependency installation
- `.github/actions/provide-guest-artifacts/action.yml`：test job 取得 guest artifact 的 composite action
- `.ci/github/append-github-output.sh`：經過驗證的 `$GITHUB_OUTPUT` writer
- `.ci/github/suggest-format.sh`：reviewdog/GitHub PR formatting suggestions

## 本機 Build Path

本機流程的起點是 Make 的 file target。 一般開發者執行 `make` 時，會建出 emulator binary 和 `minimal.dtb`。 執行 `make check` 時，Make 會因為 dependency 需要 `Image`、`rootfs.cpio`，以及預設外部 rootfs 模式下的 `ext4.img`。 Make 會看檔案是否已經存在，檔案在就直接使用，檔案不在則依照 `mk/external.mk` 定義的規則下載 rolling `prebuilt` release

Makefile 會在使用 artifact 的 target 前 include `mk/external.mk`，讓 `KERNEL_DATA`、`INITRD_DATA` 和 `TEST_TOOLS_DATA` 這幾個 guest artifact 名稱成為普通 file target。 `check` 會把 `KERNEL_DATA`、依 boot mode 決定的 `INITRD_DEP`，以及 `DISKIMG_FILE` 接到 emulator 啟動參數上。 這段關係可以簡化成下面這樣：

```make
include mk/external.mk

check: $(BIN) minimal.dtb $(KERNEL_DATA) $(INITRD_DEP) $(DISKIMG_FILE) $(SHARED_DIRECTORY)
	./$(BIN) $(strip $(KERNEL_OPT) $(DTB_OPT) $(HEADLESS_OPT) \
	    $(INITRD_OPT) $(DISKIMG_OPT) $(VIRTIOFS_OPT) \
	    $(SMP_OPT) $(NETDEV_OPT))
```

`mk/external.mk` 的下載規則刻意很薄，負責下載缺失的檔案。 下載完成後會解壓成原始檔案，最後把 `.bz2` 刪掉，所以工作區通常會看到 raw artifact

這條本機下載路徑會依照 `prebuilt.sha1` 驗證 release archive checksum，確認下載到的 `.bz2` bytes 和 release manifest 一致後才解壓。 本機 `make check` 使用的是已存在或已下載的 raw artifact，因此如果開發者改了 guest config 或 build recipe，需要明確執行 `make build-artifacts`，才能測目前 source 產生的 artifact

```make
define download_prebuilt_artifact
$(1):
	$$(Q)printf '  GET\t$$@\n'
	$$(Q)PREBUILT_URL="$$(PREBUILT_URL)" scripts/prebuilt/download-release-artifact.sh "$$@"
endef

$(foreach artifact,$(PREBUILT_ARTIFACTS),$(eval $(call download_prebuilt_artifact,$(artifact))))
```

`ext4.img` 的來源稍微不同。 預設啟用外部 rootfs 時，`ext4.img` 是從 `rootfs.cpio` 轉出來的本機衍生檔。 因此 fresh `make check` 的實際順序通常會先確保 `Image` 和 `rootfs.cpio` 存在。 如果 `ext4.img` 不存在，才會執行 `scripts/rootfs_ext4.sh rootfs.cpio ext4.img`

```make
ext4.img: $(INITRD_DATA) scripts/rootfs_ext4.sh
	$(Q)MKFS_EXT4="$(MKFS_EXT4)" scripts/rootfs_ext4.sh $(INITRD_DATA) $@
```

開發者要用目前 working tree 重新產生 guest artifact 時，改走 `make build-artifacts`。 Makefile 在這裡是薄 wrapper，真正的 CLI parsing 在 `scripts/build-artifacts.sh`

```make
ARTIFACTS ?= all
.PHONY: build-artifacts
build-artifacts:
	scripts/build-artifacts.sh $(ARTIFACTS)
```

`build-artifacts.sh` 先把使用者給的 class 轉成三個內部布林值。 `image` 會建 Linux kernel artifact，`rootfs` 會建 `rootfs.cpio` 和預設的 `ext4.img`，`test-tools` 會建 optional disk，`all` 則把三個 class 都設為需要建置。 `--x11` 和 `--directfb2-test` 屬於 test-tools recipe entry。 使用者明確給出 recipe flag 時，script 會建選到的 entry

```bash
case "$1" in
    image)      BUILD_LINUX=1 ;;
    rootfs)     BUILD_ROOTFS=1 ;;
    test-tools) BUILD_TEST_TOOLS=1 ;;
    all)
        BUILD_LINUX=1
        BUILD_ROOTFS=1
        BUILD_TEST_TOOLS=1
        ;;
    --x11)            add_test_tools_recipe_entry x11 ;;
    --directfb2-test) add_test_tools_recipe_entry directfb2 ;;
esac
```

在真正建置前，`--clean-build` 和 `--full-rebuild` 會先處理舊輸出。 兩者都會刪掉這次選到的 raw artifact output，避免 source build 中途失敗後，工作區還留著舊的 `Image`、`rootfs.cpio`、`ext4.img` 或 `test-tools.img`

`--clean-build` 會刪 Buildroot output directory，並保留 `buildroot/` checkout 和 download cache。 `--full-rebuild` 會把外部 source/build tree 一起刪掉，讓下一步重新 clone pinned revision

```bash
if [[ $CLEAN_BUILD -eq 1 || $FULL_REBUILD -eq 1 ]]; then
    remove_selected_artifact_outputs
fi

if [[ $FULL_REBUILD -eq 1 ]]; then
    remove_existing_paths "Buildroot source tree for full rebuild" buildroot
    remove_existing_paths "Linux source tree for full rebuild" linux
elif [[ $CLEAN_BUILD -eq 1 ]]; then
    remove_existing_paths "default Buildroot output for clean build" \
        "$BUILDROOT_DEFAULT_OUTPUT" buildroot/output
    remove_existing_paths "test-tools Buildroot output for clean build" \
        "$BUILDROOT_TEST_TOOLS_OUTPUT"
fi
```

實際建置邏輯集中在 `scripts/prebuilt/artifact-recipes.sh`。 這份檔案同時給本機 CLI 和 CI internal builder 使用，負責把 source tree、Buildroot output 和最後 raw artifact 串起來。 Default rootfs 和 test-tools rootfs 使用不同 Buildroot output，讓 test-tools recipe 變更和 default rootfs 隔離

```bash
BUILDROOT_DEFAULT_OUTPUT=${BUILDROOT_DEFAULT_OUTPUT:-buildroot/output-default}
BUILDROOT_TEST_TOOLS_OUTPUT=${BUILDROOT_TEST_TOOLS_OUTPUT:-buildroot/output-test-tools}

buildroot_make() {
    local output_dir=$1
    shift

    ASSERT mkdir -p "$output_dir"
    ASSERT make -C buildroot O="$PWD/$output_dir" "$@"
}
```

建 default rootfs 時，shared recipe 會用 base Buildroot config 產生 `buildroot/output-default/images/rootfs.cpio`，然後把它發布成 repository root 底下的 `rootfs.cpio`。 本機 CLI 如果沒有收到 `--no-ext4`，則會在 `rootfs.cpio` 產生後額外轉出 `ext4.img`

```bash
do_rootfs_unlocked() {
    build_buildroot_rootfs default "$BUILDROOT_DEFAULT_OUTPUT"

    echo "Publishing rootfs.cpio"
    ASSERT cp -f "$BUILDROOT_DEFAULT_OUTPUT/images/rootfs.cpio" ./rootfs.cpio
}

derive_local_ext4_from_rootfs() {
    if [[ $NO_EXT4 -eq 1 ]]; then
        echo "Skipping ext4.img build (--no-ext4)"
        return
    fi

    ASSERT ./scripts/rootfs_ext4.sh ./rootfs.cpio ./ext4.img
}
```

建 test-tools 時，recipe 會改用 `buildroot/output-test-tools`。 Recipe 包含 `x11` 時，Buildroot config 會疊上 `configs/x11.config`，並在 `test-tools.img` overlay 裡放入 runtime 需要的 C++ library

Recipe 包含 `directfb2` 時，`do_extra_packages` 會先把 DirectFB2 和 DirectFB examples 安裝到 `directfb/`，再複製到 `extra_packages/`，最後把 `extra_packages/` overlay 進 `test-tools.img`。 X11 和 DirectFB2 payload 會進入 test-tools output，default `rootfs.cpio` 和 `ext4.img` 維持 default rootfs recipe

```bash
do_test_tools_unlocked() {
    local buildroot_mode
    local buildroot_output=$BUILDROOT_TEST_TOOLS_OUTPUT
    local test_tools_rootfs

    configure_test_tools_recipe_flags

    if [[ $BUILD_X11 -eq 1 ]]; then
        buildroot_mode=x11
    else
        buildroot_mode=default
    fi
    build_buildroot_rootfs "$buildroot_mode" "$buildroot_output"
    test_tools_rootfs=./$buildroot_output/images/rootfs.cpio

    if [[ $BUILD_DIRECTFB_TEST -eq 1 ]]; then
        do_extra_packages "$buildroot_output"
        if [[ $BUILD_X11 -eq 1 ]]; then
            stage_cxx_runtime "$buildroot_output"
        fi
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    elif [[ $BUILD_X11 -eq 1 ]]; then
        ASSERT rm -rf extra_packages
        stage_cxx_runtime "$buildroot_output"
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    else
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB"
    fi
}
```

## CI Artifact Contract

CI 和 rolling release 之間的 contract 承認固定的 raw artifact 檔名和一份 sha1sum-format manifest。 Release 目錄底下會有 `Image.bz2`、`rootfs.cpio.bz2`、`test-tools.img.bz2` 和 `prebuilt.sha1`。 `prebuilt.sha1` 同時記錄 `.bz2` archive 的位元組 checksum，以及每個 artifact class 的 recipe key

這個 contract 的 class registry 由 `scripts/prebuilt/artifact-classes.sh` 定義，因為 class 名稱和 raw output 檔名屬於 source-build contract。 CI policy 放在 `.ci/prebuilt/` 底下

Registry 裡每一列包含 class 名稱和該 class 對應的 raw artifact 檔名

`.ci/prebuilt/artifact-inputs.sh` 會 source 這份 contract，再補上 CI recipe-key helper 和 plan-key helper

Plan variable 需要的 shell-safe 名稱由 class 名稱推導，例如 `test-tools` 會變成 `test_tools`

```bash
source_artifact_class_registry() {
    printf '%s\t%s\n' image Image
    printf '%s\t%s\n' rootfs rootfs.cpio
    printf '%s\t%s\n' test-tools test-tools.img
}
```

Recipe key 的來源分成兩部分。 第一部分是真實檔案，例如 Buildroot config、Linux config、BusyBox config、`target/init`、`scripts/rootfs_ext4.sh`，以及 test-tools recipe 需要的 `configs/x11.config` 或 `configs/meson-riscv-cross-file`。 第二部分是不直接以檔案形式出現、但會影響輸出內容的 recipe 變數，例如 `BUILDROOT_REV`、`LINUX_REV`、`TEST_TOOLS_SIZE_MB`，以及 `PREBUILT_TEST_TOOLS_RECIPE` 正規化後的 test-tools recipe 選擇

`prebuilt_class_recipe_manifest` 先把這兩部分都展成 sha1sum 風格的 manifest。 `prebuilt_class_recipe_key` 再把整份 manifest 摺成單一 SHA-1。 CI 後面所有「這個 artifact 是否符合目前 checkout」的判斷，都以這個 class recipe key 為準

```bash
prebuilt_class_recipe_manifest() {
    local class=$1

    prebuilt_sha1sum "${inputs[@]}"
    prebuilt_class_recipe_virtual_entries "$class"
}

prebuilt_class_recipe_key() {
    local manifest

    manifest=$(prebuilt_class_recipe_manifest "$1") || return 1
    printf '%s\n' "$manifest" | prebuilt_sha1sum | awk '{print $1}'
}
```

Package 階段會先確認 raw artifact 和 recipe input 都存在，接著把每個 raw artifact 壓成 `.bz2`。 寫出 `prebuilt.sha1` 時，前半部是每個 `.bz2` archive 的 checksum，後半部才是 class recipe key 形成的虛擬 `*.recipe-key` entry。 例如 `image` class 的 recipe key 會寫成 `Image.recipe-key`，`rootfs` class 會寫成 `rootfs.cpio.recipe-key`，`test-tools` class 會寫成 `test-tools.img.recipe-key`

```bash
write_archive_checksum_entries_for_class() {
    local class=$1
    local artifact

    while IFS= read -r artifact; do
        prebuilt_sha1sum "${artifact}.bz2"
    done < <(source_artifact_class_outputs "$class")
}

write_recipe_key_entries_for_class() {
    local class=$1
    local recipe_key=$2
    local entry

    while IFS= read -r entry; do
        printf '%s  %s\n' "$recipe_key" "$entry"
    done < <(prebuilt_class_recipe_entries "$class")
}
```

這個設計把「release 是否符合目前 recipe」和「下載到的 archive bytes 是否符合 manifest」分開。 Resolver 以 recipe key 決定 release freshness，並要求 manifest 裡存在對應的 `.bz2` checksum entry

Materializer 下載 release artifact 時，會先用 `prebuilt.sha1` 驗證 `.bz2` checksum，並確認 manifest 裡的 `*.recipe-key` 仍符合 resolver plan，才會解壓並寫入 stamp。 如果 release asset 本身 stale、損毀，或 manifest 和 asset 不一致，CI 會失敗並把 rolling release 狀態問題交給 maintainer 檢查

CI local cache 的 contract 也是 recipe key。 當 materializer 成功 download 或 build 某個 class 後，會呼叫 `prebuilt_stamp_artifacts_for_class`，將同一個 class recipe key 寫到 `.stamps/prebuilt-local/<artifact>.recipe-key`。 下一次 workflow restore raw artifact cache 後，resolver 會用這些 stamp 判斷 cache 裡的 raw artifact 是否可信

```bash
prebuilt_local_stamp_dir=.stamps/prebuilt-local

prebuilt_artifact_recipe_stamp() {
    printf '%s/%s.recipe-key\n' "$prebuilt_local_stamp_dir" "$1"
}

prebuilt_stamp_artifacts_for_class() {
    local class=$1
    local recipe_key=$2
    local artifact

    mkdir -p "$prebuilt_local_stamp_dir" || return 1
    while IFS= read -r artifact; do
        printf '%s\n' "$recipe_key" > "$(prebuilt_artifact_recipe_stamp "$artifact")"
    done < <(source_artifact_class_outputs "$class")
}
```

這裡要特別區分 `.stamps/prebuilt-local` 和本機 Make。 `.stamps/prebuilt-local` 屬於 CI raw cache，用來說明「cache restore 回來的 raw artifact 是由哪個 recipe key 產生」。 本機 `make check` 使用 Make file target 和本機檔案狀態。 開發者要重建 artifact 時，透過 `make build-artifacts` 明確決定

## CI Decision Model

CI 的決策從 `.github/workflows/main.yml` 的 `guest-artifact-build` job 開始。 這個 job checkout source tree 後，會先執行 `.ci/prebuilt/resolve-artifacts.sh`，把目前 checkout、rolling release manifest，以及工作區中可能已存在的 local stamps 比對成一份 plan

Cache restore 和 build 都排在這個 plan 後面。 這份 plan 是純 `KEY=VALUE` 檔案，後面的 dependency install、materialize 和 publish gate 都讀它

Resolver 啟動後會先 source `.ci/prebuilt/artifact-inputs.sh`，取得共用 artifact contract、CI recipe-key helper 和 plan variable naming helper。 接著它針對每個 registered class 計算 `current_*_recipe_key`。 這個 key 描述「目前 checkout 需要什麼 artifact」

```bash
while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" current_recipe_key \
        "$(prebuilt_class_recipe_key "$class")"
    resolve_local_class "$class"
done < <(source_artifact_classes)
```

同一輪中，resolver 也會檢查 workflow-local raw cache 是否已經被 restore 到工作區。 這裡的 local 指 GitHub Actions workspace 裡的 raw artifact cache。 Resolver 會要求 class 裡每個 artifact 都存在、每個 artifact 都有 `.stamps/prebuilt-local/*.recipe-key`，而且所有 stamp 的值一致。 缺少任一 artifact、任一 stamp，或同 class 裡 stamp 不一致時，這份 cache 對該 class 會被標成 invalid

```bash
class_local_recipe_key() {
    local class=$1
    local artifact
    local recipe_stamp
    local recipe_key=

    while IFS= read -r artifact; do
        recipe_stamp=$(prebuilt_artifact_recipe_stamp "$artifact")
        if [ ! -f "$artifact" ] || [ ! -f "$recipe_stamp" ]; then
            return 1
        fi
        IFS= read -r next < "$recipe_stamp" || return 1
        # 同一個 class 的所有 artifact 必須指向同一個 recipe key
    done < <(source_artifact_class_outputs "$class")

    printf '%s\n' "$recipe_key"
}
```

接著 resolver 下載 rolling release 的 `prebuilt.sha1` 到暫存目錄。 `manifest_class_recipe_key` 會先確認每個 artifact 都有對應的 `.bz2` checksum entry，再讀取 `Image.recipe-key`、`rootfs.cpio.recipe-key` 或 `test-tools.img.recipe-key` 這類虛擬 entry。 如果某個 class 缺 archive checksum entry、缺 recipe-key entry，或同一個 class 的多個 artifact entry 指到不同 key，該 class 的 release key 就視為空值，後續 action 決策會落到其他 artifact 來源

```bash
manifest_entry_value() {
    local entry=$1

    awk -v f="$entry" '$2 == f {print $1; found = 1; exit} END {exit !found}' "$manifest"
}

manifest_class_recipe_key() {
    local class=$1
    local artifact
    local next
    local recipe_key=

    while IFS= read -r artifact; do
        if ! manifest_entry_value "${artifact}.bz2" >/dev/null; then
            printf '\n'
            return
        fi
        if ! next=$(manifest_entry_value "${artifact}.recipe-key"); then
            printf '\n'
            return
        fi
        # 同一個 class 的 manifest entries 必須有一致 key
    done < <(source_artifact_class_outputs "$class")

    printf '%s\n' "$recipe_key"
}
```

當 resolver 同時掌握目前 recipe key、workflow-local raw cache 的 recipe key 與狀態，以及 rolling release 的 recipe key 後，才會做真正的 action 決策。 Release/local key 是 resolver 內部決策狀態。 Materializer plan 會輸出每個 class 的 action 和 current recipe key

優先順序是 workflow-local raw cache、rolling release，最後是 source build。 這個順序很重要，因為 workflow-local raw cache 是同一個 workflow 或相近 workflow 內部的 raw artifact bundle。 Stamp 符合目前 checkout 時，CI 會使用 restored cache。 Release 符合時，CI 會下載 release artifact。 兩邊都不符合時，CI 會從 source 建置

```bash
decide_action() {
    local current_recipe_key=$1
    local release_recipe_key=$2
    local local_recipe_key=$3
    local local_status=$4

    if [ "$local_status" = valid ] && [ "$local_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' use-action-cache
        return
    fi

    if [ -n "$release_recipe_key" ] && [ "$release_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' download-release
    else
        printf '%s\n' build
    fi
}
```

Resolver 最後輸出的 plan 會保留後續步驟需要讀取的欄位：每個 class 的 action、目前 recipe key、`requires_build`、`release_needs_update`、`platform_action_cache_tag` 與 `ci_cache_schema_tag`。 `platform_action_cache_tag` 由三個 class 的目前 recipe key 再摺成一個 cache namespace，用來命名 GitHub Actions cache。 Artifact correctness 由每個 class 自己的 action 和 recipe key 決定

```bash
printf 'plan_version=1\n'
prebuilt_plan_print_class_values action
prebuilt_plan_print_class_values current_recipe_key
printf 'platform_action_cache_tag=%s\n' "$platform_action_cache_tag"
printf 'ci_cache_schema_tag=%s\n' "$ci_cache_schema_tag"
printf 'requires_build=%s\n' "$requires_build"
printf 'release_needs_update=%s\n' "$release_needs_update"
```

Workflow 第一次 resolve 是 publish gate。 如果 `release_needs_update=false`，代表 rolling release 對所有 class 都符合目前 checkout，`guest-artifact-build` 會跳過 cache restore、materialize、package 以及 upload

後續 test jobs 透過 `.github/actions/provide-guest-artifacts` 直接從 release materialize raw artifacts

如果 `release_needs_update=true`，workflow 會 restore workflow-local raw cache，然後在 cache restore 後再跑一次 resolver，產生 post-cache plan

```yaml
- name: resolve release update plan
  id: resolve
  run: |
    .ci/prebuilt/resolve-artifacts.sh > "$RUNNER_TEMP/prebuilt-plan-main.env"
    .ci/github/append-github-output.sh "$RUNNER_TEMP/prebuilt-plan-main.env" \
      release_needs_update \
      platform_action_cache_tag \
      ci_cache_schema_tag

- name: materialization plan after cache restore
  id: post_cache_resolve
  if: steps.resolve.outputs.release_needs_update == 'true'
  run: |
    .ci/prebuilt/resolve-artifacts.sh > "$RUNNER_TEMP/prebuilt-plan-post-cache.env"
    .ci/github/append-github-output.sh "$RUNNER_TEMP/prebuilt-plan-post-cache.env" \
      requires_build
```

Materializer 負責執行 resolver 已經決定好的 plan，呼叫者需要透過 `PREBUILT_PLAN_FILE` 指向 plan 檔。 `parse_resolver_output` 會消費 `plan_version`、`*_action` 和 `current_*_recipe_key`，並略過 workflow 使用的 scalar 欄位，例如 `requires_build`、`platform_action_cache_tag`、`ci_cache_schema_tag` 與 `release_needs_update`。 其他未知 key 會 fail fast，避免舊 materializer 靜默誤解新 plan schema

```bash
if [ -z "${PREBUILT_PLAN_FILE:-}" ]; then
    echo "[!] PREBUILT_PLAN_FILE is required" >&2
    exit 1
fi

parse_resolver_output <<< "$resolver_output"

if [ "$plan_version" != 1 ]; then
    echo "[!] Unsupported prebuilt plan version: ${plan_version:-missing}" >&2
    exit 1
fi
```

Materializer 在做任何下載或 build 前，會先驗證每個 class 都有合法 action。 若 test job 設了 `PREBUILT_FORBID_BUILD=1`，而 plan 裡仍有 `build` action，materializer 會直接失敗。 這個 guard 讓 test job consume 已經決定好的 workflow artifact 或 rolling release。 測試階段遇到 publish state 問題時會直接暴露錯誤

```bash
if [ "$requires_build" = true ] && [ "$forbid_build" = true ]; then
    echo "[!] PREBUILT_FORBID_BUILD forbids source builds" >&2
    exit 1
fi
```

真的需要 source build 時，materializer 呼叫 `.ci/prebuilt/build-plan-artifacts.sh`。 這個 internal builder 會 source `scripts/prebuilt/artifact-recipes.sh` 並執行 `build_prebuilt_artifact_classes`。 Stamp 的 commit point 留在 materializer，因為 materializer 知道整個 plan action 是否成功完成

Build 成功後，materializer 會用目前 recipe key stamp 對應 class

```bash
if [ "$requires_build" = true ]; then
    build_plan_classes
    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            prebuilt_stamp_artifacts_for_class "$class" \
                "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
        fi
    done < <(source_artifact_classes)
fi
```

對 `download-release` class，materializer 會透過 `scripts/prebuilt/download-release-artifact.sh` 下載對應 `.bz2`，並用 `prebuilt.sha1` 驗證 archive checksum 和預期 recipe key

驗證成功後，materializer 會解壓成 raw artifact，然後同樣寫入 `.stamps/prebuilt-local`。 對 `use-action-cache` class，cache restore 已經在 workspace 放好 raw artifact 和 stamp

因此整個 CI decision model 的最後結果是：每個 class 最終都會在 workspace 裡留下 raw artifact。 如果這個 artifact 是 CI materializer 管理過的，也會留下對應 recipe-key stamp，供後續 cache restore 再判斷一次

```bash
while IFS= read -r class; do
    if [ "$(prebuilt_plan_get_class_var "$class" action)" = download-release ]; then
        download_release_class "$class" \
            "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
    fi
done < <(source_artifact_classes)
```

因此每一層的責任各自如下：

- Resolver 做判斷並維持 raw artifact 不變
- Materializer 依照 plan 執行
- Package 負責壓縮 materialized raw artifact，並寫出 archive checksum 和 recipe-key manifest
- Test jobs 取得 artifact 後執行測試
- Publish job 在 master push 且同一組 artifact 通過測試後更新 rolling `prebuilt` release

## GitHub Workflow

`.github/workflows/main.yml` 會在 `push` 和 `pull_request` 時執行。 主要 artifact job 是 `guest-artifact-build`

`guest-artifact-build` 的順序是：

1. Checkout source tree
2. 針對 rolling release manifest 執行 `.ci/prebuilt/resolve-artifacts.sh`
3. 將 `release_needs_update`、`platform_action_cache_tag` 和 `ci_cache_schema_tag` 發布成 step output
4. 如果 rolling release 已經符合目前 recipe key，這個 job 會跳過 materialize、package 和 upload
5. 如果 release stale，restore 以 `platform_action_cache_tag` 和 `ci_cache_schema_tag` 為 key 的 workflow-local raw artifact cache
6. Cache restore 之後再次執行 resolver
7. Post-cache plan 仍包含 `build` action 時，安裝 Buildroot/Linux build dependencies
8. 使用 post-cache plan 執行 `.ci/prebuilt/materialize-artifacts.sh`
9. 執行 `.ci/prebuilt/package.sh`
10. 上傳給 test jobs 使用的 raw artifact，以及給 publish 使用的 packaged artifact

Workflow-local raw artifact cache 包含：

- `Image`
- `rootfs.cpio`
- `test-tools.img`
- `.stamps/prebuilt-local/`

Master push 也會 restore 這份 cache。 Publish decision 是另一件事：master 需要同一份 materialized artifact 的所有 required tests 都成功後才可以發布

這裡的 `release_needs_update` 代表 artifact recipe key 漂移，以及 resolver 明確檢查的 release contract 漂移，例如 `prebuilt.sha1` 必須有 archive checksum entry。 `.ci/prebuilt/package.sh`、`.ci/prebuilt/verify-package.sh`、release transport 或 release layout 這類變更，需要讓 resolver 從舊 release 觀察到 stale 狀態，或明確把 schema 納入 gate，才會自動觸發 republish

Fork 和 mirror 可能還沒有自己的 rolling `prebuilt` release。 GitHub workflow 會對 non-canonical repository 啟用 `PREBUILT_BOOTSTRAP_ON_404`，所以 `prebuilt.sha1` 的 HTTP 404 會變成 first-run source build。 Canonical `sysprog21/semu` repository 遇到 404 時會 fail，讓 maintainer 檢查外部 release state

這個 bootstrap 例外套用在 HTTP 404。 DNS 失敗、連線逾時、HTTP 5xx 或其他非 404 錯誤，屬於外部 release state 或網路狀態問題。 Resolver 會直接失敗
