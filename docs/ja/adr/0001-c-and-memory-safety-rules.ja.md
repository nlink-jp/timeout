# ADR-0001: timeout を C で書き、メモリ安全の規則を機械で強制する

| Field | Value |
|-------|-------|
| Status | **Accepted** |
| Date | 2026-09-29 |
| Binds | timeout |
| Decision makers | nlink-jp maintainers |
| Triggered by | RFP 第 1 版の独立検証で Go では規格を満たせないと判明し、利用者が「メモリ境界と自動変数の扱いに十分留意すること」を条件に C を選んだ |

## Context

POSIX.1-2024 XCU `timeout` は、Go のプログラムにはできないことを 3 つ要求する
（go1.27.1 / macOS 27 で 2026-09-29 に実測）。

1. **受け継いだシグナル設定を保つ。** Go ランタイムは起動時に、無視で受け継いだシグナルの
   うち HUP と INT 以外へ自分のハンドラを入れる。そのため `trap '' TERM` の下でも
   `signal.Ignored(SIGTERM)` は偽になり、`os/exec` で起動した子では TERM と PIPE が既定動作になる。
2. **子と同じシグナルで自分も終わる。** `signal.Reset` の後に自分を kill すると、QUIT は exit 2
   （ゴルーチンのダンプ）になり、PIPE と USR1 では生き残る。
3. **終了系のシグナルをすべて中継する。** SEGV・BUS・ILL を受けるとランタイムが落ち、
   PIPE と PROF は `signal.Notify` に届かない。

規格・GNU・FreeBSD はいずれも C の fork・exec・sigaction を前提に書かれており、C なら
3 つとも直接書ける。加えて、GNU と同じ時間の解釈（`strtod`）と、GNU と同じ 126/127・
PATH の扱い（`execvp`）が手間なく得られる。

C の代償はメモリ安全性である。利用者の条件は「メモリ境界と自動変数の扱いに十分留意すること」。
Apple clang 21 で、わざとバグを入れたコードに対して各安全策を実測すると、単独で全部を拾える
ものは無い。

| 仕込んだバグ | 警告 | 静的解析 | ASan | `-fbounds-safety` |
|---|---|---|---|---|
| 条件次第で未初期化の自動変数 | 検出 | 検出 | — | — |
| 自動変数のアドレスを返す | 検出 | 検出 | — | — |
| 配列を越える memcpy | 見逃し | 見逃し | 検出 | — |
| 長さ 3 の配列の添字 3 | — | — | — | 実行時に停止（通常ビルドは黙って読む） |

## Decision

本体は libc だけを使う C で書き、次の規則に従わせる。規則はどれも機械で確かめる。

### コーディング規則

1. **自動変数**は宣言時に初期化する（構造体は `= {0}`）。アドレスを関数の外へ出さない —
   返さない、静的変数や呼び出しより長く生きる構造体へ保存しない。VLA と `alloca` は使わない。
   スタック上の配列は固定長で **1 KiB 以下**。
2. **メモリ境界**: 利用者の入力を固定長バッファへコピーしない — argv はポインタのまま扱う。
   `strcpy`・`strcat`・`sprintf`・`gets` を使わない。書式出力は `snprintf` を使い、戻り値を検査し、
   切り詰めを扱う。配列の添字は使う前に範囲を検査し、長さは `sizeof` か `size_t` の定数で持つ。
3. **動的確保を使わない。** 必要になったら新しい ADR で決める。
4. **数値変換**: `double` を整数や `timespec` にする前に `isfinite` と範囲を検査する（範囲外の
   変換は未定義動作）。時間の加算は桁あふれを検査して最大値へ丸める。`int`・`pid_t`・`size_t` の
   間の暗黙変換をしない。
5. **fork 後の子**はシグナル設定と mask の復元、kqueue を閉じる、`execvp`、`_exit` だけを行う。
   エラーの報告は `write(2)` で行い、出すのは固定の文言、argv のコマンド名、`strerror` の定数の
   文言だけ（プロセスは単一スレッド）。`printf` 系と stdio は使わない。（2026-09-30、リリース前の
   検証を受けて、コードの実際に合わせて明文化。）
6. **シグナルハンドラ**は同期で起きうる 5 種（ADR-0002）にだけ置き、`sigaction` 以外を呼ばず、
   自分の自動変数と、シグナル処理部の `volatile sig_atomic_t` の到着回数記録（添字は範囲を
   検査してから使う）以外に触れない。（2026-09-29、回数記録を必要とする ADR-0002 第 3 版に
   あわせて改訂。当初は「自動変数以外に触れない」。）

### 強制: 5 層

| 層 | 内容 | どこで |
|---|---|---|
| 1. 警告 | `-std=c17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wvla -Wformat=2 -Wimplicit-fallthrough -Wuninitialized -Wsometimes-uninitialized -Wreturn-stack-address` | 全ビルド |
| 2. 静的解析 | `clang --analyze -Xanalyzer -analyzer-werror` | `make lint` |
| 3. 境界検査 | `-fbounds-safety` | 配布ビルドとテストビルド |
| 4. 実行時の検出 | 配布: `-O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong`。テスト: ASan + UBSan に `-fno-sanitize-recover=all`、`-ftrivial-auto-var-init=pattern` | `make build` / `make build-test` |
| 5. 入力の揺さぶり | 生成した入力（境界値・長大文字列・不正バイト）を、テストビルドの時間とシグナル名の解析へ流す | `make test` |

各層に**陽性対照**を置く — わざとバグを入れたファイルで、その層が診断の文言つきで拒むことを
確かめる。`make test` が最初に実行する（`tests/controls/run.sh`）。外から動かすテストは、
テストビルドと配布ビルドの両方に当てる。

### 唯一認める境界の例外

`-fbounds-safety` の下で `main` は `char *__null_terminated *__counted_by(argc) argv` を受け取る。
`argv[argc]` の NULL は数えた範囲の外なので、コマンドの引数列を `execvp` へ渡す 1 か所だけは
`__unsafe_forge_null_terminated` を使う。本体ではそれ以外の `__unsafe_*` を使わない。単体テストには
1 か所ある: 入力の揺さぶりの生成器が、自分で書いた NUL の位置を明示して
`__unsafe_null_terminated_from_indexable(buf, &buf[len])` でバッファをパーサへ渡す。外から動かす
テストの補助プログラム（`tests/helpers/helper.c`）には、本体と同じ `execvp` の例外がある。

### `-fbounds-safety` の下での書き方（2026-09-29 実測）

`__unsafe_*` を使わずに通るもの:

- `SIG_IGN`・`SIG_DFL`・ハンドラを `sa_handler` へ直接代入する。
- 文字列リテラルと `argv` の要素を `const char *` の引数へ渡す。
- `argv` の要素に `strcmp` を使う。
- ローカル配列へ `snprintf` し、検査した長さで `write(fd, buf, (size_t)n)` する。

拒否されるので避けるもの（どれも `__unsafe_*` の変換が要る）:

- ローカルの `char` 配列を `const char *` の引数へ渡す — 境界検査はこれを NUL 終端として扱う。
  配列は長さつきの `const char *__counted_by(len)` で渡す。
- `SIG_IGN`/`SIG_DFL` を関数ポインタの引数で渡す — 選択子（enum）を渡し、中で代入する。
- 長さつきのバッファを `const char *` として返す — 呼び出し側のバッファへ書き、何も返さない。

## Consequences

- 規格の要求はすべて直接実装できる。
- 組織の Go 骨組みから外れる — ビルドは clang、`main.go` と cobra は無く、Go は外から動かす
  テストにだけ使う。AGENTS.md に記す。
- どの層が拾ったバグも `make test` を失敗させ、拾わなくなった層は対照で失敗する。骨組みで実測:
  `-fno-sanitize-recover=all` を外すと UBSan の対照が失敗する。
- 既定のままでは失敗せず黙って通ってしまう層が 2 つあった — `-Werror` だけの静的解析と、
  `-fno-sanitize-recover` の無い UBSan。
- Apple の SDK は 2 を超える `_FORTIFY_SOURCE` を 2 として扱うので、2 と書く。
- Apple clang に libFuzzer が無いので、5 層目は自前の生成器で行う。

## Alternatives considered

- **Go（組織標準）。** 上の 3 つの実測で不採用。
- **Go と cgo。** ランタイムより前に走る constructor で受け継いだ設定を記録でき、C の補助関数で
  シグナルによる自己終了もできる。しかし SEGV/BUS/ILL の中継はランタイム自身のハンドラと衝突する。
  ランタイムと戦う設計として不採用。
- **Swift。** `fork` が使えない（unavailable 指定）。`posix_spawn` では、shebang の無いファイルの
  扱いと 126/127 の分け方を `execvp` と同じになるよう自前で再現する必要がある。不採用。
- **層を減らした C。** 実測で、各層は他の層が拾うものを見逃す。不採用。

## References

- [RFP](../timeout-rfp.ja.md) §3
- POSIX.1-2024 XCU `timeout`: https://pubs.opengroup.org/onlinepubs/9799919799/utilities/timeout.html
- `tests/controls/`
