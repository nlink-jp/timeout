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
| 時間 0 で `-k` を指定し、シグナルを中継した | 時間 0 なら `-k` は無効（マニュアル） | 中継したシグナルから `-k` の計時を始める | DESCRIPTION: 中継したシグナルは「最初のシグナルとして扱う」（2026-09-29、実装中に発見して追加） |

### GNU に合わせて決めた未規定の点

- SIGCONT は、停止中の子に限らず、SIGKILL 以外を送るたびに送る（`-f` のときは送らない）。
- シグナルは子の pid へ直接送り、続けてグループへ送る。
- timeout が既にセッションリーダーであるために `setpgid` が失敗しても無視する。
- 時間の書式と端の値は GNU の `strtod` と `dtimespec_bound` に従う（ADR-0005）。
- オプションの解析は GNU の optstring `"+fk:ps:v"` と長いオプションを使い、曖昧でない省略形を受け付ける。
- `-v` の文言: `timeout: sending signal NAME to command 'CMD'`。

## Consequences

- 比較テストは GNU と突き合わせ、この表に無い差分で失敗する。どちら向きのずれも見える。
- 表の GNU の振る舞いに依存するスクリプトは、規格どおりの振る舞いを受け取る。6 件とも端の場面
  （`-p` とシグナル死、TTIN/TTOU を無視して起動したスクリプト、`-f` と SIGKILL、引用符の文字、
  時間制限なしの `-k`）。

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
