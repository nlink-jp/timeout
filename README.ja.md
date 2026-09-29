# timeout

macOS で、コマンドを時間制限つきで実行する。

macOS には `timeout` コマンドが無いため、Linux 向けに書かれたスクリプトや、
コーディングエージェントが書き癖で使う `timeout 10 cmd` は `command not found` で
失敗する。この `timeout` は POSIX.1-2024 の仕様に従い、GNU coreutils のオプションにも
対応するので、そうしたスクリプトを書き換えずに動かせる。

## ビルド

Xcode（Apple clang）と、テスト用に Go が必要。

```bash
make build   # dist/timeout
make test    # 安全策の陽性対照、静的解析、外から動かすテスト
```

## 文書

- [RFP](docs/ja/timeout-rfp.ja.md) — 仕様
