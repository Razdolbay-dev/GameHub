```
# NEED INSTALL

sudo apt update
sudo apt install curl git make ninja-build ccache pkg-config python3 python3-pip python3-venv tar wget unzip clang-18 clang-format-18 clang-tidy-18 lld-18
sudo apt install meson ccache bison ninja-build flex python3-pip python3-mako byacc
```

```shell
pip3 install --user --break-system-packages mako
```

Клонируйте репозиторий ps5-opengl рядом с вашим проектом GameHub:

```bash
git clone https://github.com/Phononenom/ps5-opengl.git ../ps5-opengl
```
Соберите SDK:

```bash
cd ../ps5-opengl
make source-fetch
make sdk-gl46
```

Это создаст SDK в build/sdk/ps5-opengl-gl46/.

Вернитесь в GameHub и укажите путь к собранному SDK при вызове make deps. В Makefile проекта ps5-homebrew-ui должна быть переменная для этого пути (вероятно, PS5_OPENGL_PREFIX). Попробуйте:

```bash
cd ~/CLionProjects/GameHub
make deps PS5_OPENGL_PREFIX=../ps5-opengl/build/sdk/ps5-opengl-gl46
```

Скачать готовый SDK вручную

```bash
cd /tmp
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip

cd ~/CLionProjects/ps5-opengl
make sdk-gl46 PS5_PAYLOAD_SDK=~/ps5-sdk/ps5-payload-sdk
```
