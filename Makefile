UV ?= uv
PYTHON ?= $(UV) run --locked python
INPUT ?= assets/real/mandarin.wav
OUT ?= out
SCENARIO ?= phased
SEED ?= 42
ARGS ?=
BUILD_TYPE ?= RelWithDebInfo
PROFILE_HOST ?= default
PROFILE_BUILD ?= default
MEDIA ?= ON
TUI ?= ON
TESTS ?= ON
SANITIZER ?= none
OFFLINE ?= OFF
JOBS ?= 8
WEB_PORT ?= 8765
export INPUT OUT SCENARIO SEED ARGS BUILD_TYPE PROFILE_HOST PROFILE_BUILD
export MEDIA TUI TESTS SANITIZER OFFLINE JOBS
export WEB_PORT
.PHONY: help deps doctor build start compare test fixtures fetch-speech clean asan tsan doc web
help deps doctor build start compare test fixtures clean:
	@$(PYTHON) tools/build.py $@
fetch-speech:
	@$(PYTHON) tools/fetch_speech.py
doc:
	@$(UV) run --locked --group docs python -m sphinx -b html -n -W --keep-going doc doc/_build/html
web:
	@$(UV) run --locked --group web python tools/build.py web
asan:
	@$(MAKE) test SANITIZER=address BUILD_TYPE=Debug
tsan:
	@$(MAKE) test SANITIZER=thread BUILD_TYPE=Debug
