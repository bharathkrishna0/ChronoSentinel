


# ⏱️ ChronoSentinel

**ChronoSentinel** is an intelligent time-synchronized IoT client built using the ESP32 platform. It securely transmits real-time sensor data (such as proximity/distance measurements) to a remote HTTPS server with accurate timestamps — even surviving reboots using onboard NVS storage for time persistence.

## 🚀 Features

- 📡 **Wi-Fi-Enabled IoT Client**  
  Connects to secure Wi-Fi networks and stays resilient with automatic reconnection logic.

- 🔐 **Secure HTTPS Communication**  
  Sends sensor data over HTTPS with TLS encryption using server certificate pinning.

- 🕒 **SNTP Time Synchronization**  
  Uses SNTP to fetch accurate world time and stores it in **NVS** (non-volatile storage) for future reference.

- 💾 **Time Recovery on Reboot**  
  Restores time from NVS if SNTP isn’t available after a power cycle — ensuring time consistency in logs.

- 📤 **Data Posting to HTTPS Server**  
  Sends structured JSON containing `distance` and `timestamp` to a specified HTTPS endpoint.

- 🔁 **Periodic Time Refreshing**  
  Uses `esp_timer` to periodically re-sync and update time in the background.

## 📡 Example Data Payload

```json
{
  "distance": 3.42,
  "timestamp": 1718796050
}
````

> Timestamp is in Unix epoch format (seconds since Jan 1, 1970 UTC).

## ⚙️ Tech Stack

| Layer         | Tools/Protocols                                                                                                             |
| ------------- | --------------------------------------------------------------------------------------------------------------------------- |
| MCU           | [ESP32](https://www.espressif.com/)                                                                                         |
| Language      | C (ESP-IDF)                                                                                                                 |
| Network       | Wi-Fi (STA Mode)                                                                                                            |
| Security      | HTTPS, TLS, Cert Pinning                                                                                                    |
| Time Sync     | SNTP + NVS Fallback                                                                                                         |
| Payload       | JSON via [cJSON](https://github.com/DaveGamble/cJSON)                                                                       |
| Communication | [ESP HTTP Client](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_client.html) |

## 🔧 Setup & Flashing

### Prerequisites

* ESP32 board
* ESP-IDF v5.0+
* Wi-Fi credentials
* Remote server with HTTPS endpoint (self-hosted or public)

### Flash to Board

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

> Replace `/dev/ttyUSB0` with your ESP32’s serial port.

## 🛠️ Configuration

Update the following in `main.c` or via `menuconfig`:

* `WIFI_SSID` and `WIFI_PASS`
* HTTPS endpoint URL (e.g., `https://192.168.0.140/data`)
* Certificate (embedded in binary)

## 📂 Project Structure

```
ChronoSentinel/
├── main/
│   ├── main.c
│   ├── time_sync.c
│   ├── certs/
│   │   └── server_cert.pem
├── CMakeLists.txt
├── README.md
└── sdkconfig
```

## 🧠 Behind the Name

**ChronoSentinel** = *Chrono* (Time) + *Sentinel* (Watchful Guardian)
A vigilant microcontroller that guards your sensor data with time accuracy and secure delivery.

---

## 🧩 Future Additions (Ideas)

* MQTT support
* OTA firmware updates
* Real-time dashboard visualization
* Battery health monitoring
* BLE gateway integration

---


## 🙌 Acknowledgments

* [ESP-IDF by Espressif](https://docs.espressif.com/)
* [cJSON Library](https://github.com/DaveGamble/cJSON)
* Community tutorials and open source examples

