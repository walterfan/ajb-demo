PYTHON ?= python3
INPUT ?=
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
export INPUT OUT SCENARIO SEED ARGS BUILD_TYPE PROFILE_HOST PROFILE_BUILD
export MEDIA TUI TESTS SANITIZER OFFLINE JOBS
.PHONY: help deps doctor build start compare test fixtures fetch-speech clean asan tsan
help deps doctor build start compare test fixtures clean:
	@$(PYTHON) tools/build.py $@
fetch-speech:
	@$(PYTHON) tools/fetch_speech.py
asan:
	@$(MAKE) test SANITIZER=address BUILD_TYPE=Debug
tsan:
	@$(MAKE) test SANITIZER=thread BUILD_TYPE=Debug
