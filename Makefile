IMAGE ?= shopping-sessions:latest
CPUS  ?= 8
MEM   ?= 24g
PLATFORM ?= linux/amd64
DEV   ?= dev
FIXTURES ?= fixtures
export IMAGE CPUS MEM PLATFORM

.PHONY: build fixtures run-dev check-dev

## Build the image for the scorer's platform, linux/amd64
build:
	docker buildx build --platform $(PLATFORM) --load -t $(IMAGE) .

## Run the image on all 36 edge-case fixtures and diff against the expected tables
fixtures: build
	python3 scripts/run_fixtures.py $(FIXTURES) -- scripts/docker_run.sh

## Run locally with C++ binary
local-fixtures:
	.venv/bin/python scripts/run_fixtures.py fixtures -- bin/pipeline

## Run on dev set with scorer flags and time it
run-dev: build
	rm -rf out && time scripts/docker_run.sh $(DEV) out

## Compare dev output with dev-answer: make check-dev ANSWER=dev-answer
ANSWER ?= dev-answer
check-dev:
	python3 scripts/compare.py out $(ANSWER)

