#!/usr/bin/env bash
# 校验 commit message 是否符合 Conventional Commits 格式：
#   <type>(<scope>): <subject>
# pre-commit 的 commit-msg 阶段会传入：$1 = commit message 文件路径
msg_file="$1"

if [[ -z "$msg_file" || ! -f "$msg_file" ]]; then
  echo "❌ 未找到 commit message 文件" >&2
  exit 1
fi

header="$(head -n 1 "$msg_file")"

if ! grep -Eq '^(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)(\([^)]*\))?: .+' <<<"$header"; then
  echo "❌ commit message 格式不符合 Conventional Commits" >&2
  echo "   期望: <type>(<scope>): <subject>" >&2
  echo "   示例: feat(logger): 添加级别过滤" >&2
  echo "   type: feat/fix/docs/style/refactor/perf/test/build/ci/chore/revert" >&2
  exit 1
fi
