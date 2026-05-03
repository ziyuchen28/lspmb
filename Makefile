BUILD_DIR := build

-include .env

# test 
T ?= .*

.PHONY: all config build clean 

all: config build


config:
	cmake -S . -B $(BUILD_DIR) 


build: config
	cmake --build $(BUILD_DIR) -j -- --no-print-directory


clean:
	rm -rf $(BUILD_DIR)


update-vendor: 
	git submodule update --remote
