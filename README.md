
# Backend Display with ImGui + TCP Server

Это приложение объединяет графический интерфейс на базе Dear ImGui (с поддержкой ImPlot) и TCP-сервер, который слушает порт 7777 и принимает JSON-данные от клиентов (например, с Android-устройства). Полученные данные (широта, долгота, высота, время) отображаются в окне интерфейса.

## Функциональность

- Окно с примером кнопки (демо ImGui).
- Окно отображения данных от датчика, обновляемое при получении новых JSON-сообщений.
- TCP-сервер на неблокирующих сокетах, поддерживающий одного клиента одновременно.
- Приём JSON вида `{"_Latitude":55.018,"_Longitude":82.952,"_Altitude":165.5,"_Time":1772268444506}`.

## Требования

- Компилятор с поддержкой C++17 (например, g++).
- Библиотеки:
  - SDL2
  - GLEW
  - OpenGL
  - nlohmann/json (header-only)
- Система сборки: CMake (рекомендуется) или прямая компиляция g++.

## Сборка проекта

### 1. Клонирование репозитория с подмодулями

```bash
git clone https://github.com/DobromilovS/backend-server-gps.git
cd backend-server-gps
git submodule update --init --recursive
```

### 2. Установка системных зависимостей (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install build-essential cmake libsdl2-dev libglew-dev libgl1-mesa-dev nlohmann-json3-dev
```

### 3. Сборка с помощью CMake

В корне проекта создайте файл `CMakeLists.txt` со следующим содержимым (пример ниже), затем выполните:

```bash
mkdir build
cd build
cmake ..
make
```

### 4. Запуск

```bash
./build/main
```

Программа откроет окно и начнёт слушать порт 7777. При подключении клиента (например, через `telnet` или ваше Android-приложение) и отправке JSON-строки данные появятся в окне "Sensor Data".
