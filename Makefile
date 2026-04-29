.PHONY: install setup doctor install-plugin test test-loop gateway sim sim-esp32

install:
	python3 -m venv .venv
	. .venv/bin/activate && pip install -e '.[dev]'

setup:
	. .venv/bin/activate && hermes-iot-setup --force

doctor:
	. .venv/bin/activate && hermes-iot-doctor

install-plugin:
	. .venv/bin/activate && hermes-iot-install-plugin --force

test:
	. .venv/bin/activate && python -m pytest gateway/tests

gateway:
	. .venv/bin/activate && hermes-iot-gateway

sim:
	. .venv/bin/activate && hermes-iot-sim

sim-esp32:
	. .venv/bin/activate && hermes-iot-sim --self-test --device-profile echo-pyramid --prompt "Say hello from the simulated ESP32"

test-loop:
	. .venv/bin/activate && python -m pytest gateway/tests/test_integration.py -q
