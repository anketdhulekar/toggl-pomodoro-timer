# Load ESP-IDF v5.5.6 into the current shell:  . ./idf-env.sh
# Pins the Python 3.13 environment the toolchain was installed with,
# so it works no matter which python3 is first on PATH.
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
. "$HOME/esp/esp-idf/export.sh" >/dev/null && echo "ESP-IDF ready: $(idf.py --version)"
