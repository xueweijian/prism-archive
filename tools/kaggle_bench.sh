#!/bin/sh
# tools/kaggle_bench.sh -- 一键驱动 Kaggle 基准矩阵 kernel (weijianxue/prism-bench-matrix)
#
# 用法:
#   tools/kaggle_bench.sh push      推送/更新 kernel (kaggle CLI 自动版本+1)
#   tools/kaggle_bench.sh status    查询当前运行状态
#   tools/kaggle_bench.sh wait [分钟]  轮询直到 COMPLETE/ERROR (默认 50 分钟上限)
#   tools/kaggle_bench.sh output    拉回结果到 kaggle/kaggle_out/ 并打印 speedup 摘要
#   tools/kaggle_bench.sh all       push + wait + output
#   tools/kaggle_bench.sh summary <bench_results.jsonl>  本地打印已有结果
#
# 依赖: kaggle CLI (KAGGLE_API_TOKEN 环境变量), 不硬编码任何 token.
set -eu
SLUG=weijianxue/prism-bench-matrix
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR="$HERE/../kaggle"
OUTDIR="$KDIR/kaggle_out"
cmd=${1:-all}

do_push() {
  (cd "$KDIR" && kaggle kernels push -p .)
}

do_status() {
  kaggle kernels status "$SLUG" 2>&1 | tail -1
}

do_wait() {
  limit=${2:-50}
  i=0
  while [ "$i" -lt "$limit" ]; do
    st=$(do_status || true)
    echo "[$(date +%H:%M:%S)] $st"
    case "$st" in
      *COMPLETE*) return 0 ;;
      *ERROR*)    return 1 ;;
    esac
    i=$((i + 1))
    sleep 60
  done
  echo "wait timeout after ${limit} min"
  return 2
}

do_output() {
  mkdir -p "$OUTDIR"
  (cd "$OUTDIR" && kaggle kernels output "$SLUG")
  echo "--- 摘要 (speedup = peritem_s / sweep_s) ---"
  python3 "$KDIR/bench_solid.py" --summary "$OUTDIR/bench_results.jsonl"
}

case "$cmd" in
  push)   do_push ;;
  status) do_status ;;
  wait)   do_wait "${2:-50}" ;;
  output) do_output ;;
  summary) python3 "$KDIR/bench_solid.py" --summary "$2" ;;
  all)    do_push; do_wait 50; do_output ;;
  *) echo "unknown command: $cmd"; exit 2 ;;
esac
