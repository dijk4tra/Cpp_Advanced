PYTHON ?= python3

.PHONY: all check run

all: check

check:
	$(PYTHON) -c 'compile(open("http_load_test.py", encoding="utf-8").read(), "http_load_test.py", "exec")'

run: check
	$(PYTHON) http_load_test.py
