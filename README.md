# Seeed Studio XIAO ESP32-C6 Zigbee 3.0 Coordinator for Zigbee2MQTT

Seeed Studio XIAO ESP32-C6（RISC-V 160MHz, 4MB Flash）を活用し、単一の ESP32-C6 上で Wi-Fi と IEEE 802.15.4（Zigbee 3.0 ZBOSS スタック）を同時稼働（Software Coexistence）させ、TCP（ポート 8888）経由で Zigbee2MQTT (Z2M) と通信する「Wi-Fi 接続型 Zigbee コーディネーター」ファームウェアです。

---

## 🌟 主な特徴・機能

- **単一 RF 電波共存（Software Coexistence）**:
  - Wi-Fi 6 と Zigbee 3.0 (ZBOSS) を 1 つの 2.4GHz RF 回路で安定して同時動作。
  - バランスモード調停（`ESP_COEX_PREFER_BALANCE`）により、Wi-Fi/Zigbee 双方の電波待ち時間を最小化。
- **高スループット・低遅延 TCP ブリッジ**:
  - 優先度 6 の専用 FreeRTOS タスク（`bridge_task`）が TCP ポート 8888 をハンドリングし、高速かつ安定した Z2M パケット中継を実現。
- **SSD1306 I2C OLED ディスプレイ表示 (128x64)**:
  - 1行目: Wi-Fi 接続状態、空きヒープ残量 (KB)、4段階アンテナピクトグラム＆最新 LQI 数値
  - 2行目: Z2M 接続状態 (`Z2M: CONNECTED` / `Z2M: WAITING`)、または異常リセット警告点滅
  - 3行目: 送受信パケットカウンタ (`TX:xxxx RX:xxxx`)
  - 4行目: 割り当てられたデバイス IP アドレス
  - 有機EL 画面焼き付き防止（自動ピクセルシフト保護）
  - ArduinoOTA 実行時の進捗パーセンテージバー表示
- **ステータス LED**:
  - Z2M 接続待機時: 1秒周期で点滅
  - Z2M 接続完了時: 常時点灯（Zigbee 通信パケット受信時に 50ms パルス消灯）
- **Web 診断ダッシュボード (ポート 80)**:
  - メインループから独立した専用タスク（優先度 2）で稼働。Web アクセスが重なっても Zigbee 通信や UI に一切干渉しません。
  - ブラウザから `http://<ESP32_IP>/` を開くだけで、連続稼働時間、ヒープメモリ（現在値/最小値）、Wi-Fi RSSI、Z2M 接続状態、起動要因（PowerOn / Brownout / Task WDT / INT_WDT / Panic 等）、前回の連続稼働時間、直前の実行コマンド/TSN、Flash CoreDump 情報を遠隔確認可能。
  - RAW イベントログ表示 (`/logs`) およびリモート再起動 (`/reboot`)、RESTful ステータス API (`/api/status`) を提供。
- **ArduinoOTA による無線ファームウェア更新**:
  - Wi-Fi 経由でのファームウェア書き込みに対応。設置後に USB ケーブルを接続することなくリモートでファームウェア更新が可能。
- **ハードウェア障害耐性**:
  - I2C バスの自動復旧シーケンス（SCL 9 クロックパルスによる Stuck SDA 解放）とタイムアウト保護により、電源投入・抜去時のバスロックを防止。
  - Interrupt Watchdog タイムアウト（1000ms）最適化により、重いネットワークスキャン時の一時的調停遅延による誤パニックを防止。

---

## 🛠️ ハードウェア仕様・ピンアサイン

- **ボード**: Seeed Studio XIAO ESP32-C6 (4MB Flash, RISC-V 160MHz)
- **ディスプレイ**: 0.96 インチ I2C OLED (SSD1306, 128x64, I2C アドレス: `0x3C`)
  - **SDA**: GPIO 22
  - **SCL**: GPIO 23
  - **電源**: 3.3V
- **ステータス LED**: GPIO 15 (オンボード LED)

---

## 💻 開発環境・フレームワーク

- **ビルドシステム**: PlatformIO (`platform-espressif32` by pioarduino v53.3.11+)
- **フレームワーク**: ESP-IDF v5.3.2 + Arduino-ESP32 v3.1.1
- **主要ライブラリ**:
  - `U8g2` (OLED 描画)
  - `WiFi`, `Wire`, `ArduinoOTA`, `NetworkClient`
- **パーティション構成 (`custom.csv`)**:
  - `nvs`: 16KB
  - `otadata`: 8KB
  - `app0` (OTA_0): 1664KB (約1.62MB)
  - `app1` (OTA_1): 1664KB (約1.62MB)
  - `coredump`: 64KB (Panic 発生時のスタックダンプ記録用)
  - `storage`: 512KB (LittleFS / NVRAM)

---

## 🚀 セットアップ・ビルド手順

### 1. リポジトリの取得
```bash
git clone <REPOSITORY_URL>
cd Seeed_XIAO_ESP32C6_Z2M
```

### 2. 設定ファイルの作成 (`src/config.h`)
サンプル設定ファイル `src/config.example.h` を `src/config.h` としてコピーし、接続する Wi-Fi の SSID とパスワードを設定します。

```bash
cp src/config.example.h src/config.h
```

`src/config.h` を編集：
```c
// Wi-Fi 接続設定
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// TCP ポートおよび OTA パスワード
#define Z2M_TCP_PORT 8888
#define OTA_PASSWORD "admin"
```
> [!NOTE]
> `src/config.h` は `.gitignore` に登録されているため、Wi-Fi パスワード等の個人情報が Git にコミットされる心配はありません。

### 3. ファームウェアの書き込み

#### A. USB 経由の書き込み（初回）
Seeed Studio XIAO ESP32-C6 を PC の USB ポートに接続し、以下のコマンドを実行します：

```powershell
pio run -e usb -t upload
```

シリアルモニターで起動ログを確認する場合：
```powershell
pio device monitor --port <COM_PORT> --baud 115200
```

#### B. Wi-Fi 経由の OTA 書き込み（2回目以降）
デバイスが Wi-Fi に接続された後は、PC と同一ネットワーク内から無線経由でファームウェアを更新できます：

```powershell
pio run -e ota -t upload
```
> [!TIP]
> Windows 環境で OTA 転送がタイムアウトする場合は、Windows Defender ファイアウォールで PlatformIO の Python（`python.exe`）の受信接続を許可してください。

---

## 📡 Zigbee2MQTT (Z2M) 側の設定

Zigbee2MQTT ホスト（Home Assistant アドオンや Docker コンテナ等）の `configuration.yaml` に以下を設定します。

```yaml
serial:
  # ESP32-C6 の OLED 画面または Web ダッシュボードに表示された IP アドレスを指定
  port: tcp://<ESP32_IP>:8888
  
  # ZBOSS ドライバを指定 (Zigbee2MQTT 1.34.0+ でネイティブサポート)
  adapter: zboss

advanced:
  # 必要に応じて Zigbee チャンネルや PAN ID を指定
  channel: 11
  pan_id: 0x1a62
```

設定保存後、Zigbee2MQTT を再起動（またはコンテナ起動）すると、TCP 8888 経由で ESP32-C6 と接続され、コーディネーターとして自動認識されます。

---

## 🌐 Web 診断ダッシュボード

ESP32-C6 と同一ネットワーク内のブラウザから、デバイスの IP アドレスを開くことで診断情報を確認できます：

| URL | 説明 |
| :--- | :--- |
| `http://<ESP32_IP>/` | リアルタイム診断ダッシュボード（HTML UI） |
| `http://<ESP32_IP>/api/status` | システムステータス情報（JSON API） |
| `http://<ESP32_IP>/logs` | 直近のシステムイベントログ（Plain Text） |
| `http://<ESP32_IP>/reboot` | ESP32-C6 のリモート再起動（確認ダイアログ付き） |

---

## 📄 ライセンス

本プロジェクトはオープンソースです。ライセンスの詳細はリポジトリ内の [LICENSE](LICENSE)（存在する場合）を参照してください。
