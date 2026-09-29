# RFP: timeout

> Generated: 2026-09-29
> Status: Draft（第 3 版 — 2 回目の独立検証を反映。スリープ中は数えないに変更）

## 1. Problem Statement

macOS には、コマンドを時間制限つきで実行する `timeout` が無い（macOS 27.0 で
`timeout` / `gtimeout` とも不在を実測）。Linux 向けに書かれたスクリプトや、
エージェントが反射的に書く `timeout N cmd` は `command not found`（終了コード 127）で
失敗し、それを「コマンドの失敗」と誤読する事故が起きている（2026-09-23、56 本の
`--version` 検査を全件 NG と誤判定しかけた）。代用の `perl -e 'alarm N; exec @ARGV'`
には実測で 3 つの欠陥がある — SIGALRM を無視するコマンドは止まらない、孫プロセスが
孤児として残る、終了コードが 142 で時間切れと判別できない。対象利用者は、この Mac で
シェルスクリプトやコーディングエージェントを動かす人。POSIX.1-2024 と GNU coreutils の
`timeout` と同じ名前・同じ振る舞いのコマンドを置き、既存のスクリプトを書き換えずに
動かせるようにする。

## 2. Functional Specification

### Commands / API Surface

```
timeout [-fpv] [-k 時間] [-s シグナル] 時間 コマンド [引数...]
timeout --help | --version
```

| 短 | 長（GNU 互換） | 意味 |
|----|----------------|------|
| `-f` | `--foreground` | コマンド本人だけを時間切れにする。プロセスグループを作らないので、スクリプト内からでも端末を読め、端末の Ctrl-C も届く。子孫は止めない。SIGCONT は送らない |
| `-p` | `--preserve-status` | 時間切れでも 124 ではなくコマンドの終了状態を返す |
| `-k 時間` | `--kill-after=時間` | 最初のシグナルから指定時間後にまだ生きていれば SIGKILL。どちらかの時間が 0 なら無効 |
| `-s シグナル` | `--signal=シグナル` | 時間切れで送るシグナル（既定 TERM）。名前は大小文字を区別せず `SIG` 接頭辞は任意。番号も可（番号は macOS の番号） |
| `-v` | `--verbose` | 時間切れで送ったシグナルを標準エラーへ報告 |
| — | `--help` / `--version` | 使い方（標準出力・終了 0）・版数（標準出力・終了 0） |

**オプションの書き方**: libc の `getopt_long` に GNU と同じ `"+fk:ps:v"` を渡して解析する。
先頭の `+` で最初のオペランド（時間）で止まる（`timeout 5 ls -l` の `-l` は `ls` のもの。
`+` が無いと横取りすることを実測済み）。`--` による終端、引数の連結（`-k5`、`-sKILL`）、
長いオプションの `=` 形式と空白区切り（`--kill-after=5` / `--kill-after 5`）、曖昧でない
省略形（`--pres`）を受け付け、曖昧な省略形（`--ver`）はエラーにする（macOS libc で実測・
GNU と同じ）。不正な指定は標準エラーに理由を出して 125。

**時間**: C ロケールの `strtod` で読み、任意の単位 `s`/`m`/`h`/`d`（無ければ秒）を掛ける。
`strtod` の受け付ける書式（小数、指数、16 進 `0x10`=16・`0x1d`=29 秒・`0x1p0`）は GNU と
同じになる。小数点は常に `.`（プログラムは `setlocale` を呼ばない）。`0` と `-0` は無制限
（シグナル中継だけ行う — GNU と同じ）。負数・NaN・数値の後ろの余計な文字は 125。
端の値は GNU（`dtimespec_bound`）に合わせる: 正の極小値と、`strtod` が 0 と `ERANGE` を
返す下位桁あふれ（`1e-400`）は最小単位へ切り上げる（値だけ見て「0＝無制限」にしない）。
上位桁あふれ（`1e400`）と無限大は、タイマーが受け付ける最大値へ丸める（上限値は ADR で決める。
kqueue は `NOTE_SECONDS` に `INT64_MAX` を渡すと `ERANGE`、1e10 秒は受理 — 実測）。

**振る舞い（POSIX.1-2024 XCU `timeout` に従う）**

- `-f` が無いとき: `setpgid(0, 0)` で自分をプロセスグループの先頭にしてから子を起動する
  （既にセッションリーダーで失敗する場合は無視して続行 — GNU と同じ）。時間切れでは
  **子の pid へ直接**送り、続けて**グループ全体**へ送る（子が自分でグループを変えた場合にも
  子本人には届く — GNU と同じ）。自分に当てた分は中継しない。
- SIGKILL 以外を送った後は SIGCONT も同じ宛先へ送る（規格は停止中なら必須・それ以外は任意。
  GNU に合わせて常に送る。`-f` 時は送らない）。
- **中継**: timeout 自身が次のシグナルを受けたら、時間切れ時と同じ宛先へ即座に送る。これを
  「最初のシグナル」とみなし `-k` の計時を始める。
  - macOS の `signal(3)` で既定動作が terminate / core のもののうち SIGKILL・SIGSTOP を除く全部:
    HUP INT QUIT ILL TRAP ABRT EMT FPE BUS SEGV SYS PIPE ALRM TERM XCPU XFSZ VTALRM PROF USR1 USR2
  - `-s` で指定したシグナル（既定動作が終了でなくても — 規格の ASYNCHRONOUS EVENTS）
  - ただし timeout が**無視で受け継いだ**シグナルは中継しない（子も無視のまま受け継ぐので整合する）。
- SIGTTIN / SIGTTOU は無視する（規格の文言どおり）。
- SIGCHLD は親では既定動作にする（無視で受け継ぐと `waitpid` が `ECHILD` になり終了状態を
  失う — 実測）。子では受け継いだ設定に戻す。
- **シグナル設定の引き継ぎ**: 起動直後に全シグナルの設定と mask を記録し、子では exec の前に
  その記録どおりへ戻す。唯一の例外は時間切れで送るシグナルで、子では既定動作にする。

**終了コード**

| 状況 | 終了コード |
|------|-----------|
| 時間切れ（`-p` なし、`-k` の SIGKILL を送る前に終わった） | 124 |
| 時間切れ後 `-k` で SIGKILL を送った（`-f` 併用を含む） | SIGKILL で自分も終わる（シェルでは 137。`-f` 併用時の GNU は `exit(137)` — 既知の差分） |
| `-s KILL` で時間切れ（`-f` なし） | グループへの SIGKILL で自分も死ぬ（137） |
| `-f -s KILL` で時間切れ（`-k` なし・`-p` なし） | 124（規格どおり。GNU は 137 — 既知の差分） |
| timeout 自身のエラー（引数不正・fork 失敗など） | 125 |
| コマンドはあるが実行できない | 126 |
| コマンドが見つからない | 127 |
| コマンドが正常終了した | その終了コード |
| コマンドがシグナルで死んだ（時間切れ以外、または `-p`） | 同じシグナルで自分も終わる。`RLIMIT_CORE` を 0 にしてコアを作らない |

126 と 127 の判定は `execvp` の結果による（`ENOENT` → 127、それ以外 → 126）。PATH の探索、
shebang の無い実行可能ファイルを `/bin/sh` で実行する扱いも `execvp` に従う（GNU と同じ。
macOS で実測済み）。

### Input / Output

- 標準入出力はコマンドへそのまま渡す（timeout は読み書きしない）。番号 3 以降の fd も
  そのまま子へ渡る。
- 標準エラーは timeout 自身の診断（エラー、`-v` の報告）にだけ使う。
  `-v` の形式は `timeout: sending signal TERM to command 'sleep'`。引用符は常に ASCII の
  `'`（GNU はロケールにより ‘ ’ — 既知の差分）。

### Configuration

無し（設定ファイル・環境変数なし。コマンドの探索に `PATH` のみ使う）。

### External Dependencies

- 実行時: 無し（外部サービス・資格情報なし）。libc のみ。
- ビルド: Xcode の Apple clang。サードパーティのライブラリ・ツールは使わない。
- テスト: Go 標準ライブラリ（外から本体を動かすテスト）。比較テストのみ podman と
  Linux イメージ上の GNU coreutils（照合相手としてだけ使い、配布物には入れない）。

## 3. Design Decisions

### 仕様の優先順位

1. **POSIX.1-2024**（IEEE Std 1003.1-2024 XCU `timeout`）が正本。
2. 規格が「実装定義」「未規定」とする箇所は **GNU coreutils**（9.12 のマニュアルとソース）に合わせる。
3. **GNU が規格と食い違う箇所は規格に従う**。既知の差分:
   - `-p` で時間切れになり子がシグナルで死んだ場合: GNU は exit 128+n、本実装は同じシグナルで自己終了。
   - 子の SIGTTIN/SIGTTOU: GNU は無条件に既定動作へ戻す、本実装は受け継いだ設定を保つ。
   - `-f` 併用で `-k` の SIGKILL を送った場合: GNU は `exit(137)`、本実装は SIGKILL で自己終了
     （規格の「`-p` と同じ扱い」を選ぶ。シェルからはどちらも 137）。
   - `-f -s KILL`（`-k` なし）: GNU は 137、本実装は 124（規格の EXIT STATUS どおり）。
   - `-v` の引用符（上記）。

  差分表は ADR に置き、比較テストはこの表を既知の差分として扱う。

### 名前

`timeout`（コマンド・リポジトリとも）。別名では既存スクリプトとエージェントの書き癖に
効かない。macOS と Homebrew（core・tap）に同名は無い（2026-09-29 確認）。

### 言語は C（第 1 版の Go から変更）

独立検証と再現実験（2026-09-29）で、Go では規格の 3 要求を満たせないと確認した:

- Go ランタイムは起動時に、無視で受け継いだシグナルのうち HUP/INT 以外へ自分の
  ハンドラを入れる。そのため `signal.Ignored` は偽を返し、子では既定動作に戻る
  （`trap '' TERM` 配下で TERM・PIPE が子で DFL）。
- `signal.Reset` 後の自己 kill で QUIT は exit 2、PIPE・USR1 では生き残る
  （同じシグナルで自己終了できない）。
- SEGV/BUS/ILL を受けるとランタイムが落ち、PIPE/PROF は Notify に届かない（全部を中継できない）。

cgo で一部は補えるが、中継はランタイムと衝突する。規格・GNU・FreeBSD の実装はいずれも C の
fork/exec/sigaction を前提にしており、C なら 3 要求とも直接書ける。加えて、時間の解釈
（`strtod`）と 126/127・PATH 探索（`execvp`）が自然に GNU と同じになる。依存は libc だけ。
組織の Go 骨組みからの逸脱（ビルドは clang、テストの外側は Go）は AGENTS.md に記す。

### シグナルの受け方

受信はすべて kqueue の `EVFILT_SIGNAL` で行う（無視中・塞いでいる間も配送の試みを記録する —
`kqueue(2)`、2026-09-29 に全 22 シグナルで実測）。子の終了は `EVFILT_PROC`/`NOTE_EXIT`、
時間切れは `EVFILT_TIMER`（`EV_ONESHOT`）で待つ。シグナルは 3 群に分けて扱う。

| 群 | 親での設定 | 理由 |
|---|---|---|
| 中継対象（下の同期 5 種を除く）と `-s` のシグナル、SIGCHLD | **既定動作のまま塞ぐ**（block） | 無視にすると、fork から設定の復元までの隙に子へ送ったシグナルを XNU が生成時に捨てる（300 回中 292 回消失 — 実測）。塞いだ既定動作なら子で保留され、子が設定を戻してから塞ぎを外した時点で届く |
| 同期で起きうる 5 種（SEGV BUS ILL TRAP FPE） | 最小限の `SA_SIGINFO` ハンドラ | 無視や塞ぎにすると、timeout 自身の不具合（NULL 参照、`-fbounds-safety` の停止＝SIGTRAP）で空回りし、計時も終了もしなくなる（実測）。ハンドラは `si_code` で分ける: 外からの送信（`SI_USER` 等）は何もせず戻る（kqueue が記録するので中継される）。本物の例外は既定動作へ戻して戻る（再発生して死ぬ）。ハンドラ内は `sigaction` だけを呼ぶ |
| SIGTTIN / SIGTTOU | 無視 | 規格の要求 |

- 子は、塞がれた状態のまま受け継いだ設定へ戻し、次に受け継いだ mask へ戻してから `execvp` する。
- 自分が送った分（`kill(0, sig)`・自分の pid 宛て）は、kill から戻った時点で kqueue に記録済み
  になる（実測）。記録の `data` は回数なので、自分が送った回数を差し引けば、同時刻に外から来た分も
  区別できる。
- 自己終了の直前には、自分に保留中のシグナルを無視設定で捨ててから、対象のシグナルだけを
  既定動作・塞ぎ解除にして送る（保留中の別シグナルで先に死なないため）。
- 子の終了監視の登録が `ESRCH` で失敗したら、既に終了したものとして `waitpid` へ進む
  （すぐ終わる子で起きる — 実測）。
- プログラムは単一スレッドに保ち、fork 後の子で呼べる関数の制約を満たす。
- この方式は Phase 1 の最初の実験で成立を確かめる。

### スリープ中は時間を数えない

利用者決定（2026-09-29。Linux の GNU 版と同じ）。根拠:
- GNU は `timer_create(CLOCK_REALTIME)` の相対タイマーを使い、Linux は相対の
  `CLOCK_REALTIME` タイマーを `CLOCK_MONOTONIC` で計る（`kernel/time/hrtimer.c`）。
  Linux の `CLOCK_MONOTONIC` はサスペンド中に止まるので、Linux の GNU 版はスリープ中の時間を
  数えない。
- macOS では kqueue タイマーの既定の時計（`mach_absolute_time` 系。`clock_gettime(3)` の
  `CLOCK_UPTIME_RAW` と同じで、スリープ中は進まない）を使い、`NOTE_MACH_CONTINUOUS_TIME` は
  付けない。
- 壁時計（`CLOCK_REALTIME`）は時刻合わせで跳ぶので使わない。
- 挙動は Phase 1 の実機スリープ測定で確かめる。

（第 2 版まで「スリープ中も数える＝Linux の GNU 版と同じ」としていたが、根拠が逆だった。
2 回目の独立検証で判明し、利用者が決定し直した。）

### メモリ境界と自動変数の規則（利用者指示 2026-09-29）

規則は 5 層の機械的な検査で守る。2026-09-29 に Apple clang 21 で、わざとバグを入れたコードに
対する各層の検出を実測した（下表）。1 層だけでは足りない。

| 仕込んだバグ | 警告 | 静的解析 | ASan | `-fbounds-safety` |
|---|---|---|---|---|
| 条件次第で未初期化の自動変数 | 検出 | 検出 | — | — |
| 自動変数のアドレスを返す | 検出 | 検出 | — | — |
| 配列を越える memcpy | 見逃し | 見逃し | 検出 | — |
| 長さ 3 の配列の添字 3 | — | — | — | 実行時に停止（通常ビルドは黙って読む） |

**コーディング規則**

- **自動変数**: 全部を宣言時に初期化する（構造体は `= {0}`）。アドレスを関数の外へ
  持ち出さない（返さない・静的変数や構造体に保存しない）。VLA と `alloca` を使わない。
  スタック上の配列は固定長の小さなものだけにする（上限は ADR で決める）。
- **メモリ境界**: 利用者の入力を固定長バッファへコピーしない（argv はポインタのまま扱う）。
  `strcpy`/`strcat`/`sprintf`/`gets` を使わない。書式出力は `snprintf` で、戻り値を検査して
  切り詰めを扱う。配列の添字は使う前に範囲を検査し、長さは `sizeof` か `size_t` の定数で
  持つ（シグナル名の表など）。
- **動的確保を使わない**（必要が出たら理由を ADR に記録）。
- **数値変換**: `double` から整数・`timespec` へ変換する前に `isfinite` と範囲を検査する
  （範囲外の変換は未定義動作）。時間の加算は桁あふれを検査して最大値へ丸める。
  `int`/`pid_t`/`size_t` の間の暗黙変換を許さない（`-Wconversion -Wsign-conversion`）。
- **fork 後の子**: シグナル設定の復元・mask の復元・`execvp`・`_exit` だけを行う。
  エラーの報告は `write` と固定の文言で行い、`printf` 系を呼ばない。
- **シグナルハンドラ**（同期 5 種のみ）: `sigaction` 以外を呼ばない。自動変数以外に触れない。

**機械による強制（5 層）**

| 層 | 内容 | どこで |
|---|---|---|
| 1. 警告 | `-std=c17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wvla -Wformat=2 -Wimplicit-fallthrough -Wuninitialized -Wsometimes-uninitialized -Wreturn-stack-address` | 全ビルド |
| 2. 静的解析 | `clang --analyze -Xanalyzer -analyzer-werror`（`-Werror` だけでは指摘があっても rc=0 — 実測） | `make lint`（`make test` から呼ぶ） |
| 3. 境界検査 | `-fbounds-safety`（ASan と同時に使える — 実測） | 配布ビルドとテストビルドの両方 |
| 4. 実行時の検出 | 配布: `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -O2`（Apple の SDK は 2 を超える値を 2 として扱う）。テスト: ASan + UBSan に `-fno-sanitize-recover=all`（無いと UBSan は報告して続行し rc=0 — 実測）、`-ftrivial-auto-var-init=pattern` | `make build` / `make test` |
| 5. 入力の揺さぶり | 時間とシグナル名の解析へ、生成した大量の入力（境界値・長大文字列・不正バイト）を ASan 下で流す（Apple clang に libFuzzer が無いための代替） | `make test` |

- **各層の陽性対照**: 各層に、わざとバグを入れた小さなファイルを置き、その層が本当に失敗する
  ことを `make test` の最初に確かめる（一度も失敗しない検査は検査として働いているか分からない）。
- **ASan とシグナル**: ASan は main より前に SEGV/BUS/FPE へ自分のハンドラを入れるので、
  テストビルドでは `ASAN_OPTIONS=handle_segv=0:handle_sigbus=0:handle_sigfpe=0` を付ける。
  外から動かすテストは、テストビルドと**配布ビルドの両方**に当てる。
- **`-fbounds-safety` の成立条件**（2 回目の検証で実験済み）: `int main(int argc, char
  *__null_terminated *__counted_by(argc) argv)` とし、`strtod` の endptr は
  `char *__null_terminated` にする。`execvp` へ渡す 1 か所だけは `argv[argc]` の NULL が
  範囲外になるため `__unsafe_forge_null_terminated` を使う — 規則の唯一の例外として ADR に記録する。

### 子孫への届け方と残余リスク

子孫へはプロセスグループ方式でのみ届ける。規格が挙げるもう一方（孤児を引き取る reaper
方式）に相当する機構が macOS に無い（SDK の `usr/include` に `procctl`・
`PR_SET_CHILD_SUBREAPER`・`PR_SET_PDEATHSIG` が無いことを 2026-09-29 に確認）。

許容する残余リスク（実装前に ADR へ理由つきで記録）:
1. 自分でプロセスグループを抜けた子孫（`setsid`、デーモン化）には届かない。規格も
   RATIONALE で認めている。
2. timeout 自身が SIGKILL されると子は残る（親の死で子へシグナルを送る機構が無い）。
   GNU マニュアルも同じ注意を書いている。
3. `-f` なしで使うと timeout とコマンドは別のプロセスグループになる:
   - 外側（エージェントのハーネス等）が元のグループへ kill しても届かない。時間 0 なら
     残り続ける。timeout 本人の pid へ送れば中継される — README に書く。
   - スクリプトから端末を読むコマンドは SIGTTIN で停止し、端末の Ctrl-C も届かない。
     GNU と同じ。README に `-f` の使いどころとして書く。

### 対象外

Linux / Windows 向けビルド（Linux には GNU 版がある）、ロケール依存の小数点（カンマ）と
メッセージ翻訳、GNU 以外の独自拡張。組織の Makefile テンプレート（4 platform、darwin は
arm64 のみ）から linux/windows を外すことは AGENTS.md に記す。

### 補完する既存ツール

組織内に同種のツールは無い。エージェント運用で使ってきた perl alarm の回避策を置き換える。

## 4. Development Plan

### Phase 0: ADR（実装の前に書く）

- ADR-0001 言語を C にする理由と、メモリ境界・自動変数の規則（上記の規則と 5 層、
  `__unsafe_forge_null_terminated` の例外）
- ADR-0002 シグナルの受け方（3 群の扱い・kqueue）と引き継ぎの復元
- ADR-0003 POSIX と GNU の優先順位と既知の差分表
- ADR-0004 残余リスク 1〜3
- ADR-0005 時間の扱い（スリープ中は数えない・端の値・上限値）

### Phase 1: Core

- **最初に実験**（合否の基準を先に書いてから行う）:
  - 塞いだ既定動作のシグナルが kqueue に記録され、fork 直後に子へ送ったものが失われない
    （2 回目の検証の再現手順で 300 回中 0 回消失）。
  - 同期 5 種のハンドラ: 外からの送信は中継され、本物の例外（NULL 参照・`-fbounds-safety` の停止）
    では空回りせず終わる。
  - 自己終了の手順で、保留中の別シグナルに先を越されない。
- 時間・シグナル名の解析（純関数）と C の単体テスト、入力の揺さぶりテスト。
  自己終了の準備処理（`RLIMIT_CORE` を 0 にする等）も単体テストで確かめる（この Mac では
  コアの有無を外から観測できないため — `/cores` が root 専用で、対照でもコアの印が立たない）。
- 子の起動、プロセスグループ、子 pid とグループへの送信、SIGCONT、`-k`、`-f`、`-p`、
  中継、SIGTTIN/SIGTTOU、SIGCHLD、シグナル設定と mask の復元、終了コードとシグナルでの自己終了。
- 外から本体を動かすテスト（Go 標準ライブラリ。子の設定は小さな C の補助プログラムで読む）:
  - `make test` が C のビルドを先に済ませ、本体の場所を環境変数で渡す。`go test ./...` を
    単独で走らせて本体が無ければ、skip ではなく**失敗**させる。
  - perl alarm で実測した 3 場面（無視するコマンド・孫の残存・終了コード）
  - 124〜127、shebang の無いスクリプト、PATH 上の実行権の無いファイル、すぐ終わる子
  - 受け継いだ無視（HUP・TERM・QUIT・PIPE・USR1・TTIN・CHLD）が子へ届くこと
  - 中継対象の全シグナルと `-s` のシグナルの中継、各シグナルでの自己終了
  - fork 直後に送ったシグナルが失われないこと
  - 停止中のコマンド、子が自分でグループを変えた場合、`-f`+`-k`、`-s KILL`、`-f -s KILL`
  - 外側から元のグループへ kill した場合（残余リスク 3 の挙動を固定）
  - 極小・下位桁あふれ・`-0`・NaN・無限大・範囲外の時間
  - 時間に依存するテストは許容幅を決め、複数回反復して揺らぎを見る
- **完了条件（スリープ測定）**: この Mac で、利用者に一度だけ手を貸してもらい測る。
  `timeout 120 sleep 600` を実行中にスリープさせ、3 分以上たってから起こす。起きた直後には
  終わらず、起きている時間の合計が約 120 秒になった時点で 124 で終わることと、各時刻を記録する。
  設定の変更・`pmset` の操作はしない。

### Phase 2: Features

- GNU 互換の仕上げ: 長いオプションと省略形、`-v` の出力、シグナル番号、`--help` / `--version`。
- **GNU 版との比較テスト**（2026-09-29 利用者承認）: `make test-gnu` として独立させる。
  podman 上の Linux の GNU coreutils `timeout` と、同じ入力の終了コードと振る舞いを突き合わせる。
  - podman が無ければ**失敗**させる（skip しない）。リリース手順で必ず実行する。
  - シグナルは番号ではなく名前で照合する（Linux と macOS で番号が違う）。
  - ADR-0003 の既知の差分は差分として扱う。
  - 最初に、自分の仕掛けの欠陥（プラットフォーム指定・権限・ツールチェーン）を検出する
    陽性対照を置く。

### Phase 3: Release

- README.md / README.ja.md、CHANGELOG.md、AGENTS.md。
- Developer ID 署名 + notarize、darwin-arm64 のみビルド、GitHub リリース。
- homebrew-tap に formula `timeout` を追加。
- util-series へ submodule 追加、util-series README のカタログ行、org プロフィール、
  nlink-web-site のカタログを更新、`check-org.sh`。
- knowledge へ還元:
  - POSIX.1-2024 に `timeout` があること。
  - 時計: Linux の GNU `timeout` はスリープ中を数えない。macOS の `CLOCK_MONOTONIC` と
    `NOTE_MACH_CONTINUOUS_TIME` は数え、kqueue の既定は数えない。
  - Go ではシグナル設定の引き継ぎ・自己終了・全シグナルの中継ができないこと（実測）。
  - シグナルを無視すると fork 直後の子へのシグナルが消え、同期シグナルの無視は空回りを招くこと。
  - C の安全策の各層が何を拾うか（実測表）と、`--analyze` と UBSan の既定が失敗しないこと。

各 Phase は独立にレビューできる。設計確定時（本 RFP 承認時）とリリース前に、独立した
コンテキストの検証パスを挟む（設計の検証は 2 回で打ち切り、残りは Phase 1 の実験で確かめる）。

## 5. Required API Scopes / Permissions

None.

## 6. Series Placement

Series: util-series
Reason: 外部サービスに依存しない汎用 CLI。パイプやスクリプトの部品として使う。

## 7. External Platform Constraints

外部サービスは無し。macOS（XNU）の制約:

- 孤児になった子孫を引き取る仕組み（FreeBSD `procctl(PROC_REAP_ACQUIRE)`、Linux
  `PR_SET_CHILD_SUBREAPER`）が無い → プロセスグループ方式のみ（SDK ヘッダで確認）。
- 親の死で子へシグナルを送る仕組み（Linux `PR_SET_PDEATHSIG`）が無い → 残余リスク 2。
- 無視中に生成されたシグナルは生成時に捨てられる（fork 直後の子で実測）。同期の例外シグナルを
  無視・塞ぐと空回りする（実測）。
- 時計: kqueue タイマーの既定と `CLOCK_UPTIME_RAW`（= `mach_absolute_time`）はスリープ中に
  進まない。`CLOCK_MONOTONIC` と `NOTE_MACH_CONTINUOUS_TIME` は進む。
- Apple clang に libFuzzer（`-fsanitize=fuzzer`）が無い。Apple の SDK は `_FORTIFY_SOURCE` を
  最大 2 として扱う。

---

## Discussion Log

- **2026-09-29 前提の実測**: macOS 27.0 に `timeout` / `gtimeout` 無し。perl alarm の代用を
  実測 — (A) `trap "" ALRM` するコマンドは 1 秒指定で 3 秒走りきり rc=0、(B) `sh -c 'sleep 30 & wait'`
  で親だけ死に `sleep 30` が PPID 1 で残存、(C) 時間切れの rc=142。
- **仕様の出所**: POSIX.1-2024 に `timeout` が Issue 8 で新規収録されていることを
  pubs.opengroup.org で確認。GNU coreutils 9.12 マニュアルで互換拡張と終了コード 137 を確認。
- **代替案**: Homebrew の coreutils（`gtimeout`）は名前が違い既存スクリプトに効かず、
  コミュニティ製ツールの不採用方針にも合わない。FreeBSD の `timeout.c` は reaper 機構に
  依存し macOS へそのまま移せない。
- **利用者決定（2026-09-29）**: 名前は `timeout` / GNU 互換まで含める / GNU 版とのコンテナ比較
  テストを行う。スリープ中の扱いは当初「数える」としたが後に変更（下記）。
- **第 1 版への独立検証（2026-09-29）**: 高 3 件・中 7 件・低 6 件と抜け多数の指摘。高 3 件
  （Go でシグナル設定の引き継ぎ・自己終了・全中継ができない）は本人の再現実験でも同じ結果
  （親で HUP/TERM/PIPE を無視 → `Ignored(TERM)=false`、子で TERM/PIPE=DFL。自己 kill で
  QUIT は exit 2、PIPE/USR1 は生存）。第 1 版の「Go でも規格を満たせる」は撤回。
- **利用者決定（2026-09-29）**: 言語を C に変更。**メモリ境界と自動変数の扱いに十分留意する**
  こと — §3 の規則と機械強制として具体化した。これに伴い `golang.org/x/sys` の承認は不要になった。
- **第 1 版の指摘の採否（2026-09-29）**: TTIN/TTOU は無視（規格の文言どおり）。GNU と規格の
  食い違いは規格を優先し差分表を作る。SIGCONT は GNU に合わせ常に送る（停止の検出が不要になる）。
  比較テストは skip せず失敗させ、シグナルは名前で照合。時間の端の値は GNU の規則。
  残余リスク 3 を追加。スリープ測定は借り物の機械ではなくこの Mac で利用者の手を借りて行う。
  ADR は実装前（Phase 0）。否定の断定に SDK ヘッダの根拠を付けた。カタログ更新を追加。
  抜けはすべて仕様と計画に入れた。
- **第 2 版への独立検証（2026-09-29）**: 高 2 件・中 6 件・低 6 件。高 1（同期シグナルの無視で
  空回り）は本人の再現でも同じ（SEGV を無視して NULL 参照 → 3 秒の打ち切りまで戻らず）。
  高 2（fork 直後の子へのシグナル消失 292/300）は、無視中のシグナルを生成時に捨てるという
  規格上の扱いから整合する。
- **利用者決定（2026-09-29）**: 「スリープ中も数える＝Linux の GNU 版と同じ」は根拠が逆だった
  （Linux の GNU 版は数えない）と伝えたうえで、**スリープ中は数えない**に決定し直した。
- **第 2 版の指摘の採否（2026-09-29、利用者承認）**: 同期 5 種は最小限のハンドラ、それ以外の
  受信対象は塞いだ既定動作で kqueue 受信（「ハンドラを置かない」を改めた）。`ESRCH` は終了済み扱い。
  SIGCHLD は親で既定動作。`-f` と KILL の組み合わせは規格を優先して差分表へ。静的解析と UBSan に
  失敗させる指定と各層の陽性対照。ASan の SEGV/BUS/FPE 処理を切り、配布ビルドにも外のテストを
  当てる。`-s` のシグナルも中継。端の値は GNU の規則。FORTIFY は 2。層の数と platform の記述を
  訂正。コアの有無は単体テストで確かめる。タイマーの細部と `-fbounds-safety` の成立条件を記載。
- **設計の検証は 2 回で打ち切る（2026-09-29、利用者承認）**: 重さが下がり、残る論点は実験で
  白黒がつくため、Phase 0（ADR）と Phase 1 冒頭の実験で確かめる。
