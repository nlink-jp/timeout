# timeout

macOS で、コマンドを時間制限つきで実行する。

macOS には `timeout` コマンドが無いため、Linux 向けに書かれたスクリプトや、
コーディングエージェントが書き癖で使う `timeout 10 cmd` は `command not found` で
失敗する。この `timeout` は POSIX.1-2024 の仕様に従い、GNU coreutils のオプションにも
対応するので、そうしたスクリプトを書き換えずに動かせる。

## 特徴

- POSIX.1-2024 の `-f`・`-k`・`-p`・`-s` と、GNU の長いオプション `--foreground`・
  `--kill-after`・`--preserve-status`・`--signal`・`--verbose`。
- コマンドの子孫も時間切れにし（`-f` 以外）、シグナルの後に SIGCONT を送って一時停止中の
  コマンドも起こす。
- 受けた終了系のシグナルはすべてコマンドへ中継し、コマンドがシグナルで死んだときは
  同じシグナルで終わる。
- コマンドは、`timeout` が起動時に受け継いだシグナル設定と mask をそのまま受け継ぐ
  （時間切れで送るシグナルだけ既定動作に戻す）。`nohup` の下でも SIGHUP を無視したまま動く。
- スリープ中の時間は数えない。Linux の GNU `timeout` と同じ。
- libc だけを使う C で書き、境界検査つきでビルドし、静的解析とサニタイザで検査している。

## インストール

ソースからビルドする（Apple シリコン、macOS 12 以降）:

```bash
make build            # dist/timeout
cp dist/timeout /usr/local/bin/
```

## 使い方

```
timeout [OPTION]... DURATION COMMAND [ARG]...
```

DURATION は数値に任意の単位を付けたもの: `s` 秒（既定）、`m` 分、`h` 時間、`d` 日 —
`10`、`1.5m`、`2h`。`0` は時間制限なし。

| オプション | 意味 |
|------------|------|
| `-s SIG`, `--signal=SIG` | 時間切れで送るシグナル（既定 `TERM`）。`INT`・`SIGINT` のような名前か番号 |
| `-k DUR`, `--kill-after=DUR` | 最初のシグナルからこの時間たってもまだ動いていれば `KILL` も送る |
| `-p`, `--preserve-status` | 時間切れでもコマンドの終了状態で終わる |
| `-f`, `--foreground` | コマンド本人だけを時間切れにし、子孫は止めない。端末を読めるようにする |
| `-v`, `--verbose` | 時間切れで送ったシグナルを標準エラーへ報告する |
| `--help`, `--version` | 使い方・版数を表示する |

オプションは DURATION より前に書く。コマンド名より後ろはすべてコマンドのもの
（`timeout 5 ls -l`）。`-` で始まる DURATION の前には `--` を置く。

```bash
timeout 10 make test                 # 10 秒後に TERM
timeout -s INT 5m ./server           # 5 分後に INT
timeout -k 10 1h ./batch             # 1 時間後に TERM、その 10 秒後に KILL
timeout -f 30 ./interactive-tool     # ツールが端末を読める
```

### 終了コード

| コード | 意味 |
|--------|------|
| 124 | コマンドが時間切れになった（`-p` なし） |
| 125 | `timeout` 自身が失敗した（引数の誤りなど） |
| 126 | コマンドは見つかったが実行できない |
| 127 | コマンドが見つからない |
| 137 | `-k`、または `-f` なしの `-s KILL` で `KILL` を送った |
| その他 | コマンド自身の終了コード。コマンドがシグナルで死んだときは、`timeout` も同じシグナルで終わる |

### `-f` の使いどころ

`-f` なしでは、`timeout` は自分とコマンドを独自のプロセスグループに入れて、子孫を
すべて止められるようにする。その結果:

- スクリプトから端末を読むコマンドを実行すると停止し（SIGTTIN）、端末の Ctrl-C も届かない。
  対話的なコマンドには `-f` を使う。
- `timeout` を起動したプロセスグループへ向けた kill（エージェントのハーネスや CI ランナーなど）は
  届かない。代わりに `timeout` 本人のプロセス ID へ送る。`timeout` がコマンドへ中継する。

`-f` では、コマンド本人だけが時間切れになり、その子は止めない。

### 限界

- 自分でプロセスグループを抜けた子孫（デーモン、`setsid`）には届かない。
- `timeout` が `KILL` で殺されると、コマンドは動き続ける（macOS には、親と一緒に子を終わらせる
  仕組みが無い）。
- 同じ `SEGV`・`BUS`・`ILL`・`TRAP`・`FPE` を一瞬のうちに 8 回以上 `timeout` へ送ると、
  `timeout` 自身の異常終了とみなされ、中継せずに終わる。

### GNU `timeout` との違い

GNU が POSIX.1-2024 と食い違う箇所では、この `timeout` は規格に従う:

- `-p` で時間切れになったコマンドがシグナルで死ぬと、`timeout` も同じシグナルで終わる
  （GNU は 128+n で終わる）。
- コマンドの SIGTTIN・SIGTTOU は受け継いだまま（GNU は既定動作に戻す）。
- `-f -s KILL` は 124 で終わる（GNU は 137）。
- `-v` の引用符は常に `'`。

差分の一覧は [ADR-0003](docs/ja/adr/0003-posix-and-gnu-precedence.ja.md) にある。

## ビルド

Xcode（Apple clang）と、テスト用に Go が必要。

```bash
make build   # dist/timeout
make test    # 安全策の陽性対照、静的解析、単体テスト、外から動かすテスト
make test-gnu  # Linux コンテナ内の GNU timeout と結果を突き合わせる（podman）
```

## 文書

- [RFP](docs/ja/timeout-rfp.ja.md) — 仕様
- [設計判断記録（ADR）](docs/ja/adr/)
