# iOS MCPE (v1.26.62) Emulator

Windows 10 / 11 (x64) 上で iOS 版 Minecraft（統合版）1.26.62 (arm64) を動かすエミュレータです。
iOS 全体ではなく、ゲームが使う部分だけを実装しています。
[iOS MCPE (v0.16.2) Emulator](../iOS%20MCPE(v0.16.2)%20Emulator) をもとに、1.2x 系の新しいゲームに合わせて作り直したものです。

できること：

- ゲームの起動、メニュー、ワールドの作成・読み込み、ワールド内のプレイ（通常の描画。下記「描画」）
- フルスクリーン（F11 / Alt+Enter。描画解像度もモニタに合わせて変わります）
- **PC モード**：PC のキーボードとマウスを、iPad に繋いだキーボード・マウスとしてゲームに渡します。
  ゲームはキーボード・マウス用の画面（タッチ用ボタンなし、照準、マウスカーソルでの操作）になり、
  ワールドの中ではマウスがキャプチャされて視点操作になります
- **スマホモード**：iPhone の Safari に画面を出して（Wi-Fi または USB）、iPhone のタッチで操作します。
  ゲームはタッチ用の画面（ジョイスティック / 十字キーなど）になります
- Mod（arm64 の dylib）

## 仕組み

| 層 | 実装 |
|---|---|
| CPU | [dynarmic](external/dynarmic) による ARM64 → x86-64 JIT（スレッドごとに1インスタンス） |
| メモリ | ゲストのアドレス = ホストのアドレス（ポインタをそのまま共有） |
| ローダ | 自前の Mach-O ローダ（rebase / bind / lazy bind / weak bind / **chained fixups** / TLV、`__init_offsets`） |
| アプリのフレームワーク | .ipa の `Frameworks\` の HttpClient / Xal / xsapi-c / PlayFabMultiplayer をそのまま読み込み。MinecraftSwift（StoreKit 2 の橋渡し、Swift ランタイムが必要）は読み込まず、そのクラスは空のクラスで代用 |
| libSystem | ホスト側 HLE（libc, stdio, pthread, mach, dyld, sysctl, BSD ソケット → Winsock） |
| **Metal** | ゲストの Metal / MetalKit（`guest/Metal`、`MTLDevice` などの Objective-C クラス、`CAMetalLayer`、`MTKView`）→ ホストの OpenGL 4.6 で実行（`hle_metal.cpp`）。座標系は `glClipControl` で Metal と同じ（上が原点、深度 0〜1）にし、バッファは永続マップした GL バッファ |
| シェーダー | ゲームの `renderer/materials/*.material.bin` に入っている MSL（Metal のシェーダー言語）を GLSL 4.60 に変換（`msl2glsl.cpp`。式の型を追って、MSL ではベクトルの成分ごとの比較などを GLSL の関数に直す）。リフレクション（ユニフォームのオフセット、頂点属性の名前）もこの解析から返す |
| UI の描画ライブラリ（Coherent Gameface） | ライセンス確認の RSA 署名検証を Windows の CNG（BCrypt）で実際に行う（`SecKeyRawVerify`） |
| キーボード・マウス | GameController の `GCKeyboard` / `GCMouse`（iPadOS のハードウェアキーボード・マウス）と `UIPointerInteraction`（カーソル位置）として実装（`guest/UIKit/GameController.m`）。ポインタロック（`prefersPointerLocked`）でマウスをキャプチャ（iPadOS と同じく、ルートのビューコントローラに毎フレーム問い合わせる） |
| ネットワーク | `NSURLSession` / `NSURLConnection`（`guest/Foundation/NSURLNet.m`）→ ホストの WinHTTP（`hle_http.cpp`。TLS・プロキシ・リダイレクト・gzip は Windows のもの）。`kqueue` / `kevent`（`hle_kqueue.cpp`。asio の WebSocket 用）、`nw_path_monitor` |
| Microsoft アカウントのサインイン | Xal の `ASWebAuthenticationSession` を WebView2 のウィンドウで開き、コールバックの URL スキーム（`ms-xal-…://`）への移動を捕まえて結果にする（`auth.cpp` / `auth_webview.cpp`。WebView2 が無いときは既定のブラウザ + URL スキームの登録）。デバイスキー（P-256 / ECDSA）と証明書の検証は Windows の CNG / CryptoAPI（`hle_security.cpp`） |
| libc++ / libc++abi / libunwind | LLVM のソースを arm64 向けにビルドしたゲスト用 dylib（Apple の ABI に合わせた設定） |
| Objective-C ランタイム | 自作（`guest/libobjc`、objc_msgSend は arm64 アセンブリ） |
| libdispatch, Foundation, CoreFoundation, UIKit, QuartzCore, CoreGraphics, AudioToolbox, Security など | 自作の最小実装（`guest/`、Objective-C）。コンパイル時に作られる定数（`@[...]` `@{...}` `@123` の NSConstantArray など）にも対応 |
| zlib | 公式ソースをゲスト用にビルド |
| iPhone への画面出力 | 内蔵の HTTP / WebSocket サーバー（`remote.cpp`）から Safari に JPEG の映像と PCM の音声を送り、タッチと文字入力を受け取る |
| Mod | `mods\*.dylib`（arm64）をゲームと一緒に読み込み、関数フックなどの API（`hle_mod.cpp`）を提供 |

`guest/` 以下は zig 同梱の clang で arm64 Mach-O にビルドされ、エミュレータがゲームと一緒にロードします。
ホスト ↔ ゲストの橋渡しは `svc #n` スタブ（`hle.h` の `HLE_FUNC` マクロ）です。

## 必要なもの

- Windows 10 / 11（x64）
- OpenGL 4.6 に対応した GPU とドライバー（NVIDIA / AMD / Intel の最近のもの）
- Visual Studio 2022 / 2026 と、ワークロード「C++ によるデスクトップ開発」
  （「Windows 用 C++ CMake ツール」を含む。CMake と Ninja は Visual Studio に付属）
- Python 3（[python.org](https://www.python.org/) 版。`py` コマンドが使えること）。初回ビルドで `ziglang` を pip で自動インストールします
- 自分で用意した、復号済みの `Minecraft 1.26.62`（iOS 版）の .ipa（作り方は下記「復号済み .ipa の作り方」）

App Store から入手したままの暗号化された .ipa（FairPlay DRM）は動きません（エミュレータは暗号化を検出すると、その旨を表示して終了します）。

**.ipa（ゲーム本体）はこのリポジトリにも、ビルドした exe にも含まれていません。** 著作権の都合で配布できないため、各自で用意して `ipa\` フォルダに置いてください。
エミュレータは起動時に、exe のフォルダから親フォルダへ順に `ipa\*.ipa` を探します（`--ipa <ファイル>` で直接指定も可）。
ゲームは初回起動時に `%LOCALAPPDATA%\iOS-MCPE-1.26.62\app\` に展開されます（.ipa が 700MB 近くあるため、初回は少し時間がかかります）。

### 復号済み .ipa の作り方（Windows：ipadecrypt）

[ipadecrypt](https://github.com/londek/ipadecrypt) は、App Store から .ipa をダウンロードして FairPlay の暗号化を外すコマンドラインツールです（Windows 用の exe があります）。
自分の Apple ID で入手した（購入済みの）アプリだけが対象です。

用意するもの：

- Minecraft を購入済みの Apple ID
- **脱獄済みの iPhone**（最初の 1 回だけ使います。palera1n / Dopamine など。Sileo で OpenSSH を入れ、PC から SSH で接続できること）。
  その iPhone に、同じ Apple ID の App Store アプリを 1 つ以上入れておきます

手順：

1. [リリースページ](https://github.com/londek/ipadecrypt/releases/latest) から `ipadecrypt_<バージョン>_windows_amd64.exe` をダウンロードし、
   `ipadecrypt.exe` に名前を変えて適当なフォルダに置きます（以下はそのフォルダのコマンドプロンプトで実行）
2. 初回だけ、設定のウィザードを実行します。App Store へのサインインと、脱獄済み iPhone への SSH 接続（認証情報の作成）を順に聞かれます
   ```bat
   ipadecrypt bootstrap
   ```
3. Minecraft の 1.26.62 の「externalId」（App Store の内部のバージョン番号）を調べます。一覧が表示されるので、`version` が `1.26.62` の行の `externalId` を控えます
   ```bat
   ipadecrypt versions com.mojang.minecraftpe
   ```
4. そのバージョンを指定してダウンロード・復号し、このリポジトリの `ipa\` フォルダに出力します
   ```bat
   ipadecrypt decrypt com.mojang.minecraftpe --external-version-id <控えた externalId> -o "<このフォルダ>\ipa"
   ```
   `ipa\com.mojang.minecraftpe_1.26.62.decrypted.ipa` ができます（`-o` を付けないと、今いるフォルダに出力されます）

注意：

- `--external-version-id` を付けないと、App Store の最新版がダウンロードされます。このエミュレータは 1.26.62 専用です
- `versions` で古いバージョンを次々に開くと、Apple のサーバーに多くの問い合わせをするため、Apple ID が一時的に制限されることがあると
  ipadecrypt 側で注意されています。必要なバージョンだけを開いてください
- Apple ID の情報は `%USERPROFILE%\.ipadecrypt\` に保存されます。他人に渡さないでください
- 詳しい使い方やオプションは `ipadecrypt --help` と、ipadecrypt の README を見てください

## ソースからのビルドとデバッグ（Visual Studio）

### 1. 準備

`ipa\` フォルダに .ipa を置きます（例: `ipa\com.mojang.minecraftpe_1.26.62.decrypted.ipa`。ファイル名は自由です）。
外部ソース（dynarmic とその依存ライブラリ、Boost ヘッダ、zlib、minimp3、stb_vorbis）は `external\` に入っているので、追加のダウンロードは不要です。

### 2. Visual Studio で開く

1. Visual Studio を起動し、「ローカル フォルダーを開く」（または「ファイル → 開く → フォルダー」）でこのフォルダを開きます。
   （CMake プロジェクトです。`.sln` で開きたい場合は下の「ソリューションで開く」を参照）
2. CMake の構成が自動で始まります。出力ウィンドウに `CMake generation finished` と出るまで待ちます（初回は数分）
3. ツールバーで構成を選びます（構成は `x64 Debug` と `x64 Release` の 2 つです）
   - `x64 Debug`：エミュレータ本体を最適化なしでビルド（ブレークポイント・ステップ実行用。JIT だけは最適化されます）
   - `x64 Release`：遊ぶとき・速度を測るとき（JIT とエミュレータをリンク時最適化 /LTCG でビルドします）
4. スタートアップ項目（▶ の横）で `iOSMCPE-Emulator.exe` を選びます（CMake のターゲット名は `CMakeTarget`）

#### ソリューション（.slnx / .sln）で開く

`make_sln.bat` をダブルクリックすると、`out\build\vs-solution\` にソリューションが作られ、Visual Studio で開きます
（インストールされている一番新しい Visual Studio 用：2026 では `iOS MCPE(v1.26.62) Emulator.slnx`、2022 では `.sln`）。
スタートアップ プロジェクトは最初から `CMakeTarget`（`iOSMCPE-Emulator.exe`）なので、構成を選んで F5 でデバッグできます。

#### ビルドできないとき

| エラー | 原因と対処 |
|---|---|
| `CMAKE_C_COMPILER not set` / `CMAKE_CXX_COMPILER not set`（`CMakeLists.txt:12 (project)`） | 以前の版にあった構成「Visual Studio solution」が「ローカル フォルダーを開く」で選ばれていました（コンパイラの環境が無いまま Ninja で構成される）。この構成は削除しました。ツールバーで `x64 Debug` か `x64 Release` を選び、「プロジェクト → CMake キャッシュを削除して再構成」をしてください。ソリューションが必要なら `make_sln.bat` を使います |
| `Visual Studio 2022 (プラットフォーム ツールセット = 'v143') のビルド ツールが見つかりません` | `external\` に入っていた、使っていない VS 2022 用のプロジェクト（zydis の `msvc\`、zlib の `contrib\vstudio` など）を Visual Studio が読み込んでいました。これらは削除しました（エミュレータのビルドには使いません） |

### 3. ビルド・実行・デバッグ

| 操作 | 方法 |
|---|---|
| ビルド | 「ビルド → すべてビルド」（Ctrl+Shift+B） |
| デバッグ実行 | F5 |
| デバッグなしで実行 | Ctrl+F5 |
| ビルドのやり直し | 「プロジェクト → CMake キャッシュを削除して再構成」→ すべてビルド |

- ゲスト用ライブラリ（`guest\`）も、同じビルドの中で `guest\build.py` が自動的にビルドします
  （libc++, libobjc, libz, libdispatch, Foundation, UIKit, **Metal**）
- できた exe：`out\build\x64-debug\iOS MCPE(v1.26.62) Emulator\iOSMCPE-Emulator.exe`（Release は `x64-release`）。
  同じフォルダに `guest\`（ゲスト用ライブラリ）と `remote.html`（iPhone 用ページ）がコピーされます
- ブレークポイントが効くのはエミュレータ本体（`iOS MCPE(v1.26.62) Emulator\` の C++）だけです。
  ゲームと `guest\` のコードは arm64 として JIT で実行されるため、`emulator.log` のログで確認します
- ゲームのクラッシュ時は `emulator.log` にゲスト側のレジスタとバックトレースが出ます

コマンドラインでビルドする場合：

```bat
build.bat            :: Release
build.bat debug      :: Debug
make_sln.bat         :: Visual Studio ソリューションを作って開く
```

## 実行

```bat
"out\build\x64-release\iOS MCPE(v1.26.62) Emulator\iOSMCPE-Emulator.exe"
```

起動からタイトル画面まで 20〜30 秒ほどかかります（ゲームのコードを初めて JIT 変換するため）。
ワールドの読み込みは、ワールドを選んでから 35 秒前後です（下記「既知の制限」の「速度」）。

### データの保存先

すべて `%LOCALAPPDATA%\iOS-MCPE-1.26.62\`（例: `C:\Users\<ユーザー名>\AppData\Local\iOS-MCPE-1.26.62\`）の下です。
`--data <フォルダ>` で起動すると、そのフォルダが代わりに使われます。

| 場所 | 内容 |
|---|---|
| `controls.ini` | マウス感度・フルスクリーン・フレームレート・iPhone への画面出力の設定（下記） |
| `emulator.log` | ログ（起動のたびに作り直し） |
| `mods\` | **Mod**（`*.dylib`。起動時に自動で作られます。「Mod の作り方」を参照） |
| `app\Payload\minecraftpe.app\` | .ipa から展開したゲーム本体（消すと次回起動時に展開し直します） |
| `home\` | ゲームから見た iOS のアプリのホームフォルダ |
| `home\Documents\games\com.mojang\minecraftWorlds\` | **ワールド** |
| `home\Documents\games\com.mojang\minecraftpe\options.txt` | ゲーム内の設定 |
| `home\Library\Preferences\com.mojang.minecraftpe.plist` | アプリの設定（NSUserDefaults） |
| `home\Library\Keychain.plist` | キーチェーン（端末 ID など） |
| `home\Library\Caches\`, `home\tmp\` | キャッシュ・一時ファイル（消しても問題ありません） |

オプション: `--ipa <file>` / `--app <展開済み .app>` / `--data <データフォルダ>` / `--guest-libs <フォルダ>` / `-v`（詳細ログ）/ `-vv`（さらに詳細）

### PC での操作（PC モード）

PC のキーボードとマウスは、iPad に繋いだキーボード・マウス（GameController の `GCKeyboard` / `GCMouse`）としてゲームに渡します。
ゲームが自分でキーボード・マウス用の操作と画面に切り替えるので、操作は PC 版（Windows 版）の統合版と同じです。

- メニューではマウスカーソルでボタンを押します（ホイールでインベントリや設定画面などのスクロールバーを動かせます）
- ワールドの中ではマウスがキャプチャされ（カーソルが消える）、マウスで視点移動、左クリックで破壊・攻撃、右クリックで設置・使用、
  W A S D で移動、Space でジャンプ、E でインベントリ、T でチャット、Esc でポーズ（ゲームの既定の割り当て）
- キーの割り当ては、ゲームの「設定 → キーボードとマウス」で変えられます
- **F11 / Alt+Enter** でフルスクリーンの切り替え（ボーダーレス。描画解像度もモニタに合わせて変わります）。
  ウィンドウの最大化・サイズ変更でも、ゲームの描画解像度と UI の大きさがウィンドウに合わせて変わります
- 文字キーは日本語入力（IME）を通さずにゲームに渡します（IME がオンのままでも W A S D などが効きます）。
  チャットや名前の入力欄が開いている間だけ IME が使えます
- ウィンドウからフォーカスが外れると、押していたキーとマウスは自動的に離され、マウスのキャプチャも外れます

### controls.ini

初回起動時に `%LOCALAPPDATA%\iOS-MCPE-1.26.62\controls.ini` が作られます。メモ帳などで編集してください（ファイルを消すと初期値で作り直されます）。

```ini
[mouse]
sensitivity = 1.0          ; ワールドの中での視点移動の感度
invert_y    = false        ; 上下反転

[window]
fullscreen  = false        ; 起動時にフルスクリーン
max_fps     = auto         ; フレームレート上限 (auto = モニタのリフレッシュレート, 0 = なし)
vsync       = false        ; 垂直同期

[remote]
enabled     = true         ; iPhone のブラウザへの画面出力 (false で無効)
port        = 8080         ; 待ち受けるポート (使用中なら 8081, 8082... を使います)
resolution  = 1280         ; iPhone に送る映像の解像度 (長辺のピクセル数)
quality     = 70           ; 画質 (JPEG の品質 10-100)
fps         = 60           ; 送るフレームレートの上限
pc_audio    = false        ; iPhone 接続中も PC から音を出す (false = 音は iPhone だけ)
```

`[window]` と `[remote]` は、次回の起動時に反映されます。

### 描画

ゲームの描画（RenderDragon / bgfx の Metal 版）を、エミュレータの Metal → OpenGL 4.6 で動かしています。

- ゲームは描画の方式を GPU 名で選びます（`data/tiers.bin`）。エミュレータは GPU を「Apple A11 GPU」（iPhone X 相当）として見せ、
  **通常の描画（クラシック）**を使わせています。Vibrant Visuals（遅延レンダリング・PBR）は、コンピュートシェーダーと
  イメージの機能がまだ足りないため使いません
- `options.txt` に Vibrant Visuals / レイトレーシング（`graphics_mode:2` / `3`）が保存されていると、ゲームはそれで起動してしまいます
  （とても重いうえ、ワールドの読み込みが「Loading...」のまま終わらないことがある）。エミュレータは起動時にこれを
  ファンシー（`graphics_mode:1`）に戻します（`emulator.log` に出ます。`EMU_ALLOW_VIBRANT=1` で無効）
- シェーダーは、起動中に初めて使うときに MSL から GLSL に変換・コンパイルします（ゲームの 1694 種類のシェーダーは
  すべて変換・コンパイルできることを確かめています：`iOSMCPE-Emulator.exe --msltest <MSL のフォルダ>`）

### Microsoft アカウントでサインイン・サーバーで遊ぶ

タイトル画面の「Sign In」（または「サーバー」タブの「Log in or sign up」）を押すと、
エミュレータの**サインイン用のウィンドウ**に Microsoft アカウントのページが開きます（iOS のサインイン画面と同じ役割）。
メールアドレスとパスワード（またはスマホの Authenticator での承認）でサインインすると、ウィンドウは自動で閉じ、
ゲームの画面でサインインの続きが進みます。ウィンドウを閉じるとサインインは中止です。
初めてのサインインでは、続けて「Xbox」というウィンドウに Xbox のプロフィールや同意の確認のページが出ることがあります
（iOS 版がアプリ内の WebKit 画面で出すもの。ゲストの `WKWebView`（`guest/UIKit/WebKit.m`）を WebView2 のウィンドウ（`web_view.cpp`）で表示しています）。
そのページを最後まで進めるとウィンドウが閉じ、ゲームに戻ります。

- ページの表示には Windows 11 に入っている Microsoft Edge WebView2 を使います（`auth_webview.cpp`、SDK は `external/webview2`）。
  「サインインしたままにする」の情報は、データフォルダの `WebView2\` に保存されます
- WebView2 が無い PC（または `EMU_AUTH_BROWSER=1`）では、PC の既定のブラウザでページを開きます。最後にブラウザが
  「iOSMCPE-Emulator.exe を開きますか?」と確認するので「開く」を選んでください。このために `ms-xal-…` という URL スキームを
  現在のユーザーのレジストリ（`HKEY_CURRENT_USER\Software\Classes\ms-xal-000000004c17c01a`）に登録します。
  ただしスマホでの承認のように**クリックせずに終わるサインインでは、ブラウザがエミュレータに戻してくれないことがあります**
- サインインの情報（トークン、デバイスキー）は、データフォルダの `home\Library\Keychain.plist` と
  `home\Documents\games\com.mojang\xal\` に保存されます。このフォルダを他人に渡さないでください
- サインインすると、「サーバー」タブの特集サーバーや、アドレスを追加した外部サーバーに接続できます
  （特集サーバーに接続し、リソースパックのダウンロードとロビーへの参加まで確かめています）
- サインインでうまくいかないときは、Xbox のサインイン（Xal）の警告・エラーが `emulator.log` に出ます。
  直前の起動のログは `emulator.prev.log` に残ります

### iPhone に画面を出して操作する（スマホモード）

PC で動いているゲームの画面を、同じ Wi-Fi にいる iPhone の Safari に表示し、iPhone のタッチと日本語キーボードで操作できます。
iPhone にアプリを入れる必要はありません。

1. PC でエミュレータを起動します。ウィンドウのタイトルに `iPhone: http://192.168.x.x:8080` のようにアドレスが出ます（`emulator.log` にも出ます）
2. iPhone の Safari でそのアドレスを開きます（初回、Windows のファイアウォールの確認が出たら「プライベート ネットワーク」を許可）
3. 全画面で使うには、Safari の共有ボタン →「ホーム画面に追加」で追加したアイコンから開きます（アドレスバーが消えます）

接続している間は：

- ゲームの描画解像度が iPhone の画面に合わせて変わり（画面のポイント数の 2 倍。例：812×402 ポイントの iPhone なら 1624×804）、
  PC のウィンドウには同じ画面が黒帯付きで表示されます。iPhone に送る映像は `resolution`（長辺 1280 ピクセル）に縮小します
- ゲームはタッチでの操作になります（ゲームが自分でタッチ用の画面に切り替えます）。ワールドに初めてタッチで入ると、
  操作方法（ジョイスティック / 十字キー）の選択画面が出ます
- マルチタッチに対応しています。iPhone を縦に持つと画面を 90 度回して表示します
- 接続中はゲームを 60fps 上限で動かします（iOS と同じ）
- 文字入力欄を選ぶと iPhone の画面上部に入力欄が出ます。それをタップすると iPhone のキーボードが開きます。
  日本語入力（変換）も使え、キーボードの改行キーがゲームの Enter です
- PC のマウス操作はゲームに送りません（キーボードは使えます）。iPhone を閉じる / Safari を閉じると PC の操作に戻ります
- 音は iPhone から出ます（最初に画面をタップしたときに鳴り始めます）。接続中は PC の音は止まります（`[remote]` の `pc_audio = true` で両方から出ます）
- 接続できる iPhone は 1 台で、別の端末が接続すると前の端末は切断されます

接続できるのは同じ LAN 内（プライベート IP アドレス）の端末だけです。LAN 内の誰でもアドレスを開けば操作できるので、
不要なときは `controls.ini` の `[remote]` で `enabled = false` にしてください。
映像が重い / カクつくときは `resolution`（例: 960）や `quality`（例: 50）を下げてください。
ゲームは、つないだ iPhone の画面の大きさ（ポイント数）の 2 倍のピクセル数で描画するので、UI の大きさや配置は実際の iPhone と同じになります。

#### ケーブル（USB）で接続する

Wi-Fi より遅延が少なくなります。iPhone の「インターネット共有」（テザリング）を USB で使います
（ケーブルだけでは iPhone の Safari から PC に接続する経路が無いためです）。

1. iPhone を PC にケーブルで接続し、iPhone の「設定 → インターネット共有 → ほかの人の接続を許可」をオンにします
   （「このコンピュータを信頼しますか？」と出たら「信頼」）。PC に「Apple Mobile Device Ethernet」というネットワークが追加されます
2. 数秒後、エミュレータのウィンドウのタイトルが `iPhone USB: http://172.20.10.x:8080` になります（`emulator.log` にも出ます）。
   `(USB: iPhone のインターネット共有がオフ)` と出ている場合は、iPhone 側で共有が有効になっていません
3. iPhone の Safari でその `USB:` のアドレスを開きます。接続中のタイトルは `iPhone 接続中 (USB)` になります

注意：

- インターネット共有は携帯電話会社の契約・SIM によっては使えません
- 数分で USB 接続が切れる場合は、Windows の USB の省電力機能（「USB のセレクティブ サスペンド」、
  デバイス マネージャーの「Apple Mobile Device Ethernet → 電源の管理」）をオフにしてください
- Windows はこの新しいネットワークを「パブリック ネットワーク」として扱うことがあります。つながらないときは
  「プライベート ネットワーク」にするか、ファイアウォールの確認で「パブリック ネットワーク」も許可してください

## Mod の作り方

Mod はゲームと同じ形式（arm64 の Mach-O dylib）のプログラムで、`%LOCALAPPDATA%\iOS-MCPE-1.26.62\mods\` に置くとゲームと一緒に読み込まれます。
`__attribute__((constructor))` を付けた関数がゲームの初期化より先に実行され、ゲームと同じ関数（libc、libc++、
Foundation / Objective-C ランタイム、ゲーム自身の関数）と、エミュレータの Mod 用関数（`mod-template\include\emu_mod.h`：
ログ出力 `emu_log`、関数フック `emu_hook`、`emu_game_base`、`emu_set_title` など）を使えます。

**`mod-template\` フォルダをコピーして使うのが一番簡単です**（Visual Studio の「ローカル フォルダーを開く」で開き、
構成「MCPE mod (arm64, Release)」を選んで「すべてビルド」。できた `example_mod.dylib` は自動で `mods\` にコピーされます）。
例の Mod は、Objective-C ランタイムでゲームのメソッド（`-[minecraftpeAppDelegate applicationDidBecomeActive:]`）を置き換えます。
Objective-C のメソッドは名前で見つかるので、ゲームのアドレスを調べなくても使えます。

- Mod はゲーム内（arm64 の JIT 上）で動くため、Visual Studio のブレークポイントは効きません。`emu_log` / `EMU_LOGF` で `emulator.log` に出力して確認してください
- ゲームの C++ 関数をフックするときは、逆アセンブラで `minecraftpe`（ベースアドレス 0x100000000）を開いてアドレスを調べ、`emu_hook` を使います
- Mod 独自の画面（メニューなど）は overlay API で、ゲームの画面に重ねて描けます（`overlay.cpp`）。
  三角形のリスト（Dear ImGui の描画データと同じ形）を `emu_overlay_render` で毎フレーム渡すと、ゲームの画面の上に描かれ、
  PC のウィンドウにも接続中の iPhone にも表示されます。`emu_overlay_capture` で指定した範囲のマウス・タッチ操作はゲームではなく
  Mod に渡り（`emu_overlay_events` で受け取る）、`EMU_OVERLAY_WANT_CURSOR` を付けるとワールドの中でもマウスが解放されます。
  日本語の文字を描くためのシステムフォント（メイリオなど）は `emu_system_font` で取得できます
- `emu_readable(アドレス, 長さ)` で、読んでよいメモリかを確かめられます（構造の分からないポインタをたどるとき用）
- `emu_bundle_write(アプリ内のパス, データ, 長さ)` で、ゲームからは読み取り専用のアプリ（`minecraftpe.app`）にファイルを追加・置き換えできます
  （組み込みのリソースパックに piece を足すときなど）。ファイルはデータフォルダの `home\bundle_overlay\` に置かれ、起動のたびに消去されるので、
  アプリ本体は変わらず、Mod を外せば元に戻ります。ゲームがファイルを読む前（constructor の中）に呼んでください
- `emu_http_add_body_filter(URL の一部, 関数)` で、ゲームの HTTP(S) のレスポンス本文を受け取る前に書き換えられます。
  `emu_http_add_local_response(URL, データ, 長さ, Content-Type)` で、その URL への要求にネットを使わずデータを返せます
  （どちらもゲスト用 Foundation の NSURLSession の実装 `guest/Foundation/NSURLNet.m`）
- カスタムエモートを追加する Mod の例：[CustomEmotesMod](https://github.com/ShuperDark/CustomEmotesMod-Emu)
- このエミュレータ用の大きな Mod の例：[ShupaClient](../ShupaClient-Emu)（ImGui のメニュー、vtable フック、QuickJS の JS MOD）
- 必要なのは Python と `py -m pip install ziglang` だけです（エミュレータのビルドで入っていればそのまま使えます）

## GitHub に公開するとき

`.gitignore` で、`ipa\*.ipa`（ゲーム本体）、`out\`（ビルド結果）、`.vs\` などはコミットされないようにしてあります。
`external\` の中の git リポジトリ（dynarmic とその依存、zlib）は、v0.16.2 版の README と同じく、最初のコミットの前に `.git` を削除してください。

第三者のソースのライセンス：dynarmic（0BSD）、fmt / mcl / robin-map / zycore / zydis（MIT）、xbyak（BSD-3-Clause）、
Boost（Boost Software License 1.0）、zlib（zlib License）、minimp3（CC0）、stb_vorbis（MIT またはパブリックドメイン）。
ゲスト用 libc++ は zig に同梱の LLVM libc++（Apache-2.0 WITH LLVM-exception）からビルドします。
このプロジェクト自体（`external\` 以外）は MIT License です（`LICENSE`）。

## デバッグ用環境変数

| 変数 | 内容 |
|---|---|
| `EMU_WINDOW_SIZE=1600x900` | ウィンドウサイズ（ピクセル） |
| `EMU_SCREENSHOT=<dir>` / `EMU_SCREENSHOT_EVERY=<秒>` | フレームを BMP で保存 |
| `EMU_AUTOTAP="40:hover=640,358;41:640,358;60:key=E;62:keydown=W;64:keyup=W;66:mouse=100,0;70:touch=300,200;72:text=abc"` | 指定秒にマウス移動（hover）/ クリック / キー / 相対移動（mouse）/ タッチ / 文字入力（text）（自動テスト用） |
| `EMU_WATCHDOG=<秒>` | 定期的に全ゲストスレッドのレジスタ/バックトレースを出力 |
| `EMU_TRACE_EXCEPTIONS=1` | ホスト側の first-chance 例外を出力 |
| `EMU_TRACE_INPUT=1` | キーボード・マウス（GCKeyboard / GCMouse）とタッチの配送をログに出力 |
| `EMU_TRACE_KEYCHAIN=1` | キーチェーンへのアクセスをログに出力 |
| `EMU_MTL_TRACE=<フレーム数>` / `EMU_MTL_TRACE_AT=<秒>` | Metal の描画パスと描画命令をログに出力（指定秒から） |
| `EMU_MTL_DUMP=<dir>` | 変換した GLSL とゲームの MSL をファイルに保存 |
| `EMU_MTL_FLIP_WINDING=1` | 表面の向き（カリング）を反転（描画の切り分け用） |
| `EMU_NO_METAL=1` | Metal を使えないことにする |
| `EMU_LOGHOOK=<16進アドレス>:<レジスタ番号>` | ゲームの関数の呼び出しを、引数の文字列付きでログに出力 |
| `EMU_SLEEP_STATS=1` | ゲームがスリープしている場所（呼び出し元と時間）を 10 秒ごとに出力 |
| `EMU_SHOW_FPS=1` | タイトルバーにフレームレートを表示（`-v` ではログにも出力） |
| `EMU_PROFILE=<秒>` | サンプリングプロファイラ（スレッド別の内訳：ゲームのコード / JIT コンパイラ / エミュレータ / OS、HLE 関数の時間と呼び出し回数） |
| `EMU_PROFILE_GUEST=1` | プロファイラにゲスト側の内訳を追加（時間のかかるゲストのブロック、呼び出し元、待っている場所） |
| `EMU_PROFILE_PERIOD=<ミリ秒>` / `EMU_PROFILE_NOHLE=1` | サンプリング間隔（既定 1）/ HLE 関数の時間を計らない（計測自体の負荷を下げる） |
| `EMU_TRACE_OPEN=1` | ゲームが開くファイルをすべてログに出力 |
| `EMU_NCPU=<数>` | ゲームに見せる CPU 数（既定は論理 CPU 数の半分、2〜8） |
| `EMU_OFFLINE=1` | ネットワークに接続していないことにする（HTTP の通信もすべて「オフライン」で失敗させる） |
| `EMU_TRACE_HTTP=1` / `=2` / `=3` | HTTP の通信をログに出力（エラーのときは本文も。`2` はすべての応答の本文の先頭、`3` は送信する本文も。トークンなどがログに残るので注意） |
| `EMU_TRACE_RESIZE=1` | ウィンドウ / iPhone の画面サイズの変更と、ゲームの描画サイズの変更をログに出力 |
| `EMU_TRACE_XAL=1` | libHttpClient と Xbox のサインイン（Xal）のトレースをすべてコンソールに出力（警告・エラーは常に `emulator.log` へ） |
| `EMU_AUTH_BROWSER=1` | サインインのページを、エミュレータのウィンドウではなく PC の既定のブラウザで開く |
| `EMU_TEST_AUTH_WINDOW=<URL>` | 起動時にサインイン用のウィンドウをその URL で開く（WebView2 の動作確認用） |
| `EMU_TEST_WKWEBVIEW=<URL>` | 起動時にゲストの WKWebView でその URL を開き、ナビゲーションデリゲートが受け取る内容をログに出す（動作確認用） |
| `EMU_ACCURATE_FP=1` | JIT の浮動小数点の高速化を無効化 |
| `EMU_ACCURATE_ATOMICS=1` | JIT の排他ロード/ストア（アトミック操作）を dynarmic のグローバルモニタ方式に戻す（下記「速度」） |
| `EMU_ALLOW_VIBRANT=1` | `options.txt` の Vibrant Visuals / レイトレーシングの設定を戻さない |
| `EMU_NO_AUDIO=1` | 音声を無効化 |

## 既知の制限

- **速度**：ワールドの読み込みはワールドを選んでから 35 秒前後、ワールドの中は 130fps 前後です（開発に使った PC での計測）。
  読み込みの前半は、ゲームが初めて実行するコード（ブロック・アイテム・バイオームなどの定義を読む処理）の JIT 変換待ちが大半です。
  高速化のために行っていること：
  - アトミック操作（`LDAXR`/`STLXR`。shared_ptr の参照カウントなど）を、全スレッド共通のロックと 128 スレッド分の予約の走査を
    使わずに、読んだ値との比較交換（cmpxchg）で実行（描画スレッドの約 3 割を占めていた）
  - JIT コンパイラ（dynarmic）の高速化：ブロックごとの 0.5MB の確保をやめる、レジスタ割り当ての全探索を減らす、
    メモリアクセスごとのハッシュマップ登録を配列にする、リンク時最適化
  - エミュレータとゲームの malloc / new を、スレッドごとのキャッシュ付きアロケータ（`fastheap.cpp`）で処理
  - JIT のコードキャッシュはスレッドごとのため、複数のスレッドが同じコードを実行すると、それぞれで変換します
- Vibrant Visuals は使えません（上記「描画」）
- マーケットプレイスでの購入、ストア（StoreKit）は使えません
- iOS 版から外した機能：0.16.2 版にあったシェーダーパック（影 Mod）、OptiFine 風のビデオ設定は、ゲームの描画が Metal になったため使えません
