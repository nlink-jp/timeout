# ADR-0003: POSIX を先に、GNU を次に。既知の差分

| Field | Value |
|-------|-------|
| Status | **Accepted** |
| Date | 2026-09-29 |
| Binds | timeout |
| Decision makers | nlink-jp maintainers |
| Triggered by | GNU coreutils の `timeout` が POSIX.1-2024 と食い違う箇所がいくつかあり、比較テストには想定内の差分の一覧が要る |

## Context

このツールは、Linux 向けのスクリプトを書き換えずに動かすためにある。その点では GNU の振る舞いに
合わせたい。しかし正本は POSIX.1-2024 XCU `timeout` であり、GNU はいくつかの箇所で、規格の
RATIONALE が名指しで批判していることをしている。GNU との比較テスト（Phase 2）は、そうした箇所を
一覧にしておかないと、そのたびに失敗する。

## Decision

1. **POSIX.1-2024 XCU `timeout` を正本とする。**
2. 規格が「実装定義」「未規定」とする箇所は、**GNU coreutils に合わせる**（マニュアルとソース）。
3. GNU が規格と食い違う箇所は**規格に従い**、下の表に載せる。比較テストはこの表を想定内の差分として
   扱い、それ以外の差分は不具合とみなす。

### GNU との既知の差分

| 場面 | GNU | 本プロジェクト | 規格 |
|---|---|---|---|
| `-p` で時間切れになり、子がシグナル *n* で死んだ | exit 128+*n* | *n* で自己終了 | EXIT STATUS: 自己終了で wait 状態をまねる |
| 子の SIGTTIN/SIGTTOU | 常に既定動作へ戻す | 受け継いだ設定を保つ | ASYNCHRONOUS EVENTS と RATIONALE: 受け継いだ設定をそのまま渡す |
| `-f` で、`-k` の SIGKILL を送った | `exit(137)` | SIGKILL で自己終了（シェルからはどちらも 137） | EXIT STATUS: 124 か「`-p` と同じ扱い」 — 後者を選ぶ |
| `-f -s KILL`、`-k` も `-p` も無し | 137 | 124 | EXIT STATUS: `-k` が無ければ 124 |
| `-v` の引用符 | ロケールによる（UTF-8 では ‘ ’） | 常に ASCII の `'` | 規定なし。ロケールは扱わない（RFP §3 対象外） |
| `-s CHLD` | 戻ってこない（9.7 で観測） | 時間切れの後にコマンドが終われば 124 | 規定なし。戻ることだけが役に立つ振る舞い（2026-09-30 追加。`make test-gnu` では止まってしまうので扱わない） |

### GNU に合わせて決めた未規定の点

- `-s 0` と `-s EXIT`（大小文字を問わず、`SIG` 接頭辞は任意）は空のシグナルを表す。何も届かず、
  時間切れの後にコマンドが終われば 124 で終わる。規格は 0 の受け付けを求めていないが、GNU は受け付ける
  （2026-09-30、比較テストで発見）。
- SIGCONT は、停止中の子に限らず、SIGKILL 以外を送るたびに送る（`-f` のときは送らない）。
- シグナルは子の pid へ直接送り、続けてグループへ送る。
- timeout が既にセッションリーダーであるために `setpgid` が失敗しても無視する。
- 時間の書式と端の値は GNU の `strtod` と `dtimespec_bound` に従う（ADR-0005）。
- オプションの解析は GNU の optstring `"+fk:ps:v"` と長いオプションを使い、曖昧でない省略形を受け付ける。
- `-v` の文言: `timeout: sending signal NAME to command 'CMD'`。送るすべてのシグナルについて出す —
  時間切れのもの、`-k` の KILL、中継したもの（GNU は中継したシグナルも報告する。2026-09-30、
  リリース前の検証で発見）。
- `-f -k`: KILL で終わるのは、その KILL がコマンドを終わらせた場合だけ。直前にコマンドが自分で
  終わっていれば 124。GNU と同じ判定。

### 逸脱ではなく環境の違い

- 31 を超えるシグナル番号は拒否する。macOS には 32 番以上のシグナルが無い。Linux には 64 番までの
  リアルタイムシグナルがあり、GNU では `-s 64` が有効。

### 訂正（2026-09-30）

2026-09-29 に 6 行目「時間 0 で `-k` を指定しシグナルを中継した: GNU は無効」を、GNU マニュアルの
「主の時間 … が 0 ならこのオプションは効かない」という一文から追加した。比較テストで、GNU 9.7 は
中継したシグナルから `-k` を始めると分かった — 規格の要求どおりで、本プロジェクトと同じ。マニュアルの
一文は、シグナルを何も送らない場合の話だった。行は削除した。照合相手は GNU coreutils 9.7（Debian 13）で、
読んだマニュアルは 9.12 のもの。

## Consequences

- 比較テストは GNU と突き合わせ、この表に無い差分で失敗する。どちら向きのずれも見える。
- 表の GNU の振る舞いに依存するスクリプトは、規格どおりの振る舞いを受け取る。5 件とも端の場面
  （`-p` とシグナル死、TTIN/TTOU を無視して起動したスクリプト、`-f` と SIGKILL、引用符の文字）。

## Alternatives considered

- **すべて GNU。** 「Linux のスクリプトが書き換えずに動く」に最も近いが、規格が欠陥と名指しする
  振る舞い（`nohup` 型の設定の喪失）を再現してしまう。不採用。
- **POSIX だけ、GNU は無視。** 未規定の点がすべて創作になり、スクリプトが使う長いオプションも
  落ちる。不採用。

## References

- POSIX.1-2024 XCU `timeout`: https://pubs.opengroup.org/onlinepubs/9799919799/utilities/timeout.html
- GNU coreutils マニュアル `timeout` invocation: https://www.gnu.org/software/coreutils/manual/html_node/timeout-invocation.html
- GNU coreutils `src/timeout.c`
- [RFP](../timeout-rfp.ja.md) §3 仕様の優先順位
