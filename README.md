
# Sensor Dashboard: ZMQ Server & Remote Control

Приложение принимает GPS и cellular-данные с Android-устройства, сохраняет их в PostgreSQL и показывает состояние в интерактивной панели на **Dear ImGui**/**ImPlot**. Обмен с клиентом идет через **ZeroMQ** в режиме REQ-REP: на каждый пакет данных сервер возвращает текущие настройки фильтрации.

## Функциональность

* прием JSON-пакетов по ZeroMQ;
* запись входящих сообщений в `Location_save_data/received_data.jsonl`;
* сохранение координат и параметров сот в PostgreSQL;
* отображение GPS, таблицы cellular-данных и графиков уровня сигнала;
* карта маршрута на OSM-тайлах;
* фильтры координат, которые сервер отправляет Android-клиенту в ответном сообщении.

## Требования

* Компилятор с поддержкой **C++17**.
* **CMake**.
* **ZeroMQ** (`libzmq`) и заголовки **cppzmq**.
* **PostgreSQL libpq**.
* **SDL2**, **OpenGL**, **GLEW**.
* **nlohmann/json**.
* Для загрузки OSM-тайлов: `curl` и `ffmpeg`.

## Установка зависимостей (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install build-essential cmake libsdl2-dev libglew-dev libgl1-mesa-dev \
  nlohmann-json3-dev libzmq3-dev cppzmq-dev libpq-dev curl ffmpeg
```

## Настройка

Создайте `.env` в корне проекта:

```env
DB_HOST=localhost
DB_PORT=5433
DB_USER=gps_user
DB_PASS=gps_password
DB_NAME=gps_db
ZMQ_BIND_ENDPOINT=tcp://*:7777
MAP_POINTS_LIMIT=0
```

Перед запуском создайте таблицы из `init.sql`.

## Сборка и запуск

```bash
mkdir build && cd build
cmake ..
make
./main
```

## Протокол обмена

### Android -> Server (Data)

Сервер ожидает JSON с координатами в объекте `location` и временем в поле `time`:

```json
{
  "location": {
    "_Latitude": 55.018,
    "_Longitude": 82.952,
    "_Altitude": 120.5
  },
  "time": 1772268444506,
  "cells": [
    {
      "type": "lte",
      "ci": 12345,
      "pci": 17,
      "tac": 456,
      "rsrp": -92,
      "rsrq": -11,
      "rssi": -70,
      "ta": 2
    }
  ]
}
```

### Server -> Android (Command)

В ответ на каждый пакет сервер отправляет текущее состояние фильтров из GUI:

```json
{
  "f_lat": true,
  "f_lon": true,
  "f_alt": false
}
```

## Сохраненные данные

Входящие пакеты дополнительно пишутся в `Location_save_data/received_data.jsonl`. Этот файл удобно использовать для отладки, повторной обработки или ручного импорта в БД.
